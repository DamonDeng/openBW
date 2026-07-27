// sd_sprite_dump — render one preview PNG per SD sprite in an openBW
// data install. Companion to tools/anim_dump.cpp for the HD side.
// Enables side-by-side eyeball comparison in the HD Mapping tab
// (see sprite_viewer/mapping_tab.cpp).
//
// Iterates arr/images.tbl (openBW's 999-slot image table), resolves
// each entry's GRP filename, opens the GRP, decodes frame 4 (facing
// east / pose 0 for units with the standard 17-facing wheel) into a
// palette-indexed buffer, applies a fixed .wpe palette, and writes
// the result as an RGBA PNG at:
//   <out-dir>/<image_id>_<safe_name>.png
//
// GRPs with fewer than 5 frames (buildings, sprites without facings)
// fall back to frame 0. Entries with a null grp_filename_index or a
// GRP that fails to decode are skipped and logged; the tool never
// fails the whole run over one bad entry.
//
// Build (with OPENBW_BUILD_TOOLS=ON):
//   cmake --build build_srv --target sd_sprite_dump
//
// Run:
//   ./build_srv/tools/sd_sprite_dump \
//     --data-path original_resources \
//     --out-dir  original_resources/sd_previews
//
// Only the PNG writer needs a dependency: zlib (for the IDAT chunk
// deflate). Everything else is self-contained: no linkage against
// openbw_ui, sync.h, or the sim.

#include "../data_loading.h"

#include <zlib.h>

#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;

// ---- args -----------------------------------------------------------

struct Args {
	std::string data_path = "original_resources";
	std::string out_dir   = "original_resources/sd_previews";
	std::string wpe_path  = "Tileset/jungle.wpe";
	int max_dim = 256;   // canvas W and H
};

static void usage(const char* prog) {
	fprintf(stderr,
		"Usage: %s [--data-path DIR] [--out-dir DIR] "
		"[--palette Tileset/<name>.wpe] [--canvas N]\n"
		"Renders one PNG per entry in arr/images.tbl.\n",
		prog);
}

static bool parse_args(int argc, char** argv, Args& out) {
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		auto need = [&](const char* what) -> const char* {
			if (i + 1 >= argc) {
				fprintf(stderr, "%s requires an argument\n", what);
				return nullptr;
			}
			return argv[++i];
		};
		if (a == "--data-path") {
			auto v = need("--data-path"); if (!v) return false;
			out.data_path = v;
		} else if (a == "--out-dir") {
			auto v = need("--out-dir"); if (!v) return false;
			out.out_dir = v;
		} else if (a == "--palette") {
			auto v = need("--palette"); if (!v) return false;
			out.wpe_path = v;
		} else if (a == "--canvas") {
			auto v = need("--canvas"); if (!v) return false;
			out.max_dim = atoi(v);
		} else if (a == "-h" || a == "--help") {
			usage(argv[0]); return false;
		} else {
			fprintf(stderr, "unknown arg: %s\n", a.c_str());
			usage(argv[0]);
			return false;
		}
	}
	return true;
}

// ---- images.tbl -----------------------------------------------------

// Same format we've decoded twice already:
//   u16 count
//   u16 offsets[count]        (from the start of the file)
//   NUL-terminated ASCII strings
// The retail table has 929 valid entries but only 999 "slots" that
// image_type_t indexes into. We iterate 0..count-1 and skip nulls.
static std::vector<std::string> read_images_tbl(
	const std::vector<u8>& bytes)
{
	std::vector<std::string> out;
	if (bytes.size() < 2) return out;
	u16 count = (u16)bytes[0] | ((u16)bytes[1] << 8);
	if ((size_t)2 + (size_t)count * 2 > bytes.size()) return out;
	out.reserve(count);
	for (int i = 0; i < count; ++i) {
		size_t p = 2 + i * 2;
		u16 off = (u16)bytes[p] | ((u16)bytes[p + 1] << 8);
		if (off == 0 || off >= bytes.size()) {
			out.emplace_back();
			continue;
		}
		size_t end = off;
		while (end < bytes.size() && bytes[end] != 0) ++end;
		out.emplace_back((const char*)&bytes[off], end - off);
	}
	return out;
}

// ---- GRP decode -----------------------------------------------------

