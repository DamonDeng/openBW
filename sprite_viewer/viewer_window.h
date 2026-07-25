// Qt window that hosts the sprite viewer UI + a paint canvas backed
// by openBW's own draw_sprite pipeline.
//
// Layout:
//   +----------------------+---------------------------------+
//   |  Race                |                                 |
//   |    [Terran ▾]        |                                 |
//   |  Unit                |                                 |
//   |    [Marine ▾]        |     draw_sprite canvas          |
//   |  Action              |     (rendered by openBW into    |
//   |    [Idle ▾]          |      an indexed surface, then   |
//   |  Direction  [ 4 ▓]   |      blitted to the widget's    |
//   |  [⏸ Pause]           |      QImage framebuffer)        |
//   |  frame N, dir X      |                                 |
//   +----------------------+---------------------------------+
//
// The left column is a plain QWidget with QComboBox + QSlider.
// The right column is a custom QWidget whose paintEvent asks the
// SimHarness to render the current sprite to a QImage.

#ifndef OPENBW_SPRITE_VIEWER_WINDOW_H
#define OPENBW_SPRITE_VIEWER_WINDOW_H

#include <QtWidgets/QMainWindow>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QWidget>
#include <QtCore/QTimer>
#include <QtGui/QImage>

#include <memory>
#include <string>
#include <vector>

namespace sprite_viewer {

class SpriteCanvas;
struct SimHarness;

// Registry entry for one unit. For the MVP we only ship Marine;
// the structure is here so extending to more is one entry per line.
struct UnitEntry {
	const char* label;
	int unit_type_id;   // bwgame::UnitTypes enum value
};

class ViewerWindow : public QMainWindow {
	Q_OBJECT
public:
	ViewerWindow(std::string data_path,
	             std::string map_relpath,
	             QWidget* parent = nullptr);
	~ViewerWindow();

private slots:
	void on_race_changed(int index);
	void on_unit_changed(int index);
	void on_anim_changed(int index);
	void on_direction_changed(int value);
	void on_playpause_clicked();
	void on_tick();

private:
	void populate_units_for_race(int race_index);
	void populate_anims_for_current_unit();
	void refresh_readout();

	// Model.
	std::unique_ptr<SimHarness> sim;
	std::string data_path;
	std::string map_relpath;
	bool booted = false;
	bool playing = true;
	int current_dir = 4;

	// UI widgets (owned by Qt).
	QComboBox* race_cb = nullptr;
	QComboBox* unit_cb = nullptr;
	QComboBox* anim_cb = nullptr;
	QSlider* dir_slider = nullptr;
	QPushButton* playpause_btn = nullptr;
	QLabel* readout = nullptr;
	SpriteCanvas* canvas = nullptr;

	// Sim tick timer -- retail is 42 ms/frame (~24 FPS).
	QTimer tick_timer;
	static constexpr int TICK_MS = 42;

	// Populated in the constructor from bwenums.h.
	// One dict per race; race names in this vector are the dropdown
	// entries. For MVP only Terran has entries.
	struct RaceEntry {
		const char* label;
		std::vector<UnitEntry> units;
	};
	std::vector<RaceEntry> races;
};

}   // namespace sprite_viewer

#endif
