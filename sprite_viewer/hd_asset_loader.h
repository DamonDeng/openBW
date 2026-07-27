// hd_asset_loader.h: SC:R HD asset loader for sprite_viewer.
//
// Given a path to a user's StarCraft: Remastered install, opens the
// CASC storage, resolves an openBW/SC:R image_id to its main_NNN.anim
// file, parses the ANIM structure (see tools/anim_dump.cpp for the
// full layout spec), decodes the diffuse layer's DXT5-compressed
// DDS atlas to a plain RGBA QImage, and lets callers slice per-frame
// crops out.
//
// This header keeps the Qt integration light -- HdAssetLoader owns
// the CASC handle for its lifetime; a per-unit call returns an
// HdSprite bundle that fully describes the sprite (atlas as QImage,
// frame table, offsets). The viewer's paint code reads only from the
// returned HdSprite, not from CascLib or ANIM bytes directly.
//
// Not linked into simsc_app, sync.h, or any server code. Purely
// client-side / development. See docs/wasm_observer_deprecated.md
// for the rationale behind reading a user's own SC:R install rather
// than shipping assets.

#pragma once

#include <QtCore/QString>
#include <QtGui/QImage>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sprite_viewer {

// One frame's atlas rect + drawing offset. Coordinates are in atlas
// pixels; the caller (paint code) subtracts (offset_x, offset_y)
// from the unit's world position to place the frame on screen.
struct HdFrame {
	uint16_t atlas_x = 0;
	uint16_t atlas_y = 0;
	uint16_t offset_x = 0;
	uint16_t offset_y = 0;
	uint16_t w = 0;
	uint16_t h = 0;
	uint32_t flags = 0;
};

// One unit's HD sprite: the decoded diffuse atlas + per-frame table.
// Optionally holds a pre-composited "diffuse + teamcolor * player
// color" atlas baked at load time (option A in the design
// discussion). We render `composited` when it's non-null; falls
// back to `diffuse` alone otherwise.
struct HdSprite {
	QImage diffuse;             // raw diffuse layer, kept for
	                             // reference / debugging.
	QImage composited;          // diffuse + teamcolor*playercolor,
	                             // premultiplied; drawn as-is.
	uint16_t sprite_w = 0;      // sprite bounding box (max frame size)
	uint16_t sprite_h = 0;
	std::vector<HdFrame> frames;
};

// One row in images.rel that has an HD anim attached. Used by the
// HD Mapping tab to enumerate every candidate unit sprite so the
// user can eyeball them against the openBW unit names.
struct HdRowInfo {
	int sc_r_row = -1;           // row index into images.rel (0..998)
	uint32_t flag = 0;            // 8=HD, 4=HD2, 16=Carbot, ...
	uint32_t anim_num = 0;        // main_<anim_num>.anim
	uint16_t atlas_w = 0;
	uint16_t atlas_h = 0;
	uint16_t sprite_w = 0;
	uint16_t sprite_h = 0;
	int frame_count = 0;
	// Two small previews shown side-by-side in the mapping tab.
	// Frame 0 is usually the canonical standing / init pose;
	// mid-anim frame (frame 8, or the last one if fewer) adds
	// a visual delta (walking / firing / working) that makes
	// same-role-different-unit sprites easier to tell apart.
	// Each is letterboxed into 128x128 RGBA8888.
	QImage frame0_preview;
	QImage frame_mid_preview;
	int    frame_mid_index = 0;   // which frame_mid_preview shows
};

class HdAssetLoader {
public:
	HdAssetLoader();
	~HdAssetLoader();

	HdAssetLoader(const HdAssetLoader&) = delete;
	HdAssetLoader& operator=(const HdAssetLoader&) = delete;

	// Open the CASC storage at `sc_remastered_path` (the folder
	// containing Data/, Maps/, x86_64/, ...). Also loads images.rel
	// into memory. Returns false + a message via last_error() on
	// failure. Safe to call multiple times; subsequent calls close
	// the previous handle first.
	bool open(const QString& sc_remastered_path);

	// True after a successful open().
	bool is_open() const;

	// Human-readable error from the most recent open() or load()
	// call. Empty when everything succeeded.
	QString last_error() const { return err_; }

	// Load one unit's HD sprite. `sc_image_id` is the row index into
	// images.rel (SC:R's own 999-slot table). Returns an owning
	// HdSprite via unique_ptr on success. nullptr + last_error() on
	// failure (image_id has no HD anim, .anim malformed, DXT5 decode
	// fails, ...). Callers should cache the returned sprite by
	// sc_image_id -- decoding a 2048x560 atlas costs a few ms and
	// several MB of RAM per unit.
	std::unique_ptr<HdSprite> load_sprite(int sc_image_id);

	// Enumerate every images.rel row with the given variant flag,
	// decoding just enough of each entry's .anim to fill an
	// HdRowInfo (metadata + a 128x128 preview crop of frame 0's
	// diffuse). Skips rows that fail to load. Total memory footprint
	// is roughly rows * 128*128*4 = a few MB even for the full
	// ~230 flag=8 set. Progress callback fires per row so the
	// caller can drive a progress bar without blocking Qt too long.
	// Cheap enough for the HD Mapping tab to call once at startup.
	std::vector<HdRowInfo> enumerate_rows(
		uint32_t flag_filter,
		const std::function<void(int done, int total)>&
			progress = nullptr);

	// Read arr/images.tbl (openBW image_id -> path string, e.g.
	// "terran\\marine.grp"). Used by the HD Mapping tab as the
	// authoritative vocabulary of unit / building / addon names.
	// Empty vector + last_error() on failure.
	std::vector<QString> read_images_tbl();

	// Load a runtime bw_id -> sc_r_row lookup table produced by
	// tools/validate_hd_mapping.py (schema
	// "openbw_hd_mapping_v1"). Returns true iff the file parses
	// and the schema tag matches. Missing file is NOT an error --
	// callers use has_mapping_table() to gate the HD render path.
	bool load_mapping_table(const QString& path);
	bool has_mapping_table() const;

	// Look up a bw_id (openBW image_type_t::id). Returns the
	// SC:R images.rel row that render_sprite() should use, or -1
	// if no HD mapping is known for that bw_id.
	int sc_r_row_for_bw_id(int bw_id) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	QString err_;
};

}   // namespace sprite_viewer
