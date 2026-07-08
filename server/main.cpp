// openbw_server: headless authoritative sim.
//
// For the initial observer-mode test, the server:
//   1) loads a map (same file the observer will load),
//   2) binds a TCP port, accepts observer peers as sync.h clients,
//   3) starts the game with a fixed seed,
//   4) advances one BW frame every ~42ms, letting sync.h broadcast to
//      any connected observers.
//
// No agents, no HTTP, no observation JSON. Once this + observer client are
// working end-to-end we'll layer the agent RPC on top.

#include "bwgame.h"
#include "sync.h"
#include "sync_server_asio_tcp.h"

#include "auth.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

using namespace bwgame;

namespace bwgame {
namespace ui {
// sync.h -> bwgame.h uses this for panic messages. Provide a plain stderr
// implementation for the headless server.
void log_str(a_string str) {
	fwrite(str.data(), str.size(), 1, stderr);
	fflush(stderr);
}
void fatal_error_str(a_string str) {
	fprintf(stderr, "fatal error: %.*s\n", (int)str.size(), str.data());
	std::terminate();
}
}
}

namespace {

struct args_t {
	std::string data_path = ".";
	std::string map_path;
	int port = 6112;
	uint32_t seed = 42;
	int wait_observers = 1;
	std::string users_path;
	bool no_auth = false;
};

args_t parse_args(int argc, char** argv) {
	args_t a;
	for (int i = 1; i < argc; ++i) {
		auto eq = [&](const char* s) { return std::strcmp(argv[i], s) == 0; };
		if (eq("--data-path") && i + 1 < argc) a.data_path = argv[++i];
		else if (eq("--map") && i + 1 < argc) a.map_path = argv[++i];
		else if (eq("--port") && i + 1 < argc) a.port = std::atoi(argv[++i]);
		else if (eq("--seed") && i + 1 < argc) a.seed = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
		else if (eq("--wait-observers") && i + 1 < argc) a.wait_observers = std::atoi(argv[++i]);
		else if (eq("--users") && i + 1 < argc) a.users_path = argv[++i];
		else if (eq("--no-auth")) a.no_auth = true;
		else if (eq("--help") || eq("-h")) {
			fprintf(stderr,
				"usage: %s --map <path> (--users <path> | --no-auth) [options]\n"
				"  --map              path to .scm/.scx map file\n"
				"  --data-path        dir containing StarDat.mpq et al (default: .)\n"
				"  --port             TCP port to bind (default: 6112)\n"
				"  --seed             RNG seed (default: 42)\n"
				"  --wait-observers N wait for N observers to connect before\n"
				"                     starting the game (default: 1). Late\n"
				"                     joiners are rejected until task #13 lands.\n"
				"  --users <path>     users.json for API-key auth\n"
				"  --no-auth          disable auth entirely (dev-only)\n",
				argv[0]);
			std::exit(0);
		} else {
			fprintf(stderr, "unknown arg: %s (try --help)\n", argv[i]);
			std::exit(1);
		}
	}
	if (a.map_path.empty()) {
		fprintf(stderr, "error: --map is required (try --help)\n");
		std::exit(1);
	}
	if (a.users_path.empty() && !a.no_auth) {
		fprintf(stderr, "error: --users <path> is required (or pass --no-auth for dev)\n");
		std::exit(1);
	}
	if (!a.users_path.empty() && a.no_auth) {
		fprintf(stderr, "error: --users and --no-auth are mutually exclusive\n");
		std::exit(1);
	}
	if (a.wait_observers < 1) a.wait_observers = 1;
	return a;
}

} // anonymous namespace

