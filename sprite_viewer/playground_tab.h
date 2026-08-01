// playground_tab.h: SD gameplay sandbox tab.
//
// Turns the sprite viewer into a live openBW playground. Unlike the
// per-unit animation browser (which spawns ONE unit at map center and
// runs its iscript directly), the playground drives openBW through
// its actual gameplay entry points: create_completed_unit at arbitrary
// world positions, set_unit_order for actions, retail's own tick
// loop. Two consequences that motivated building it:
//
//  1. Sim-driven visual states -- muzzle flashes correctly anchored,
//     medic heal beams, spider mine burrow anims, etc. -- render
//     correctly because they get spawned through openBW's real fire
//     path, not through our shortcut iscript_run_anim().
//
//  2. Multi-unit scenarios are testable: place two units, order one
//     to attack the other, watch the whole interaction.
//
// Phase 1 (this file): scaffolding. Widened canvas, list of alive
// units, spawn-at-click, world-coordinate readout. No orders yet;
// spawned units just sit there or (for units with a default idle
// anim) idle in place.
//
// Runs on its OWN SimHarness instance so its wider render surface
// and different camera model don't interfere with the animation-
// browser tabs. Lazy-booted the first time the tab becomes visible;
// callers pay no cost if they never open it.

#pragma once

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <memory>
#include <string>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTimer;

namespace sprite_viewer {

class SimHarness;
class PlaygroundCanvas;

class PlaygroundTab : public QWidget {
	Q_OBJECT
public:
	// data_path + map_relpath match ViewerWindow's own boot args --
	// the tab spins up its own SimHarness against the same map so
	// the playground has real terrain to walk on. Deferred: not
	// booted until showEvent fires.
	PlaygroundTab(std::string data_path,
	              std::string map_relpath,
	              QWidget* parent = nullptr);
	~PlaygroundTab() override;

	// Boot the harness if we haven't yet. Called on first show or
	// when the user explicitly retries after a boot failure.
	void ensure_booted();

protected:
	void showEvent(QShowEvent* e) override;

private slots:
	void on_race_changed(int race_index);
	void on_spawn_clicked();
	void on_center_clicked();
	void on_tick();
	void on_units_selection_changed();
	void on_move_clicked();
	void on_attack_clicked();
	void on_stop_clicked();
	void on_hold_clicked();
	void on_siege_clicked();
	void on_unsiege_clicked();
	void on_kill_clicked();
	void on_canvas_clicked(int world_x, int world_y);

private:
	void refresh_unit_list();
	void populate_units_for_race(int race_index);
	int  selected_unit_id() const;
	// Update button enabled states based on the currently-selected
	// unit (Siege/Unsiege only for the right type, etc.).
	void update_action_buttons();
	// Interaction modes: "pending click will be interpreted as..."
	// None = clicks are informational (log). Move/Attack = the
	// next click on the canvas dispatches that order.
	enum class ClickMode { None, Move, Attack };
	void set_click_mode(ClickMode m, const char* status);
	ClickMode click_mode_ = ClickMode::None;
	// Tracks the last known selected-unit id so we can distinguish
	// "list rebuilt, selection kept" from "user actually picked a
	// different unit". Cancelling a Move/Attack toggle only happens
	// on the latter.
	int last_seen_selection_ = -1;

	std::string data_path_;
	std::string map_relpath_;
	std::unique_ptr<SimHarness> sim_;
	bool boot_failed_ = false;
	QString boot_error_;

	// Widgets (owned by Qt).
	QComboBox*    race_cb_       = nullptr;
	QComboBox*    unit_cb_       = nullptr;
	QComboBox*    owner_cb_      = nullptr;
	QPushButton*  spawn_btn_     = nullptr;
	QPushButton*  center_btn_    = nullptr;
	QLabel*       hover_label_   = nullptr;
	QLabel*       status_label_  = nullptr;
	QListWidget*  units_list_    = nullptr;
	PlaygroundCanvas* canvas_    = nullptr;
	QTimer*       tick_timer_    = nullptr;
	// Action buttons -- kept as members so we can toggle their
	// enabled state when selection changes (Siege only for Tank
	// Mode, Unsiege only for Siege Mode, etc.).
	QPushButton*  move_btn_      = nullptr;
	QPushButton*  attack_btn_    = nullptr;
	QPushButton*  stop_btn_      = nullptr;
	QPushButton*  hold_btn_      = nullptr;
	QPushButton*  siege_btn_     = nullptr;
	QPushButton*  unsiege_btn_   = nullptr;
	QPushButton*  kill_btn_      = nullptr;
};

}   // namespace sprite_viewer
