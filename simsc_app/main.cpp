// simsc_app Day 2: Qt6 native spectator with an actual window.
//
// End-to-end shape mirrors ui/observer.cpp:
//   1. Load MPQs + map.
//   2. Set up sync_state / sync_functions.
//   3. Open the QWebSocket transport (sync_server_qt_ws.h).
//   4. Create ui_functions + a Qt window (via native_window::window
//      whose backing store is now GameWidget, provided by
//      qt_native_window.cpp).
//   5. Drive next_frame + ui.update from a QTimer that fires at ~42 ms
//      (BW "fastest"). QTimer keeps the sim tick and the Qt event
//      loop co-operating on one thread -- no thread affinity headaches.
//
// The only behavioural difference from the SDL observer is the pump:
// SDL polls a native queue inside its own while(true); here Qt's event
// loop already runs, and our timer piggy-backs on it. Rendering
// throughput is identical (QImage + QPainter can easily beat 24 FPS
// at 1280x800), so the spectator experience is the same.

#include "ui.h"
#include "common.h"
#include "../bwgame.h"
#include "../sync.h"
#include "sync_server_qt_ws.h"

#include <QtCore/QCommandLineOption>
#include <QtCore/QCommandLineParser>
#include <QtCore/QDebug>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>

#include <array>
#include <cstdio>
#include <fstream>
#include <memory>

using namespace bwgame;

// Shared log sink -- mirrors ui/observer.cpp exactly.
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

struct args_t {
	QString data_path = ".";
	QString map_path;
	QString url;                 // ws:// or wss:// /observer URL
	QString api_key;
	QString sync_log_path;
	std::array<int, 8> race_overrides = {-1, -1, -1, -1, -1, -1, -1, -1};
	int screen_width  = 1280;
	int screen_height = 800;
};

static bool parse_args(const QStringList& argv, args_t& out) {
	QCommandLineParser p;
	p.setApplicationDescription(
		"simsc_app Day 2: Qt6 native openbw spectator (QWebSocket + QImage)");
	p.addOption({"data-path",
		"Directory holding StarDat.mpq / BrooDat.mpq / Patch_rt.mpq.",
		"dir", "."});
	p.addOption({"map", "Map .scm/.scx path.", "path"});
	p.addOption({"url",
		"Full ws:// or wss:// URL to the /observer endpoint.",
		"url"});
	p.addOption({"api-key", "API key for sync.h id_auth.", "key"});
	p.addOption({"sync-log",
		"Append per-frame TICK / AGENT_APPLY events to this file "
		"(same format as ui/observer.cpp's --sync-log for "
		"line-for-line diffing).", "path"});
	p.addOption({"race",
		"Per-slot race override in the form N=<zerg|terran|protoss>. "
		"Repeat for each slot. Must match the server's --race args.",
		"spec"});
	p.addOption({"width",  "Window width  (default 1280).",  "n", "1280"});
	p.addOption({"height", "Window height (default 800).",   "n", "800"});
	p.parse(argv);

	out.data_path     = p.value("data-path");
	out.map_path      = p.value("map");
	out.url           = p.value("url");
	out.api_key       = p.value("api-key");
	out.sync_log_path = p.value("sync-log");
	out.screen_width  = p.value("width").toInt();
	out.screen_height = p.value("height").toInt();

	for (const auto& v : p.values("race")) {
		int eq = v.indexOf('=');
		if (eq < 0) { qCritical() << "bad --race:" << v; return false; }
		int slot = v.left(eq).toInt();
		QString race = v.mid(eq + 1);
		int race_id = -1;
		if      (race == "zerg")    race_id = 0;
		else if (race == "terran")  race_id = 1;
		else if (race == "protoss") race_id = 2;
		else { qCritical() << "bad race:" << race; return false; }
		if (slot < 0 || slot > 7) { qCritical() << "slot 0..7"; return false; }
		out.race_overrides[slot] = race_id;
	}

	if (out.map_path.isEmpty() || out.url.isEmpty()) {
		qCritical() << "usage: --map <path> --url <ws://...>";
		return false;
	}
	return true;
}

} // anonymous namespace

