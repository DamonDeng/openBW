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
#include "replay_saver.h"

#include "auth.h"
#include "command_queue.h"
#include "observe_request.h"
#include "observation.h"
#include "queries.h"
#include "query_request.h"
#include "ws_server.h"

#include <atomic>
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
	int ws_port = 6113;
	uint32_t seed = 42;
	// Default 0: server starts the sim immediately and late-joining
	// observers catch up via id_catchup_data. Set >0 if you want the
	// server to hold the pre-game lobby until N observers have finished
	// the auth+uid handshake (useful when you want everyone to see the
	// opening seconds live rather than fast-forwarded).
	int wait_observers = 0;
	std::string users_path;
	bool no_auth = false;
	bool no_agents = false;
	// ms/frame. Retail BW ships seven speeds: slowest=167, slower=111,
	// slow=83, normal=67, fast=56, faster=48, fastest=42. Campaign
	// defaults to fast; multiplayer defaults to fastest. We pick fastest
	// as our default so agent iteration is snappy.
	int tick_ms = 42;
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
		else if (eq("--ws-port") && i + 1 < argc) a.ws_port = std::atoi(argv[++i]);
		else if (eq("--no-agents")) a.no_agents = true;
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
				"usage: %s --map <path> (--users <path> | --no-auth) [options]\n"
				"  --map              path to .scm/.scx map file\n"
				"  --data-path        dir containing StarDat.mpq et al (default: .)\n"
				"  --port             TCP port to bind (default: 6112)\n"
				"  --seed             RNG seed (default: 42)\n"
				"  --wait-observers N wait for N observers to connect before\n"
				"                     starting the game (default: 0). With 0,\n"
				"                     the game starts immediately and late\n"
				"                     joiners catch up via replay fast-forward.\n"
				"  --users <path>     users.json for API-key auth\n"
				"  --no-auth          disable auth entirely (dev-only)\n"
				"  --ws-port          TCP port for agent WebSocket (default: 6113)\n"
				"  --no-agents        disable the agent WebSocket server\n"
				"  --game-speed <s>   ms/frame; either an integer or a BW\n"
				"                     name: slowest, slower, slow, normal,\n"
				"                     fast, faster, fastest (default:\n"
				"                     fastest = 42 ms/frame ~ 24 FPS).\n"
				"                     Retail BW campaign uses 'fast' (56).\n",
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
	if (a.wait_observers < 0) a.wait_observers = 0;
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

	// 2. Bind TCP acceptor.
	sync_server_asio_tcp server;
	server.bind("0.0.0.0", args.port);
	fprintf(stderr, "[srv] listening on 0.0.0.0:%d\n", args.port);

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
		c.frame = (uint8_t)sync_st.sync_frame;
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

	// Start the agent WS server unless disabled.
	std::unique_ptr<openbw_agents::ws_server> ws;
	if (!args.no_agents && !args.users_path.empty()) {
		ws = std::make_unique<openbw_agents::ws_server>(
			registry, cmd_queue, obs_queue, q_queue, current_frame_atomic);
		ws->start((uint16_t)args.ws_port);
	} else if (!args.no_agents) {
		fprintf(stderr, "[srv] agent WS requires --users; skipping (or pass --no-agents).\n");
	}

	// 4. Fixed-rate tick loop.
	using clock_t = std::chrono::steady_clock;
	const auto tick_interval = std::chrono::milliseconds(args.tick_ms);
	fprintf(stderr, "[srv] tick_interval=%dms (%.1f FPS)\n",
		args.tick_ms, 1000.0 / args.tick_ms);
	auto next_tick = clock_t::now() + tick_interval;
	auto last_heartbeat = clock_t::now();

	while (true) {
		// Drain pending agent commands in slot order (0 -> 7, FIFO within
		// each slot). Each command was pre-encoded by ws_server into one
		// or more BW action blobs (select + verb, typically). We do two
		// things per blob:
		//   1) schedule_action on the local virtual client so the server's
		//      own sim applies it via execute_scheduled_actions.
		//   2) broadcast_agent_action to every connected observer so their
		//      sim schedules the same bytes on their own virtual client
		//      and stays frame-for-frame with the server. Without step 2,
		//      live observers would silently drift out of sync as soon as
		//      the first agent command fires.
		cmd_queue.drain([&](int slot, const uint8_t* data, size_t size) {
			auto* vc = virtual_clients[slot];
			if (!vc) return;
			// Keep virtual client's frame counter aligned so sync's lag
			// check doesn't stall on this "client".
			vc->frame = (uint8_t)sync_st.sync_frame;
			funcs.schedule_action(vc, data, size);
			funcs.broadcast_agent_action(server, slot, data, size);
		});

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
				c.frame = (uint8_t)sync_st.sync_frame;
			}
		}

		funcs.next_frame(server); // advances sim + syncs to observers
		current_frame_atomic.store(st.current_frame);

		auto now = clock_t::now();
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
			fprintf(stderr, "[srv] frame=%d observers=%d virtual-agents=%d pending-cmds=%zu\n",
				st.current_frame, n_observers, n_agents, cmd_queue.total_pending());
			last_heartbeat = now;
		}

		auto sleep_until = next_tick;
		next_tick += tick_interval;
		if (clock_t::now() < sleep_until) std::this_thread::sleep_until(sleep_until);
		else next_tick = clock_t::now() + tick_interval; // fell behind; reset schedule
	}
	return 0;
}
