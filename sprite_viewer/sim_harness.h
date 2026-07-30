// Minimal openBW sim bootstrap for the sprite viewer.
//
// Loads the MPQs + a small map (default: (4)Blood Bath.scm) with
// `setup_info.create_no_units = true` -- gives us a valid
// `bwgame::state` (tile grid, tileset palette, region/pathfinding
// data) with zero units on the field.
//
// The viewer then spawns ONE unit via create_completed_unit and
// drives its iscript directly via iscript_run_anim; the sim advances
// via player.next_frame() each Qt tick, which ticks all animations.
//
// Rendering: we skip ui_functions::update()'s tile/fog/minimap
// passes and paint the sprite directly onto a solid background,
// so the viewer canvas is clean.

#ifndef OPENBW_SPRITE_VIEWER_SIM_HARNESS_H
#define OPENBW_SPRITE_VIEWER_SIM_HARNESS_H

// Deliberately NOT including ui.h here: it has no include guard,
// so including it in two different .cpp files that share this
// header multiply-defines its free functions. Consumers of this
// header get forward declarations only for `ui_functions`; the
// .cpp files that actually touch it must include ui.h themselves.
//
// data_loading.h IS include-guarded and safe.

#include "../data_loading.h"

#include <memory>
#include <string>

namespace bwgame {
	struct unit_t;
	struct ui_functions;
}

namespace sprite_viewer {

// Owns all sim state. One instance per viewer window.
struct SimHarness {
	// The MPQ loader has to outlive ui_functions because ui_functions
	// stores a `load_data_file` callback that reads from it on demand.
	// Held as unique_ptr because data_files_loader is move-only (each
	// mpq_file owns a FILE*). The pointer lifetime lets us construct
	// after the harness itself.
	std::unique_ptr<bwgame::data_loading::data_files_loader<>> mpq_loader;

	// game_player is constructed inside boot() as a local, then moved
	// into ui_functions (which takes it by value). ui_functions is the
	// only thing we hold long-term; it owns the state through its
	// `player` member. Both must outlive any unit_t*/sprite_t* we
	// hand out.
	std::unique_ptr<bwgame::ui_functions> ui;

	// The one and only unit we spawn. Owned by the sim's unit table,
	// not by us -- we just hold a pointer.
	bwgame::unit_t* unit = nullptr;

	// Set after kill_unit(). While true, tick() is safe to keep
	// running (the death animation plays over several frames) but
	// the pointer may become invalid at any tick. We validate
	// unit->sprite before every dereference. When the unit's
	// sprite is finally cleaned up, we treat that as "gone" and
	// stop rendering.
	bool unit_dying = false;

	// Current iscript animation running on unit->sprite->main_image.
	int current_anim = -1;   // -1 = never started

	// Map dimensions in world (pixel) coords, cached after map load.
	int map_pixel_width = 0;
	int map_pixel_height = 0;

	SimHarness();
	~SimHarness();
	SimHarness(const SimHarness&) = delete;
	SimHarness& operator=(const SimHarness&) = delete;

	// Boot procedure. Blocks on I/O (MPQ load). Throws bwgame::exception
	// on failure. `data_path` should be the dir containing StarDat.mpq
	// (and the map file); `map_relpath` is relative to data_path.
	void boot(const std::string& data_path, const std::string& map_relpath);

	// Create the hidden native window ui.h renders into, and set
	// screen dims. Call once, after boot(), before the first
	// render_frame(). Width/height define the sprite canvas size
	// the caller gets back from render_frame().
	void configure_viewport(int width, int height);

	// Spawn (or respawn) a unit of the given type at the map center,
	// owned by player 0. If a unit already exists, kill_unit() it
	// first. Returns false if the unit_type is invalid or spawning
	// fails.
	bool spawn_unit(int unit_type_id);

	// Run one of the iscript_anims::* on the unit's main image.
	// Silently no-ops (returns false) if the image doesn't have an
	// anim_pc for that id (which is common -- most units don't have
	// Landing, LiftOff, WarpIn, etc). See data_types.h:446 for the
	// full list.
	bool run_anim(int anim_id);

	// Set the unit's heading. slider_dir is 0..16 with 0=up, 4=right,
	// 8=down, 12=left. Internally translates to BW's 256-direction
	// raw byte via direction_from_index. See bwgame.h:13281 for how
	// this drives frame_index_offset.
	void set_heading_from_slider(int slider_dir);

	// One iscript / sim tick. Advances all animation state machines,
	// same as retail's per-frame update (bwgame.h:13185 next_frame).
	void tick();

