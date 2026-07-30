// See sim_harness.h for the design.

#include "sim_harness.h"

// bwgame.h and ui.h are unguarded header-only mega-blobs (see the
// note in sim_harness.h). They MUST live only in .cpp files and
// each .cpp needs its own copy. Keep them here; viewer_window.cpp
// gets its own set independently.
#include "bwgame.h"
#include "ui.h"

#include <cstdio>
#include <functional>
#include <string>

namespace sprite_viewer {

using namespace bwgame;

// Names for iscript_anims (data_types.h:446). Indexed by enum value.
const char* const ISCRIPT_ANIM_NAMES[28] = {
	"Init",             //  0
	"Death",            //  1
	"GndAttkInit",      //  2
	"AirAttkInit",      //  3
	"Unused1",          //  4
	"GndAttkRpt",       //  5
	"AirAttkRpt",       //  6
	"CastSpell",        //  7
	"GndAttkToIdle",    //  8
	"AirAttkToIdle",    //  9
	"Unused2",          // 10
	"Walking",          // 11
	"WalkingToIdle",    // 12
	"SpecialState1",    // 13
	"SpecialState2",    // 14
	"AlmostBuilt",      // 15
	"Built",            // 16
	"Landing",          // 17
	"LiftOff",          // 18
	"IsWorking",        // 19
	"WorkingToIdle",    // 20
	"WarpIn",           // 21
	"Unused3",          // 22
	"StarEditInit",     // 23
	"Disable",          // 24
	"Burrow",           // 25
	"UnBurrow",         // 26
	"Enable",           // 27
};

SimHarness::SimHarness() = default;
SimHarness::~SimHarness() = default;

void SimHarness::configure_viewport(int width, int height) {
	if (!ui) return;
	ui->wnd.create("sprite-viewer-hidden", 0, 0, width, height);
	ui->resize(width, height);
	ui->set_image_data();
}

void SimHarness::boot(const std::string& data_path,
                      const std::string& map_relpath) {
	// 1. Load data files (MPQs from data_path). Store the loader
	//    on the harness so it outlives boot() and remains callable
	//    from the ui.load_data_file lambda we install below.
	mpq_loader = std::make_unique<data_loading::data_files_loader<>>(
		data_loading::data_files_directory(
			a_string(data_path.c_str())));
	auto& load_data_file = *mpq_loader;

	// 2. Construct game_player -- triggers global_init: loads
	//    units.dat / weapons.dat / iscript.bin / images.tbl / every
	//    GRP referenced by images.tbl / all 8 tilesets' .cv5 + .vf4.
	game_player player(load_data_file);

	// 3. Load the map with setup_info.create_no_units = true so no
	//    starting units spawn (we spawn our own single unit later).
	//    Also promote controller_open -> controller_occupied so
	//    the player slot is "real" for the unit's owner.
	{
		game_load_functions loader(player.st());
		loader.setup_info.create_no_units = true;
		for (size_t i = 0; i < 8; ++i) {
			loader.setup_info.create_melee_units_for_player[i] = false;
		}
		state& st = player.st();
		auto setup_f = [&st]() {
			for (size_t i = 0; i != 12; ++i) {
				if (st.players[i].controller ==
				    player_t::controller_open) {
					st.players[i].controller =
					    player_t::controller_occupied;
				}
			}
		};
		std::string full_map =
			(data_path.empty() ? std::string() : data_path + "/") +
			map_relpath;
		loader.load_map_file(a_string(full_map.c_str()), setup_f);
	}

	map_pixel_width = (int)player.st().game->map_width;
	map_pixel_height = (int)player.st().game->map_height;

	// 4. ui_functions handles rendering. Move the loaded player into
	//    it (ui_functions takes by value), then load per-tileset PCX
	//    + all GRPs' image data + set up image lookups.
	ui = std::make_unique<ui_functions>(std::move(player));
	ui->load_all_image_data(load_data_file);
	// Capture by pointer to the harness member so the closure stays
	// valid even after this method returns. Never dereference this
	// pointer inside the lambda after ~SimHarness runs; ui_functions
	// dies first (owned by SimHarness member) so that's guaranteed.
	auto* loader_ptr = mpq_loader.get();
	ui->load_data_file = [loader_ptr](a_vector<uint8_t>& data,
	                                    a_string filename) {
		(*loader_ptr)(data, std::move(filename));
	};
	ui->init();
	// Log the resolved tileset so the HD tile loader (if any)
	// knows which .dds.vr4 to open. Names match ui.h:188 --
	// 0=badlands, 1=platform, 2=install, 3=AshWorld, 4=Jungle,
	// 5=Desert, 6=Ice, 7=Twilight.
	fprintf(stderr, "[sim_harness] tileset_index=%zu\n",
		ui->game_st.tileset_index);
	// Spectator: no fog / no visibility filter (see ui.h:1247-1252).
	ui->viewing_slot = -1;
	// Mute the sound path. iscript's `playsnd` opcode triggers
	// load_data_file("sound/...") which errors if the file isn't
	// present -- and slim MPQs deliberately drop the sound/ tree.
	// global_volume=0 short-circuits play_sound before it tries
	// to load anything (ui.h:640). No audio needed for a viewer.
	ui->global_volume = 0;
}

bool SimHarness::spawn_unit(int unit_type_id) {
	if (!ui) return false;
	auto& funcs = *ui;

	// Kill any previous unit so we start fresh (avoid piling corpses
	// or duplicated iscript state). Also clear our own bookkeeping
	// so a Death-then-different-action-click doesn't keep sending
	// commands to a corpse.
	if (unit != nullptr && !unit_dying) {
		funcs.kill_unit(unit);
	}
	unit = nullptr;
	unit_dying = false;

	const unit_type_t* ut = funcs.get_unit_type((UnitTypes)unit_type_id);
	if (!ut) return false;

	// Spawn at map center.
	xy pos(map_pixel_width / 2, map_pixel_height / 2);

	unit = funcs.create_completed_unit(ut, pos, /*owner=*/0);
	if (!unit) return false;

	// Default facing "up" (dir 0 in retail).
	funcs.set_unit_heading(unit, funcs.direction_from_index(0));
	current_anim = -1;
	// New unit -> drop any previous unit's turret override so the
	// new subunit (if any) starts free-tracking the base. UI will
	// reapply its own slider value right after spawn_unit returns.
	turret_dir_override = -1;
	return true;
}

// Small helper: is the current unit pointer safe to dereference for
// rendering? Two ways it can be unsafe:
//   1. unit is null (never spawned or just gone).
//   2. unit is non-null but its sprite has been cleaned up during
//      the Orders::Die tail (sim reclaims the sprite before the
//      unit_t slot is recycled).
static bool unit_is_alive_for_render(const bwgame::unit_t* unit) {
	if (!unit) return false;
	if (!unit->sprite) return false;
	if (!unit->sprite->main_image) return false;
	return true;
}

bool SimHarness::run_anim(int anim_id) {
	if (!ui) return false;
	// If the unit already died and no respawn happened yet, only the
	// Death re-request is meaningful (but there's nothing to kill).
	// The viewer's own action-change handler is responsible for
	// respawning before calling run_anim on a fresh unit.
	if (!unit_is_alive_for_render(unit)) return false;

	// Special case: Death. iscript's Death sequence issues opc_end,
	// which destroys the main image (bwgame.h:14847). Calling it
	// directly on the main image via iscript_run_anim leaves us
	// with a dangling unit->sprite pointer -- next render_frame
	// crashes on `unit->sprite->main_image`.
	//
	// The proper path is set_unit_order(Orders::Die), which
	// kill_unit does (bwgame.h:2625-2632). That schedules the
	// death animation over the next N ticks + eventual unit
	// disposal. We flag `unit_dying` so the tick loop stops
	// touching the unit once it's actually gone.
	if (anim_id == /*iscript_anims::Death*/ 1) {
		ui->kill_unit(unit);
		current_anim = anim_id;
		unit_dying = true;
		return true;
	}

	auto* image = unit->sprite->main_image;
	// Check that this image_type has an anim_pc for the requested
	// animation before dispatch; iscript_run_anim would error
	// (fatal) otherwise (bwgame.h:15145).
	auto* image_type = image->image_type;
	auto script_it = ui->global_st.iscript.scripts.find(
		image_type->iscript_id);
	if (script_it == ui->global_st.iscript.scripts.end()) return false;
	const auto& anim_pcs = script_it->second.animation_pc;
	if ((size_t)anim_id >= anim_pcs.size() || anim_pcs[anim_id] == 0) {
		return false;
	}
	ui->iscript_run_anim(image, anim_id);
	current_anim = anim_id;
	return true;
}

void SimHarness::set_heading_from_slider(int slider_dir) {
	if (!ui || !unit_is_alive_for_render(unit)) return;
	// Slider 0..16 -> raw byte 0..255. Retail maps a 17-index
	// direction to raw = idx * 8 with a small +4 rounding wart in
	// the reverse map (bwgame.h:13284). To go forward we can just
	// use slider*8 and let set_image_heading resolve the exact
	// frame offset.
	int raw = (slider_dir * 16) % 256;
	// Clamp defensively.
	if (raw < 0) raw = 0;
	if (raw > 255) raw = 255;
	ui->set_unit_heading(unit, ui->direction_from_index((size_t)raw));
}

bool SimHarness::has_turret() const {
	if (!ui || !unit_is_alive_for_render(unit)) return false;
	return unit->subunit && ui->ut_turret(unit->subunit);
}

void SimHarness::set_turret_heading_from_slider(int slider_dir) {
	turret_dir_override = slider_dir;
	if (!has_turret()) return;
	int raw = (slider_dir * 16) % 256;
	if (raw < 0) raw = 0;
	if (raw > 255) raw = 255;
	// set_unit_heading on the SUBUNIT -- iterates its own sprite's
	// images, not the base's, so the base facing is untouched.
	ui->set_unit_heading(unit->subunit,
		ui->direction_from_index((size_t)raw));
}

void SimHarness::tick() {
	if (!ui) return;
	// ui_functions IS-A state_functions (via ui_util_functions ->
	// replay_functions -> action_functions -> state_functions).
	// We can't call ui->next_frame() because replay_functions::
	// next_frame gates on end_frame == current_frame (replay.h:234)
	// and we never loaded a replay -- both are 0. Bypass that layer
	// by calling state_functions::next_frame() directly, which
	// runs iscript_execute for every image + steps `wait` counters
	// (bwgame.h:13185 -> process_frame -> update_thingies).
	ui->state_functions::next_frame();

	// Reassert the user's turret heading. openBW's turn_turret sets
	// status flag 0x2000000 ("auto-follow base") when the turret is
	// idle and its heading matches the base's, then snaps
	// turret->heading = base->heading each tick until an order
	// target appears (bwgame.h:12381-12398). For a viewer we want
	// the slider to be authoritative, so overwrite here every tick.
	if (turret_dir_override >= 0 && has_turret()) {
		int raw = (turret_dir_override * 16) % 256;
		if (raw < 0) raw = 0;
		if (raw > 255) raw = 255;
		ui->set_unit_heading(unit->subunit,
			ui->direction_from_index((size_t)raw));
	}

	// Non-looping actions (GndAttkInit, GndAttkRpt, GndAttkToIdle,
	// AirAttk*, CastSpell, ...) run to completion in iscript, then
	// iscript falls back to Idle -- so a firing marine shoots once
	// and returns to idle. In real gameplay the game state (attack
	// order, target in range, etc.) re-triggers these. We don't
	// have that state, so instead we auto-loop by re-firing the
	// user's selection whenever iscript has moved off it. Skip
	// while dying so Death gets to complete.
	//
	// current_anim < 0 means the user hasn't picked an action yet.
	// Anim 0 (Init) is the "just spawned" state -- don't touch it.
	if (!unit_dying && current_anim > 0 && unit_is_alive_for_render(unit)) {
		auto* image = unit->sprite->main_image;
		if (image->iscript_state.animation != current_anim) {
			// iscript has transitioned away from the selected anim.
			// Re-fire it. Guard with anim_available in case the
			// image_type doesn't have this anim (shouldn't happen
			// since we filter the dropdown, but be defensive).
			if (anim_available(current_anim)) {
				ui->iscript_run_anim(image, current_anim);
			}
		}
	}
}

void SimHarness::center_viewport() {
	if (!ui || !unit_is_alive_for_render(unit)) return;
	// screen_pos is subtracted from world coords in draw_image (see
	// ui.h:877). Setting it to (unit_pos - screen_center) places
	// the unit at the pixel center of the viewport.
	ui->screen_pos = {
		unit->sprite->position.x - (int)ui->screen_width / 2,
		unit->sprite->position.y - (int)ui->screen_height / 2,
	};
}

SimHarness::FramePixels SimHarness::render_frame(bool draw_sprite) {
	FramePixels out;
	if (!ui) return out;

	// Lazy-init surfaces via ui.update() the first time only. That
	// call also does a full ui frame including tiles/fog/minimap;
	// we accept that on the first tick, then skip to draw_sprite
	// only on subsequent ones.
	if (!ui->indexed_surface) {
		ui->update();
		return out;   // let the widget request a repaint next frame
	}

	center_viewport();

	auto* idx = ui->indexed_surface.get();
	uint8_t* data = (uint8_t*)idx->lock();
	size_t pitch = (size_t)idx->pitch;

	// Ground: let openBW paint the map tiles under our viewport
	// (draw_tiles walks screen_tile_bounds against st.tiles and
	// blits per-tile from the tileset's megatile atlas). The
	// backing map is loaded at boot ('(4)Blood Bath.scm' by
	// default) and center_viewport() below anchors the viewport
	// on the spawned unit's world position -- so we get real
	// terrain rather than a solid-color fill under the sprite.
	// Pre-fill is unnecessary; draw_tiles covers every screen
	// pixel because we sized the surface to fit the tile grid.
	ui->draw_tiles(data, pitch);

	// Only composite the unit sprite when the caller wants it.
	// The HD canvas paints tiles here then overlays its own
	// HD-scale unit on top; painting the SD sprite in that path
	// too produces a double-draw (SD unit at 2x behind the HD
	// unit at 1x, both offset by kHdScale). See render_frame's
	// draw_sprite parameter docs.
	if (draw_sprite && unit_is_alive_for_render(unit)) {
		ui->draw_sprite(unit->sprite, data, pitch);
	}
	idx->unlock();

	// Blit indexed -> rgba -> window_surface -- same ordering as
	// ui/sdl2.cpp::update_surface.
	if (ui->rgba_surface) {
		ui->rgba_surface->fill(0, 0, 0, 255);
		ui->indexed_surface->blit(&*ui->rgba_surface, 0, 0);
		if (ui->window_surface) {
			ui->rgba_surface->blit(&*ui->window_surface, 0, 0);
			ui->wnd.update_surface();
		}
	}

	if (!ui->window_surface) return out;
	out.data = ui->window_surface->lock();
	out.width = (int)ui->window_surface->w;
	out.height = (int)ui->window_surface->h;
	out.pitch = (int)ui->window_surface->pitch;
	// NOTE: the surface stays locked until the next render_frame.
	// The Qt widget's paintEvent must not hold the pixels past the
	// end of this call — copy or blit inside the same paintEvent.
	ui->window_surface->unlock();
	return out;
}

SimHarness::FrameInfo SimHarness::current_frame_info() const {
	FrameInfo out;
	if (!unit_is_alive_for_render(unit)) return out;
	const auto* image = unit->sprite->main_image;
	out.frame_index = (int)image->frame_index;
	out.frame_base = (int)image->frame_index_base;
	out.frame_offset = (int)image->frame_index_offset;
	return out;
}

bool SimHarness::anim_available(int anim_id) const {
	if (!ui || !unit_is_alive_for_render(unit)) return false;
	auto* image = unit->sprite->main_image;
	int iscript_id = image->image_type->iscript_id;
	auto it = ui->global_st.iscript.scripts.find(iscript_id);
	if (it == ui->global_st.iscript.scripts.end()) return false;
	const auto& anim_pcs = it->second.animation_pc;
	if ((size_t)anim_id >= anim_pcs.size()) return false;
	return anim_pcs[anim_id] != 0;
}

int SimHarness::tileset_index() const {
	if (!ui) return -1;
	return (int)ui->game_st.tileset_index;
}

std::vector<SimHarness::TileCell> SimHarness::visible_tiles() const {
	// Compute the same on-screen tile grid ui.h::draw_tiles walks
	// so HD paint stays in lockstep with SD. Layout:
	//   from_tile.x/y = screen_pos.xy / 32  (clamped)
	//   to_tile.x/y   = (screen_pos.xy + view_wh + 31) / 32  (clamped)
	// screen pos of each 32x32 megatile:
	//   sx = tile_x * 32 - screen_pos.x
	//   sy = tile_y * 32 - screen_pos.y
	std::vector<TileCell> out;
	if (!ui || !ui->indexed_surface) return out;

	int view_w = (int)ui->view_width;
	int view_h = (int)ui->view_height;
	int spx = ui->screen_pos.x;
	int spy = ui->screen_pos.y;
	int mtw = (int)ui->game_st.map_tile_width;
	int mth = (int)ui->game_st.map_tile_height;

	int from_x = spx / 32;
	if (from_x < 0 || from_x >= mtw) from_x = 0;
	int from_y = spy / 32;
	if (from_y < 0 || from_y >= mth) from_y = 0;
	int to_x = (spx + view_w + 31) / 32;
	if (to_x > mtw) to_x = mtw;
	int to_y = (spy + view_h + 31) / 32;
	if (to_y > mth) to_y = mth;

	const auto* mega = ui->st.tiles_mega_tile_index.data();
	out.reserve((size_t)(to_y - from_y) * (to_x - from_x));
	for (int ty = from_y; ty < to_y; ++ty) {
		for (int tx = from_x; tx < to_x; ++tx) {
			TileCell c;
			c.megatile_index = (int)mega[ty * mtw + tx];
			c.screen_x = tx * 32 - spx;
			c.screen_y = ty * 32 - spy;
			out.push_back(c);
		}
	}
	return out;
}

int SimHarness::current_image_id() const {
	// Traverse the same chain openBW's own renderer uses:
	// unit -> sprite -> main_image -> image_type -> id.
	// This is the ImageTypes enum ordinal (index into images.dat's
	// 999-slot table), NOT the arr/images.tbl string ordinal.
	if (!ui || !unit_is_alive_for_render(unit)) return -1;
	auto* image = unit->sprite->main_image;
	if (!image || !image->image_type) return -1;
	return (int)image->image_type->id;
}

std::vector<SimHarness::SpriteImage>
SimHarness::current_sprite_images() const {
	// Walk sprite->images in reverse (matches ui.h::draw_sprite's
	// `for (auto* image : reverse(sprite->images))`). Deepest image
	// first -- shadow first, main body next, muzzle-flash / attack
	// overlays last. Consumers blit in the returned order so
	// higher-index entries land on top.
	//
	// Two-part units (Siege Tank, Goliath, Vulture) own a `subunit`
	// pointer with its own sprite -- the turret. In real gameplay
	// ui.h renders every sprite in st.sprites_on_tile_line, which
	// naturally picks up both the base and the turret. Here we only
	// have one unit and one center, so we splice the subunit's images
	// onto the tail of the same list. Draw order is base-first
	// (shadow + body + base overlays), then turret (shadow + body +
	// muzzle flash) -- matches sprite_depth_order's convention that
	// the turret paints ON TOP of the base.
	std::vector<SpriteImage> out;
	if (!ui || !unit_is_alive_for_render(unit)) return out;

	auto push_sprite = [&](const bwgame::sprite_t* sprite) {
		if (!sprite) return;
		for (const auto& image : bwgame::reverse(sprite->images)) {
			SpriteImage e;
			int gfi = (int)image.image_type->grp_filename_index;
			e.grp_filename_index = (gfi == 0) ? -1 : (gfi - 1);
			e.frame_index = (int)image.frame_index;
			e.offset_x    = image.offset.x;
			e.offset_y    = image.offset.y;
			e.flipped     = (image.flags
				& bwgame::image_t::flag_horizontally_flipped) != 0;
			e.hidden      = (image.flags
				& bwgame::image_t::flag_hidden) != 0;
			e.is_shadow   = (image.modifier == 10);
			e.image_id    = (int)image.image_type->id;
			if (image.grp) {
				e.grp_width  = (int)image.grp->width;
				e.grp_height = (int)image.grp->height;
			}
			out.push_back(e);
		}
	};

	push_sprite(unit->sprite);
	// Turret: only splice the subunit if it IS a turret (flag_turret
	// set in unit.dat). Non-turret subunits (rare -- Terran Larva
	// building subunit, etc.) shouldn't be rendered on top of the
	// parent.
	if (unit->subunit && ui->ut_turret(unit->subunit)) {
		push_sprite(unit->subunit->sprite);
	}
	return out;
}

bool SimHarness::current_flipped() const {
	// Reads image_t::flag_horizontally_flipped, set by openBW's
	// set_image_heading() when direction wraps past facing 16
	// (bwgame.h:13286-13288). Encapsulated here so viewer_window
	// stays clear of bwgame.h internals.
	if (!ui || !unit_is_alive_for_render(unit)) return false;
	auto* image = unit->sprite->main_image;
	if (!image) return false;
	return (image->flags & bwgame::image_t::flag_horizontally_flipped) != 0;
}

int SimHarness::current_grp_filename_index() const {
	// Chain: image_type -> grp_filename_index -> position in
	// arr/images.tbl. Two subtleties:
	//
	// (a) openBW stores grp_filename_index as a 1-based index --
	//     the retail images.dat / images.tbl convention where
	//     index 0 means "no GRP" (bwgame.h:22228 does an explicit
	//     `if (!index) return 0` and then subtracts 1 when
	//     seeking into the .tbl offset table). Callers such as the
	//     HD Mapping tab iterate the .tbl as a 0-based array of
	//     names, so we convert to that convention here by
	//     subtracting 1.
	//
	// (b) A stored value of 0 means "no GRP" -- return -1 so
	//     callers fall through to the classic render path.
	if (!ui || !unit_is_alive_for_render(unit)) return -1;
	auto* image = unit->sprite->main_image;
	if (!image || !image->image_type) return -1;
	int idx = (int)image->image_type->grp_filename_index;
	if (idx == 0) return -1;
	return idx - 1;   // 1-based -> 0-based
}

}   // namespace sprite_viewer
