// Phase 4 WASM observer: loads MPQ + map (as in Phase 3), then opens a
// WebSocket to openbw_server and drives sync.h in a browser main-loop
// callback. Renders the live sim to an HTML5 canvas.
//
// Deliberately parallel to ui/observer.cpp:
//   - Same log_str / fatal_error_str hooks.
//   - Same map-load ceremony (game_load_functions + setup_f).
//   - ui.init() + ui.update() from the same ui_functions class.
//   - sync_state / sync_functions wired to a transport, same as native.
//
// Divergences from native observer.cpp:
//   - No argv. Params come from URL-embedded globals set by JS before
//     main() runs (see observer_shell.html). Defaults fall back to
//     something usable in dev: (2)Bottleneck, ws://127.0.0.1:6114.
//   - sync_server_emscripten_ws instead of sync_server_asio_ws.
//   - No while(true). emscripten_set_main_loop drives each frame.
//   - No blocking pre-loop connect wait -- browsers can't block. The
//     main-loop callback pumps funcs.next_frame(); render happens even
//     while the WebSocket is still handshaking, and the fog-off/god
//     view of the map is visible immediately.

#include "ui.h"
#include "common.h"
#include "../bwgame.h"
#include "../sync.h"
#include "../sync_server_emscripten_ws.h"

#include <emscripten.h>
#include <emscripten/html5.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

using namespace bwgame;

namespace bwgame {
namespace ui {
void log_str(a_string str) {
	fwrite(str.data(), str.size(), 1, stdout);
	fflush(stdout);
}
void fatal_error_str(a_string str) {
	log("fatal error: %s\n", str);
	std::fflush(stdout);
	std::abort();
}
}
}

namespace {

struct args_wasm {
	std::string data_path = "original_resources";
	std::string map_path  = "original_resources/(2)Bottleneck.scm";
	// Full WebSocket URL to connect to. If empty, fall back to host+port.
	// Set by the shell page as window.OPENBW_URL — typically
	// wss://simsc.agentnumber47.com/game/{id}/observer?key=...
	std::string server_url;
	std::string server_host = "127.0.0.1";
	int server_port = 6114;
	int screen_width  = 1280;
	int screen_height = 800;
	std::string api_key;  // matches native --api-key; both HTTP-upgrade and id_auth
	std::array<int, 8> race_overrides = {-1, -1, -1, -1, -1, -1, -1, -1};
};

// Read a JS-side string global. Returns default_ if the global is
// undefined or empty. Uses emscripten_run_script_string, which lifetime-
// pins the returned char* for the duration of this call frame.
std::string js_string_or(const char* js_expr, const char* default_) {
	const char* got = emscripten_run_script_string(js_expr);
	if (!got || !*got) return default_;
	return got;
}
int js_int_or(const char* js_expr, int default_) {
	auto s = js_string_or(js_expr, "");
	if (s.empty()) return default_;
	return std::atoi(s.c_str());
}

args_wasm read_args_from_js() {
	args_wasm a;
	// Reads window.OPENBW_* if present. observer_shell.html sets these
	// from URL params before the module boots. Fall back to hardcoded
	// defaults if the shell didn't set anything.
	a.map_path    = js_string_or("(typeof OPENBW_MAP === 'string') ? OPENBW_MAP : ''",         a.map_path.c_str());
	a.server_url  = js_string_or("(typeof OPENBW_URL === 'string') ? OPENBW_URL : ''",         "");
	a.server_host = js_string_or("(typeof OPENBW_HOST === 'string') ? OPENBW_HOST : ''",       a.server_host.c_str());
	a.server_port = js_int_or   ("(typeof OPENBW_PORT === 'number') ? String(OPENBW_PORT) : ''", a.server_port);
	a.api_key     = js_string_or("(typeof OPENBW_KEY === 'string') ? OPENBW_KEY : ''",         "");
	// Per-slot race overrides. window.OPENBW_RACES is a comma-separated
	// list of "zerg"/"terran"/"protoss"/"" applied to slots 0..7. Must
	// match what the server launched with (--race N=<race>). Without
	// this the observer loads the map with whatever race the map file
	// specified for each slot -- usually Protoss for both -- and the
	// starting units on the WASM canvas won't match the actual game.
	// Catchup fixes st.players[i].race but NOT the units that were
	// already spawned in load_map_file's create_starting_units call.
	// (bwgame::race_t: 0=zerg 1=terran 2=protoss)
	for (int i = 0; i < 8; ++i) {
		char expr[256];
		snprintf(expr, sizeof(expr),
			"(typeof OPENBW_RACES === 'string' && OPENBW_RACES.split(',')[%d]) "
			"? OPENBW_RACES.split(',')[%d].trim() : ''", i, i);
		std::string r = js_string_or(expr, "");
		if (r.empty()) continue;
		if      (r == "zerg")    a.race_overrides[i] = 0;
		else if (r == "terran")  a.race_overrides[i] = 1;
		else if (r == "protoss") a.race_overrides[i] = 2;
	}
	return a;
}

// Global state captured by the emscripten main-loop callback. Contains
// everything the frame function needs to advance sync + render.
struct wasm_state {
	std::unique_ptr<ui_functions> ui;
	std::unique_ptr<action_state> action_st;
	std::unique_ptr<sync_state>   sync_st;
	std::unique_ptr<sync_functions> funcs;
	std::unique_ptr<sync_server_emscripten_ws> server;
	int last_logged_slot = -2;
	bool first_frame_logged = false;
	bool connect_logged = false;
	int frame_count = 0;