int main(int argc, char** argv) {
	// QApplication (not QCoreApplication) because we render widgets.
	QApplication app(argc, argv);
	QCoreApplication::setApplicationName("simsc_app");
	QCoreApplication::setOrganizationName("openbw");

	args_t args;
	if (!parse_args(QCoreApplication::arguments(), args)) return 1;

	ui::log("[simsc_app] starting: map=%s url=%s\n",
		args.map_path.toStdString().c_str(),
		args.url.toStdString().c_str());

	// ------------------------------------------------------------------
	// 1. Load MPQs + map. Same setup_f trick as ui/observer.cpp so
	//    per-slot race overrides land BEFORE create_starting_units --
	//    both sides must spawn matching-race units with matching unit_ids
	//    at map-load frame 0 or the very first TICK diverges.
	// ------------------------------------------------------------------
	auto load_data_file = data_loading::data_files_directory(
		a_string(args.data_path.toStdString().c_str()));
	game_player player(load_data_file);
	{
		game_load_functions loader(player.st());
		for (size_t i = 0; i < 8; ++i)
			loader.setup_info.create_melee_units_for_player[i] = true;
		state& st = player.st();
		auto setup_f = [&args, &st]() {
			for (size_t i = 0; i != 12; ++i) {
				if (st.players[i].controller == player_t::controller_open) {
					st.players[i].controller = player_t::controller_occupied;
				}
				if (st.players[i].controller == player_t::controller_computer) {
					st.players[i].controller = player_t::controller_computer_game;
				}
			}
			for (size_t i = 0; i < 8; ++i) {
				if (args.race_overrides[i] < 0) continue;
				st.players[i].race = (race_t)args.race_overrides[i];
			}
		};
		loader.load_map_file(
			a_string(args.map_path.toStdString().c_str()),
			setup_f);
	}
	ui::log("[simsc_app] map loaded\n");

	// ------------------------------------------------------------------
	// 2. ui_functions: owns the rendering pipeline (palette, sprite
	//    caches, minimap, fog blitting, camera scroll). Feed it the
	//    already-loaded game_player, then hand it a data-file loader
	//    for on-demand asset reads (sprites, tiles, GRPs).
	// ------------------------------------------------------------------
	ui_functions ui(std::move(player));
	ui.load_all_image_data(load_data_file);
	ui.load_data_file = [&](a_vector<uint8_t>& data, a_string filename) {
		load_data_file(data, std::move(filename));
	};
	ui.init();

	// ------------------------------------------------------------------
	// 3. sync + transport wiring. Same shape as observer.cpp; just the
	//    transport class name differs (sync_server_qt_ws vs
	//    sync_server_asio_ws).
	// ------------------------------------------------------------------
	action_state action_st;
	sync_state sync_st;
	sync_functions funcs(ui.st, action_st, sync_st);
	game_load_functions::setup_info_t setup_info;
	sync_st.setup_info = &setup_info;
	sync_st.latency = 2;
	sync_st.local_client->name = "simsc_app";
	if (!args.api_key.isEmpty()) {
		sync_st.outgoing_api_key =
			a_string(args.api_key.toStdString().c_str());
	}

	// Optional sync-log for byte-level diff against a native observer.
	std::shared_ptr<std::ofstream> sync_log_file;
	if (!args.sync_log_path.isEmpty()) {
		sync_log_file = std::make_shared<std::ofstream>(
			args.sync_log_path.toStdString(),
			std::ios::out | std::ios::trunc);
		if (!sync_log_file->good()) {
			qCritical() << "failed to open --sync-log ="
			            << args.sync_log_path;
			return 1;
		}
		auto f = sync_log_file;
		sync_st.sync_log = [f](const a_string& s) {
			f->write(s.data(), (std::streamsize)s.size());
			f->flush();
		};
	}

	sync_server_qt_ws server;
	server.client_url_path = "/observer";        // ignored by connect_url
	server.client_api_key  = args.api_key.toStdString();
	server.connect_url(args.url.toStdString());
	ui::log("[simsc_app] connecting to %s%s\n",
		args.url.toStdString().c_str(),
		args.api_key.isEmpty() ? "" : "?key=(set)");

	// ------------------------------------------------------------------
	// 4. Create the window through ui_functions -- this reaches into
	//    qt_native_window.cpp's window_impl::create and pops up a
	//    GameWidget. resize() plus screen_pos centre the initial view
	//    on the map, then set_image_data pushes tileset + palette.
	// ------------------------------------------------------------------
	auto& wnd = ui.wnd;
	wnd.create("simsc_app (openbw spectator)", 0, 0,
	           args.screen_width, args.screen_height);
	ui.resize(args.screen_width, args.screen_height);
	ui.screen_pos = {
		(int)ui.game_st.map_width  / 2 - args.screen_width  / 2,
		(int)ui.game_st.map_height / 2 - args.screen_height / 2,
	};
	ui.set_image_data();

	// ------------------------------------------------------------------
	// 5. Sim loop as a QTimer, 42 ms = BW "fastest". The Qt event loop
	//    is already running (QApplication::exec below); the timer fires
	//    from that loop and calls next_frame -> poll (drains WS
	//    messages) -> advance sim, then ui.update -> paint.
	//
	//    We match observer.cpp's book-keeping (perspective log,
	//    sync-log initial rand, inventory dumps every 300 frames).
	// ------------------------------------------------------------------
	struct loop_state {
		int last_slot = -2;
		int last_inv  = -1;
		bool rand_logged = false;
	};
	loop_state loop_st;

	QTimer sim_timer;
	sim_timer.setInterval(42);   // BW fastest
	QObject::connect(&sim_timer, &QTimer::timeout, &app,
		[&funcs, &server, &sync_st, &ui, &loop_st]() {
		funcs.next_frame(server);

		if (!loop_st.rand_logged && sync_st.game_started
		    && sync_st.sync_log) {
			char buf[64];
			snprintf(buf, sizeof(buf),
				"GAME_START\tinitial_rand=%08x",
				sync_st.initial_rand_state);
			bwgame::sync_log_line(sync_st, 'O', a_string(buf));
			loop_st.rand_logged = true;
		}
		int cf = (int)ui.st.current_frame;
		if (sync_st.sync_log && cf > 0
		    && cf != loop_st.last_inv && cf % 300 == 0) {
			for (int s = 0; s < 2; ++s) funcs.log_inventory('O', s);
			loop_st.last_inv = cf;
		}

		if (ui.viewing_slot != sync_st.viewing_slot) {
			ui.viewing_slot = sync_st.viewing_slot;
		}
		if (sync_st.viewing_slot != loop_st.last_slot) {
			ui::log("[simsc_app] viewing perspective: slot=%d\n",
				(int)sync_st.viewing_slot);
			loop_st.last_slot = sync_st.viewing_slot;
		}
		ui.update();
	});
	sim_timer.start();

	return app.exec();
}