struct GrpFrame {
	int xOff = 0, yOff = 0;
	int w = 0, h = 0;
	u32 dataOff = 0;
};

struct Grp {
	int width = 0, height = 0;
	std::vector<GrpFrame> frames;
	std::vector<u8> raw;   // full GRP bytes; frames reference offsets in
};

// Parse the fixed 6-byte GRP header + per-frame directory. Actual
// per-row RLE decoding happens in decode_frame().
static bool parse_grp(const std::vector<u8>& bytes, Grp& out) {
	if (bytes.size() < 6) return false;
	u16 frame_count = (u16)bytes[0] | ((u16)bytes[1] << 8);
	out.width  = (u16)bytes[2] | ((u16)bytes[3] << 8);
	out.height = (u16)bytes[4] | ((u16)bytes[5] << 8);
	out.frames.resize(frame_count);
	if (bytes.size() < 6 + (size_t)frame_count * 8) return false;
	for (int i = 0; i < frame_count; ++i) {
		size_t p = 6 + (size_t)i * 8;
		auto& f = out.frames[i];
		f.xOff = bytes[p + 0];
		f.yOff = bytes[p + 1];
		f.w    = bytes[p + 2];
		f.h    = bytes[p + 3];
		f.dataOff = (u32)bytes[p + 4]
			| ((u32)bytes[p + 5] << 8)
			| ((u32)bytes[p + 6] << 16)
			| ((u32)bytes[p + 7] << 24);
	}
	out.raw = bytes;
	return true;
}

// Decode one frame into an 8-bit palette-index buffer sized f.w * f.h.
// Transparent pixels are left as 0 (which the palette maps to alpha=0).
static bool decode_grp_frame(const Grp& g, int idx, std::vector<u8>& px)
{
	if (idx < 0 || idx >= (int)g.frames.size()) return false;
	const GrpFrame& f = g.frames[idx];
	px.assign((size_t)f.w * f.h, 0);
	if (f.w == 0 || f.h == 0) return true;
	const u8* base = g.raw.data();
	size_t base_sz = g.raw.size();
	if (f.dataOff + (size_t)f.h * 2 > base_sz) return false;
	for (int y = 0; y < f.h; ++y) {
		size_t row_off = (u16)base[f.dataOff + y * 2]
			| ((u16)base[f.dataOff + y * 2 + 1] << 8);
		size_t p = (size_t)f.dataOff + row_off;
		int x = 0;
		while (x < f.w) {
			if (p >= base_sz) return false;
			u8 c = base[p++];
			if (c & 0x80) {          // skip
				x += (c & 0x7F);
			} else if (c & 0x40) {   // run
				if (p >= base_sz) return false;
				u8 v = base[p++];
				int n = c & 0x3F;
				while (n-- > 0 && x < f.w) px[y * f.w + x++] = v;
			} else {                 // raw
				int n = c;
				while (n-- > 0 && x < f.w) {
					if (p >= base_sz) return false;
					px[y * f.w + x++] = base[p++];
				}
			}
		}
	}
	return true;
}

// ---- palette --------------------------------------------------------

// 256 RGBA entries. Loaded from a .wpe (256 * 4 bytes: R G B pad).
// Entry 0 gets alpha=0 (transparent) to match retail's convention.
struct Palette {
	u8 rgba[256][4] = {};
};

static bool load_palette(const std::vector<u8>& wpe, Palette& out) {
	if (wpe.size() != 256 * 4) return false;
	for (int i = 0; i < 256; ++i) {
		out.rgba[i][0] = wpe[i * 4 + 0];
		out.rgba[i][1] = wpe[i * 4 + 1];
		out.rgba[i][2] = wpe[i * 4 + 2];
		out.rgba[i][3] = (i == 0) ? 0 : 255;
	}
	return true;
}

// ---- canvas composition ---------------------------------------------

