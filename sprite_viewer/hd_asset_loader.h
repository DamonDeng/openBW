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
	// offset_x/offset_y are SIGNED int16 in Blizzard's anim format.
	// Overlay anims (SCV flame, Firebat spray, ...) routinely author
	// negative offsets to anchor above/left of the atlas rect. Reading
	// as uint16 turned -13 into 65523 and drew flames ~30000 widget
	// pixels off-screen. See rd16le() in the parser.
	int16_t offset_x = 0;
	int16_t offset_y = 0;
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

	// Loader debug helper: load the anim for `sc_image_id` and
	// decode a SPECIFIC layer by name (e.g. "diffuse", "ao_depth",
	// "bright"). Fills the diffuse field of the returned HdSprite
	// so the frames tab can render it uniformly. teamcolor layer is
	// NOT composited in -- we want to see the raw layer. Returns
	// null on any failure; last_error() has the details.
	std::unique_ptr<HdSprite> load_sprite_layer(int sc_image_id,
	                                             const std::string& layer_name);

	// List the layer names present in a given image_id's anim file.
	// Returns empty on failure; last_error() has details. Meant
	// for the frames browser's layer dropdown.
	std::vector<std::string> list_layers_for_image_id(int sc_image_id);

	// Anim-number variants of the two loader helpers above. Bypass
	// images.rel entirely -- open anim\main_<anim_num>.anim directly.
	// Useful for the frames browser when the user wants to inspect
	// an orphan anim (no images.rel row points at it, e.g. SCV's
	// shadow anim=248 or its engine-flame overlay anim=249) or a
	// row whose flag isn't in {8, 4} (Carbot=16, SD-only=1). If the
	// anim file doesn't exist in CASC, returns empty / null; the
	// error message names the anim path we looked at.
	std::vector<std::string> list_layers_for_anim(uint32_t anim_num);
	std::unique_ptr<HdSprite> load_sprite_layer_by_anim(
	    uint32_t anim_num, const std::string& layer_name);

	// Look up the first images.rel row whose anim_num equals
	// `anim_num` and whose flag is HD (8) or HD2 (4). Returns
	// -1 if no such row exists. Used by the frames-browser tab
	// to accept anim numbers as input (matching what shows up
	// in the anim filenames on disk) rather than requiring users
	// to know which SD image_id maps to that anim.
	int image_id_for_anim(uint32_t anim_num) const;

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

	// Open an HD tileset for the given index (matching ui.h's
	// tileset_names table: 0=badlands, 1=platform, 2=install,
	// 3=AshWorld, 4=Jungle, 5=Desert, 6=Ice, 7=Twilight). Loads
	// HD2/TileSet/<name>.dds.vr4 -- a container of 5711 (or
	// tileset-specific) 64x64 DXT1 megatile textures. Returns
	// false + last_error() on failure. Safe to call multiple
	// times; a new open replaces the prior tileset.
	bool open_hd_tileset(int tileset_index);
	bool has_hd_tileset() const;

	// Decode-on-demand: return a 64x64 RGBA QImage for the given
	// megatile index. Cached per index -- subsequent calls with
	// the same index return the same QImage. Empty (null) image
	// when the index is out of range or the decode fails.
	QImage hd_megatile(int megatile_index);

	// SC:R stores shadow sprites in their own main_NNN.anim files
	// (only diffuse layer populated -- no teamcolor / normal /
	// specular). They live adjacent to the body anim in anim_num
	// (typically body_anim +/- 1..3). openBW's SD renderer treats
	// each shadow as a separate image_t on the sprite, but the
	// mapping tab doesn't include shadow entries -- they were
	// visually indistinguishable in the eyeball workflow.
	//
	// This method infers a shadow HdSprite by scanning anim files
	// near the body anim for an only-diffuse layout, then decoding
	// that file the same way load_sprite() does the body. Cached
	// per body sc_r_row so repeated calls are cheap. Returns
	// nullptr when no plausible shadow anim exists.
	HdSprite* shadow_sprite_for(int body_sc_r_row);

public:
	// Public only so file-scope helpers inside hd_asset_loader.cpp
	// can dereference the fields. Not a stable API -- treat as
	// implementation-private for all external callers.
	struct Impl;
private:
	std::unique_ptr<Impl> impl_;
	QString err_;
};

}   // namespace sprite_viewer
