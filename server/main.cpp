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
#include "sync_server_asio_ws.h"
#include "replay_saver.h"

#include "auth.h"
#include "command_queue.h"
#include "observe_request.h"
#include "observation.h"
#include "queries.h"
#include "query_request.h"
#include "ws_server.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
	// Observer sync WebSocket port. Speaks ws://host:PORT/observer with
	// RFC 6455 binary frames. Was raw TCP on 6112 pre-2026-07 -- we swap
	// to WS so observer traffic uses a firewall-friendlier protocol.
	int port = 6114;
	// Agent JSON WebSocket port. Speaks ws://host:PORT/agent with text
	// frames carrying the JSON agent protocol.
	int ws_port = 6113;
	uint32_t seed = 42;
	// Deterministic-repro knobs. Off by default.
	//
	// pin_start_seed: replaces the random per-game seed that
	// send_start_game() puts on the wire. Ties the pre-XOR raw
	// seed across runs but does NOT prevent client-UID mixing.
	// Set via --pin-start-seed.
	//
	// fixed_initial_rand: bypasses the seed-XOR-UID mixing and
	// uses fixed_initial_rand_value directly as the initial LCG
	// state. This is what you want for byte-exact soak repro.
	// Set via --fixed-initial-rand <hex>.
	bool pin_start_seed = false;
	uint32_t start_seed = 0;
	bool fixed_initial_rand = false;
	uint32_t fixed_initial_rand_value = 0;
	// Default 0: server starts the sim immediately and late-joining
	// observers catch up via id_catchup_data. Set >0 if you want the
	// server to hold the pre-game lobby until N observers have finished
	// the auth+uid handshake (useful when you want everyone to see the
	// opening seconds live rather than fast-forwarded).
	int wait_observers = 0;
	std::string users_path;
	// Inline user specs: alias:api_key:role[:slot], repeatable. Used by
	// control planes (EKS pods) that pass credentials as pod args
	// instead of shipping a users.json file into the container.
	std::vector<std::string> user_specs;
	// Hashed inline user specs: alias:sha256hex:role[:slot]. Used when
	// the control plane keeps only hashed keys and wants to pass them
	// on the CLI without exposing plaintext. Repeatable, and multiple
	// entries with the same alias (same role+slot) are allowed — one
	// user can have several active API keys, all valid.
	std::vector<std::string> user_hash_specs;
	bool no_auth = false;
	bool no_agents = false;
	// When true, the observer WS accepts any request path (not just
	// /observer). Meant for deployments behind an ALB that path-routes
	// each game pod (e.g. /game/{id}/observer -> container:6114).
	bool any_ws_path = false;
	// Diagnostic: if non-empty, append AGENT_SCHED/APPLY events for
	// each agent action to this file. Same format as the observer's
	// sync-log so `diff` catches divergence.
	std::string sync_log_path;
	// ms/frame. Retail BW ships seven speeds: slowest=167, slower=111,
	// slow=83, normal=67, fast=56, faster=48, fastest=42. Campaign
	// defaults to fast; multiplayer defaults to fastest. We pick fastest
	// as our default so agent iteration is snappy.
	int tick_ms = 42;

	// Per-slot race override. -1 means "use map default". Indices are
	// player slots (0..7). We only accept overrides for the 8 melee
	// slots; slots 8..11 are reserved (neutral/rescue/etc.).
	// race_t values match game_types.h: 0=zerg, 1=terran, 2=protoss.
	std::array<int8_t, 8> race_overrides{{-1,-1,-1,-1,-1,-1,-1,-1}};
};

// Named speeds (matches retail BW's ms/frame table).
inline int speed_name_to_ms(const std::string& name) {
	if (name == "slowest") return 167;
	if (name == "slower")  return 111;
	if (name == "slow")    return 83;
	if (name == "normal")  return 67;
	if (name == "fast")    return 56;
	if (name == "faster")  return 48;
	if (name == "fastest") return 42;
	return -1;
}

// Parse "N=NAME" (e.g., "0=protoss") into (slot, race_id). Returns
// false on malformed input. race_id matches game_types.h race_t.
inline bool parse_race_override(const std::string& v, int& slot, int8_t& race_id) {
	auto eq_pos = v.find('=');
	if (eq_pos == std::string::npos) return false;
	std::string lhs = v.substr(0, eq_pos);
	std::string rhs = v.substr(eq_pos + 1);
	if (lhs.empty() || rhs.empty()) return false;
	slot = std::atoi(lhs.c_str());
	if (slot < 0 || slot > 7) return false;
	if (rhs == "zerg")    { race_id = 0; return true; }
	if (rhs == "terran")  { race_id = 1; return true; }
	if (rhs == "protoss") { race_id = 2; return true; }
	return false;
}

