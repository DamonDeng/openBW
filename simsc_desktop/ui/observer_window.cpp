// See observer_window.h. This file is a straight port of the
// bootstrap + tick loop from simsc_app/main.cpp:140-416, wrapped
// in a QObject so multiple observers can coexist in one process.
//
// Also the ONE translation unit in simsc_desktop that pulls in
// bwgame.h / ui.h. Those headers define non-inline global tables
// (see korean.h), so at most one TU per binary can include them.
// The log_str / fatal_error_str hooks the game code expects at
// link time live at the bottom of this file for the same reason.

#include "observer_window.h"

#include "ui.h"                       // ui_functions, native_window
#include "bwgame.h"                   // game_player, sync_state
#include "sync.h"                     // sync_functions
#include "data_loading.h"             // data_files_loader (concrete type)
#include "sync_server_qt_ws.h"        // WebSocket transport (simsc_app/)

#include <QtCore/QTimer>

#include <memory>
#include <utility>

namespace simsc_desktop {

struct ObserverWindow::Impl {
	// Ordering here matters: destruction runs bottom-to-top, and
	// ui_functions references data pulled in by load_data_file.
	// Keep the loader alive as long as ui does.
	//
	// data_files_loader<> is move-only (mpq_file inside it wraps
	// a raw FILE*), so we hold it by unique_ptr instead of by
	// std::function like simsc_app does on its stack.
	std::unique_ptr<bwgame::data_loading::data_files_loader<>> load_data_file;
	std::unique_ptr<bwgame::ui_functions> ui;
	bwgame::action_state action_st;
	bwgame::sync_state sync_st;
	std::unique_ptr<bwgame::sync_functions> funcs;
	bwgame::game_load_functions::setup_info_t setup_info;
	std::unique_ptr<bwgame::sync_server_qt_ws> transport;
	QTimer* timer = nullptr;

