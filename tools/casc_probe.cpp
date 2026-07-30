// casc_probe: enumerate a set of likely .rel / .dat file names inside
// SC:R's CASC storage and report which ones exist + their size.
//
// Purpose: SC:R ships its own HD-side data (images.rel is one we
// already read). The SD codebase's `sprites.dat` maps a sprite to its
// composite image list (body + shadow + overlays); we suspect a
// parallel `sprites.rel` (or similar) exists in HD data. If so, that's
// the canonical source for HD body <-> shadow pairings, and we no
// longer have to probe adjacent anim_num values with heuristics.
//
// This spike does NOT parse anything -- it just tells us which of a
// few candidate paths CascOpenFile accepts. If nothing lands, we
// widen the probe list; if we hit `sprites.rel` (or similar), we
// know where to read from and can parse in a proper loader change.
//
// Build: added to tools/CMakeLists.txt alongside anim_dump.
// Run:
//   ./build_srv/tools/casc_probe --sc-remastered-path /path/to/StarCraft

#include "CascLib.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
	// --sc-remastered-path <root>          [required]
	// --image-ids id1,id2,...              [optional] dump those rows
	// --histogram                          [optional] flag distribution
	// --anim-range LO,HI                   [optional] enumerate main_LO..main_HI
	const char* root = nullptr;
	const char* image_ids_arg = nullptr;
	const char* anim_range_arg = nullptr;
	bool histogram = false;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--sc-remastered-path") == 0 && i + 1 < argc) {
			root = argv[++i];
		} else if (std::strcmp(argv[i], "--image-ids") == 0 && i + 1 < argc) {
			image_ids_arg = argv[++i];
		} else if (std::strcmp(argv[i], "--anim-range") == 0 && i + 1 < argc) {
			anim_range_arg = argv[++i];
		} else if (std::strcmp(argv[i], "--histogram") == 0) {
			histogram = true;
		}
	}
	if (!root) {
		std::fprintf(stderr,
			"usage: %s --sc-remastered-path <SC:R install root>\n", argv[0]);
		return 1;
	}

	HANDLE hStorage = NULL;
	if (!CascOpenStorage(root, 0, &hStorage)) {
		std::fprintf(stderr, "CascOpenStorage(%s) failed err=%u\n",
			root, GetCascError());
		return 1;
	}
	std::printf("opened CASC at %s\n\n", root);

	// Candidate paths. `arr\\` and `Arr\\` are common Blizzard
	// container prefixes for tabular data; we include both cases
	// and slash variants. `.rel` is the HD suffix we've confirmed
	// with images.rel; `.dat` is the classic SD suffix that HD
	// might mirror; we probe both.
	const char* names[] = {
		"images.rel",
		"sprites.rel",
		"flingy.rel",
		"units.rel",
		"orders.rel",
		"upgrades.rel",
		"weapons.rel",
		"tech.rel",
		"portraits.rel",
		"arr/images.rel",
		"arr/sprites.rel",
		"arr\\sprites.rel",
		"arr\\images.rel",
		"Arr/sprites.rel",
		"arr/units.rel",
		"anim/sprites.rel",
		"anim\\sprites.rel",
		"anim/main_000.anim",   // sanity: known to exist
		"arr/sprites.dat",
		"arr\\sprites.dat",
		"sprites.dat",
		"unit\\sprites.dat",
		// Shadow / shared-anim probes.
		"anim/shadow.anim",
		"anim\\shadow.anim",
		"anim/shared_shadow.anim",
		"anim/main_shadow.anim",
		"anim\\shared.anim",
		"anim/effects.anim",
		"anim/misc.anim",
		"anim/anims.rel",
		"anim/effects.rel",
	};

	for (const char* n : names) {
		HANDLE h = NULL;
		if (!CascOpenFile(hStorage, n, 0, CASC_OPEN_BY_NAME, &h)) {
			std::printf("  MISS  %s (err=%u)\n", n, GetCascError());
			continue;
		}
		DWORD sz = CascGetFileSize(h, NULL);
		std::printf("  HIT   %s  size=%u\n", n, (unsigned)sz);
		CascCloseFile(h);
	}

	// Histogram of flag values across the whole images.rel table.
	// Used to confirm what variants exist beyond the known set
	// {1=empty, 4=HD2, 8=HD, 16=Carbot, 512=misc}. A rare flag
	// that maps SD-only image_ids to their HD counterparts would
	// show up here as a substantial count.
	if (histogram) {
		HANDLE h = NULL;
		if (!CascOpenFile(hStorage, "images.rel", 0, CASC_OPEN_BY_NAME, &h)) {
			std::fprintf(stderr, "open images.rel failed err=%u\n",
				GetCascError());
		} else {
			DWORD sz = CascGetFileSize(h, NULL);
			std::vector<unsigned char> buf(sz);
			DWORD got = 0;
			CascReadFile(h, buf.data(), sz, &got);
			CascCloseFile(h);
			size_t n_entries = got / 8;
			std::printf("\nimages.rel: %zu entries\n", n_entries);
			// Small map of flag -> (count, first_id_seen)
			struct FlagBin { size_t count = 0; int first_id = -1; };
			std::vector<std::pair<unsigned, FlagBin>> bins;
			for (size_t i = 0; i < n_entries; ++i) {
				const auto* row = buf.data() + i * 8;
				unsigned flag = (unsigned)row[0]
					| ((unsigned)row[1] << 8)
					| ((unsigned)row[2] << 16)
					| ((unsigned)row[3] << 24);
				bool found = false;
				for (auto& b : bins) {
					if (b.first == flag) {
						b.second.count++;
						found = true;
						break;
					}
				}
				if (!found) {
					bins.push_back({flag, {1, (int)i}});
				}
			}
			std::printf("flag histogram:\n");
			for (const auto& b : bins) {
				std::printf("  flag=0x%08x  count=%zu  first_id=%d\n",
					b.first, b.second.count, b.second.first_id);
			}
		}
	}

	// Dump images.rel rows for the requested image_ids so we can
	// confirm that SD image_id directly indexes an HD anim_num
	// (i.e. no separate "shadow" data structure needed).
	if (image_ids_arg) {
		HANDLE h = NULL;
		if (!CascOpenFile(hStorage, "images.rel", 0, CASC_OPEN_BY_NAME, &h)) {
			std::fprintf(stderr, "open images.rel failed err=%u\n",
				GetCascError());
		} else {
			DWORD sz = CascGetFileSize(h, NULL);
			std::vector<unsigned char> buf(sz);
			DWORD got = 0;
			CascReadFile(h, buf.data(), sz, &got);
			CascCloseFile(h);
			size_t n_entries = got / 8;
			std::printf("\nimages.rel: %zu entries\n", n_entries);
			// Parse requested ids.
			std::string ids(image_ids_arg);
			size_t p = 0;
			while (p < ids.size()) {
				size_t comma = ids.find(',', p);
				if (comma == std::string::npos) comma = ids.size();
				int id = std::atoi(ids.substr(p, comma - p).c_str());
				if (id >= 0 && (size_t)id < n_entries) {
					const auto* row = buf.data() + id * 8;
					unsigned flag = (unsigned)row[0]
						| ((unsigned)row[1] << 8)
						| ((unsigned)row[2] << 16)
						| ((unsigned)row[3] << 24);
					unsigned anim = (unsigned)row[4]
						| ((unsigned)row[5] << 8)
						| ((unsigned)row[6] << 16)
						| ((unsigned)row[7] << 24);
					std::printf("  image_id=%d  flag=0x%08x  anim_num=%u\n",
						id, flag, anim);
				} else {
					std::printf("  image_id=%d  OUT OF RANGE\n", id);
				}
				p = comma + 1;
			}
		}
	}

	// Dump the .anim header for a specific anim_num (bypasses
	// images.rel routing so we can inspect orphan .anim files).
	if (anim_range_arg) {
		int lo = 0, hi = 0;
		bool is_range = std::sscanf(anim_range_arg, "%d,%d", &lo, &hi) == 2;
		if (is_range && lo == hi) {
			// Single anim: dump header + layer table.
			char name[64];
			std::snprintf(name, sizeof(name),
				"anim\\main_%03d.anim", lo);
			HANDLE h = NULL;
			if (CascOpenFile(hStorage, name, 0, CASC_OPEN_BY_NAME, &h)) {
				DWORD sz = CascGetFileSize(h, NULL);
				std::vector<unsigned char> buf(sz);
				DWORD got = 0;
				CascReadFile(h, buf.data(), sz, &got);
				CascCloseFile(h);
				// Minimal parse of the anim header. Format:
				//   0x00 magic 'ANIM' (4 bytes)
				//   0x04 version (u16)
				//   0x06 pad? / flags?
				//   0x08 layer_count (u16) at offset 0x0A per anim_dump
				// We just print the raw first bytes + guess sprite_wh
				// at 0x150 as anim_dump does.
				if (got >= 0x160) {
					unsigned magic = (unsigned)buf[0]
						| ((unsigned)buf[1] << 8)
						| ((unsigned)buf[2] << 16)
						| ((unsigned)buf[3] << 24);
					unsigned version = (unsigned)buf[4]
						| ((unsigned)buf[5] << 8);
					unsigned layers = (unsigned)buf[0x0A]
						| ((unsigned)buf[0x0B] << 8);
					// Body section starts at 0x14C:
					//   0x14C  u16 frame_count
					//   0x14E  u16 unk
					//   0x150  u16 sprite_w
					//   0x152  u16 sprite_h
					unsigned frame_count = (unsigned)buf[0x14C]
						| ((unsigned)buf[0x14D] << 8);
					unsigned sw = (unsigned)buf[0x150]
						| ((unsigned)buf[0x151] << 8);
					unsigned sh = (unsigned)buf[0x152]
						| ((unsigned)buf[0x153] << 8);
					std::printf("\nanim %d: size=%u magic=0x%08x "
						"version=0x%04x layers=%u\n"
						"  sprite=%ux%u  frame_count=%u\n"
						"  layer_names @ 0x0C..0x148 (32B each):\n",
						lo, (unsigned)got, magic, version, layers,
						sw, sh, frame_count);
					for (unsigned i = 0; i < layers && i < 10; ++i) {
						const unsigned char* n32 = buf.data() + 0x0C + i * 32;
						// Layer descriptors that we can peek at later --
						// for now just names.
						std::printf("    layer[%u] name='%.32s'\n",
							i, (const char*)n32);
					}
				} else {
					std::printf("anim %d: file too small (%u)\n",
						lo, (unsigned)got);
				}
				goto done_probe;
			}
		}
	}
	// Enumerate anim\main_<N>.anim over a range. Confirms which
	// anim numbers actually have files (whether or not images.rel
	// references them). If an anim exists but no images.rel row
	// points at it, that means Blizzard references it from some
	// other index we haven't found -- likely the shadow-anim path.
	if (anim_range_arg) {
		int lo = 0, hi = 0;
		if (std::sscanf(anim_range_arg, "%d,%d", &lo, &hi) == 2 && lo <= hi) {
			std::printf("\nanim range %d..%d (fc = frame_count, "
				"L = layer count, L1 = first layer name):\n", lo, hi);
			for (int n = lo; n <= hi; ++n) {
				char name[64];
				std::snprintf(name, sizeof(name),
					"anim\\main_%03d.anim", n);
				HANDLE h = NULL;
				if (!CascOpenFile(hStorage, name, 0, CASC_OPEN_BY_NAME, &h))
					continue;
				DWORD sz = CascGetFileSize(h, NULL);
				std::vector<unsigned char> buf(sz);
				DWORD got = 0;
				CascReadFile(h, buf.data(), sz, &got);
				CascCloseFile(h);
				if (got < 0x160) {
					std::printf("  anim=%d  size=%u  (too small)\n",
						n, (unsigned)sz);
					continue;
				}
				unsigned layers = (unsigned)buf[0x0A]
					| ((unsigned)buf[0x0B] << 8);
				unsigned fc = (unsigned)buf[0x14C]
					| ((unsigned)buf[0x14D] << 8);
				unsigned sw = (unsigned)buf[0x150]
					| ((unsigned)buf[0x151] << 8);
				unsigned sh = (unsigned)buf[0x152]
					| ((unsigned)buf[0x153] << 8);
				char l1[33] = {0};
				std::memcpy(l1, buf.data() + 0x0C, 32);
				std::printf("  anim=%d  size=%-8u  fc=%-4u  L=%u  "
					"sprite=%ux%u  L1='%s'\n",
					n, (unsigned)sz, fc, layers, sw, sh, l1);
			}
		}
	}

done_probe:
	CascCloseStorage(hStorage);
	return 0;
}
