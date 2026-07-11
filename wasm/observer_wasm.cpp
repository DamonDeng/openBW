// Phase 3 WASM observer: loads MPQ + a map, renders the initial frame to
// an HTML5 canvas via SDL2. No networking, no sim advance -- just prove
// the rendering pipeline works in the browser.
//
// Deliberately kept parallel to ui/observer.cpp:
//   - Same log_str / fatal_error_str hooks (writes to stdout, which
//     emscripten routes to the browser console).
//   - Same map-load ceremony (game_load_functions + setup_f).
//   - ui.init() + ui.update() from the same ui_functions class as native.
//
// Divergences from native observer.cpp:
//   - No argv parsing. All parameters hardcoded for PoC. See args_wasm
//     struct below.
//   - No sync_state / sync_server_asio_ws. Phase 4 adds them.
//   - No while(true). emscripten_set_main_loop drives each frame.

#include "ui.h"
#include "common.h"
#include "../bwgame.h"

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
// Same log sink as native observer.cpp -- fwrite to stdout. Emscripten
// routes stdout to console.log by default, which is exactly what we
// want for a browser build.
void log_str(a_string str) {
	fwrite(str.data(), str.size(), 1, stdout);
	fflush(stdout);
}
void fatal_error_str(a_string str) {
	log("fatal error: %s\n", str);
	// std::terminate in wasm produces an uncatchable trap; call abort
	// explicitly so the browser console shows the log line above first.
	std::fflush(stdout);
	std::abort();
}
}
}

namespace {

// Same shape as native observer.cpp args, but populated from constants
// (Phase 3) or JS ccall'd setters (Phase 4+). All paths are relative to
// the preloaded emscripten VFS root -- matches --preload-file target
// in build_observer_wasm.sh.
struct args_wasm {
	std::string data_path = "original_resources";
	std::string map_path  = "original_resources/(2)Bottleneck.scm";
	int screen_width  = 1280;
	int screen_height = 800;
	// One entry per player slot 0..7. -1 = "use map default". Same
	// semantics as observer.cpp's race_overrides. For PoC we run
	// map-default (no override); Phase 4 will accept these from JS.
	std::array<int, 8> race_overrides = {-1, -1, -1, -1, -1, -1, -1, -1};
};

// Everything the main-loop callback needs. Heap-allocated once in main()
// and captured via a raw pointer -- emscripten_set_main_loop_arg accepts
// a void* userdata slot for this exact purpose, but the simplest thing
// here is a file-scope pointer.
struct wasm_state {
	std::unique_ptr<ui_functions> ui;
	// Non-empty after the first ui.update() -- used to log "first frame
	// rendered" once so the JS side can hide any loading spinner.
	bool first_frame_logged = false;
	int frame_count = 0;
};
static wasm_state* g_state = nullptr;

extern "C" void wasm_frame() {
	if (!g_state) return;
	auto& ui = *g_state->ui;
	// SDL_PollEvent is pumped inside ui.update() (it consumes mouse +
	// keyboard). We do NOT advance the sim here in Phase 3 -- the same
	// frame renders every callback, which is fine: the map + starting
	// units are visible and interactive-panning works.
	ui.update();

	if (!g_state->first_frame_logged) {
		ui::log("[wasm] first frame rendered\n");
		g_state->first_frame_logged = true;
	}
	g_state->frame_count++;
	if (g_state->frame_count % 60 == 0) {
		ui::log("[wasm] frames=%d (steady-state render)\n",
			g_state->frame_count);
	}
}

} // anonymous namespace

int main() {
	args_wasm args;
	ui::log("[wasm] starting: data=%s map=%s\n",
		args.data_path.c_str(), args.map_path.c_str());

	// 1. Load MPQs + map. Same setup_f dance as native observer:
	//    promote map's "open" and "computer" slots to occupied so
	//    create_starting_units actually spawns them, then apply any
	//    race overrides before the SIDE chunk runs.
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

	// 2. Init ui_functions -- same wiring as native observer.
	g_state = new wasm_state();
	g_state->ui = std::unique_ptr<ui_functions>(
		new ui_functions(std::move(player)));
	auto& ui = *g_state->ui;
	ui.load_all_image_data(load_data_file);
	ui.load_data_file = [&](a_vector<uint8_t>& data, a_string filename) {
		load_data_file(data, std::move(filename));
	};
	ui.init();
	ui::log("[wasm] ui initialized\n");

	// 3. Create the SDL2 window -> emscripten binds it to the HTML canvas.
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

	// 4. emscripten main loop. fps=0 means "use requestAnimationFrame"
	//    (~60fps). simulate_infinite_loop=1 makes main() effectively
	//    never return, which is what SDL_main wants.
	emscripten_set_main_loop(wasm_frame, 0, 1);
	return 0;
}
