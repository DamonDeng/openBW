// Day 1 smoke test: no window yet. Connects to an openbw_server via
// the new QWebSocket transport, runs the sync loop headlessly, and
// prints frame counters. The point is to prove the transport works
// end-to-end (WS handshake, id_client_uid, id_auth, id_start_game,
// id_agent_action, etc.) before we start dealing with rendering.
//
// Usage:
//   simsc_app --data-path <dir> --map <path> \
//             --url ws://127.0.0.1:6114/observer \
//             [--api-key sk-xxx] [--sync-log path]
//             [--race 0=terran --race 1=protoss]
//
// On Day 2 we add a QWidget game surface and drop the frame-counter
// prints; on Day 3 we add a ConnectDialog for API key + URL entry.

// Day 1 headless: we don't include ui.h (SDL renderer) yet — Day 2
// adds the QWidget game surface. bwgame.h + sync.h are enough to
// drive the sim wire protocol without a screen. ui/common.h gives us
// bwgame::ui::log — a variadic printf-flavored wrapper around log_str
// (implementation below). Zero SDL cost — common.h is header-only.
#include "../bwgame.h"
#include "../sync.h"
#include "../ui/common.h"
#include "sync_server_qt_ws.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCommandLineOption>
#include <QtCore/QCommandLineParser>
#include <QtCore/QDebug>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>

#include <array>
#include <cstdio>
#include <fstream>
#include <memory>

// We hijack the openbw ui:: symbol namespace for logging (matches what
// the native observer does — bwgame.h wires several macros through it).
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

// ----------------------------------------------------------------------
// CLI. Simplified from ui/observer.cpp — we skip the SDL screen sizing
// flags entirely (Day 2 gets a QWidget default), and add --url as the
// primary connect entry.
// ----------------------------------------------------------------------
struct args_t {
    QString data_path = ".";
    QString map_path;
    QString url;                 // e.g. ws://127.0.0.1:6114/observer
    QString api_key;
    QString sync_log_path;
    std::array<int, 8> race_overrides = {-1, -1, -1, -1, -1, -1, -1, -1};
    int max_frames = 0;          // 0 = run forever
};

