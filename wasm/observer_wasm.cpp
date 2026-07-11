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
	a.server_host = js_string_or("(typeof OPENBW_HOST === 'string') ? OPENBW_HOST : ''",       a.server_host.c_str());
	a.server_port = js_int_or   ("(typeof OPENBW_PORT === 'number') ? String(OPENBW_PORT) : ''", a.server_port);
	a.api_key     = js_string_or("(typeof OPENBW_KEY === 'string') ? OPENBW_KEY : ''",         "");
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
};
static wasm_state* g_state = nullptr;

extern "C" void wasm_frame() {
	if (!g_state) return;
	auto& st = *g_state;

	// Drive sync -> sim. Same call the native observer makes in its
	// while(true). Internally: server.poll() delivers WS messages,
	// sync.h ingests, advances sim if it's this frame's turn.
	st.funcs->next_frame(*st.server);

	// Log connection state once, when the sync layer first shows us
	// clients (server-side peer registered) so the browser console
	// clearly signals "we're actually talking to the server now".
	if (!st.connect_logged && st.sync_st->clients.size() >= 2) {
		ui::log("[wasm] connected to server (clients=%d)\n",
			(int)st.sync_st->clients.size());
		st.connect_logged = true;
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

	// Sync-log intentionally not wired: at steady state it's thousands
	// of lines per second. Transport-level [ws-rx] logging inside
	// sync_server_emscripten_ws.h is enough to diagnose Phase 4 issues.

	// Transport. Same URL contract as native ws observer:
	//   ws://host:port/observer?key=<api-key>
	g_state->server = std::unique_ptr<sync_server_emscripten_ws>(
		new sync_server_emscripten_ws());
	g_state->server->client_url_path = "/observer";
	g_state->server->client_api_key  = args.api_key;
	g_state->server->connect(a_string(args.server_host.c_str()),
	                          args.server_port);
	ui::log("[wasm] connecting to ws://%s:%d/observer ...\n",
		args.server_host.c_str(), args.server_port);

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
