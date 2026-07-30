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
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtGui/QImage>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sprite_viewer {

class SpriteCanvas;
struct SimHarness;
class HdAssetLoader;
struct HdSprite;
class FramesTab;

// Registry entry for one unit. For the MVP we only ship Marine;
// the structure is here so extending to more is one entry per line.
struct UnitEntry {
	const char* label;
	int unit_type_id;   // bwgame::UnitTypes enum value
};

class ViewerWindow : public QMainWindow {
	Q_OBJECT
public:
	// SpriteCanvas needs to read `sim` and `current_hd` in its
	// paintEvent; keep them private-by-default but visible to the
	// canvas which is essentially an inner rendering helper.
	friend class SpriteCanvas;
	// sc_version:  "classic" or "remastered"
	// sc_remastered_path: SC:R install root (only used when
	//   sc_version=="remastered"). Empty otherwise.
	ViewerWindow(std::string data_path,
	             std::string map_relpath,
	             QString sc_version,
	             QString sc_remastered_path,
	             QWidget* parent = nullptr);
	~ViewerWindow();

private slots:
	void on_race_changed(int index);
	void on_unit_changed(int index);
	void on_anim_changed(int index);
	void on_direction_changed(int value);
	void on_turret_direction_changed(int value);
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
	int current_turret_dir = 4;

	// HD mode. Both empty in classic mode.
	QString sc_version;
	QString sc_remastered_path;
	std::unique_ptr<HdAssetLoader> hd_loader;
	// Per-openBW-unit_type sprite cache. Keyed by unit_type_id, not
	// image_id -- the mapping from unit -> SC:R image_id is resolved
	// once per unit and stashed in the value.
	std::unordered_map<int, std::unique_ptr<HdSprite>> hd_cache;
	// Per-image cache keyed by grp_filename_index (0-based). Used
	// by paint_hd's per-image loop so the shadow, muzzle-flash,
	// etc. each get their own HdSprite without re-decoding on
	// every frame. Keyed differently from hd_cache above (which
	// is unit_type-keyed for the initial current_hd lookup); over
	// time hd_cache should probably fold into this map, but for
	// now the two coexist.
	std::unordered_map<int, std::unique_ptr<HdSprite>> hd_image_cache;
	// Parallel cache keyed by SD image_id (0..998), used by the
	// shadow path since a shadow's grp_filename_index typically
	// matches its body's -- the two can't share a single cache
	// keyed on gfi. images.rel gives us a direct image_id -> HD
	// anim mapping, so this cache is the natural key.
	std::unordered_map<int, std::unique_ptr<HdSprite>> hd_image_by_image_id_cache;
	// Currently displayed HD sprite (non-owning view into hd_cache).
	HdSprite* current_hd = nullptr;

	// UI widgets (owned by Qt).
	QComboBox* race_cb = nullptr;
	QComboBox* unit_cb = nullptr;
	QComboBox* anim_cb = nullptr;
	QSlider* dir_slider = nullptr;
	// Turret direction slider + its label. Only visible for units
	// with a subunit turret (Tank, Goliath, Vulture). We keep both
	// hide/show as a pair so the layout doesn't gap when hidden.
	QLabel*  turret_dir_label = nullptr;
	QSlider* turret_dir_slider = nullptr;
	QPushButton* playpause_btn = nullptr;
	QLabel* readout = nullptr;
	// Split canvas: SD on the left, HD on the right. Both driven
	// by the same SimHarness -- so iscript state advances in
	// lockstep and both views represent the same tick.
	SpriteCanvas* sd_canvas = nullptr;
	SpriteCanvas* hd_canvas = nullptr;
	// Frames tab: HD-only, updated on unit change to auto-browse
	// the current unit's body anim.
	FramesTab* frames_tab = nullptr;

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