static bool parse_args(const QStringList& argv, args_t& out) {
    QCommandLineParser p;
    p.setApplicationDescription(
        "simsc_app Day 1 smoke: headless observer using QWebSocket");
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
        "(same format as the native observer's --sync-log for "
        "line-for-line diffing).", "path"});
    p.addOption({"race",
        "Per-slot race override in the form N=<zerg|terran|protoss>. "
        "Repeat for each slot. Must match the server's --race args.",
        "spec"});
    p.addOption({"max-frames",
        "Exit after N sim frames (0 = run indefinitely). Useful for "
        "bounded smoke tests.", "n", "0"});
    p.parse(argv);

    out.data_path     = p.value("data-path");
    out.map_path      = p.value("map");
    out.url           = p.value("url");
    out.api_key       = p.value("api-key");
    out.sync_log_path = p.value("sync-log");
    out.max_frames    = p.value("max-frames").toInt();

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
        qCritical() << "usage: --map <path> --url <ws://…>";
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("simsc_app");
    QCoreApplication::setOrganizationName("openbw");

    args_t args;
    if (!parse_args(QCoreApplication::arguments(), args)) return 1;

    bwgame::ui::log("[simsc_app] starting: map=%s url=%s\n",
        args.map_path.toStdString().c_str(),
        args.url.toStdString().c_str());

    // ------------------------------------------------------------------
    // 1. Load MPQs + map. Copy-paste from ui/observer.cpp — the same
    //    game_load_functions + setup_f dance to install per-slot race
    //    overrides before create_starting_units runs. If the observer
    //    and server don't agree on slot races here, sim units spawn
    //    with different types and the very first TICK diverges.
    // ------------------------------------------------------------------
    auto load_data_file = bwgame::data_loading::data_files_directory(
        bwgame::a_string(args.data_path.toStdString().c_str()));
    bwgame::game_player player(load_data_file);
    {
        bwgame::game_load_functions loader(player.st());
        for (size_t i = 0; i < 8; ++i)
            loader.setup_info.create_melee_units_for_player[i] = true;
        auto& st = player.st();
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
        loader.load_map_file(
            bwgame::a_string(args.map_path.toStdString().c_str()),
            setup_f);
    }
    bwgame::ui::log("[simsc_app] map loaded\n");

    // ------------------------------------------------------------------
    // 2. sync_state / sync_functions plumbing. Same shape as the
    //    native observer's, minus ui_functions (Day 2). We drive the
    //    sim purely for its wire behavior in Day 1.
    // ------------------------------------------------------------------
    bwgame::action_state action_st;
    bwgame::sync_state sync_st;
    bwgame::sync_functions funcs(player.st(), action_st, sync_st);
    bwgame::game_load_functions::setup_info_t setup_info;
    sync_st.setup_info = &setup_info;
    sync_st.latency = 2;
    sync_st.local_client->name = "simsc_app_day1";
    if (!args.api_key.isEmpty()) {
        sync_st.outgoing_api_key =
            bwgame::a_string(args.api_key.toStdString().c_str());
    }

    // Diagnostic sync-log — same format as the native observer, so we
    // can diff a headless simsc_app run vs an openbw_observer run
    // against the same server and confirm byte-identical replay.
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
        sync_st.sync_log = [f](const bwgame::a_string& s) {
            f->write(s.data(), (std::streamsize)s.size());
            f->flush();
        };
    }

    // ------------------------------------------------------------------
    // 3. QWebSocket transport.
    // ------------------------------------------------------------------
    bwgame::sync_server_qt_ws server;
    server.client_url_path = "/observer";  // ignored by connect_url path
    server.client_api_key  = args.api_key.toStdString();
    server.connect_url(args.url.toStdString());
    bwgame::ui::log("[simsc_app] connecting to %s%s\n",
        args.url.toStdString().c_str(),
        args.api_key.isEmpty() ? "" : "?key=(set)");

    // ------------------------------------------------------------------
    // 4. Sim loop as a QTimer. The Qt event loop is already running
    //    (QCoreApplication::exec below), so the timer fires from that
    //    loop and calls next_frame → poll → drain incoming → advance
    //    sim. Same body the native observer runs in its while(true).
    //    Interval is 10 ms (game-speed=10 tick_ms) so we can keep up
    //    with a fast server; the sim itself is gated by
    //    sync_state.latency + peer clock so this timer doesn't
    //    freewheel past the server.
    // ------------------------------------------------------------------
    bool rand_logged = false;
    int last_inv = -1;
    int last_slot = -2;
    int frame_count = 0;

    QTimer sim_timer;
    sim_timer.setInterval(10);
    QObject::connect(&sim_timer, &QTimer::timeout, &app,
        [&]() {
        funcs.next_frame(server);

        if (!rand_logged && sync_st.game_started && sync_st.sync_log) {
            char buf[64];
            snprintf(buf, sizeof(buf),
                "GAME_START\tinitial_rand=%08x",
                sync_st.initial_rand_state);
            bwgame::sync_log_line(sync_st, 'O', bwgame::a_string(buf));
            rand_logged = true;
        }

        int cf = (int)player.st().current_frame;
        if (sync_st.sync_log && cf > 0 && cf != last_inv && cf % 300 == 0) {
            for (int s = 0; s < 2; ++s) funcs.log_inventory('O', s);
            last_inv = cf;
        }
        if (sync_st.viewing_slot != last_slot) {
            bwgame::ui::log("[simsc_app] viewing perspective: slot=%d\n",
                (int)sync_st.viewing_slot);
            last_slot = sync_st.viewing_slot;
        }

        // Progress line every 300 sim frames.
        if (sync_st.game_started && cf > 0 && cf % 300 == 0
            && cf != frame_count) {
            bwgame::ui::log(
                "[simsc_app] frame=%d clients=%d viewing_slot=%d\n",
                cf, (int)sync_st.clients.size(),
                (int)sync_st.viewing_slot);
            frame_count = cf;
        }

        if (args.max_frames > 0 && cf >= args.max_frames) {
            bwgame::ui::log("[simsc_app] reached --max-frames=%d, exiting\n",
                args.max_frames);
            QCoreApplication::quit();
        }
    });
    sim_timer.start();

    return app.exec();
}
