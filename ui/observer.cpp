// openbw_observer: SDL2 client that connects to openbw_server and renders
// the game as a read-only spectator.
//
// - Loads the same map file the server loaded.
// - Connects to server via sync.h; joins as an observer (player_slot = -1).
// - Sim advances one frame per iteration when the server broadcasts.
// - ui_functions handles all input + drawing (camera scroll, minimap,
//   selection); no unit-command hotkeys are wired (spectator mode).

#include "ui.h"
#include "common.h"
#include "../bwgame.h"
#include "../sync.h"
#include "../sync_server_asio_tcp.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

using namespace bwgame;

FILE* log_file = nullptr;

namespace bwgame {
namespace ui {
void log_str(a_string str) {
	fwrite(str.data(), str.size(), 1, stdout);
	fflush(stdout);
	if (!log_file) log_file = fopen("observer_log.txt", "wb");
	if (log_file) {
		fwrite(str.data(), str.size(), 1, log_file);
		fflush(log_file);
	}
}
void fatal_error_str(a_string str) {
	log("fatal error: %s\n", str);
	std::terminate();
}
}
}

namespace {

struct args_t {
	std::string data_path = ".";
	std::string map_path;
	std::string server_host = "127.0.0.1";
	int server_port = 6112;
	int screen_width = 1280;
	int screen_height = 800;
	std::string api_key;
	std::string sync_log_path;
};

args_t parse_args(int argc, char** argv) {
	args_t a;
	for (int i = 1; i < argc; ++i) {
		auto eq = [&](const char* s) { return std::strcmp(argv[i], s) == 0; };
		if (eq("--data-path") && i + 1 < argc) a.data_path = argv[++i];
		else if (eq("--map") && i + 1 < argc) a.map_path = argv[++i];
		else if (eq("--server") && i + 1 < argc) {
			std::string s = argv[++i];
			auto colon = s.find(':');
			if (colon == std::string::npos) {
				fprintf(stderr, "error: --server expects host:port, got %s\n", s.c_str());
				std::exit(1);
			}
			a.server_host = s.substr(0, colon);
			a.server_port = std::atoi(s.substr(colon + 1).c_str());
		} else if (eq("--width") && i + 1 < argc) a.screen_width = std::atoi(argv[++i]);
		else if (eq("--height") && i + 1 < argc) a.screen_height = std::atoi(argv[++i]);
		else if (eq("--api-key") && i + 1 < argc) a.api_key = argv[++i];
		else if (eq("--sync-log") && i + 1 < argc) a.sync_log_path = argv[++i];
		else if (eq("--help") || eq("-h")) {
			fprintf(stderr,
				"usage: %s --map <path> [--server 127.0.0.1:6112] [--data-path .]\n"
				"  --map        map file (must match the server's map)\n"
				"  --server     host:port to connect to (default 127.0.0.1:6112)\n"
				"  --data-path  MPQ dir (default: .)\n"
				"  --width      window width (default: 1280)\n"
				"  --height     window height (default: 800)\n"
				"  --api-key    API key for auth (omit if server has --no-auth)\n"
				"  --sync-log <path>  append per-frame agent-action events to <path>.\n"
				"                     Diff against server's sync-log to find replay divergence.\n",
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
	return a;
}

} // anonymous namespace

int main(int argc, char** argv) {
	auto args = parse_args(argc, argv);

	ui::log("[obs] starting: map=%s server=%s:%d\n",
		args.map_path.c_str(), args.server_host.c_str(), args.server_port);

	// 1. Load MPQs + map. Same trick as the server -- drive game_load_functions
	//    ourselves to set create_melee_units_for_player so units spawn at
	//    frame 0. Both server and observer must load with matching setup or
	//    they'll desync immediately.
	auto load_data_file = data_loading::data_files_directory(a_string(args.data_path.c_str()));
	game_player player(load_data_file);
	{
		game_load_functions loader(player.st());
		for (size_t i = 0; i < 8; ++i) loader.setup_info.create_melee_units_for_player[i] = true;
		loader.load_map_file(a_string(args.map_path.c_str()));
	}

	ui_functions ui(std::move(player));
	ui.load_all_image_data(load_data_file);
	ui.load_data_file = [&](a_vector<uint8_t>& data, a_string filename) {
		load_data_file(data, std::move(filename));
	};

	ui.init();

	// 2. Set up sync state and start the TCP connect BEFORE creating the
	//    SDL2 window. Window creation on macOS can serialize through
	//    WindowServer and stall for a while when multiple observers boot
	//    at once. If we open the socket first, the TCP handshake runs in
	//    parallel and is usually done by the time we enter the main loop.
	action_state action_st;
	sync_state sync_st;
	sync_functions funcs(ui.st, action_st, sync_st);
	game_load_functions::setup_info_t setup_info;
	sync_st.setup_info = &setup_info;
	sync_st.latency = 2;
	sync_st.local_client->name = "openbw_observer";
	if (!args.api_key.empty()) {
		sync_st.outgoing_api_key = a_string(args.api_key.c_str());
	}

	// Diagnostic sync-log sink. Matches the server's --sync-log so a
	// diff of both files reveals replay divergence.
	if (!args.sync_log_path.empty()) {
		auto f = std::make_shared<std::ofstream>(args.sync_log_path,
			std::ios::out | std::ios::trunc);
		if (!f->good()) {
			ui::log("[obs] failed to open --sync-log=%s\n",
				args.sync_log_path.c_str());
			std::exit(1);
		}
		ui::log("[obs] sync-log -> %s\n", args.sync_log_path.c_str());
		sync_st.sync_log = [f](const bwgame::a_string& s) {
			f->write(s.data(), (std::streamsize)s.size());
			f->flush();
		};
	}

	sync_server_asio_tcp server;
	server.connect(a_string(args.server_host.c_str()), args.server_port);
	ui::log("[obs] connecting to %s:%d ...\n", args.server_host.c_str(), args.server_port);

	// 3. Now create the window. Pump the io_service a few times while we
	//    wait for the window to open, so the async_connect callback fires
	//    promptly.
	auto& wnd = ui.wnd;
	wnd.create("openbw_observer", 0, 0, args.screen_width, args.screen_height);
	ui.resize(args.screen_width, args.screen_height);
	ui.screen_pos = {
		(int)ui.game_st.map_width / 2 - args.screen_width / 2,
		(int)ui.game_st.map_height / 2 - args.screen_height / 2,
	};
	ui.set_image_data();

	// 4. Wait (briefly) for the sync connection to be established so we
	//    don't enter the sim loop with a half-formed handshake. sync()
	//    pumps the io_service.
	auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (sync_st.clients.size() < 2 && std::chrono::steady_clock::now() < connect_deadline) {
		funcs.sync(server);
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	if (sync_st.clients.size() < 2) {
		ui::log("[obs] WARNING: never connected to server; will keep retrying in main loop\n");
	} else {
		ui::log("[obs] connected to server (clients=%d)\n", (int)sync_st.clients.size());
	}

	// 5. Main loop. next_frame drives sync + sim; ui.update handles render.
	int last_logged_slot = -2;
	bool rand_logged = false;
	int last_inventory_frame = -1;
	while (true) {
		funcs.next_frame(server);

		// Emit the initial rand state once, as soon as game_started
		// flips true on our side. If server and observer disagree,
		// they're playing different sims from that instant on.
		if (!rand_logged && sync_st.game_started && sync_st.sync_log) {
			char buf[64];
			snprintf(buf, sizeof(buf), "GAME_START\tinitial_rand=%08x",
				sync_st.initial_rand_state);
			bwgame::sync_log_line(sync_st, 'O',
				bwgame::a_string(buf));
			rand_logged = true;
		}

		// Diagnostic: every 300 frames dump inventory for slots 0..1.
		// Matches the server's cadence so a diff of the two logs lines
		// up frame-for-frame.
		int cf = (int)ui.st.current_frame;
		if (sync_st.sync_log && cf > 0 && cf != last_inventory_frame
		    && cf % 300 == 0) {
			for (int s = 0; s < 2; ++s) funcs.log_inventory('O', s);
			last_inventory_frame = cf;
		}

		// Server sent id_assign_perspective after our auth -- pick it up
		// and route it into the UI so fog rendering activates.
		if (ui.viewing_slot != sync_st.viewing_slot) {
			ui.viewing_slot = sync_st.viewing_slot;
		}
		if (sync_st.viewing_slot != last_logged_slot) {
			ui::log("[obs] viewing perspective: slot=%d\n", (int)sync_st.viewing_slot);
			last_logged_slot = sync_st.viewing_slot;
		}
		ui.update();
		// Small yield so we don't 100% spin the CPU when server is idle.
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return 0;
}
