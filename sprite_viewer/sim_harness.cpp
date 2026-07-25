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

SimHarness::FramePixels SimHarness::render_frame() {
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
	size_t buf_size = pitch * (size_t)idx->h;
	// Palette entry 0 = transparent/black-ish on all tilesets.
	std::memset(data, 0, buf_size);

	if (unit_is_alive_for_render(unit)) {
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

}   // namespace sprite_viewer