	// Debug harness: track sim advance history so a snapshot can show
	// e.g. "sim advanced 240 times in the last 300 render frames".
	int sim_advances = 0;
	int last_snapshot_sim = 0;

	// Sync-log buffer. sync.h calls sync_state::sync_log(a_string) for
	// every AGENT_APPLY / AGENT_RECV / INVENTORY / GAME_START etc. We
	// append to this string so a Download button can hand the browser
	// a Blob (see openbw_sync_log_get). Grows to whatever the game
	// produces; a full Z-v-Z at speed=42 is on the order of a few
	// hundred KB per minute.
	std::string sync_log_buffer;
};
static wasm_state* g_state = nullptr;

// Sync-log accessors -- JS calls these from the Download button.
// _size: current buffer length. _get: base64-encoded blob (via Module.HEAPU8
// slice + TextDecoder in JS is more direct, so we just return the pointer
// and length in a struct-of-two via two calls).
extern "C" EMSCRIPTEN_KEEPALIVE size_t openbw_sync_log_size() {
	return g_state ? g_state->sync_log_buffer.size() : 0;
}
extern "C" EMSCRIPTEN_KEEPALIVE const char* openbw_sync_log_ptr() {
	return g_state ? g_state->sync_log_buffer.data() : nullptr;
}
extern "C" EMSCRIPTEN_KEEPALIVE void openbw_sync_log_clear() {
	if (g_state) g_state->sync_log_buffer.clear();
}

// Debug snapshot -- called from a JS button (see observer_shell.html).
// Dumps the state of both the transport and sync_state to stdout so we
// can see exactly where the observer is stuck. Cheap; safe to call
// any time.
extern "C" EMSCRIPTEN_KEEPALIVE void openbw_snapshot() {
	if (!g_state) { printf("[snap] not initialized\n"); return; }
	auto& st = *g_state;
	printf("[snap] --- observer snapshot ---\n");
	printf("[snap] render_frames=%d sim_advances=%d "
	       "current_frame=%d sync_frame=%d\n",
	       st.frame_count, st.sim_advances,
	       (int)st.ui->st.current_frame,
	       (int)st.sync_st->sync_frame);
	printf("[snap] latency=%d viewing_slot=%d game_started=%d "
	       "catching_up=%d\n",
	       (int)st.sync_st->latency,
	       (int)st.sync_st->viewing_slot,
	       (int)st.sync_st->game_started,
	       (int)st.sync_st->catching_up);
	// Sync-state clients: h==nullptr => virtual (server-side agent
	// slot placeholder); &c == local_client => ourselves.
	int idx = 0;
	for (auto& c : st.sync_st->clients) {
		const char* kind = "peer";
		if (&c == st.sync_st->local_client) kind = "local";
		else if (c.h == nullptr) kind = "virtual";
		int32_t lag = (int32_t)((uint32_t)st.sync_st->sync_frame - c.frame);
		printf("[snap] client[%d] %s slot=%d frame=%u lag=%d "
		       "has_uid=%d has_auth=%d has_greeted=%d "
		       "scheduled=%zu name='%s'\n",
		       idx++, kind, c.player_slot,
		       (unsigned)c.frame, (int)lag,
		       (int)c.has_uid, (int)c.has_auth, (int)c.has_greeted,
		       c.scheduled_actions.size(), c.name.c_str());
	}
	// Transport state: per-client rx/tx counters + backlog.
	int tidx = 0;
	for (auto& c : st.server->clients) {
		printf("[snap] ws-client[%d] socket=%d open=%d dead=%d "
		       "allow_send=%d rx=%d(%d B) delivered=%d tx=%d "
		       "queued=%zu pending_sends=%zu\n",
		       tidx++,
		       (int)c->socket, (int)c->is_open, (int)c->is_dead,
		       (int)c->allow_send_flag,
		       c->msgs_received, (int)c->bytes_received,
		       c->msgs_delivered, c->msgs_sent,
		       c->incoming.size(), c->pending_sends.size());
	}
	printf("[snap] --- end snapshot ---\n");
	fflush(stdout);
}

// Read a JS int global, defaulting when undefined. Used to poll
// pause/step flags each frame.
static int js_bool(const char* expr) {
	// Returns the numeric string "0" or "1"; atoi handles both.
	const char* s = emscripten_run_script_string(expr);
	if (!s || !*s) return 0;
	return std::atoi(s);
}

extern "C" void wasm_frame() {
	if (!g_state) return;
	auto& st = *g_state;

	// Pause/step harness. Read two JS globals each frame:
	//   window.OPENBW_PAUSED  -- if true, skip next_frame.
	//   window.OPENBW_STEP    -- integer; if >0, decrement and advance
	//                            ONE frame even when paused.
	// The shell HTML exposes buttons that flip these. When paused we
	// still poll the transport (so WS messages queue up and we can
	// snapshot the queue depth) and still render.
	bool paused = js_bool("(typeof OPENBW_PAUSED === 'boolean' && OPENBW_PAUSED) ? 1 : 0");
	bool step   = js_bool("(typeof OPENBW_STEP === 'number' && OPENBW_STEP > 0) ? "
	                       "(OPENBW_STEP -= 1, 1) : 0");
	bool advance = !paused || step;

	if (advance) {
		// Drive sync -> sim. Same call the native observer makes in its
		// while(true). Internally: server.poll() delivers WS messages,
		// sync.h ingests, advances sim if it's this frame's turn.
		st.funcs->next_frame(*st.server);
		st.sim_advances++;
	} else {
		// Still poll the transport so WS messages accumulate; the
		// snapshot can then show the backlog. No sim advance.
		st.server->poll([&](const void* h){
			// Should not fire post-init, but obey the interface.
			(void)h;
		});
	}

	// Log connection state once, when the sync layer first shows us
	// clients (server-side peer registered) so the browser console
	// clearly signals "we're actually talking to the server now".
	if (!st.connect_logged && st.sync_st->clients.size() >= 2) {
		ui::log("[wasm] connected to server (clients=%d)\n",
			(int)st.sync_st->clients.size());
		st.connect_logged = true;
	}

	// Emit GAME_START into the sync-log once game_started flips true --
	// matches native observer.cpp:287-294 so a diff of the two logs
	// lines up from frame 0. The server's log includes the same line
	// (with S tag); diff should show identical initial_rand.
	static bool rand_logged = false;
	if (!rand_logged && st.sync_st->game_started && st.sync_st->sync_log) {
		char buf[64];
		snprintf(buf, sizeof(buf), "GAME_START\tinitial_rand=%08x",
			st.sync_st->initial_rand_state);
		bwgame::sync_log_line(*st.sync_st, 'O', bwgame::a_string(buf));
		rand_logged = true;
	}

	// Periodic INVENTORY dump for slots 0..1 every 300 frames, matching
	// the server's cadence. Diffing server_sync.log against wasm sync
	// log at the same frame numbers should show identical unit counts
	// per slot if the wasm replay is exact.
	static int last_inv_frame = -1;
	int cf = (int)st.ui->st.current_frame;
	if (st.sync_st->sync_log && cf > 0 && cf != last_inv_frame && cf % 300 == 0) {
		for (int s = 0; s < 2; ++s) st.funcs->log_inventory('O', s);
		last_inv_frame = cf;
	}

	// Pick up perspective assignment (server-assigned player-slot view)
	// exactly like native observer.cpp does.
	if (st.ui->viewing_slot != st.sync_st->viewing_slot) {
		st.ui->viewing_slot = st.sync_st->viewing_slot;
	}
	if (st.sync_st->viewing_slot != st.last_logged_slot) {
		ui::log("[wasm] viewing perspective: slot=%d\n",
			(int)st.sync_st->viewing_slot);
		st.last_logged_slot = st.sync_st->viewing_slot;
	}

	st.ui->update();

	if (!st.first_frame_logged) {
		ui::log("[wasm] first frame rendered\n");
		st.first_frame_logged = true;
	}
	st.frame_count++;
	if (st.frame_count % 300 == 0) {
		int rx = 0, dl = 0, tx = 0;
		uint64_t rb = 0;
		// Assume 1 peer (the observer connects to a single server).
		// Report its per-msg-id histogram so we can see which sync.h
		// messages are dominant vs missing.
		int per_id[256]{};
		for (auto& c : st.server->clients) {
			rx += c->msgs_received;
			dl += c->msgs_delivered;
			tx += c->msgs_sent;
			rb += c->bytes_received;
			for (int i = 0; i < 256; ++i) per_id[i] += c->msg_id_hist[i];
		}
		ui::log("[wasm] frames=%d sim_frame=%d sync_frame=%d clients=%d "
		        "ws:rx=%d(%d B) delivered=%d tx=%d\n",
			st.frame_count, (int)st.ui->st.current_frame,
			(int)st.sync_st->sync_frame,
			(int)st.sync_st->clients.size(),
			rx, (int)rb, dl, tx);
		// Histogram: print only nonzero ids so the line stays readable.
		// Legend: 0=client_uid 1=client_frame 3=start_game 6=game_started
		// 13=auth 14=assign_perspective 15=catchup_data 16=agent_action.
		char buf[512];
		int n = snprintf(buf, sizeof(buf), "[wasm] msg-ids:");
		for (int i = 0; i < 256 && n < (int)sizeof(buf) - 20; ++i) {
			if (per_id[i] == 0) continue;
			n += snprintf(buf + n, sizeof(buf) - n,
				" 0x%02x=%d", i, per_id[i]);
		}
		ui::log("%s\n", buf);
	}
}

} // anonymous namespace

