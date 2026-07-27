// See hd_asset_loader.h for the public API + rationale.

#include "hd_asset_loader.h"

#include <CascLib.h>

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <unordered_map>

namespace sprite_viewer {

namespace {

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;

static u16 rd16le(const u8* p) { return (u16)p[0] | ((u16)p[1] << 8); }
static u32 rd32le(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8)
		| ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

// ANIM layout (see tools/anim_dump.cpp for the reverse-engineered
// spec):
//   0x000  ANIM magic
//   0x004  u16 version
//   0x008  u16 layer_count
//   0x00C  10 * 32-byte layer name slots (fixed-size)
//   0x14C  u16 frame_count
//   0x150  u16 sprite_w, sprite_h
//   0x154  u32 frame_table_off
//   0x158  layer_count * 12-byte descriptors:
//            u32 data_offset (0 = layer absent)
//            u32 data_size
//            u16 atlas_w, u16 atlas_h
static constexpr int    ANIM_LAYER_SLOT_COUNT = 10;
static constexpr size_t ANIM_LAYER_NAME_SIZE  = 32;
static constexpr size_t ANIM_BODY_OFFSET =
	12 + ANIM_LAYER_SLOT_COUNT * ANIM_LAYER_NAME_SIZE;   // 0x14C

struct LayerInfo {
	std::string name;
	u32 data_offset = 0;
	u32 data_size   = 0;
	u16 atlas_w = 0;
	u16 atlas_h = 0;
};

struct ParsedAnim {
	u16 sprite_w = 0;
	u16 sprite_h = 0;
	u16 frame_count = 0;
	std::vector<LayerInfo> layers;
	std::vector<HdFrame> frames;
};

// Parse an .anim in memory. Returns false + error string on any
// structural issue; the caller propagates it up to last_error().
static bool parse_anim(const std::vector<u8>& bytes, ParsedAnim& out,
	std::string& err)
{
	if (bytes.size() < ANIM_BODY_OFFSET + 8) {
		err = ".anim file too small";
		return false;
	}
	if (rd32le(bytes.data()) != 0x4D494E41 /* "ANIM" */) {
		err = ".anim bad magic";
		return false;
	}
	u16 layer_count = rd16le(bytes.data() + 8);
	if (layer_count == 0 || layer_count > ANIM_LAYER_SLOT_COUNT) {
		err = ".anim implausible layer_count";
		return false;
	}
	// Layer names in 32-byte NUL-padded slots.
	std::vector<std::string> names;
	names.reserve(layer_count);
	for (int i = 0; i < layer_count; ++i) {
		const char* p = (const char*)&bytes[12 + i * ANIM_LAYER_NAME_SIZE];
		names.emplace_back(p);
	}
	// Body @ 0x14C.
	const u8* body = bytes.data() + ANIM_BODY_OFFSET;
	out.frame_count     = rd16le(body + 0);
	out.sprite_w        = rd16le(body + 4);
	out.sprite_h        = rd16le(body + 6);
	u32 frame_table_off = rd32le(body + 8);

	// Layer descriptors.
	size_t desc_off = ANIM_BODY_OFFSET + 12;
	for (int i = 0; i < layer_count; ++i) {
		if (desc_off + 12 > bytes.size()) {
			err = ".anim truncated layer descriptor";
			return false;
		}
		LayerInfo li;
		li.name        = names[i];
		li.data_offset = rd32le(bytes.data() + desc_off + 0);
		li.data_size   = rd32le(bytes.data() + desc_off + 4);
		li.atlas_w     = rd16le(bytes.data() + desc_off + 8);
		li.atlas_h     = rd16le(bytes.data() + desc_off + 10);
		out.layers.push_back(std::move(li));
		desc_off += 12;
	}

	// Frame table.
	if (frame_table_off == 0
	    || frame_table_off + (size_t)out.frame_count * 16 > bytes.size())
	{
		err = ".anim frame_table out of range";
		return false;
	}
	const u8* ft = bytes.data() + frame_table_off;
	out.frames.reserve(out.frame_count);
	for (int i = 0; i < out.frame_count; ++i) {
		HdFrame f;
		const u8* p = ft + i * 16;
		f.atlas_x  = rd16le(p + 0);
		f.atlas_y  = rd16le(p + 2);
		f.offset_x = rd16le(p + 4);
		f.offset_y = rd16le(p + 6);
		f.w        = rd16le(p + 8);
		f.h        = rd16le(p + 10);
		f.flags    = rd32le(p + 12);
		out.frames.push_back(f);
	}
	return true;
}

// ---- DXT5 / BC3 decoder ---------------------------------------------
//
// DXT5 encodes a texture as 4x4 pixel blocks. Each block is 16 bytes:
//   [8 bytes alpha]  BC4/DXT5 alpha:
//     [u8 a0] [u8 a1]  endpoint alphas
//     [u48 index]      3 bits per pixel, 16 pixels
//   [8 bytes color]  BC1/DXT1-style color:
//     [u16 c0] [u16 c1]  endpoint colors, RGB565
//     [u32 index]        2 bits per pixel, 16 pixels
//
// Alpha lookup table depends on a0 vs a1:
//   a0>a1 : linear interp between a0 and a1 across 8 values
//   else  : 6 interp values + explicit 0 and 255
//
// Color lookup: c0/c1 unpack to RGB888; 4 colors per block.
// We ignore the c0>c1 branch (which encodes 1-bit alpha in DXT1);
// DXT5 always uses the 4-color branch since alpha lives elsewhere.

static inline u8 clamp8(int v) {
	return (u8)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void unpack_rgb565(u16 c, u8& r, u8& g, u8& b) {
	// Expand 5/6/5 -> 8/8/8 via the classic top-bits replication trick
	// (matches what GPU samplers produce for compressed textures).
	int r5 = (c >> 11) & 0x1f;
	int g6 = (c >> 5)  & 0x3f;
	int b5 =  c        & 0x1f;
	r = (u8)((r5 << 3) | (r5 >> 2));
	g = (u8)((g6 << 2) | (g6 >> 4));
	b = (u8)((b5 << 3) | (b5 >> 2));
}

// Decode one 4x4 pixel block into `out_rgba` (16*4 bytes) laid out
// row-major within the block.
static void decode_bc3_block(const u8* src, u8* out_rgba)
{
	// --- alpha (BC4) ---
	u8 a0 = src[0], a1 = src[1];
	u8 alpha_lut[8];
	alpha_lut[0] = a0;
	alpha_lut[1] = a1;
	if (a0 > a1) {
		alpha_lut[2] = (u8)((6 * a0 + 1 * a1) / 7);
		alpha_lut[3] = (u8)((5 * a0 + 2 * a1) / 7);
		alpha_lut[4] = (u8)((4 * a0 + 3 * a1) / 7);
		alpha_lut[5] = (u8)((3 * a0 + 4 * a1) / 7);
		alpha_lut[6] = (u8)((2 * a0 + 5 * a1) / 7);
		alpha_lut[7] = (u8)((1 * a0 + 6 * a1) / 7);
	} else {
		alpha_lut[2] = (u8)((4 * a0 + 1 * a1) / 5);
		alpha_lut[3] = (u8)((3 * a0 + 2 * a1) / 5);
		alpha_lut[4] = (u8)((2 * a0 + 3 * a1) / 5);
		alpha_lut[5] = (u8)((1 * a0 + 4 * a1) / 5);
		alpha_lut[6] = 0;
		alpha_lut[7] = 255;
	}
	// 48-bit alpha index: bytes 2..7, little-endian.
	uint64_t aidx = 0;
	for (int i = 0; i < 6; ++i) {
		aidx |= (uint64_t)src[2 + i] << (i * 8);
	}

	// --- color (BC1-style, 4-color branch) ---
	u16 c0 = (u16)src[8]  | ((u16)src[9]  << 8);
	u16 c1 = (u16)src[10] | ((u16)src[11] << 8);
	u8 r0, g0, b0, r1, g1, b1;
	unpack_rgb565(c0, r0, g0, b0);
	unpack_rgb565(c1, r1, g1, b1);
	u8 rgb[4][3];
	rgb[0][0] = r0; rgb[0][1] = g0; rgb[0][2] = b0;
	rgb[1][0] = r1; rgb[1][1] = g1; rgb[1][2] = b1;
	// DXT5 always uses the 4-color interpolation branch. c0/c1
	// ordering doesn't matter here -- some encoders emit c0<=c1
	// and it's still valid in the DXT5 context because the
	// 1-bit-alpha DXT1 mode does not apply.
	rgb[2][0] = (u8)((2 * r0 + r1) / 3);
	rgb[2][1] = (u8)((2 * g0 + g1) / 3);
	rgb[2][2] = (u8)((2 * b0 + b1) / 3);
	rgb[3][0] = (u8)((r0 + 2 * r1) / 3);
	rgb[3][1] = (u8)((g0 + 2 * g1) / 3);
	rgb[3][2] = (u8)((b0 + 2 * b1) / 3);
	u32 cidx = (u32)src[12]
	         | ((u32)src[13] << 8)
	         | ((u32)src[14] << 16)
	         | ((u32)src[15] << 24);

	// Emit 16 pixels row-major.
	for (int i = 0; i < 16; ++i) {
		int ci = (cidx >> (i * 2)) & 0x3;
		int ai = (int)((aidx >> (i * 3)) & 0x7);
		u8* o = &out_rgba[i * 4];
		o[0] = rgb[ci][0];   // R
		o[1] = rgb[ci][1];   // G
		o[2] = rgb[ci][2];   // B
		o[3] = alpha_lut[ai];// A
	}
}

// Decode one 4x4 BC1 (DXT1) block into out_rgba (16*4 bytes).
// BC1 is the color-only half of BC3: two RGB565 endpoints followed
// by a 32-bit index table (2 bits per pixel). Used by SC:R's
// teamcolor layer where alpha isn't needed -- the mask is
// grayscale so we treat the color output as a luminance signal.
//
// SC:R's teamcolor DDS is guaranteed to use the 4-color branch
// (c0 > c1). Even if a block encodes the c0 <= c1 branch (which
// would produce an explicit-black-color-3 variant), the max of
// (r,g,b) still represents the intended mask strength, which is
// what the compositor needs.
static void decode_bc1_block(const u8* src, u8* out_rgba)
{
	u16 c0 = (u16)src[0] | ((u16)src[1] << 8);
	u16 c1 = (u16)src[2] | ((u16)src[3] << 8);
	u8 r0, g0, b0, r1, g1, b1;
	unpack_rgb565(c0, r0, g0, b0);
	unpack_rgb565(c1, r1, g1, b1);
	u8 rgb[4][3];
	rgb[0][0] = r0; rgb[0][1] = g0; rgb[0][2] = b0;
	rgb[1][0] = r1; rgb[1][1] = g1; rgb[1][2] = b1;
	if (c0 > c1) {
		rgb[2][0] = (u8)((2 * r0 + r1) / 3);
		rgb[2][1] = (u8)((2 * g0 + g1) / 3);
		rgb[2][2] = (u8)((2 * b0 + b1) / 3);
		rgb[3][0] = (u8)((r0 + 2 * r1) / 3);
		rgb[3][1] = (u8)((g0 + 2 * g1) / 3);
		rgb[3][2] = (u8)((b0 + 2 * b1) / 3);
	} else {
		rgb[2][0] = (u8)((r0 + r1) / 2);
		rgb[2][1] = (u8)((g0 + g1) / 2);
		rgb[2][2] = (u8)((b0 + b1) / 2);
		rgb[3][0] = 0; rgb[3][1] = 0; rgb[3][2] = 0;
	}
	u32 cidx = (u32)src[4]
		| ((u32)src[5] << 8)
		| ((u32)src[6] << 16)
		| ((u32)src[7] << 24);
	for (int i = 0; i < 16; ++i) {
		int ci = (cidx >> (i * 2)) & 0x3;
		u8* o = &out_rgba[i * 4];
		o[0] = rgb[ci][0];
		o[1] = rgb[ci][1];
		o[2] = rgb[ci][2];
		o[3] = 255;   // BC1 has no alpha
	}
}

// Decode a full BC1 mip level into a QImage. Same layout as the BC3
// decoder below but 8 bytes/block instead of 16. Used for teamcolor
// masks.
static QImage decode_bc1_dds(const u8* dds_bytes, size_t dds_size,
	u16 atlas_w, u16 atlas_h, std::string& err)
{
	if (dds_size < 128) { err = "DDS: too small"; return {}; }
	if (dds_bytes[0] != 'D' || dds_bytes[1] != 'D'
	    || dds_bytes[2] != 'S' || dds_bytes[3] != ' ')
	{ err = "DDS: bad magic"; return {}; }
	u32 hdr_h = rd32le(dds_bytes + 12);
	u32 hdr_w = rd32le(dds_bytes + 16);
	if (hdr_w != atlas_w || hdr_h != atlas_h) {
		char buf[256];
		snprintf(buf, sizeof(buf),
			"DDS dims %ux%u disagree with layer %ux%u",
			hdr_w, hdr_h, atlas_w, atlas_h);
		err = buf;
		return {};
	}
	size_t blocks_x = (atlas_w + 3) / 4;
	size_t blocks_y = (atlas_h + 3) / 4;
	size_t need = 128 + blocks_x * blocks_y * 8;
	if (dds_size < need) {
		err = "DDS: payload too small for declared dims";
		return {};
	}
	const u8* payload = dds_bytes + 128;
	QImage img(atlas_w, atlas_h, QImage::Format_RGBA8888);
	if (img.isNull()) { err = "QImage alloc failed"; return {}; }
	u8 block_rgba[64];
	for (size_t by = 0; by < blocks_y; ++by) {
		for (size_t bx = 0; bx < blocks_x; ++bx) {
			const u8* blk = payload + (by * blocks_x + bx) * 8;
			decode_bc1_block(blk, block_rgba);
			for (int py = 0; py < 4; ++py) {
				int y = (int)(by * 4) + py;
				if (y >= atlas_h) continue;
				u8* row = img.scanLine(y);
				for (int px = 0; px < 4; ++px) {
					int x = (int)(bx * 4) + px;
					if (x >= atlas_w) continue;
					const u8* src = &block_rgba[(py * 4 + px) * 4];
					u8* dst = row + x * 4;
					dst[0] = src[0];
					dst[1] = src[1];
					dst[2] = src[2];
					dst[3] = src[3];
				}
			}
		}
	}
	return img;
}

// Decode a full DXT5 mip level into a QImage (Format_RGBA8888).
// dds_bytes points at the start of the DDS file (magic 'DDS ');
// we skip the 128-byte header and decode the first mip only --
// SC:R atlases don't need mips for our purposes.
static QImage decode_dxt5_dds(const u8* dds_bytes, size_t dds_size,
	u16 atlas_w, u16 atlas_h, std::string& err)
{
	// Minimum: 128-byte DDS header + 4x4 block bytes for the
	// smallest possible atlas. Real atlases are 2048x560, so
	// content dwarfs the header. Just guard against corruption.
	if (dds_size < 128) {
		err = "DDS: too small";
		return {};
	}
	if (dds_bytes[0] != 'D' || dds_bytes[1] != 'D'
	    || dds_bytes[2] != 'S' || dds_bytes[3] != ' ')
	{
		err = "DDS: bad magic";
		return {};
	}
	// dwHeight @ 0x0C, dwWidth @ 0x10 in DDS_HEADER (right after
	// the 4-byte magic). Sanity-check against the layer info.
	u32 hdr_h = rd32le(dds_bytes + 12);
	u32 hdr_w = rd32le(dds_bytes + 16);
	if (hdr_w != atlas_w || hdr_h != atlas_h) {
		char buf[256];
		snprintf(buf, sizeof(buf),
			"DDS dims %ux%u disagree with layer %ux%u",
			hdr_w, hdr_h, atlas_w, atlas_h);
		err = buf;
		return {};
	}
	// DXT5 uses 16 bytes per 4x4 block. Round up to block grid.
	size_t blocks_x = (atlas_w + 3) / 4;
	size_t blocks_y = (atlas_h + 3) / 4;
	size_t need = 128 + blocks_x * blocks_y * 16;
	if (dds_size < need) {
		err = "DDS: payload too small for declared dims";
		return {};
	}
	const u8* payload = dds_bytes + 128;

	QImage img(atlas_w, atlas_h, QImage::Format_RGBA8888);
	if (img.isNull()) {
		err = "QImage alloc failed";
		return {};
	}
	u8 block_rgba[64];   // 4x4 pixels * 4 bytes = 64
	for (size_t by = 0; by < blocks_y; ++by) {
		for (size_t bx = 0; bx < blocks_x; ++bx) {
			const u8* blk =
				payload + (by * blocks_x + bx) * 16;
			decode_bc3_block(blk, block_rgba);
			// Write pixels back into img, clipping at edges when
			// atlas dims aren't multiples of 4.
			for (int py = 0; py < 4; ++py) {
				int y = (int)(by * 4) + py;
				if (y >= atlas_h) continue;
				u8* row = img.scanLine(y);
				for (int px = 0; px < 4; ++px) {
					int x = (int)(bx * 4) + px;
					if (x >= atlas_w) continue;
					const u8* src = &block_rgba[(py * 4 + px) * 4];
					u8* dst = row + x * 4;
					dst[0] = src[0];
					dst[1] = src[1];
					dst[2] = src[2];
					dst[3] = src[3];
				}
			}
		}
	}
	return img;
}

// images.rel entry -- 8 bytes: [u32 variant_flag][u32 anim_num].
// variant_flag values seen: 1=empty, 4=HD2 half-res, 8=HD full-res,
// 16=Carbot, 512=other. We handle 8 first (full-res HD) with a
// fallback to 4 (HD2) so a unit with no HD but a valid HD2 anim
// still renders at reduced fidelity.
struct ImagesRelEntry {
	u32 flag;
	u32 anim_num;
};

}   // namespace

// ---- Impl -----------------------------------------------------------

struct HdAssetLoader::Impl {
	HANDLE storage = NULL;
	std::vector<ImagesRelEntry> images_rel;
	// bw_id -> sc_r_row, populated by load_mapping_table(). Empty
	// map means no HD mapping known -> HD render path is a no-op.
	std::unordered_map<int, int> bw_id_to_sc_r;
	bool have_table = false;
};

HdAssetLoader::HdAssetLoader()
	: impl_(std::make_unique<Impl>()) {}

HdAssetLoader::~HdAssetLoader() {
	if (impl_ && impl_->storage) CascCloseStorage(impl_->storage);
}

bool HdAssetLoader::is_open() const {
	return impl_ && impl_->storage != NULL;
}

// Small helper: read one CASC file into an owned byte vector.
static bool casc_read_file(HANDLE h, const char* name,
	std::vector<u8>& out, std::string& err)
{
	HANDLE hf = NULL;
	if (!CascOpenFile(h, name, 0, CASC_OPEN_BY_NAME, &hf)) {
		char buf[256];
		snprintf(buf, sizeof(buf),
			"CascOpenFile(%s) err=%u", name, GetCascError());
		err = buf;
		return false;
	}
	DWORD sz = CascGetFileSize(hf, NULL);
	if (sz == CASC_INVALID_SIZE) {
		err = "CascGetFileSize failed";
		CascCloseFile(hf);
		return false;
	}
	out.resize(sz);
	DWORD got = 0;
	if (!CascReadFile(hf, out.data(), sz, &got) || got != sz) {
		err = "CascReadFile short";
		CascCloseFile(hf);
		return false;
	}
	CascCloseFile(hf);
	return true;
}

bool HdAssetLoader::open(const QString& sc_remastered_path) {
	if (impl_->storage) {
		CascCloseStorage(impl_->storage);
		impl_->storage = NULL;
	}
	impl_->images_rel.clear();
	err_.clear();

	std::string path = sc_remastered_path.toStdString();
	if (!CascOpenStorage(path.c_str(), 0, &impl_->storage)) {
		err_ = QString("CascOpenStorage(%1) failed err=%2")
			.arg(sc_remastered_path).arg(GetCascError());
		impl_->storage = NULL;
		return false;
	}

	// Load images.rel once and keep it in memory. It's 8 KB.
	std::vector<u8> bytes;
	std::string ce;
	if (!casc_read_file(impl_->storage, "images.rel", bytes, ce)) {
		err_ = QString::fromStdString("load images.rel: " + ce);
		CascCloseStorage(impl_->storage);
		impl_->storage = NULL;
		return false;
	}
	if (bytes.size() % 8 != 0) {
		err_ = "images.rel size not a multiple of 8";
		return false;
	}
	size_t n = bytes.size() / 8;
	impl_->images_rel.resize(n);
	std::memcpy(impl_->images_rel.data(), bytes.data(), bytes.size());
	return true;
}

// Extract a preview thumbnail for one images.rel row: decode the
// anim's diffuse layer just enough to crop frame 0, then scale to
// 128x128 for the mapping tab. Returns a fully-composed HdRowInfo
// on success. Sets err on failure so the caller can skip cleanly.
static bool make_row_preview(HANDLE storage, int row,
	uint32_t flag, uint32_t anim_num,
	HdRowInfo& out, std::string& err)
{
	char name[64];
	snprintf(name, sizeof(name), "anim\\main_%03u.anim", anim_num);
	std::vector<u8> bytes;
	if (!casc_read_file(storage, name, bytes, err)) return false;

	ParsedAnim pa;
	if (!parse_anim(bytes, pa, err)) return false;
	if (pa.frames.empty()) { err = "no frames"; return false; }

	const LayerInfo* diff = nullptr;
	for (const auto& li : pa.layers) {
		if (li.name == "diffuse") { diff = &li; break; }
	}
	if (!diff || diff->data_offset == 0
	    || diff->data_offset + diff->data_size > bytes.size())
	{
		err = "diffuse missing";
		return false;
	}
	// Decode the full atlas (2048x560 is typical). This is the
	// costly bit; ~5-15 ms per row, times ~230 rows = a few
	// seconds. We drop the atlas after cropping frame 0 -- see
	// HdRowInfo docs for the memory-savings rationale.
	QImage atlas = decode_dxt5_dds(
		bytes.data() + diff->data_offset,
		diff->data_size, diff->atlas_w, diff->atlas_h, err);
	if (atlas.isNull()) return false;

	// Local helper: crop one frame from the decoded atlas, letterbox
	// into a 128x128 RGBA image. Returns an empty QImage + writes err
	// if the frame rect is out of atlas bounds.
	auto make_preview = [&](int frame_idx,
	                        std::string& sub_err) -> QImage {
		if (frame_idx < 0 || frame_idx >= (int)pa.frames.size()) {
			sub_err = "frame index OOB";
			return {};
		}
		const HdFrame& fp = pa.frames[frame_idx];
		if (fp.atlas_x + fp.w > diff->atlas_w
		    || fp.atlas_y + fp.h > diff->atlas_h)
		{
			sub_err = "frame out of atlas";
			return {};
		}
		QImage crop = atlas.copy(fp.atlas_x, fp.atlas_y, fp.w, fp.h);
		int side = std::max<int>(crop.width(), crop.height());
		QImage p(128, 128, QImage::Format_RGBA8888);
		p.fill(0x00000000);
		QImage scaled = (side > 128)
			? crop.scaled(128, 128, Qt::KeepAspectRatio,
				Qt::SmoothTransformation)
			: crop;
		int dx = (128 - scaled.width())  / 2;
		int dy = (128 - scaled.height()) / 2;
		for (int y = 0; y < scaled.height(); ++y) {
			const uint8_t* src = scaled.scanLine(y);
			uint8_t* dst = p.scanLine(dy + y) + dx * 4;
			std::memcpy(dst, src, scaled.width() * 4);
		}
		return p;
	};

	// Frame 0: canonical pose. Always present.
	std::string sub_err;
	QImage frame0 = make_preview(0, sub_err);
	if (frame0.isNull()) { err = "frame 0: " + sub_err; return false; }

	// Frame mid: 8 if available, else the last frame. If the sprite
	// has only 1 frame, both previews will be identical -- that's
	// fine; the user just sees the same image twice, still informative.
	int mid_idx = (pa.frame_count > 8) ? 8 : (pa.frame_count - 1);
	if (mid_idx < 0) mid_idx = 0;
	QImage frame_mid = make_preview(mid_idx, sub_err);
	if (frame_mid.isNull()) {
		// Non-fatal: still ship frame 0 alone if the mid frame
		// somehow fails (shouldn't -- same atlas).
		frame_mid = frame0;
		mid_idx = 0;
	}

	out.sc_r_row          = row;
	out.flag              = flag;
	out.anim_num          = anim_num;
	out.atlas_w           = diff->atlas_w;
	out.atlas_h           = diff->atlas_h;
	out.sprite_w          = pa.sprite_w;
	out.sprite_h          = pa.sprite_h;
	out.frame_count       = pa.frame_count;
	out.frame0_preview    = std::move(frame0);
	out.frame_mid_preview = std::move(frame_mid);
	out.frame_mid_index   = mid_idx;
	return true;
}

std::vector<QString> HdAssetLoader::read_images_tbl() {
	std::vector<QString> out;
	if (!is_open()) { err_ = "loader not open"; return out; }
	std::vector<u8> bytes;
	std::string ce;
	if (!casc_read_file(impl_->storage, "arr\\images.tbl", bytes, ce)) {
		err_ = QString::fromStdString("read images.tbl: " + ce);
		return out;
	}
	// Format: [u16 count][u16 offset]*count [NUL-terminated strings].
	// Offsets are file-relative. Strings are Latin-1 filenames like
	// "terran\\marine.grp".
	if (bytes.size() < 2) return out;
	u16 count = rd16le(bytes.data());
	if ((size_t)2 + count * 2 > bytes.size()) {
		err_ = "images.tbl truncated offset table";
		return out;
	}
	out.reserve(count);
	for (int i = 0; i < count; ++i) {
		u16 off = rd16le(bytes.data() + 2 + i * 2);
		if (off >= bytes.size()) {
			out.emplace_back();
			continue;
		}
		size_t end = off;
		while (end < bytes.size() && bytes[end] != 0) ++end;
		out.emplace_back(QString::fromLatin1(
			(const char*)bytes.data() + off, (int)(end - off)));
	}
	return out;
}

std::vector<HdRowInfo> HdAssetLoader::enumerate_rows(
	uint32_t flag_filter,
	const std::function<void(int done, int total)>& progress)
{
	std::vector<HdRowInfo> out;
	if (!is_open()) {
		err_ = "loader not open";
		return out;
	}
	// First pass: collect the rows that match the filter so we can
	// report progress against a real total.
	std::vector<std::pair<int, uint32_t>> targets;  // (row, anim_num)
	for (size_t i = 0; i < impl_->images_rel.size(); ++i) {
		const auto& e = impl_->images_rel[i];
		if (e.flag == flag_filter && e.anim_num != 0xffffffff) {
			targets.push_back({(int)i, e.anim_num});
		}
	}
	int total = (int)targets.size();
	out.reserve(total);
	for (int k = 0; k < total; ++k) {
		HdRowInfo row;
		std::string perr;
		if (make_row_preview(impl_->storage, targets[k].first,
		    flag_filter, targets[k].second, row, perr))
		{
			out.push_back(std::move(row));
		} else {
			// Non-fatal: some anim files reference textures Blizzard
			// no longer ships in current CASC. Silently skip.
		}
		if (progress) progress(k + 1, total);
	}
	return out;
}

std::unique_ptr<HdSprite> HdAssetLoader::load_sprite(int sc_image_id) {
	if (!is_open()) {
		err_ = "loader not open";
		return nullptr;
	}
	if (sc_image_id < 0
	    || (size_t)sc_image_id >= impl_->images_rel.size()) {
		err_ = QString("image_id %1 out of range").arg(sc_image_id);
		return nullptr;
	}
	const auto& e = impl_->images_rel[sc_image_id];
	// Prefer full-res HD (flag=8). Fall back to HD2 (flag=4) if
	// the row we hit isn't HD. If neither, bail -- caller can
	// probe adjacent image_ids or fall back to SD.
	u32 anim_num = 0;
	bool got = false;
	if ((e.flag == 8 || e.flag == 4) && e.anim_num != 0xffffffff) {
		anim_num = e.anim_num;
		got = true;
	}
	if (!got) {
		err_ = QString("image_id %1 has no HD anim "
			"(flag=%2, anim_num=0x%3)")
			.arg(sc_image_id).arg(e.flag)
			.arg(e.anim_num, 8, 16, QChar('0'));
		return nullptr;
	}

	// Load anim\main_NNN.anim.
	char name[64];
	snprintf(name, sizeof(name), "anim\\main_%03u.anim", anim_num);
	std::vector<u8> anim_bytes;
	std::string ce;
	if (!casc_read_file(impl_->storage, name, anim_bytes, ce)) {
		err_ = QString::fromStdString("read " + std::string(name)
			+ ": " + ce);
		return nullptr;
	}

	ParsedAnim pa;
	std::string perr;
	if (!parse_anim(anim_bytes, pa, perr)) {
		err_ = QString::fromStdString("parse " + std::string(name)
			+ ": " + perr);
		return nullptr;
	}

	// Find the diffuse layer.
	const LayerInfo* diff = nullptr;
	for (const auto& li : pa.layers) {
		if (li.name == "diffuse") { diff = &li; break; }
	}
	if (!diff || diff->data_offset == 0) {
		err_ = "diffuse layer absent";
		return nullptr;
	}
	if (diff->data_offset + diff->data_size > anim_bytes.size()) {
		err_ = "diffuse layer range OOB";
		return nullptr;
	}

	// Decode DXT5 -> RGBA.
	QImage atlas = decode_dxt5_dds(
		anim_bytes.data() + diff->data_offset,
		diff->data_size,
		diff->atlas_w, diff->atlas_h, perr);
	if (atlas.isNull()) {
		err_ = QString::fromStdString("DXT5 decode: " + perr);
		return nullptr;
	}

	// Optional teamcolor layer (DXT1 grayscale mask). SC:R's shader
	// adds "teamcolor.rgb * playerColor" on top of the diffuse to
	// pull dark diffuse regions toward the player's color. Blizzard
	// authors diffuse dark on purpose expecting this pass -- if we
	// skip it, HD units look drained of color (the "dark Marine"
	// problem we saw before wiring this in).
	//
	// Hardcoded player color: red (Player 1 default). Matches
	// openBW's slot-0 palette convention. Will be lifted to an
	// argument once we care about non-red owners.
	static constexpr uint8_t PLAYER_RED[3]   = { 224,   0,   0 };
	static constexpr uint8_t PLAYER_COLOR[3] = { PLAYER_RED[0],
	                                             PLAYER_RED[1],
	                                             PLAYER_RED[2] };

	QImage composited = atlas;   // starts as a copy of diffuse
	const LayerInfo* tc = nullptr;
	for (const auto& li : pa.layers) {
		if (li.name == "teamcolor") { tc = &li; break; }
	}
	if (tc && tc->data_offset != 0
	    && tc->data_offset + tc->data_size <= anim_bytes.size())
	{
		QImage tc_atlas = decode_bc1_dds(
			anim_bytes.data() + tc->data_offset,
			tc->data_size,
			tc->atlas_w, tc->atlas_h, perr);
		if (tc_atlas.isNull()) {
			// Non-fatal: unit gets un-tinted diffuse.
			fprintf(stderr,
				"warn: teamcolor decode failed for %s: %s\n",
				name, perr.c_str());
		} else if (tc_atlas.width() == composited.width()
		           && tc_atlas.height() == composited.height())
		{
			// SC:R teamcolor blend is a modulate, not an addition:
			//   tint_factor = mix(vec3(1), player_color/255, mask)
			//   final.rgb   = diffuse.rgb * tint_factor
			//
			// Additive was wrong -- it saturated highlights toward
			// white ("pink Marine" symptom the user hit). Modulate
			// preserves the diffuse shading and tints only what the
			// mask marks: mask=0 leaves diffuse alone, mask=1 makes
			// a white armor plate become pure player color with the
			// original shading multiplied in.
			int H = composited.height();
			int W = composited.width();
			for (int y = 0; y < H; ++y) {
				uint8_t* dst = composited.scanLine(y);
				const uint8_t* mask = tc_atlas.scanLine(y);
				for (int x = 0; x < W; ++x) {
					int g = mask[x * 4];   // R channel = mask
					if (g == 0) continue;
					uint8_t* p = dst + x * 4;
					// Per-channel modulate factor in fixed-point
					// [0..255]: at mask=0 factor=255 (no change),
					// at mask=255 factor=PLAYER_COLOR[c].
					int fr = 255 + (g * ((int)PLAYER_COLOR[0] - 255)) / 255;
					int fg = 255 + (g * ((int)PLAYER_COLOR[1] - 255)) / 255;
					int fb = 255 + (g * ((int)PLAYER_COLOR[2] - 255)) / 255;
					int r = (p[0] * fr) / 255;
					int gg = (p[1] * fg) / 255;
					int b = (p[2] * fb) / 255;
					p[0] = (uint8_t)(r > 255 ? 255 : r);
					p[1] = (uint8_t)(gg > 255 ? 255 : gg);
					p[2] = (uint8_t)(b > 255 ? 255 : b);
				}
			}
		}
	}

	auto sp = std::make_unique<HdSprite>();
	sp->diffuse     = std::move(atlas);
	sp->composited  = std::move(composited);
	sp->sprite_w    = pa.sprite_w;
	sp->sprite_h    = pa.sprite_h;
	sp->frames      = std::move(pa.frames);
	return sp;
}

// ---- mapping table --------------------------------------------------

bool HdAssetLoader::load_mapping_table(const QString& path) {
	impl_->bw_id_to_sc_r.clear();
	impl_->have_table = false;
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		err_ = QString("open mapping table failed: %1")
			.arg(f.errorString());
		return false;
	}
	QJsonParseError perr;
	auto doc = QJsonDocument::fromJson(f.readAll(), &perr);
	f.close();
	if (!doc.isObject()) {
		err_ = QString("mapping table not JSON object: %1")
			.arg(perr.errorString());
		return false;
	}
	auto root = doc.object();
	// Enforce the schema tag so a stale hand-edited file doesn't
	// silently produce wrong sprites.
	if (root.value("schema").toString()
	    != QStringLiteral("openbw_hd_mapping_v1"))
	{
		err_ = "mapping table has unexpected schema tag";
		return false;
	}
	auto entries = root.value("bw_id_to_sc_r_row").toObject();
	for (auto it = entries.begin(); it != entries.end(); ++it) {
		bool ok = false;
		int bw = it.key().toInt(&ok);
		if (!ok) continue;
		auto e = it.value().toObject();
		int sc = e.value("sc_r_row").toInt(-1);
		if (sc < 0) continue;
		impl_->bw_id_to_sc_r[bw] = sc;
	}
	impl_->have_table = !impl_->bw_id_to_sc_r.empty();
	return impl_->have_table;
}

bool HdAssetLoader::has_mapping_table() const {
	return impl_ && impl_->have_table;
}

int HdAssetLoader::sc_r_row_for_bw_id(int bw_id) const {
	if (!impl_) return -1;
	auto it = impl_->bw_id_to_sc_r.find(bw_id);
	if (it == impl_->bw_id_to_sc_r.end()) return -1;
	return it->second;
}

}   // namespace sprite_viewer