	// Same book-keeping as simsc_app/main.cpp's loop_state.
	int last_slot = -2;
	int last_inv  = -1;
	bool rand_logged = false;
};

ObserverWindow::ObserverWindow(ObserverParams params, QObject* parent)
	: QObject(parent), d_(new Impl) {

	using namespace bwgame;

	// ------------------------------------------------------------
	// 1. MPQ + map.
	// ------------------------------------------------------------
	d_->load_data_file.reset(new data_loading::data_files_loader<>(
		data_loading::data_files_directory(
			a_string(params.data_path.toStdString().c_str()))));
	game_player player(*d_->load_data_file);
	{
		game_load_functions loader(player.st());
		for (size_t i = 0; i < 8; ++i)
			loader.setup_info.create_melee_units_for_player[i] = true;
		state& st = player.st();
		auto setup_f = [&params, &st]() {
			for (size_t i = 0; i != 12; ++i) {
				if (st.players[i].controller == player_t::controller_open) {
					st.players[i].controller = player_t::controller_occupied;
				}
				if (st.players[i].controller
				    == player_t::controller_computer) {
					st.players[i].controller
					    = player_t::controller_computer_game;
				}
			}
			for (size_t i = 0; i < 8; ++i) {
				if (params.race_overrides[i] < 0) continue;
				st.players[i].race = (race_t)params.race_overrides[i];
			}
		};
		loader.load_map_file(
			a_string(params.map_path.toStdString().c_str()),
			setup_f);
	}

	// ------------------------------------------------------------
	// 2. ui_functions.
	// ------------------------------------------------------------
	d_->ui.reset(new ui_functions(std::move(player)));
	// Critical: type_quit must NOT std::exit(); we want the
	// observer window to close but the rest of simsc_desktop to
	// stay alive.
	d_->ui->exit_on_close = false;
	d_->ui->load_all_image_data(*d_->load_data_file);
	auto* loader = d_->load_data_file.get();
	d_->ui->load_data_file = [loader](a_vector<uint8_t>& data,
	                                  a_string filename) {
		(*loader)(data, std::move(filename));
	};
	d_->ui->init();

	// ------------------------------------------------------------
	// 3. sync + transport.
	// ------------------------------------------------------------
	d_->funcs.reset(new sync_functions(
		d_->ui->st, d_->action_st, d_->sync_st));
	d_->sync_st.setup_info = &d_->setup_info;
	d_->sync_st.latency = 2;
	d_->sync_st.client_batch_size = params.client_batch_size;
	d_->sync_st.local_client->name = "simsc_desktop";
	if (!params.api_key.isEmpty()) {
		d_->sync_st.outgoing_api_key =
			a_string(params.api_key.toStdString().c_str());
	}

	d_->transport.reset(new sync_server_qt_ws());
	d_->transport->client_url_path = "/observer";   // ignored by connect_url
	d_->transport->client_api_key  = params.api_key.toStdString();
	d_->transport->connect_url(params.url.toStdString());

	// ------------------------------------------------------------
	// 4. GameWidget. window_impl::create() constructs + show()s a
	//    top-level QWidget internally; no reparenting on our side.
	// ------------------------------------------------------------
	auto& wnd = d_->ui->wnd;
	wnd.create(params.title.toStdString().c_str(), 0, 0,
	           params.screen_width, params.screen_height);
	d_->ui->resize(params.screen_width, params.screen_height);
	d_->ui->screen_pos = {
		(int)d_->ui->game_st.map_width  / 2 - params.screen_width  / 2,
		(int)d_->ui->game_st.map_height / 2 - params.screen_height / 2,
	};
	d_->ui->set_image_data();

	// ------------------------------------------------------------
	// 5. Sim tick QTimer. Same interval logic as simsc_app.
	// ------------------------------------------------------------
	d_->timer = new QTimer(this);
	const int tick_interval_ms = (params.client_batch_size > 1) ? 42 : 10;
	d_->timer->setInterval(tick_interval_ms);
	connect(d_->timer, &QTimer::timeout, this, [this] {
		using namespace bwgame;
		auto& s = *d_;
		s.funcs->next_frame(*s.transport);

		if (!s.rand_logged && s.sync_st.game_started && s.sync_st.sync_log) {
			char buf[64];
			snprintf(buf, sizeof(buf),
				"GAME_START\tinitial_rand=%08x",
				s.sync_st.initial_rand_state);
			bwgame::sync_log_line(s.sync_st, 'O', a_string(buf));
			s.rand_logged = true;
		}
		int cf = (int)s.ui->st.current_frame;
		if (s.sync_st.sync_log && cf > 0
		    && cf != s.last_inv && cf % 300 == 0) {
			for (int slot = 0; slot < 2; ++slot) {
				s.funcs->log_inventory('O', slot);
			}
			s.last_inv = cf;
		}

		if (s.ui->viewing_slot != s.sync_st.viewing_slot) {
			s.ui->viewing_slot = s.sync_st.viewing_slot;
		}
		if (s.sync_st.viewing_slot != s.last_slot) {
			s.last_slot = s.sync_st.viewing_slot;
		}

		// HUD state -- same computation as simsc_app.
		{
			auto& wnd = s.ui->wnd;
			native_window::hud_state_t hs;
			int slot = s.ui->viewing_slot;
			if (slot >= 0 && slot < 8 && s.sync_st.game_started) {
				int r = (int)s.ui->st.players[slot].race;
				if (r < 0 || r > 2) r = 1;
				hs.minerals = s.ui->st.current_minerals[slot];
				hs.gas      = s.ui->st.current_gas[slot];
				hs.supply_used =
					s.ui->st.supply_used[slot][r].integer_part() / 2;
				hs.supply_max  =
					s.ui->st.supply_available[slot][r].integer_part() / 2;
			}
			auto area = s.ui->get_minimap_area();
			hs.minimap_x = area.from.x;
			hs.minimap_y = area.from.y;
			hs.minimap_w = area.to.x - area.from.x;
			hs.minimap_h = area.to.y - area.from.y;
			wnd.set_hud_state(hs);
		}

		s.ui->update();

		// User closed the GameWidget -- ui->update() surfaced the
		// type_quit event and flipped window_closed. Tear ourselves
		// down; deleteLater is the safe way from inside a slot.
		if (s.ui->window_closed) {
			s.timer->stop();
			deleteLater();
		}
	});
	d_->timer->start();
}

ObserverWindow::~ObserverWindow() {
	if (d_ && d_->timer) d_->timer->stop();
	delete d_;
}

}   // namespace simsc_desktop

// ------------------------------------------------------------------
// ui::log_str / ui::fatal_error_str hooks.
//
// ui.h declares these; every executable that links the game code
// must provide exactly one definition. simsc_app defines them in
// its main.cpp; we can't do the same because simsc_desktop's
// main.cpp deliberately avoids including bwgame.h (see comment
// there). Define them here, in the one TU that already pulls in
// ui.h.
// ------------------------------------------------------------------
namespace bwgame {
namespace ui {
void log_str(a_string str) {
	std::fwrite(str.data(), str.size(), 1, stdout);
	std::fflush(stdout);
}
void fatal_error_str(a_string str) {
	log("fatal error: %s\n", str);
	std::fflush(stdout);
	std::abort();
}
}   // namespace ui
}   // namespace bwgame
