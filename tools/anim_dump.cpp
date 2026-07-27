// anim_dump — spike for the SC:R HD renderer path.
//
// Opens a user's StarCraft: Remastered install via CascLib, reads the
// `images.rel` table to map an image_id to a specific `main_NNN.anim`
// file inside CASC storage, parses that .anim header (Blizzard's HD
// sprite atlas container: multiple named "layers" -- diffuse, bright,
// teamcolor, emissive, normal, specular, ao_depth -- each pointing at
// an embedded DDS texture), and dumps a report + writes each layer's
// DDS payload to disk for eyeball verification.
//
// This is the bottom-up spike toward Qt HD rendering in simsc_app.
// It is NOT linked against openbw_ui, sync.h, or the sim. It only
// asserts our ability to reach and decode the HD asset for a chosen
// unit (default: image_id 65 = Terran SCV main graphic).
//
// Build (with OPENBW_BUILD_TOOLS=ON and a submodule-inited CascLib):
//   cmake -B build_srv -DOPENBW_BUILD_TOOLS=ON
//   cmake --build build_srv --target anim_dump
//
// Run:
//   ./build_srv/tools/anim_dump \
//     --sc-version remastered \
//     --sc-remastered-path /Users/you/BlizzardApps/StarCraft \
//     --image-id 65 \
//     --out-dir /tmp/anim_dump/scv
//
// The path passed to --sc-remastered-path should be the folder that
// directly contains `Data/`, `Maps/`, `x86_64/`, etc. -- same shape
// as ../casc_space/StarCraft in the parallel scratch tree.
//
// Output: <out-dir>/main_NNN.anim.dds.0, .1, ... one raw .dds per
// layer that has a DDS payload. macOS Preview + Qt's QImage w/ a
// DDS plugin open these directly; no PNG conversion in this spike.

#include <CascLib.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;

// ---- args -----------------------------------------------------------

struct Args {
	std::string sc_version;         // "classic" | "remastered"
	std::string sc_remastered_path; // path to SC:R install root
	int image_id = 65;              // SCV main graphic; see arr/images.tbl
	std::string out_dir = "/tmp/anim_dump";
};

static void usage(const char* prog) {
	fprintf(stderr,
		"Usage: %s --sc-version <classic|remastered> "
		"--sc-remastered-path <root> [--image-id N] [--out-dir DIR]\n"
		"\n"
		"Reads image_id -> anim_file mapping from <root>/Data/casc's "
		"images.rel, opens the target .anim file, prints its header, "
		"and writes each embedded DDS layer to <out-dir>.\n",
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
		if (a == "--sc-version") {
			auto v = need("--sc-version"); if (!v) return false;
			out.sc_version = v;
		} else if (a == "--sc-remastered-path") {
			auto v = need("--sc-remastered-path"); if (!v) return false;
			out.sc_remastered_path = v;
		} else if (a == "--image-id") {
			auto v = need("--image-id"); if (!v) return false;
			out.image_id = atoi(v);
		} else if (a == "--out-dir") {
			auto v = need("--out-dir"); if (!v) return false;
			out.out_dir = v;
		} else if (a == "-h" || a == "--help") {
			usage(argv[0]); return false;
		} else {
			fprintf(stderr, "unknown arg: %s\n", a.c_str());
			usage(argv[0]);
			return false;
		}
	}
	if (out.sc_version != "remastered") {
		fprintf(stderr,
			"--sc-version must be 'remastered' for this spike\n");
		return false;
	}
	if (out.sc_remastered_path.empty()) {
		fprintf(stderr, "--sc-remastered-path is required\n");
		return false;
	}
	return true;
}

// ---- CASC helpers ---------------------------------------------------

// Read one CASC file into a byte buffer, or return false with an
// error message on stderr. `name` uses forward or back slashes
// per Blizzard's stored convention; CascOpenFile normalizes.
static bool casc_read(HANDLE hStorage, const char* name,
	std::vector<u8>& out)
{
	HANDLE hFile = NULL;
	if (!CascOpenFile(hStorage, name, 0, CASC_OPEN_BY_NAME, &hFile)) {
		fprintf(stderr, "CascOpenFile(%s) failed err=%u\n",
			name, GetCascError());
		return false;
	}
	DWORD sz = CascGetFileSize(hFile, NULL);
	if (sz == CASC_INVALID_SIZE) {
		fprintf(stderr, "CascGetFileSize(%s) failed err=%u\n",
			name, GetCascError());
		CascCloseFile(hFile);
		return false;
	}
	out.resize(sz);
	DWORD got = 0;
	if (!CascReadFile(hFile, out.data(), sz, &got) || got != sz) {
		fprintf(stderr, "CascReadFile(%s) short read %u/%u err=%u\n",
			name, got, sz, GetCascError());
		CascCloseFile(hFile);
		return false;
	}
	CascCloseFile(hFile);
	return true;
}