// Center a frame on a canvas_w x canvas_h RGBA image so the sprite's
// origin sits in the middle. GRP frames are stored with (xOff, yOff)
// relative to the sprite bounding box; using the sprite's midpoint
// as anchor gives a stable centered preview.
static std::vector<u8> composite(const Grp& g, int frame_idx,
	const std::vector<u8>& px, const Palette& pal,
	int canvas_w, int canvas_h)
{
	std::vector<u8> canvas((size_t)canvas_w * canvas_h * 4, 0);
	const GrpFrame& f = g.frames[frame_idx];
	int cx = canvas_w / 2 - g.width  / 2;
	int cy = canvas_h / 2 - g.height / 2;
	for (int y = 0; y < f.h; ++y) {
		int dy = cy + f.yOff + y;
		if (dy < 0 || dy >= canvas_h) continue;
		for (int x = 0; x < f.w; ++x) {
			int dx = cx + f.xOff + x;
			if (dx < 0 || dx >= canvas_w) continue;
			u8 idx = px[y * f.w + x];
			if (idx == 0) continue;  // transparent
			u8* p = &canvas[((size_t)dy * canvas_w + dx) * 4];
			p[0] = pal.rgba[idx][0];
			p[1] = pal.rgba[idx][1];
			p[2] = pal.rgba[idx][2];
			p[3] = pal.rgba[idx][3];
		}
	}
	return canvas;
}

// ---- PNG writer -----------------------------------------------------

// Minimal PNG writer for RGBA8 images. Uses zlib for the IDAT stream.
// Same shape as grp2png.cpp's writer -- kept inline here so the
// tool stays self-contained.

static void write_be32(std::vector<u8>& v, u32 x) {
	v.push_back((u8)(x >> 24));
	v.push_back((u8)(x >> 16));
	v.push_back((u8)(x >> 8));
	v.push_back((u8)x);
}

static void write_chunk(std::vector<u8>& out, const char type[4],
	const u8* data, size_t n)
{
	write_be32(out, (u32)n);
	size_t typeStart = out.size();
	out.push_back(type[0]); out.push_back(type[1]);
	out.push_back(type[2]); out.push_back(type[3]);
	if (n) out.insert(out.end(), data, data + n);
	u32 crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, out.data() + typeStart, 4 + n);
	write_be32(out, crc);
}

static bool write_png(const std::string& path,
	const std::vector<u8>& rgba, int w, int h)
{
	// Filter=0 per row (no filtering); zlib-compress the resulting
	// scanline stream (w*4+1 bytes per row).
	std::vector<u8> raw;
	raw.reserve((size_t)h * (w * 4 + 1));
	for (int y = 0; y < h; ++y) {
		raw.push_back(0);
		raw.insert(raw.end(),
			rgba.data() + (size_t)y * w * 4,
			rgba.data() + (size_t)(y + 1) * w * 4);
	}
	uLongf comp_sz = compressBound((uLong)raw.size());
	std::vector<u8> comp(comp_sz);
	if (compress2(comp.data(), &comp_sz, raw.data(), raw.size(),
	              Z_BEST_SPEED) != Z_OK) return false;
	comp.resize(comp_sz);

	std::vector<u8> out;
	// Signature.
	static const u8 sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
	out.insert(out.end(), sig, sig + 8);
	// IHDR.
	u8 ihdr[13];
	u32 W = (u32)w, H = (u32)h;
	ihdr[0]=W>>24; ihdr[1]=W>>16; ihdr[2]=W>>8; ihdr[3]=W;
	ihdr[4]=H>>24; ihdr[5]=H>>16; ihdr[6]=H>>8; ihdr[7]=H;
	ihdr[8]=8;    // bit depth
	ihdr[9]=6;    // color type RGBA
	ihdr[10]=0;   // compression
	ihdr[11]=0;   // filter
	ihdr[12]=0;   // interlace
	write_chunk(out, "IHDR", ihdr, sizeof(ihdr));
	write_chunk(out, "IDAT", comp.data(), comp.size());
	write_chunk(out, "IEND", nullptr, 0);

	FILE* fp = fopen(path.c_str(), "wb");
	if (!fp) return false;
	size_t got = fwrite(out.data(), 1, out.size(), fp);
	fclose(fp);
	return got == out.size();
}

// Normalize a name like "terran\\marine.grp" -> "terran_marine_grp"
// for a filesystem-safe filename. Preserves the original for
// logging.
static std::string safe_name(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		if (c == '\\' || c == '/' || c == ' ' || c == '.') out.push_back('_');
		else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
		      || (c >= '0' && c <= '9') || c == '_' || c == '-')
			out.push_back(c);
	}
	return out;
}

// ---- main -----------------------------------------------------------