	// Center the viewport on the current unit so `draw_sprite`
	// paints it at the canvas center.
	void center_viewport();

	// Paint the current unit into ui's indexed_surface + blit to
	// window_surface. Returns pointer to the RGBA window surface
	// data + width/height/pitch so the caller can copy pixels onto
	// its own widget. Returns nullptr / zeros if the sim isn't
	// ready yet. The pointer is invalidated by the next tick().
	struct FramePixels {
		const void* data = nullptr;
		int width = 0;
		int height = 0;
		int pitch = 0;
	};
	// draw_sprite = true (default): tiles + unit sprite composited
	// into the returned buffer, the way the classic viewer wants
	// it. draw_sprite = false: only the tile terrain -- callers
	// use this to build a ground layer under a differently-drawn
	// unit (e.g. the HD canvas overlays its own scaled-down HD
	// sprite on the tiles).
	FramePixels render_frame(bool draw_sprite = true);

	// Which HD2 tileset this map uses (0..7, matching the enum in
	// ui.h::load_tileset_image_data). -1 if not booted.
	int tileset_index() const;

	// Description of one on-screen megatile: its megatile_index
	// (row in tiles_mega_tile_index / index into the HD .dds.vr4
	// container) plus the widget-relative screen rect (top-left
	// x,y and 32x32 dimensions in SD's own coordinates). The
	// caller (HD canvas) scales this to whatever HD dimensions
	// the tileset ships. Empty when the sim isn't booted yet.
	struct TileCell {
		int megatile_index = -1;
		int screen_x = 0;    // relative to render_frame surface top-left
		int screen_y = 0;
	};
	std::vector<TileCell> visible_tiles() const;

	// Return frame_index / base / offset for the readout. Any of
	// these being negative means "no unit spawned" or "no image".
	struct FrameInfo {
		int frame_index = -1;
		int frame_base = -1;
		int frame_offset = -1;
	};
	FrameInfo current_frame_info() const;

	// True if the current unit's iscript has an anim_pc for the
	// given anim id. Used to filter the anim dropdown to just what
	// this unit supports.
	bool anim_available(int anim_id) const;

	// Return the openBW image_type_t::id (bw_id) of the current
	// unit's main sprite image -- an index into images.dat / the
	// ImageTypes enum (999 slots). -1 if no unit is spawned.
	int current_image_id() const;

	// Return the arr/images.tbl string ordinal (grp_filename_index)
	// of the current unit's main image. This is the number the HD
	// Mapping tab's combobox uses to identify unit names, so it's
	// the correct key into hd_mapping_table.json. Distinct from
	// current_image_id() -- images.tbl has 929 valid strings while
	// images.dat has 999 image_type slots; the two indexings do
	// NOT line up.
	int current_grp_filename_index() const;

	// True iff the current unit's main image has the
	// horizontally_flipped flag set by iscript. openBW stores only
	// facings 0..16 in the frame table; facings 17..31 are drawn
	// by mirroring one of the stored facings horizontally. The HD
	// renderer needs to honor the same convention -- otherwise
	// every left-facing direction looks identical to its right-
	// facing mirror.
	bool current_flipped() const;

	// One entry per image_t on the current unit's sprite, in draw
	// order (deepest first -- shadow, then body, then overlays).
	// Matches ui.h::draw_sprite which iterates reverse(sprite->images)
	// so the topmost image is rendered last. Consumers (HD renderer)
	// loop through this and blit each image; images with no HD
	// mapping fall through to classic SD rendering, which the
	// underlying tile surface already contains.
	struct SpriteImage {
		int  grp_filename_index = -1;  // key into hd_mapping_table
		int  frame_index = 0;          // atlas frame to draw
		int  offset_x = 0;             // image_t.offset relative to sprite center
		int  offset_y = 0;
		bool flipped = false;
		bool is_shadow = false;        // modifier == 10 -> shadow blend
		int  image_id  = -1;           // ImageTypes enum value (unique per image_t kind)
		bool hidden = false;           // flag_hidden set -> skip
		int  grp_width = 0;            // SD grp bounding-box width, used
		int  grp_height = 0;           //  by HD renderer to compute the
		                               //  scale ratio HD_frame_w / SD_grp_w
		                               //  that makes HD sprites visually
		                               //  match SD on the same widget.
	};
	std::vector<SpriteImage> current_sprite_images() const;

private:
	bool tried_first_update = false;
};

// Names for the iscript_anims enum in data_types.h:446. Indexed by
// the enum value; used to populate the anim dropdown.
extern const char* const ISCRIPT_ANIM_NAMES[28];

}   // namespace sprite_viewer

#endif