// ---- images.rel parsing ---------------------------------------------

// images.rel format (community-reversed): an array of per-image_id
// entries, 8 bytes each. Interpretation of those 8 bytes has been
// documented as [u32 flag / has_hd][u32 anim_file_num]. flag==1 with
// anim_file_num==0xffffffff means "no HD anim, fall back to SD grp".
// We dump the raw bytes and the interpreted anim_file_num so we can
// verify visually. Entry size might turn out to be different across
// SC:R patches -- this spike is here specifically to confirm.
struct ImagesRelEntry {
	u32 raw0;
	u32 raw1;
};

static bool load_images_rel(HANDLE hStorage,
	std::vector<ImagesRelEntry>& out)
{
	std::vector<u8> bytes;
	if (!casc_read(hStorage, "images.rel", bytes)) return false;
	if (bytes.size() % sizeof(ImagesRelEntry) != 0) {
		fprintf(stderr,
			"images.rel size %zu is not a multiple of %zu\n",
			bytes.size(), sizeof(ImagesRelEntry));
		return false;
	}
	size_t n = bytes.size() / sizeof(ImagesRelEntry);
	out.resize(n);
	std::memcpy(out.data(), bytes.data(), bytes.size());
	printf("images.rel: %zu entries (%zu bytes each)\n",
		n, sizeof(ImagesRelEntry));
	return true;
}

// ---- ANIM parsing ---------------------------------------------------

// Full layout (reverse-engineered from a real main_247.anim -- SC:R's
// SCV atlas):
//
//   0x000  "ANIM" magic (4 bytes)
//   0x004  u16 version           (0x0204 seen)
//   0x006  u16 unk_06            (0)
//   0x008  u16 layer_count       (7 seen: diffuse, bright, teamcolor,
//                                 emissive, normal, specular, ao_depth)
//   0x00A  u16 unk_0a            (1)
//   0x00C  layer_count * 32-byte NUL-padded ASCII layer names
//          followed by (16 - layer_count) * 32 bytes of zero padding.
//          So the layer-names section is FIXED at 16*32 = 512 bytes.
//   0x14C  u16 frame_count
//   0x14E  u16 unk_14e           (0xffff)
//   0x150  u16 atlas_bounding_w  (max frame width, sprite-cell size)
//   0x152  u16 atlas_bounding_h  (max frame height)
//   0x154  u32 frame_table_off   (file offset to the per-frame table
//                                 in the trailer -- 16 bytes/entry)
//   0x158  per-layer descriptors, 12 bytes each, one per LAYER SLOT
//          (16 slots, matching the 16*32 name slots above; unused
//          slots are all-zero):
//            u32 data_offset      (file offset of the DDS chunk)
//            u32 data_size        (bytes)
//            u32 dims             (packed [u16 w][u16 h] = atlas size)
//          For layer_count=7 that consumes 16*12 = 192 bytes,
//          ending at 0x218. First DDS starts at 0x1AC per the
//          descriptor -- data actually begins mid-descriptor region;
//          the "16 slots" pattern is a design guess but matches
//          because unused slots have data_offset=0 which is skipped.
//
//   0x1AC..EOF  DDS payloads (concatenated), then at the tail...
//   [frame_table_off].. frame_count * 16-byte frame descriptors:
//            u16 atlas_x, atlas_y
//            u16 offset_x, offset_y   (draw offset for centering)
//            u16 w, h
//            u32 flags                (=1 in samples)
struct AnimLayerInfo {
	u32 data_offset;   // 0 = layer not present
	u32 data_size;
	u16 atlas_w;
	u16 atlas_h;
};

struct AnimFrame {
	u16 atlas_x;
	u16 atlas_y;
	u16 offset_x;
	u16 offset_y;
	u16 w;
	u16 h;
	u32 flags;
};

struct AnimHeader {
	u32 magic;                        // 'MINA' little-endian == "ANIM"
	u16 version;
	u16 unk_06;
	u16 layer_count;
	u16 unk_0a;
	std::vector<std::string> layer_names;   // named layers only