int main(int argc, char** argv) {
	auto args = parse_args(argc, argv);

	fprintf(stderr, "[srv] starting: map=%s data=%s port=%d seed=%u\n",
		args.map_path.c_str(), args.data_path.c_str(), args.port, args.seed);

	// 1. Load game data + map. We can't use game_player::load_map_file
	//    directly because it constructs its own game_load_functions with
	//    default setup_info (no starting units in melee mode). Drive it
	//    ourselves so we can set create_melee_units_for_player[] = true.
	game_player player{a_string(args.data_path.c_str())};
	state& st = player.st();
	{
		game_load_functions loader(st);
		for (size_t i = 0; i < 8; ++i) loader.setup_info.create_melee_units_for_player[i] = true;
		loader.load_map_file(a_string(args.map_path.c_str()));
	}
	action_state action_st;
	sync_state sync_st;
	sync_functions funcs(st, action_st, sync_st);
	game_load_functions::setup_info_t setup_info;
	for (size_t i = 0; i < 8; ++i) setup_info.create_melee_units_for_player[i] = true;
	sync_st.setup_info = &setup_info;
	sync_st.latency = 2;

	// Give the local (server) client a name so the sync handshake is happy.
	sync_st.local_client->name = "openbw_server";

	// Wire auth if requested.
	openbw_auth::user_registry registry;
	if (!args.users_path.empty()) {
		try {
			size_t n = registry.load_file(args.users_path);
			fprintf(stderr, "[srv] loaded %zu users from %s\n", n, args.users_path.c_str());
		} catch (const std::exception& e) {
			fprintf(stderr, "[srv] auth load failed: %s\n", e.what());
			return 1;
		}
		sync_st.auth_check = [&registry](const uint8_t* key, size_t key_len) -> const void* {
			std::string_view sv((const char*)key, key_len);
			const auto* u = registry.verify(sv);
			if (u) {
				fprintf(stderr, "[srv] auth OK: alias=%s slot=%d\n", u->alias.c_str(), u->assigned_slot);
			} else {
				fprintf(stderr, "[srv] auth FAIL (unknown key)\n");
			}
			return u;
		};
	} else {
		fprintf(stderr, "[srv] WARNING: running with --no-auth\n");
	}

	// 2. Bind TCP acceptor.
	sync_server_asio_tcp server;
	server.bind("0.0.0.0", args.port);
	fprintf(stderr, "[srv] listening on 0.0.0.0:%d\n", args.port);

	// 3. sync.h refuses new connections once game_started is true (this is
	//    the "no late-join" gap tracked in task #13). Until we implement
	//    fast-forward replay for late joiners, wait for `wait_observers`
	//    observers to connect and complete the handshake before starting.
	//
	//    Use funcs.sync(server) here rather than raw server.poll() -- sync()
	//    binds the proper on_new_client handler in sync.h that actually
	//    registers new peers in sync_st.clients.
	auto count_ready_observers = [&]() {
		int n = 0;
		for (auto& c : sync_st.clients) {
			if (&c == sync_st.local_client) continue;
			// has_uid == true means the client has completed the greeting +
			// uid exchange (see sync.h::recv id_client_uid). Anything less
			// and we can't safely start_game.
			if (c.has_uid) ++n;
		}
		return n;
	};

	fprintf(stderr, "[srv] waiting for %d observer(s) to connect...\n", args.wait_observers);
	int last_reported = -1;
	auto start_wait = std::chrono::steady_clock::now();
	while (true) {
		funcs.sync(server);
		int n_ready = count_ready_observers();
		if (n_ready != last_reported) {
			fprintf(stderr, "[srv]  observers ready: %d/%d\n", n_ready, args.wait_observers);
			last_reported = n_ready;
			start_wait = std::chrono::steady_clock::now();
		}
		if (n_ready >= args.wait_observers) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	// Extra grace period so any in-flight handshakes settle. Without this,
	// a second observer that arrives right at the boundary can miss the
	// pre-game window.
	fprintf(stderr, "[srv] target observer count reached; short grace period...\n");
	auto grace_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
	while (std::chrono::steady_clock::now() < grace_end) {
		funcs.sync(server);
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	(void)start_wait;

	funcs.start_game(server);
	fprintf(stderr, "[srv] game started with seed=%u (%d observers)\n",
		args.seed, count_ready_observers());

	// 4. Fixed-rate tick loop.
	using clock_t = std::chrono::steady_clock;
	const auto tick_interval = std::chrono::milliseconds(42); // ~24 FPS
	auto next_tick = clock_t::now() + tick_interval;
	auto last_heartbeat = clock_t::now();

	while (true) {
		funcs.next_frame(server); // advances sim + syncs to observers

		auto now = clock_t::now();
		if (now - last_heartbeat >= std::chrono::seconds(1)) {
			int n_clients = 0;
			int n_observers = 0;
			for (auto& c : sync_st.clients) {
				++n_clients;
				if (c.player_slot == -1 && &c != sync_st.local_client) ++n_observers;
			}
			fprintf(stderr, "[srv] frame=%d clients=%d observers=%d\n",
				st.current_frame, n_clients, n_observers);
			last_heartbeat = now;
		}

		auto sleep_until = next_tick;
		next_tick += tick_interval;
		if (clock_t::now() < sleep_until) std::this_thread::sleep_until(sleep_until);
		else next_tick = clock_t::now() + tick_interval; // fell behind; reset schedule
	}
	return 0;
}
