// drawing_playground_tab.h: manual pixel-offset calibration tab.
//
// Purpose: the sieged tank muzzle-flash overlay drifts down-right of
// the barrel in both retail SC:BW and openBW. This tab lets us
// hand-tune a compensating (dx, dy) offset for the flame (and, if
// needed, the turret) while looking at the live SD render.
//
// Layout: gray canvas showing a sieged tank firing continuously.
// Left sidebar: radio picker for "Turret" vs "Flame" (which layer
// the arrow keys nudge), readout of the current (dx, dy) deltas.
// Top: text of the current deltas so a user can copy the numbers.
// Keys: Up/Down/Left/Right nudge active layer by 1 SD pixel,
// Shift+arrow by 10 pixels. Space cycles through facings 0..16.
//
// Runs on its own SimHarness (same pattern as PlaygroundTab) so
// no debug overrides leak into the animation-browser tabs.

#pragma once

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <memory>
#include <string>

class QLabel;
class QRadioButton;
class QSlider;
class QTimer;

namespace sprite_viewer {

class SimHarness;
class DrawingCanvas;

class DrawingPlaygroundTab : public QWidget {
	Q_OBJECT
public:
	DrawingPlaygroundTab(std::string data_path,
	                     std::string map_relpath,
	                     QWidget* parent = nullptr);
	~DrawingPlaygroundTab() override;

protected:
	void showEvent(QShowEvent* e) override;
	void keyPressEvent(QKeyEvent* e) override;

private slots:
	void on_tick();
	void on_facing_changed(int slider_value);

private:
	void ensure_booted();
	void refresh_readout();
	void apply_deltas();

	std::string data_path_;
	std::string map_relpath_;
	std::unique_ptr<SimHarness> sim_;
	bool boot_failed_ = false;
	QString boot_error_;

	// Tunable deltas -- what we're trying to discover.
	int flame_dx_  = 0, flame_dy_  = 0;
	int turret_dx_ = 0, turret_dy_ = 0;

	// UI widgets (owned by Qt).
	DrawingCanvas* canvas_       = nullptr;
	QLabel*        readout_      = nullptr;
	QRadioButton*  pick_turret_  = nullptr;
	QRadioButton*  pick_flame_   = nullptr;
	QSlider*       facing_slider_= nullptr;
	QLabel*        help_label_   = nullptr;
	QTimer*        tick_timer_   = nullptr;
};

}   // namespace sprite_viewer