	// Body:
	u16 frame_count = 0;
	u16 unk_14e = 0;
	u16 sprite_w = 0;              // sprite bounding box (max frame w)
	u16 sprite_h = 0;              // sprite bounding box (max frame h)
	u32 frame_table_off = 0;       // file offset of frame descriptors
	std::vector<AnimLayerInfo> layers;   // aligned with layer_names
	std::vector<AnimFrame> frames;
};

static u16 rd16le(const u8* p) { return (u16)p[0] | ((u16)p[1] << 8); }
static u32 rd32le(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8)
		| ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

// Layer-names section is a FIXED 16 * 32-byte slot table (512 bytes)
// regardless of how many layers are actually named. Beyond
// layer_count the remaining slots are zero-filled.
static constexpr int ANIM_MAX_LAYERS = 16;
static constexpr size_t ANIM_LAYER_NAME_SIZE = 32;
static constexpr size_t ANIM_HEADER_FIXED_SIZE =
	12 + ANIM_MAX_LAYERS * ANIM_LAYER_NAME_SIZE;   // = 12 + 512 = 524 -> but real files show body @ 0x14c = 332? see below

// Empirical: for main_247.anim (SC:R SCV) the body section begins at
// 0x14C = 332 -- which is 12 + 10*32 = 332, i.e. 10 layer slots not
// 16. Not clear whether Blizzard fixed the slot count or if it's a
// per-file quirk, but 10 matches the observed offset exactly. Use
// (0x14c - 12) / 32 = 10 as the layer-slot count going forward.
static constexpr int ANIM_LAYER_SLOT_COUNT = 10;
static constexpr size_t ANIM_BODY_OFFSET =
	12 + ANIM_LAYER_SLOT_COUNT * ANIM_LAYER_NAME_SIZE;   // 0x14C