int main() {
	auto args = read_args_from_js();
	ui::log("[wasm] starting: data=%s map=%s server=%s:%d key=%s\n",
		args.data_path.c_str(), args.map_path.c_str(),
		args.server_host.c_str(), args.server_port,
		args.api_key.empty() ? "(none)" : "(set)");
	// Log races so we can tell at a glance whether the shell passed them.
	static const char* race_names[3] = {"zerg", "terran", "protoss"};
	char rbuf[128]; int rn = 0;
	for (int i = 0; i < 8; ++i) {
		if (args.race_overrides[i] < 0) continue;
		rn += snprintf(rbuf + rn, sizeof(rbuf) - rn, " %d=%s",
			i, race_names[args.race_overrides[i]]);
	}
	ui::log("[wasm] race overrides:%s\n", rn ? rbuf : " (none)");

	// 1. Load MPQ + map -- identical to native observer.cpp.
	auto load_data_file = data_loading::data_files_directory(
		a_string(args.data_path.c_str()));
	game_player player(load_data_file);
	{
		game_load_functions loader(player.st());
		for (size_t i = 0; i < 8; ++i)
			loader.setup_info.create_melee_units_for_player[i] = true;
		state& st = player.st();
		auto setup_f = [&args, &st]() {
			for (size_t i = 0; i != 12; ++i) {
				if (st.players[i].controller == bwgame::player_t::controller_open) {
					st.players[i].controller = bwgame::player_t::controller_occupied;
				}
				if (st.players[i].controller == bwgame::player_t::controller_computer) {
					st.players[i].controller = bwgame::player_t::controller_computer_game;
				}
			}
			for (size_t i = 0; i < 8; ++i) {
				if (args.race_overrides[i] < 0) continue;
				st.players[i].race = (bwgame::race_t)args.race_overrides[i];
			}
		};
		loader.load_map_file(a_string(args.map_path.c_str()), setup_f);
	}
	ui::log("[wasm] map loaded\n");

	// 2. Build ui_functions + sync_state + transport. Same shape as
	//    ui/observer.cpp:211..250, but with the emscripten transport.
	g_state = new wasm_state();
	g_state->ui = std::unique_ptr<ui_functions>(
		new ui_functions(std::move(player)));
	auto& ui = *g_state->ui;
	ui.load_all_image_data(load_data_file);
	ui.load_data_file = [&](a_vector<uint8_t>& data, a_string filename) {
		load_data_file(data, std::move(filename));
	};
	ui.init();

	g_state->action_st = std::unique_ptr<action_state>(new action_state());
	g_state->sync_st   = std::unique_ptr<sync_state>(new sync_state());
	g_state->funcs = std::unique_ptr<sync_functions>(
		new sync_functions(ui.st, *g_state->action_st, *g_state->sync_st));

	// setup_info + latency + local client name -- mirror native.
	static game_load_functions::setup_info_t setup_info;
	g_state->sync_st->setup_info = &setup_info;
	g_state->sync_st->latency = 2;
	g_state->sync_st->local_client->name = "openbw_wasm_observer";
	if (!args.api_key.empty()) {
		g_state->sync_st->outgoing_api_key = a_string(args.api_key.c_str());
	}

	// Sync-log: append each line into an in-memory buffer that the JS
	// "download sync log" button can pull. Byte-identical to the native
	// observer's --sync-log output modulo the O/S side marker, so a
	// diff between the two proves the WASM sim replays the server's
	// actions correctly. Same shape as native observer.cpp:224.
	g_state->sync_st->sync_log = [](const bwgame::a_string& s) {
		if (g_state) {
			g_state->sync_log_buffer.append(s.data(), s.size());
		}
	};

	// Transport. If OPENBW_URL is set, use it verbatim — that's the
	// production shape (wss://…/game/{id}/observer?key=…). Otherwise
	// build ws://host:port/observer?key=… from OPENBW_HOST/PORT/KEY for
	// local dev.
	g_state->server = std::unique_ptr<sync_server_emscripten_ws>(
		new sync_server_emscripten_ws());
	std::string url;
	if (!args.server_url.empty()) {
		url = args.server_url;
	} else {
		url = "ws://";
		url += args.server_host;
		url += ':';
		url += std::to_string(args.server_port);
		url += "/observer";
		if (!args.api_key.empty()) {
			url += "?key=";
			url += args.api_key;
		}
	}
	g_state->server->connect_url(url);
	// Log with the key stripped — never write it to a place the user
	// might paste from (console, dev tools, screenshots).
	std::string safe_url = url;
	auto q = safe_url.find("?key=");
	if (q != std::string::npos) safe_url = safe_url.substr(0, q) + "?key=…";
	ui::log("[wasm] connecting to %s ...\n", safe_url.c_str());

	// 3. Create the SDL2 window -> HTML canvas, same as Phase 3.
	auto& wnd = ui.wnd;
	wnd.create("openbw_wasm_observer", 0, 0,
		args.screen_width, args.screen_height);
	ui.resize(args.screen_width, args.screen_height);
	ui.screen_pos = {
		(int)ui.game_st.map_width  / 2 - args.screen_width  / 2,
		(int)ui.game_st.map_height / 2 - args.screen_height / 2,
	};
	ui.set_image_data();
	ui::log("[wasm] canvas ready %dx%d\n",
		args.screen_width, args.screen_height);

	// 4. Enter the main loop. next_frame internally polls the
	//    transport, so the connect handshake completes over the first
	//    few frames while the map is already rendering (nice: user
	//    sees the map immediately, sim starts as soon as server ready).
	emscripten_set_main_loop(wasm_frame, 0, 1);
	return 0;
}
