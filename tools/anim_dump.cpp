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

// Header layout as observed in a real main_000.anim from SC:R:
//   00: "ANIM" magic (4 bytes)
//   04: u16 version (0x0204 seen)
//   06: u16 unknown (0)
//   08: u16 layer_count (7 seen)
//   0A: u16 unknown (1)
//   0C: layer_count * 32-byte NUL-padded ASCII layer names
//        (diffuse, bright, teamcolor, emissive, normal,
//         specular, ao_depth in the sample)
//   0C + layer_count*32: per-frame table starts. Frame count and
//        per-frame descriptor layout is what this spike is here to
//        confirm; we make a first-cut guess below.
//
// The layers use fixed 32-byte name slots up to a hard cap; we dump
// all 32-byte slots up to a heuristic limit or a NUL first-byte.
struct AnimHeader {
	u32 magic;              // 'MINA' little-endian == "ANIM"
	u16 version;
	u16 unk_06;
	u16 layer_count;
	u16 unk_0a;
	std::vector<std::string> layer_names;
};

static u16 rd16le(const u8* p) { return (u16)p[0] | ((u16)p[1] << 8); }
static u32 rd32le(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8)
		| ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static bool parse_anim_header(const std::vector<u8>& bytes,
	AnimHeader& out, size_t& body_off)
{
	if (bytes.size() < 12) {
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

	if (out.layer_count == 0 || out.layer_count > 16) {
		fprintf(stderr, ".anim implausible layer_count=%u\n",
			out.layer_count);
		return false;
	}
	size_t off = 12;
	for (int i = 0; i < out.layer_count; ++i) {
		if (off + 32 > bytes.size()) {
			fprintf(stderr, ".anim truncated at layer %d\n", i);
			return false;
		}
		// Layer name: 32-byte NUL-padded ASCII.
		std::string name((const char*)&bytes[off]);
		out.layer_names.push_back(std::move(name));
		off += 32;
	}
	body_off = off;
	return true;
}

// Find every "DDS " signature in the file body -- Blizzard embeds
// each layer's texture as a raw DDS chunk with the standard
// "DDS " magic at its head. This is a coarse scan (linear byte
// search); good enough for a spike where we just want to prove
// the payload is present and eyeball-viewable.
static void scan_and_dump_dds(const std::vector<u8>& bytes,
	const std::string& out_dir, const std::string& stem)
{
	// DDS header is exactly 128 bytes: 4-byte magic 'DDS ' + 124-byte
	// DDS_HEADER. The next DDS start bounds this one's payload; we
	// carve out the substring between successive DDS starts.
	std::vector<size_t> starts;
	const u8 magic[4] = { 'D','D','S',' ' };
	for (size_t i = 0; i + 4 <= bytes.size(); ++i) {
		if (std::memcmp(&bytes[i], magic, 4) == 0) starts.push_back(i);
	}
	printf("dds scan: found %zu DDS chunks\n", starts.size());
	fs::create_directories(out_dir);
	for (size_t k = 0; k < starts.size(); ++k) {
		size_t begin = starts[k];
		size_t end = (k + 1 < starts.size()) ? starts[k + 1]
		                                     : bytes.size();
		char path[512];
		snprintf(path, sizeof(path), "%s/%s.dds.%zu",
			out_dir.c_str(), stem.c_str(), k);
		FILE* fp = fopen(path, "wb");
		if (!fp) {
			fprintf(stderr, "open %s: %s\n", path, strerror(errno));
			continue;
		}
		fwrite(&bytes[begin], 1, end - begin, fp);
		fclose(fp);
		printf("  wrote %s (%zu bytes, offset %zu)\n",
			path, end - begin, begin);
	}
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
	size_t body_off = 0;
	if (!parse_anim_header(anim_bytes, ah, body_off)) {
		CascCloseStorage(hStorage); return 1;
	}
	printf("  version    = 0x%04x\n", ah.version);
	printf("  layer_count= %u\n", ah.layer_count);
	for (int i = 0; i < ah.layer_count; ++i) {
		printf("  layer[%d]  = %s\n", i, ah.layer_names[i].c_str());
	}
	printf("  body starts at offset %zu\n", body_off);

	char stem[64];
	snprintf(stem, sizeof(stem), "main_%03u", anim_num);
	scan_and_dump_dds(anim_bytes, args.out_dir, stem);

	CascCloseStorage(hStorage);
	return 0;
}