static bool parse_anim(const std::vector<u8>& bytes, AnimHeader& out)
{
	if (bytes.size() < ANIM_BODY_OFFSET + 8) {
		fprintf(stderr, ".anim too small: %zu bytes\n", bytes.size());
		return false;
	}
	out.magic = rd32le(bytes.data());
	// "ANIM" = 41 4E 49 4D  in ASCII, little-endian u32 is 0x4D494E41
	if (out.magic != 0x4D494E41) {
		fprintf(stderr, ".anim bad magic 0x%08x (want 0x4D494E41)\n",
			out.magic);
		return false;
	}
	out.version     = rd16le(bytes.data() + 4);
	out.unk_06      = rd16le(bytes.data() + 6);
	out.layer_count = rd16le(bytes.data() + 8);
	out.unk_0a      = rd16le(bytes.data() + 10);

	if (out.layer_count == 0 || out.layer_count > ANIM_LAYER_SLOT_COUNT) {
		fprintf(stderr, ".anim implausible layer_count=%u (max %d)\n",
			out.layer_count, ANIM_LAYER_SLOT_COUNT);
		return false;
	}
	// Layer names occupy the first layer_count 32-byte slots; the
	// rest of the fixed 10-slot region is zero padding.
	size_t off = 12;
	for (int i = 0; i < out.layer_count; ++i) {
		std::string name((const char*)&bytes[off]);
		out.layer_names.push_back(std::move(name));
		off += ANIM_LAYER_NAME_SIZE;
	}

	// Body section starts at ANIM_BODY_OFFSET (0x14C).
	const u8* body = bytes.data() + ANIM_BODY_OFFSET;
	out.frame_count     = rd16le(body + 0);
	out.unk_14e         = rd16le(body + 2);
	out.sprite_w        = rd16le(body + 4);
	out.sprite_h        = rd16le(body + 6);
	out.frame_table_off = rd32le(body + 8);

	// Per-layer descriptors: 12 bytes each, layer_count of them.
	// data_offset==0 means "layer not present in this file".
	size_t layer_desc_off = ANIM_BODY_OFFSET + 12;
	for (int i = 0; i < out.layer_count; ++i) {
		if (layer_desc_off + 12 > bytes.size()) {
			fprintf(stderr, ".anim truncated at layer_desc %d\n", i);
			return false;
		}
		AnimLayerInfo li;
		li.data_offset = rd32le(bytes.data() + layer_desc_off + 0);
		li.data_size   = rd32le(bytes.data() + layer_desc_off + 4);
		li.atlas_w     = rd16le(bytes.data() + layer_desc_off + 8);
		li.atlas_h     = rd16le(bytes.data() + layer_desc_off + 10);
		out.layers.push_back(li);
		layer_desc_off += 12;
	}

	// Frame table lives at file offset frame_table_off,
	// frame_count entries of 16 bytes each. Sanity-check the pointer.
	if (out.frame_table_off == 0
	    || out.frame_table_off + (size_t)out.frame_count * 16
	       > bytes.size())
	{
		fprintf(stderr,
			".anim frame_table_off=%u + %u*16 out of range\n",
			out.frame_table_off, out.frame_count);
		return false;
	}
	const u8* ft = bytes.data() + out.frame_table_off;
	out.frames.reserve(out.frame_count);
	for (int i = 0; i < out.frame_count; ++i) {
		AnimFrame f;
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

// Emit each present layer's DDS payload as a standalone .dds file
// (raw carve using the layer descriptor's offset+size), plus a
// manifest.json that captures everything a downstream renderer
// needs: sprite dimensions, per-layer file/atlas info, and the
// per-frame atlas rects. Consumers open the .dds via any DDS
// loader (Qt image plugin, stb_image with a DXT decoder, GL
// texture upload, etc.) and slice with the frame table.
static void dump_layers_and_manifest(const std::vector<u8>& bytes,
	const AnimHeader& ah,
	const std::string& out_dir, const std::string& stem)
{
	fs::create_directories(out_dir);

	// --- write DDS files, one per present layer -----------------
	std::vector<std::string> layer_paths(ah.layer_count);
	for (int i = 0; i < ah.layer_count; ++i) {
		const auto& li = ah.layers[i];
		if (li.data_offset == 0 || li.data_size == 0) continue;
		if (li.data_offset + li.data_size > bytes.size()) {
			fprintf(stderr,
				"layer %d: offset+size out of range\n", i);
			continue;
		}
		char fname[128];
		snprintf(fname, sizeof(fname), "%s.%s.dds",
			stem.c_str(), ah.layer_names[i].c_str());
		layer_paths[i] = fname;
		std::string full = out_dir + "/" + fname;
		FILE* fp = fopen(full.c_str(), "wb");
		if (!fp) {
			fprintf(stderr, "open %s: %s\n",
				full.c_str(), strerror(errno));
			continue;
		}
		fwrite(&bytes[li.data_offset], 1, li.data_size, fp);
		fclose(fp);
		printf("  wrote %s (%u bytes, %ux%u)\n",
			full.c_str(), li.data_size, li.atlas_w, li.atlas_h);
	}

	// --- write manifest.json ------------------------------------
	// Small hand-emitted JSON so we don't drag in a dep.
	std::string manifest_path = out_dir + "/" + stem + ".manifest.json";
	FILE* fp = fopen(manifest_path.c_str(), "w");
	if (!fp) {
		fprintf(stderr, "open %s: %s\n",
			manifest_path.c_str(), strerror(errno));
		return;
	}
	fprintf(fp, "{\n");
	fprintf(fp, "  \"stem\": \"%s\",\n", stem.c_str());
	fprintf(fp, "  \"anim_version\": \"0x%04x\",\n", ah.version);
	fprintf(fp, "  \"sprite_w\": %u,\n", ah.sprite_w);
	fprintf(fp, "  \"sprite_h\": %u,\n", ah.sprite_h);
	fprintf(fp, "  \"layers\": [\n");
	for (int i = 0; i < ah.layer_count; ++i) {
		const auto& li = ah.layers[i];
		bool present = li.data_offset != 0 && li.data_size != 0;
		fprintf(fp,
			"    { \"name\": \"%s\", \"present\": %s, "
			"\"file\": \"%s\", \"atlas_w\": %u, \"atlas_h\": %u }%s\n",
			ah.layer_names[i].c_str(),
			present ? "true" : "false",
			present ? layer_paths[i].c_str() : "",
			li.atlas_w, li.atlas_h,
			(i + 1 < ah.layer_count) ? "," : "");
	}
	fprintf(fp, "  ],\n");
	fprintf(fp, "  \"frame_count\": %u,\n", ah.frame_count);
	fprintf(fp, "  \"frames\": [\n");
	for (size_t i = 0; i < ah.frames.size(); ++i) {
		const auto& f = ah.frames[i];
		fprintf(fp,
			"    { \"x\": %u, \"y\": %u, \"w\": %u, \"h\": %u, "
			"\"offset_x\": %u, \"offset_y\": %u, "
			"\"flags\": %u }%s\n",
			f.atlas_x, f.atlas_y, f.w, f.h,
			f.offset_x, f.offset_y, f.flags,
			(i + 1 < ah.frames.size()) ? "," : "");
	}
	fprintf(fp, "  ]\n");
	fprintf(fp, "}\n");
	fclose(fp);
	printf("  wrote %s\n", manifest_path.c_str());
}

// ---- main -----------------------------------------------------------

int main(int argc, char** argv) {
	Args args;
	if (!parse_args(argc, argv, args)) return 1;

	// CascOpenStorage wants a path CascLib recognizes; the SC:R
	// install root ("<path>/Data" is autodetected by the lib).
	HANDLE hStorage = NULL;
	if (!CascOpenStorage(args.sc_remastered_path.c_str(), 0, &hStorage)) {
		fprintf(stderr,
			"CascOpenStorage(%s) failed err=%u\n"
			"(hint: --sc-remastered-path should point at the folder "
			"containing Data/, Maps/, x86_64/, ...)\n",
			args.sc_remastered_path.c_str(), GetCascError());
		return 1;
	}
	printf("opened CASC storage at %s\n\n",
		args.sc_remastered_path.c_str());

	std::vector<ImagesRelEntry> images_rel;
	if (!load_images_rel(hStorage, images_rel)) {
		CascCloseStorage(hStorage); return 1;
	}
	if (args.image_id < 0 || (size_t)args.image_id >= images_rel.size()) {
		fprintf(stderr,
			"image_id %d out of range (0..%zu)\n",
			args.image_id, images_rel.size());
		CascCloseStorage(hStorage); return 1;
	}
	const auto& e = images_rel[args.image_id];
	printf("image_id %d: raw0=0x%08x raw1=0x%08x\n",
		args.image_id, e.raw0, e.raw1);

	// Heuristic: anim_file_num is one of the two u32s. On sample
	// dumps of images.rel, the pattern for a real HD image is
	// raw0=1 and raw1=N (small integer). For "no HD", raw1 is
	// 0xffffffff. Adjust if the mapping is different.
	u32 anim_num;
	if (e.raw0 == 1 && e.raw1 != 0xffffffff) {
		anim_num = e.raw1;
	} else if (e.raw0 == 8 && e.raw1 != 0xffffffff) {
		// Alt code path -- also seen in the dump; still worth trying
		anim_num = e.raw1;
	} else {
		fprintf(stderr,
			"image_id %d has no HD anim (raw0=%u raw1=0x%08x). "
			"Try a different image_id -- SCV is often 65.\n",
			args.image_id, e.raw0, e.raw1);
		CascCloseStorage(hStorage); return 1;
	}

	char anim_name[64];
	snprintf(anim_name, sizeof(anim_name),
		"anim\\main_%03u.anim", anim_num);
	printf("resolving image_id %d -> %s\n\n",
		args.image_id, anim_name);

	std::vector<u8> anim_bytes;
	if (!casc_read(hStorage, anim_name, anim_bytes)) {
		CascCloseStorage(hStorage); return 1;
	}
	printf(".anim file: %zu bytes\n", anim_bytes.size());

	AnimHeader ah;
	if (!parse_anim(anim_bytes, ah)) {
		CascCloseStorage(hStorage); return 1;
	}
	printf("  version     = 0x%04x\n", ah.version);
	printf("  layer_count = %u\n", ah.layer_count);
	for (int i = 0; i < ah.layer_count; ++i) {
		const auto& li = ah.layers[i];
		printf("  layer[%d]   = %-10s data_off=%u size=%u atlas=%ux%u\n",
			i, ah.layer_names[i].c_str(),
			li.data_offset, li.data_size, li.atlas_w, li.atlas_h);
	}
	printf("  sprite_wh   = %ux%u\n", ah.sprite_w, ah.sprite_h);
	printf("  frame_count = %u\n", ah.frame_count);
	printf("  frame_table @ %u\n", ah.frame_table_off);
	if (ah.frame_count > 0) {
		auto& f0 = ah.frames[0];
		printf("  frames[0]   = xy=(%u,%u) wh=(%u,%u) off=(%u,%u) flags=%u\n",
			f0.atlas_x, f0.atlas_y, f0.w, f0.h,
			f0.offset_x, f0.offset_y, f0.flags);
	}

	char stem[64];
	snprintf(stem, sizeof(stem), "main_%03u", anim_num);
	dump_layers_and_manifest(anim_bytes, ah, args.out_dir, stem);

	CascCloseStorage(hStorage);
	return 0;
}