int main(int argc, char** argv) {
	Args args;
	if (!parse_args(argc, argv, args)) return 1;

	// Set up openBW's data loader over the .mpq files.
	// data_files_directory picks up slim MPQs if present.
	auto load_data_file =
		bwgame::data_loading::data_files_directory(
			bwgame::a_string(args.data_path.c_str()));

	fprintf(stderr, "reading %s ... ", args.wpe_path.c_str());
	bwgame::a_vector<u8> wpe_bytes;
	load_data_file(wpe_bytes, bwgame::a_string(args.wpe_path.c_str()));
	if (wpe_bytes.size() != 256 * 4) {
		fprintf(stderr, "bad palette size %zu\n", wpe_bytes.size());
		return 1;
	}
	Palette pal;
	if (!load_palette(std::vector<u8>(wpe_bytes.begin(),
	                                   wpe_bytes.end()), pal)) {
		fprintf(stderr, "palette load failed\n");
		return 1;
	}
	fprintf(stderr, "ok\n");

	fprintf(stderr, "reading arr/images.tbl ... ");
	bwgame::a_vector<u8> tbl_bytes;
	load_data_file(tbl_bytes, bwgame::a_string("arr/images.tbl"));
	auto names = read_images_tbl(
		std::vector<u8>(tbl_bytes.begin(), tbl_bytes.end()));
	fprintf(stderr, "%zu entries\n", names.size());

	std::error_code ec;
	fs::create_directories(args.out_dir, ec);
	if (ec) {
		fprintf(stderr, "mkdir %s: %s\n",
			args.out_dir.c_str(), ec.message().c_str());
		return 1;
	}

	int wrote = 0, skipped = 0;
	for (size_t i = 0; i < names.size(); ++i) {
		const std::string& raw_name = names[i];
		if (raw_name.empty()) { ++skipped; continue; }

		bwgame::a_string grp_path = bwgame::format("unit/%s",
			bwgame::a_string(raw_name.c_str()));
		bwgame::a_vector<u8> grp_bytes;
		try {
			load_data_file(grp_bytes, grp_path);
		} catch (const std::exception& e) {
			fprintf(stderr, "[%3zu] %-40s: unreadable (%s)\n",
				i, raw_name.c_str(), e.what());
			++skipped;
			continue;
		}
		Grp g;
		if (!parse_grp(std::vector<u8>(grp_bytes.begin(),
		                                grp_bytes.end()), g)
		    || g.frames.empty())
		{
			fprintf(stderr, "[%3zu] %-40s: bad grp\n",
				i, raw_name.c_str());
			++skipped;
			continue;
		}
		// Facing 4 (east) if available -- most units have >=5
		// frames per pose in retail's 17-facing layout, and
		// frame 4 lands ~ east in the standard wheel. Falls back
		// to frame 0 for buildings / non-facing sprites.
		int frame_idx = (g.frames.size() >= 5) ? 4 : 0;
		std::vector<u8> px;
		if (!decode_grp_frame(g, frame_idx, px)) {
			fprintf(stderr, "[%3zu] %-40s: decode failed frame %d\n",
				i, raw_name.c_str(), frame_idx);
			++skipped;
			continue;
		}

		// Canvas size: max(sprite bounding box, --canvas) so
		// large buildings still fit. Round up to nearest 8 for
		// pretty numbers.
		int canvas = args.max_dim;
		if (g.width  > canvas) canvas = g.width  + 8;
		if (g.height > canvas) canvas = g.height + 8;
		auto rgba = composite(g, frame_idx, px, pal, canvas, canvas);

		char fname[512];
		snprintf(fname, sizeof(fname), "%s/%03zu_%s.png",
			args.out_dir.c_str(), i, safe_name(raw_name).c_str());
		if (!write_png(fname, rgba, canvas, canvas)) {
			fprintf(stderr, "[%3zu] %-40s: png write failed\n",
				i, raw_name.c_str());
			++skipped;
			continue;
		}
		++wrote;
		if (wrote % 50 == 0) {
			fprintf(stderr, "  ... wrote %d PNGs\n", wrote);
		}
	}

	fprintf(stderr, "\ndone: %d PNGs written, %d skipped\n",
		wrote, skipped);
	fprintf(stderr, "output in %s\n", args.out_dir.c_str());
	return 0;
}