inline const char* race_name(int8_t r) {
	switch (r) { case 0: return "zerg"; case 1: return "terran";
	             case 2: return "protoss"; default: return "?"; }
}

args_t parse_args(int argc, char** argv) {
	args_t a;
	for (int i = 1; i < argc; ++i) {
		auto eq = [&](const char* s) { return std::strcmp(argv[i], s) == 0; };
		if (eq("--data-path") && i + 1 < argc) a.data_path = argv[++i];
		else if (eq("--map") && i + 1 < argc) a.map_path = argv[++i];
		else if ((eq("--port") || eq("--obs-port")) && i + 1 < argc) a.port = std::atoi(argv[++i]);
		else if (eq("--seed") && i + 1 < argc) a.seed = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
		else if (eq("--pin-start-seed") && i + 1 < argc) {
			// Pin the wire-side start_game seed. Same value across
			// runs => identical id_start_game payload; the classic
			// UID-XOR still mixes in and produces a different final
			// initial_rand unless --fixed-initial-rand is also set.
			a.pin_start_seed = true;
			a.start_seed = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
		}
		else if (eq("--fixed-initial-rand") && i + 1 < argc) {
			// Byte-exact deterministic repro: set the initial LCG
			// state to this exact 32-bit value, bypassing the
			// seed-XOR-UID mixing entirely. Accepts hex (0x-prefixed
			// or bare hex like the value printed in GAME_START).
			a.fixed_initial_rand = true;
			// strtoul with base=0 handles 0x prefix; bare hex like
			// "63bbfad3" would parse as decimal though, so try hex
			// first for values without prefix.
			const char* v = argv[++i];
			char* end = nullptr;
			a.fixed_initial_rand_value =
				(uint32_t)std::strtoul(v, &end, 16);
		}
		else if (eq("--wait-observers") && i + 1 < argc) a.wait_observers = std::atoi(argv[++i]);
		else if (eq("--users") && i + 1 < argc) a.users_path = argv[++i];
		else if (eq("--user") && i + 1 < argc) a.user_specs.push_back(argv[++i]);
		else if (eq("--user-hash") && i + 1 < argc) a.user_hash_specs.push_back(argv[++i]);
		else if (eq("--no-auth")) a.no_auth = true;
		else if (eq("--ws-port") && i + 1 < argc) a.ws_port = std::atoi(argv[++i]);
		else if (eq("--no-agents")) a.no_agents = true;
		else if (eq("--any-ws-path")) a.any_ws_path = true;
		else if (eq("--sync-log") && i + 1 < argc) a.sync_log_path = argv[++i];
		else if (eq("--race") && i + 1 < argc) {
			std::string v = argv[++i];
			int slot; int8_t race_id;
			if (!parse_race_override(v, slot, race_id)) {
				fprintf(stderr,
					"error: --race expects <slot>=<zerg|terran|protoss>, "
					"slot in 0..7. got %s\n", v.c_str());
				std::exit(1);
			}
			a.race_overrides[slot] = race_id;
		}
		else if (eq("--game-speed") && i + 1 < argc) {
			std::string v = argv[++i];
			int as_name = speed_name_to_ms(v);
			if (as_name > 0) {
				a.tick_ms = as_name;
			} else {
				int as_int = std::atoi(v.c_str());
				if (as_int <= 0 || as_int > 1000) {
					fprintf(stderr,
						"error: --game-speed must be one of "
						"slowest/slower/slow/normal/fast/faster/fastest, "
						"or an integer number of ms/frame (1-1000). "
						"got %s\n", v.c_str());
					std::exit(1);
				}
				a.tick_ms = as_int;
			}
		}
		else if (eq("--help") || eq("-h")) {
			fprintf(stderr,
				"usage: %s --map <path> (--users <path> | --user <spec>... | --no-auth) [options]\n"
				"  --map              path to .scm/.scx map file\n"
				"  --data-path        dir containing StarDat.mpq et al (default: .)\n"
				"  --obs-port         Observer WebSocket port (default: 6114).\n"
				"                     Observers connect to ws://host:PORT/observer\n"
				"                     ?key=API_KEY and speak sync.h via WS binary\n"
				"                     frames. --port is a deprecated alias.\n"
				"  --seed             RNG seed (default: 42). Historical\n"
				"                     note: this value did NOT reach the sim\n"
				"                     until 2026-07-13; see --pin-start-seed\n"
				"                     and --fixed-initial-rand for reproducible\n"
				"                     runs.\n"
				"  --pin-start-seed <n> Pin the wire-side start_game seed to\n"
				"                     <n> so runs share the pre-mix seed.\n"
				"                     Client UIDs still mix in — use\n"
				"                     --fixed-initial-rand for byte-exact\n"
				"                     repro.\n"
				"  --fixed-initial-rand <hex>  Bypass the seed-XOR-UID mixing\n"
				"                     entirely and set the initial LCG state\n"
				"                     directly. Value is hex like the\n"
				"                     `initial_rand=XXXX` line the server\n"
				"                     prints at startup. Byte-exact repro\n"
				"                     across runs.\n"
				"  --wait-observers N wait for N observers to connect before\n"
				"                     starting the game (default: 0). With 0,\n"
				"                     the game starts immediately and late\n"
				"                     joiners catch up via replay fast-forward.\n"
				"  --users <path>     users.json file for API-key auth\n"
				"  --user <spec>      inline user, alias:api_key:role[:slot].\n"
				"                     role is one of player/observer/admin.\n"
				"                     slot required for player, ignored else.\n"
				"                     Repeat for multiple users. Composes with\n"
				"                     --users (both are loaded together). Meant\n"
				"                     for control planes that pass creds as pod\n"
				"                     args instead of shipping a users.json file.\n"
				"  --user-hash <spec> same shape as --user but middle field is\n"
				"                     a 64-char hex SHA-256 of the API key.\n"
				"                     Lets control planes pass hashed creds\n"
				"                     without ever exposing plaintext on the\n"
				"                     command line. Multiple entries per alias\n"
				"                     are allowed (all active keys map to the\n"
				"                     same identity).\n"
				"  --no-auth          disable auth entirely (dev-only)\n"
				"  --ws-port          TCP port for agent WebSocket (default: 6113)\n"
				"  --no-agents        disable the agent WebSocket server\n"
				"  --any-ws-path      accept observer WS on any path\n"
				"                     (default: /observer only). Used\n"
				"                     when ALB path-routes to this pod.\n"
				"  --sync-log <path>  append per-frame agent-action events\n"
				"                     to <path>. Diff against observer's\n"
				"                     sync-log to find replay divergence.\n"
				"  --game-speed <s>   ms/frame; either an integer or a BW\n"
				"                     name: slowest, slower, slow, normal,\n"
				"                     fast, faster, fastest (default:\n"
				"                     fastest = 42 ms/frame ~ 24 FPS).\n"
				"                     Retail BW campaign uses 'fast' (56).\n"
				"  --race N=RACE      override slot N's race, one of\n"
				"                     zerg/terran/protoss. Repeat for\n"
				"                     multiple slots (e.g. --race 0=zerg\n"
				"                     --race 1=terran). Overrides the race\n"
				"                     the map assigned to that slot. Only\n"
				"                     meaningful on melee maps where\n"
				"                     starting units are spawned by race.\n",
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
	bool has_auth_source =
		!a.users_path.empty() || !a.user_specs.empty() || !a.user_hash_specs.empty();
	if (!has_auth_source && !a.no_auth) {
		fprintf(stderr,
			"error: pass --users <path>, --user <spec>, --user-hash <spec>, "
			"or --no-auth for dev\n");
		std::exit(1);
	}
	if (has_auth_source && a.no_auth) {
		fprintf(stderr,
			"error: --users / --user / --user-hash and --no-auth are mutually exclusive\n");
		std::exit(1);
	}
	if (a.wait_observers < 0) a.wait_observers = 0;
	return a;
}

} // anonymous namespace

int main(int argc, char** argv) {
	auto args = parse_args(argc, argv);

	fprintf(stderr, "[srv] starting: map=%s data=%s obs-port=%d agent-port=%d seed=%u\n",
		args.map_path.c_str(), args.data_path.c_str(),
		args.port, args.ws_port, args.seed);

	// 1. Load game data + map. We can't use game_player::load_map_file
	//    directly because it constructs its own game_load_functions with
	//    default setup_info (no starting units in melee mode). Drive it
	//    ourselves so we can set create_melee_units_for_player[] = true.
	game_player player{a_string(args.data_path.c_str())};
	state& st = player.st();
	{
		game_load_functions loader(st);
		for (size_t i = 0; i < 8; ++i) loader.setup_info.create_melee_units_for_player[i] = true;

		// setup_f runs inside load_map_file AFTER the map's SIDE chunk
		// has populated st.players[i].race and BEFORE create_starting_units
		// spawns the initial units. This is where we override, so the
		// spawn code sees the new race and hands out the right nexus/
		// hatchery/CC + workers. See bwgame.h load_map_data around the
		// setup_f() call and the create_starting_units loop below it.
		auto setup_f = [&args, &st]() {
			// Preserve BW's default: turn "open" slots into "occupied"
			// so create_starting_units actually spawns for them. This
			// mirrors the fallback block that runs when setup_f is
			// omitted (bwgame.h ~ line 21790).
			for (size_t i = 0; i != 12; ++i) {
				if (st.players[i].controller == player_t::controller_open) {
					st.players[i].controller = player_t::controller_occupied;
				}
				if (st.players[i].controller == player_t::controller_computer) {
					st.players[i].controller = player_t::controller_computer_game;
				}
			}
			// Then apply race overrides.
			for (size_t i = 0; i < 8; ++i) {
				if (args.race_overrides[i] < 0) continue;
				race_t old_race = st.players[i].race;
				st.players[i].race = (race_t)args.race_overrides[i];
				fprintf(stderr,
					"[srv] slot %zu race override: %d -> %s\n",
					i, (int)old_race,
					race_name(args.race_overrides[i]));
			}
		};
		loader.load_map_file(a_string(args.map_path.c_str()), setup_f);
	}
	action_state action_st;
	sync_state sync_st;
	sync_functions funcs(st, action_st, sync_st);
	game_load_functions::setup_info_t setup_info;
	for (size_t i = 0; i < 8; ++i) setup_info.create_melee_units_for_player[i] = true;
	sync_st.setup_info = &setup_info;
	sync_st.latency = 2;

	// Wire deterministic-repro flags into sync_state. Off by default.
	if (args.pin_start_seed) {
		sync_st.fixed_start_seed_set = true;
		sync_st.fixed_start_seed = args.start_seed;
	}
	if (args.fixed_initial_rand) {
		sync_st.fixed_initial_rand = true;
		sync_st.fixed_initial_rand_value = args.fixed_initial_rand_value;
	}

	// Give the local (server) client a name so the sync handshake is happy.
	sync_st.local_client->name = "openbw_server";

	// Enable the replay recorder so every applied action gets appended to
	// an in-memory history buffer. This is what we ship to late-joining
	// observers in id_catchup_data.
	replay_saver_state replay_saver;
	sync_st.save_replay = &replay_saver;

	// Provide the catchup bundle when sync.h asks for it. Concatenates the
	// history deque into one contiguous byte vector -- observers stream it
	// through action_functions::execute_actions to fast-forward.
	sync_st.catchup_provider = [&]() {
		sync_state::catchup_bundle_t b;
		b.current_frame = (uint32_t)st.current_frame;
		b.seed = sync_st.initial_rand_state;
		// Ship the server's PRE-random-pick races (initial_slot_races,
		// captured at first sync_next_frame from st.players[i].race
		// AFTER setup_f applied any --race overrides). Observer will
		// install these into st.players[i].race BEFORE start_game_local
		// so its start_game_impl runs the identical lcg_rand(144) calls
		// for "any race" slots (race==5) that the server did. That
		// keeps the RNG stream synchronized through the randomize_slots
		// pass at the bottom of start_game_impl -- otherwise on 4+
		// player melee maps the observer picks a different slot
		// permutation and its whole game is slot-swapped from the
		// server's (SyncBreaker #3, 2026-07-11).
		//
		// We intentionally do NOT ship the post-randomize st.players[i]
		// .race here. If we did, observer would see race<=2 and skip
		// the lcg_rand(144) call that server made, desyncing the RNG.
		for (int i = 0; i < 12; ++i) {
			b.slot_races[i] = (uint8_t)sync_st.initial_slot_races[i];
		}
		size_t total = 0;
		for (auto& chunk : replay_saver.history) total += chunk.size();
		b.action_bytes.reserve(total);
		for (auto& chunk : replay_saver.history) {
			b.action_bytes.insert(b.action_bytes.end(), chunk.begin(), chunk.end());
		}
		fprintf(stderr, "[srv] catchup bundle: frame=%u seed=%08x action_bytes=%zu\n",
			b.current_frame, b.seed, b.action_bytes.size());
		return b;
	};

	// Wire diagnostic sync-log sink if requested.
	if (!args.sync_log_path.empty()) {
		auto f = std::make_shared<std::ofstream>(args.sync_log_path,
			std::ios::out | std::ios::trunc);
		if (!f->good()) {
			fprintf(stderr, "[srv] failed to open --sync-log=%s\n",
				args.sync_log_path.c_str());
			return 1;
		}
		fprintf(stderr, "[srv] sync-log -> %s\n", args.sync_log_path.c_str());
		// Sync-log writers can come from two threads:
		//   - sim thread (regular TICK / INVENTORY / AGENT_* rows)
		//   - WS worker thread (AGENT_ISSUE rows from handle_cmd)
		// Wrap the ofstream in a small mutex so their writes don't
		// interleave mid-line. Static so the closure captures the
		// same instance for both threads.
		static std::mutex sync_log_mu;
		sync_st.sync_log = [f](const bwgame::a_string& s) {
			std::lock_guard<std::mutex> lk(sync_log_mu);
			f->write(s.data(), (std::streamsize)s.size());
			f->flush();
		};
	}

	// Wire auth if requested. --users (file) and --user (inline specs)
	// compose: both are loaded into the same registry. This lets a k8s
	// control plane pass credentials as --user args without shipping a
	// users.json file into the pod, while local dev keeps using the file.
	openbw_auth::user_registry registry;
	if (!args.users_path.empty()) {
		try {
			size_t n = registry.load_file(args.users_path);
			fprintf(stderr, "[srv] loaded %zu users from %s\n", n, args.users_path.c_str());
		} catch (const std::exception& e) {
			fprintf(stderr, "[srv] auth load failed: %s\n", e.what());
			return 1;
		}
	}
	if (!args.user_specs.empty()) {
		try {
			size_t n = 0;
			for (const auto& spec : args.user_specs) n += registry.add_from_spec(spec);
			fprintf(stderr, "[srv] loaded %zu inline user(s) from --user\n", n);
		} catch (const std::exception& e) {
			fprintf(stderr, "[srv] --user parse failed: %s\n", e.what());
			return 1;
		}
	}
	if (!args.user_hash_specs.empty()) {
		try {
			size_t n = 0;
			for (const auto& spec : args.user_hash_specs)
				n += registry.add_from_spec_hash(spec);
			fprintf(stderr, "[srv] loaded %zu hashed user(s) from --user-hash\n", n);
		} catch (const std::exception& e) {
			fprintf(stderr, "[srv] --user-hash parse failed: %s\n", e.what());
			return 1;
		}
	}
	if (registry.size() > 0) {
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
		// After auth: tell each client which slot's perspective to render.
		// Player role -> their own slot. Observer role -> the slot they're
		// assigned to (or -1 = full vision). Admin -> full vision.
		sync_st.perspective_for = [](const void* auth_user) -> int8_t {
			if (!auth_user) return -1;
			auto* u = (const openbw_auth::user_t*)auth_user;
			if (u->role == openbw_auth::role_t::admin) return -1;
			return (int8_t)u->assigned_slot;
		};
	} else {
		fprintf(stderr, "[srv] WARNING: running with --no-auth\n");
	}

	// 2. Bind WebSocket acceptor for observer sync-transport traffic.
	//    Speaks ws://host:PORT/observer with WS binary frames carrying
	//    the sync.h wire protocol. Auth uses the SAME user_registry
	//    as the agent WS -- key comes in via ?key=API_KEY query param,
	//    validated at HTTP upgrade time before we send 101.
	//
	//    Was raw TCP with u16-length framing on 6112 before 2026-07;
	//    migrated to WS so observer traffic passes corporate firewalls
	//    that block non-standard ports and non-HTTP protocols. The old
	//    sync_server_asio_tcp transport is retired -- its .h stays in
	//    tree for now as a reference but is no longer wired.
	sync_server_asio_ws server;
	// Under ALB path-routing (e.g. /game/{id}/observer -> container
	// port 6114), the load balancer has already gated by path before
	// the request arrives; the server accepting any path is safe.
	// --any-ws-path opts in.
	server.server_path = args.any_ws_path ? std::string() : std::string("/observer");
	if (!args.no_auth) {
		server.auth_fn = [&registry](const std::string& api_key) {
			if (api_key.empty()) return false;
			const auto* u = registry.verify(api_key);
			return u != nullptr;
		};
	}
	server.bind("0.0.0.0", args.port);
	fprintf(stderr, "[srv] observer WS listening on ws://0.0.0.0:%d/observer\n",
		args.port);

	// 3. Optionally hold pre-game until N observers have connected. With
	//    N == 0 (default), the sim starts right away and late joiners
	//    catch up via id_catchup_data.
	auto count_ready_observers = [&]() {
		int n = 0;
		for (auto& c : sync_st.clients) {
			if (&c == sync_st.local_client) continue;
			if (c.has_uid) ++n;
		}
		return n;
	};

	if (args.wait_observers > 0) {
		fprintf(stderr, "[srv] waiting for %d observer(s) to connect...\n", args.wait_observers);
		int last_reported = -1;
		while (true) {
			funcs.sync(server);
			int n_ready = count_ready_observers();
			if (n_ready != last_reported) {
				fprintf(stderr, "[srv]  observers ready: %d/%d\n", n_ready, args.wait_observers);
				last_reported = n_ready;
			}
			if (n_ready >= args.wait_observers) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		fprintf(stderr, "[srv] target observer count reached; short grace period...\n");
		auto grace_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
		while (std::chrono::steady_clock::now() < grace_end) {
			funcs.sync(server);
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	} else {
		fprintf(stderr, "[srv] starting immediately; observers may join at any time.\n");
	}

	// funcs.start_game() only kicks off a countdown; the actual sim init
	// (and initial_rand_state population) happens a few sync cycles later
	// inside process_messages(). Log the true state after that settles.
	funcs.start_game(server);
	while (!sync_st.game_started) {
		funcs.sync(server);
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	fprintf(stderr, "[srv] game started, initial_rand=%08x (%d observers)\n",
		sync_st.initial_rand_state, count_ready_observers());
	if (sync_st.sync_log) {
		char buf[64];
		snprintf(buf, sizeof(buf), "GAME_START\tinitial_rand=%08x",
			sync_st.initial_rand_state);
		bwgame::sync_log_line(sync_st, 'S', bwgame::a_string(buf));
	}

	// Register 8 virtual sync clients -- one per player slot -- so agent
	// actions can be dispatched via sync.h's execute_scheduled_actions
	// path. These have has_auth=true, has_uid=true, socket handle nullptr,
	// and their assigned player_slot. sync.h::execute_scheduled_actions
	// gates on client->player_slot != -1, so these virtual clients are
	// the only way to inject agent-owned actions.
	sync_state::client_t* virtual_clients[8] = {nullptr};
	for (int slot = 0; slot < 8; ++slot) {
		if (st.players[slot].controller != bwgame::player_t::controller_occupied &&
		    st.players[slot].controller != bwgame::player_t::controller_computer_game) {
			continue; // inactive slot; no unit to command
		}
		sync_st.clients.emplace_back();
		auto& c = sync_st.clients.back();
		c.local_id = sync_st.next_client_id++;
		c.uid = sync_state::uid_t::generate();
		c.has_uid = true;
		c.has_auth = true;
		c.has_greeted = true;
		// game_started is normally flipped when a client receives the
		// id_game_started network message. Virtual clients have no
		// socket and never receive that message, but the sim IS started
		// on the server, so we set it directly. Without this,
		// execute_scheduled_actions skips their queued actions.
		c.game_started = true;
		c.player_slot = slot;
		c.frame = (uint32_t)sync_st.sync_frame;
		c.name = bwgame::a_string("agent_") + bwgame::a_string(std::to_string(slot).c_str());
		virtual_clients[slot] = &c;
		// Publish into the sync_state array so recv-side code (on
		// observers) can look them up by slot, and so future code paths
		// don't need this local `virtual_clients` array to know per-slot
		// identity.
		sync_st.virtual_clients_by_slot[slot] = &c;
		fprintf(stderr, "[srv] registered virtual client for slot %d\n", slot);
	}

	// Debug: dump initial units per player so agents can see real unit
	// ids until the observation serializer (task #11) lands. Note that
	// unit_id embeds a 5-bit generation counter in the top bits, so raw
	// unit_id.raw_value != unit index -- use the raw value in agent
	// commands.
	// Dump one line per active slot so agents can see their race/units at
	// startup without an observe() call. observe() is the intended way to
	// discover unit ids at runtime.
	for (int slot = 0; slot < 8; ++slot) {
		if (st.players[slot].controller != bwgame::player_t::controller_occupied &&
		    st.players[slot].controller != bwgame::player_t::controller_computer_game) continue;
		int nunits = 0;
		for (auto* u : bwgame::ptr(st.player_units[slot])) { (void)u; ++nunits; }
		fprintf(stderr, "[srv] slot %d race=%d units=%d\n",
			slot, (int)st.players[slot].race, nunits);
	}

	// Command queue: producers push here from WebSocket handler threads;
	// the sim thread drains once per tick.
	openbw_agents::command_queue cmd_queue;
	// Observation request queue: WS handler pushes when an agent calls
	// observe(); sim thread drains + serializes on tick.
	openbw_agents::observe_queue obs_queue;
	// General read-only query queue: find_placement, future kinds.
	openbw_agents::query_queue q_queue;
	std::atomic<int> current_frame_atomic{0};

	// Start the agent WS server unless disabled. Agent WS needs auth --
	// either --users or --user must have loaded at least one user.
	std::unique_ptr<openbw_agents::ws_server> ws;
	if (!args.no_agents && registry.size() > 0) {
		ws = std::make_unique<openbw_agents::ws_server>(
			registry, cmd_queue, obs_queue, q_queue, current_frame_atomic);
		// When --sync-log is set, also emit AGENT_ISSUE rows into the
		// same file so agent-side send events can be joined to the
		// sim-side AGENT_SCHED_LOCAL / SEND / APPLY rows via the rid.
		// Sync_log's own mutex (set up above) serializes writes across
		// this WS thread and the sim thread.
		if (sync_st.sync_log) {
			auto sync_log_ref = sync_st.sync_log;  // std::function copy
			ws->action_log_fn = [sync_log_ref](const std::string& line) {
				sync_log_ref(bwgame::a_string(line.c_str()));
			};
		}
		ws->start((uint16_t)args.ws_port);
	} else if (!args.no_agents) {
		fprintf(stderr, "[srv] agent WS requires --users or --user; skipping (or pass --no-agents).\n");
	}

	// 4. Fixed-rate tick loop.
	using clock_t = std::chrono::steady_clock;
	const auto tick_interval = std::chrono::milliseconds(args.tick_ms);
	fprintf(stderr, "[srv] tick_interval=%dms (%.1f FPS)\n",
		args.tick_ms, 1000.0 / args.tick_ms);
	auto next_tick = clock_t::now() + tick_interval;
	auto last_heartbeat = clock_t::now();

	// Diagnostic counters, reset every heartbeat second. If the loop
	// body (queue drains + next_frame + observer broadcasts) exceeds
	// tick_interval, our fixed-rate scheduler can't sleep and the sim
	// runs as fast as the code allows -- --game-speed then has no
	// effect. Track this so we can tell a "flag not working" issue
	// from a "budget exhausted" issue.
	int overrun_frames = 0;      // frames whose body exceeded tick_interval
	int total_frames = 0;
	long long worst_ms = 0;      // slowest single tick body this second

	while (true) {
		auto loop_start = clock_t::now();

		// Drain pending agent commands in slot order (0 -> 7, FIFO within
		// each slot). Each command was pre-encoded by ws_server into one
		// or more BW action blobs (select + verb, typically).
		//
		// SyncBreaker #8 fix (2026-07-14): buffer ALL of this tick's
		// actions into one batch and broadcast them together with
		// broadcast_agent_action_batch, preserving the strict slot-major
		// FIFO-within-slot order the server itself applies them in. The
		// old code called broadcast_agent_action() per action, which
		// produced N separate wire messages. That was correct on the
		// wire but the observer's per-vc scheduled_actions queues + the
		// order it iterated sync_st.clients could reorder intra-tick
		// actions -- a real divergence source under load. See task #144
		// and the id_agent_action_batch enum comment in sync.h.
		//
		// Local server sim still uses schedule_action per action -- its
		// own execute_scheduled_actions runs immediately below, and its
		// client-list ordering was already correct because start_game
		// sorts by player_slot up-front (a property the observer's
		// lazily-created vc list doesn't share).
		// Own-the-bytes buffer so pointers stay valid through the batch
		// broadcast that fires after drain() returns. cmd_queue.drain
		// hands us pointers into deques it swap-owns internally; those
		// deques are destroyed at drain-return time.
		struct owned_action { int slot; std::vector<uint8_t> data; };
		std::vector<owned_action> batch_owned;
		cmd_queue.drain([&](int slot, const uint8_t* data, size_t size) {
			auto* vc = virtual_clients[slot];
			if (!vc) return;
			// Keep virtual client's frame counter aligned so sync's lag
			// check doesn't stall on this "client".
			vc->frame = (uint32_t)sync_st.sync_frame;
			funcs.schedule_action(vc, data, size);
			batch_owned.push_back({slot,
				std::vector<uint8_t>(data, data + size)});
		});
		if (!batch_owned.empty()) {
			std::vector<bwgame::action_batch_entry> batch_view;
			batch_view.reserve(batch_owned.size());
			for (auto& e : batch_owned) {
				batch_view.push_back({e.slot, e.data.data(), e.data.size()});
			}
			funcs.broadcast_agent_action_batch(server, batch_view);
		}

		// Drain pending observe requests: build the observation JSON on
		// the sim thread (safe -- we own state here), then hand it back
		// via the request's respond callback which posts to the WS thread.
		obs_queue.drain([&](int slot, openbw_agents::observe_request& req) {
			auto opts = openbw_agents::parse_targets(req.targets);
			std::string body = openbw_agents::build_observation(
				funcs, slot, (uint32_t)st.current_frame,
				req.request_id, opts);
			if (req.respond) req.respond(std::move(body));
		});

		// Drain pending read-only queries (find_placement, ...). Sim
		// thread safe to read state here.
		q_queue.drain([&](int slot, openbw_agents::query_request& req) {
			std::string body;
			if (req.kind == "find_placement") {
				body = openbw_agents::build_placement_response(
					funcs, slot, req.request_id, req.payload);
			} else {
				nlohmann::json err;
				err["type"] = "error";
				err["id"] = req.request_id;
				err["message"] = "unknown query kind: " + req.kind;
				body = err.dump();
			}
			if (req.respond) req.respond(std::move(body));
		});

		// Keep every virtual client's frame counter up to date, even if
		// no commands arrived, or all_clients_in_sync could stall.
		for (auto& c : sync_st.clients) {
			if (&c != sync_st.local_client && c.player_slot >= 0) {
				c.frame = (uint32_t)sync_st.sync_frame;
			}
		}

		funcs.next_frame(server); // advances sim + syncs to observers
		current_frame_atomic.store(st.current_frame);

		// Diagnostic: every 300 frames dump inventory for slots 0..1 so
		// we can compare against the observer's dump. Diff reveals sim
		// divergence.
		if (sync_st.sync_log && st.current_frame > 0
		    && st.current_frame % 300 == 0)
		{
			for (int s = 0; s < 2; ++s) funcs.log_inventory('S', s);
		}

		auto now = clock_t::now();

		// Measure tick-body cost. If we routinely exceed tick_interval,
		// --game-speed has no effect: the fixed-rate loop can't sleep.
		auto body_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			now - loop_start).count();
		++total_frames;
		if (body_ms > args.tick_ms) ++overrun_frames;
		if (body_ms > worst_ms) worst_ms = body_ms;

		if (now - last_heartbeat >= std::chrono::seconds(1)) {
			int n_clients = 0;
			int n_observers = 0;
			int n_agents = 0;
			for (auto& c : sync_st.clients) {
				++n_clients;
				if (&c == sync_st.local_client) continue;
				if (c.player_slot == -1) ++n_observers;
				else if (c.h == nullptr) ++n_agents;
			}
			fprintf(stderr,
				"[srv] frame=%d observers=%d virtual-agents=%d pending-cmds=%zu "
				"loop_body: %d/%d over budget (worst=%lldms, budget=%dms)\n",
				st.current_frame, n_observers, n_agents, cmd_queue.total_pending(),
				overrun_frames, total_frames, worst_ms, args.tick_ms);
			if (overrun_frames > total_frames / 2) {
				fprintf(stderr,
					"[srv] WARNING: over half of ticks exceeded budget; "
					"--game-speed setting won't slow the sim below what "
					"the loop body can achieve.\n");
			}
			overrun_frames = 0;
			total_frames = 0;
			worst_ms = 0;
			last_heartbeat = now;
		}

		auto sleep_until = next_tick;
		next_tick += tick_interval;
		if (clock_t::now() < sleep_until) std::this_thread::sleep_until(sleep_until);
		else next_tick = clock_t::now() + tick_interval; // fell behind; reset schedule
	}
	return 0;
}
