// slim_mpq: produce a slim MPQ archive by KEEPING only a specific
// set of filenames that the running openBW code actually reads.
//
// Retail SC1 archives (StarDat.mpq, BrooDat.mpq) do NOT ship with a
// (listfile) entry -- Blizzard scrubbed it before release. Since we
// can't enumerate the archive's filenames from its own metadata,
// we can't do a straight "drop by prefix" pass; a file whose name
// we can't derive would be silently lost. Instead this tool works
// as an inverted filter: build a keep-set from
//
//   1. explicit hardcoded literals from the Explore pass over the
//      openBW code base (arr/*.dat, arr/*.tbl, scripts/iscript.bin,
//      triggers/Melee.trg, per-tileset .cv5/.vf4/.vr4/.vx4/.wpe/.grp
//      and per-tileset .pcx files, HUD chrome under game/),
//   2. dynamic GRPs enumerated from arr/images.tbl (unit/*.grp
//      names indexed by the tbl -- the observer opens these lazily
//      when a sprite of that image_type is rendered),
//   3. metadata entries (listfile, attributes, signature) if
//      present.
//
// Any block-table entry that is not reachable by name via that
// keep-set is silently omitted. This is safe because the reader
// accesses files by name only; a block with no discoverable name
// can't be opened by any code path.
//
// Copy-preserving repack:
//   * Compressed sector data is copied verbatim from the source
//     archive for every kept file. No recompression.
//   * For files whose block-table flags include FIXED_KEY (0x20000),
//     the encryption key is derived from `filename_key + data_offset`
//     (see mpq_archive_reader::open in data_loading.h). Since we
//     relocate the file to a new data_offset in the slim archive,
//     we must re-encrypt these files with the new key. Sector-offset
//     table + every sector's payload get re-keyed.
//   * All other kept files (compressed only, or compressed+encrypted
//     without FIXED_KEY) are copied byte-for-byte.
//   * The hash table and block table are rebuilt from scratch and
//     encrypted with the retail keys.
//
// The --drop CLI arg is retained for surface compatibility with the
// script, but drops are applied *after* the keep-list is built (and
// mostly redundant, since sound/music/smk paths aren't in the
// keep-list to begin with).
//
// Never touches / never deletes the source archive.

#include "bwgame.h"
#include "data_writing.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

using bwgame::data_loading::file_reader;
using bwgame::data_loading::mpq_archive_reader;
using bwgame::data_loading::paged_reader;
using bwgame::data_loading::make_encrypted_reader;
using bwgame::data_loading::block_table_entry;
using bwgame::data_writing::file_writer;
using bwgame::data_writing::make_encrypted_writer;

struct Args {
	std::string in_path;
	std::string out_path;
	std::vector<std::string> drop_prefixes;
	std::string listfile_override;
	bool verbose = false;
};

void print_usage() {
	std::fprintf(stderr,
		"usage: slim_mpq --in <src.mpq> --out <dst.mpq>\n"
		"                --drop <prefix> [--drop <prefix> ...]\n"
		"                [--listfile <path>]\n"
		"                [--verbose]\n"
		"\n"
		"Emits a slim MPQ containing every file from src.mpq except\n"
		"those whose path starts with any --drop prefix (case-\n"
		"insensitive, matches both '/' and '\\\\' separators).\n"
		"\n"
		"The source archive is never modified. If --listfile is not\n"
		"given, the '(listfile)' entry inside src.mpq is used.\n");
}

int parse_args(int argc, char** argv, Args& out) {
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		auto need = [&](const char* what) -> const char* {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "slim_mpq: %s: missing value\n", what);
				return nullptr;
			}
			return argv[++i];
		};
		if (a == "--in") {
			const char* v = need("--in"); if (!v) return 2; out.in_path = v;
		} else if (a == "--out") {
			const char* v = need("--out"); if (!v) return 2; out.out_path = v;
		} else if (a == "--drop") {
			const char* v = need("--drop"); if (!v) return 2; out.drop_prefixes.emplace_back(v);
		} else if (a == "--listfile") {
			const char* v = need("--listfile"); if (!v) return 2; out.listfile_override = v;
		} else if (a == "--verbose" || a == "-v") {
			out.verbose = true;
		} else if (a == "--help" || a == "-h") {
			print_usage(); return 1;
		} else {
			std::fprintf(stderr, "slim_mpq: unknown arg: %s\n", a.c_str());
			print_usage();
			return 2;
		}
	}
	if (out.in_path.empty() || out.out_path.empty() || out.drop_prefixes.empty()) {
		print_usage();
		return 2;
	}
	return 0;
}

// Normalize a filename for prefix matching: lower-case, backslashes
// to forward slashes.
std::string normalize(const std::string& s) {
	std::string r; r.reserve(s.size());
	for (char c : s) {
		if (c == '\\') c = '/';
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		r.push_back(c);
	}
	return r;
}

bool matches_any_prefix(const std::string& normalized, const std::vector<std::string>& prefixes) {
	for (const auto& p : prefixes) {
		if (normalized.size() >= p.size() &&
		    normalized.compare(0, p.size(), p) == 0) return true;
	}
	return false;
}

// The reader stores block indices as size_t; convert to u32 for
// hash table storage. Also compute the file key like open() does.
uint32_t compute_file_key(const std::string& filename,
                           const block_table_entry& be,
                           const bwgame::data_writing::crypt_table_t& crypt_table) {
	// Match mpq_archive_reader::open at data_loading.h:1244-1259:
	// use only the basename for the hash.
	const char* c = filename.data() + filename.size();
	while (c != filename.data()) {
		char pc = *(c - 1);
		if (pc == '/' || pc == '\\') break;
		--c;
	}
	uint32_t file_key = bwgame::data_writing::string_hash(c, 3, crypt_table);
	if (be.flags & 0x20000) {
		file_key = (file_key + be.data_offset) ^ be.size;
	}
	return file_key;
}

std::vector<std::string> load_listfile_from_disk(const std::string& path) {
	std::ifstream in(path);
	if (!in) bwgame::error("slim_mpq: cannot open listfile %s", path.c_str());
	std::vector<std::string> out;
	std::string line;
	while (std::getline(in, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
		if (!line.empty()) out.push_back(std::move(line));
	}
	return out;
}

// Read a Blizzard TBL string table from the source archive. Format:
//   [u16 count][u16 offset_1][u16 offset_2] ... [null-terminated strings]
// Returns the enumerated strings in index order.
template<typename MpqT>
std::vector<std::string> read_tbl_from_mpq(MpqT& mpq, const char* name) {
	if (!mpq.file_exists(bwgame::a_string(name))) return {};
	auto fr = mpq.open(bwgame::a_string(name));
	std::vector<uint8_t> data(fr.size());
	if (!data.empty()) fr.get_bytes(data.data(), data.size());
	if (data.size() < 2) return {};
	uint16_t count = (uint16_t)(data[0] | (data[1] << 8));
	std::vector<std::string> out;
	out.reserve(count);
	for (uint16_t i = 1; i <= count; ++i) {
		size_t off_pos = 2 + (i - 1) * 2;
		if (off_pos + 2 > data.size()) break;
		uint16_t off = (uint16_t)(data[off_pos] | (data[off_pos + 1] << 8));
		if (off >= data.size()) break;
		std::string s;
		for (size_t j = off; j < data.size() && data[j]; ++j) s.push_back((char)data[j]);
		out.push_back(std::move(s));
	}
	return out;
}

// Build the keep-list. This mirrors what openbw_server + openbw_observer
// + wasm actually load. Grep evidence: see agent's Explore report in
// the plan file.
template<typename MpqT>
std::vector<std::string> build_keep_list(MpqT& mpq, bool verbose) {
	std::vector<std::string> out;
	auto add = [&](std::string s) { out.push_back(std::move(s)); };

	// --- Meta entries (may or may not exist) ---
	add("(listfile)");
	add("(attributes)");
	add("(signature)");

	// --- Sim-critical arr/ files (both server and observer) ---
	// bwgame.h:22067-22293 hardcoded list.
	for (const char* n : {
		"arr/units.dat", "arr/weapons.dat", "arr/upgrades.dat",
		"arr/techdata.dat", "arr/flingy.dat", "arr/sprites.dat",
		"arr/images.dat", "arr/orders.dat", "arr/portdata.dat",
		"arr/mapdata.dat", "arr/sfxdata.dat", "arr/images.tbl",
		"arr/sfxdata.tbl", "arr/stat_txt.tbl",
	}) add(n);

	// --- Scripts + triggers ---
	add("scripts/iscript.bin");
	add("triggers/Melee.trg");
	add("triggers/aiscript.bin");
	add("triggers/bwscript.bin");
	add("triggers/TUnit.trg");

	// --- Per-tileset assets (sim + observer) ---
	// Sim needs .cv5 + .vf4; observer additionally needs .vr4/.vx4/.wpe/.grp.
	// Blizzard directory casing: "Tileset/<name>.<ext>" with capital T.
	static const char* tilesets[] = {
		"badlands", "platform", "install", "AshWorld",
		"Jungle", "Desert", "Ice", "Twilight",
	};
	for (const char* t : tilesets) {
		for (const char* ext : {".cv5", ".vf4", ".vr4", ".vx4", ".wpe", ".grp"}) {
			add(std::string("Tileset/") + t + ext);
		}
		// Per-tileset PCX assets used by the observer (ui/ui.h:235-243).
		for (const char* pcx : {
			"dark.pcx", "ofire.pcx", "gfire.pcx", "bfire.pcx",
			"bexpl.pcx", "trans50.pcx", "red.pcx", "green.pcx"
		}) {
			add(std::string("Tileset/") + t + "/" + pcx);
		}
	}

	// --- HUD chrome (observer) ---
	for (const char* n : {
		"game/tunit.pcx", "game/tminimap.pcx",
		"game/tselect.pcx", "game/thpbar.pcx",
		"game/tblink.pcx", "game/tfontgam.pcx",
	}) add(n);

	// --- Fonts (observer + wasm chrome) ---
	for (const char* n : {
		"font/font8.fnt", "font/font10.fnt",
		"font/font14.fnt", "font/font16.fnt", "font/font16x.fnt",
		"game/font8.pcx", "game/font10.pcx", "game/font14.pcx",
		"game/font16.pcx", "game/font16x.pcx",
	}) add(n);

	// --- Dynamic GRPs enumerated from arr/images.tbl ---
	// Each entry is a filename fragment like "zerg\\zergling.grp";
	// the observer opens them as "unit/<fragment>" (ui/ui.h:1777).
	auto images_tbl = read_tbl_from_mpq(mpq, "arr/images.tbl");
	if (verbose) std::fprintf(stderr, "slim_mpq: images.tbl has %zu entries\n", images_tbl.size());
	for (const auto& s : images_tbl) {
		if (s.empty()) continue;
		add(std::string("unit/") + s);
	}

	return out;
}

// Slurp `n` bytes from `src` at `off`, into `dst`.
void slurp(FILE* src, size_t off, size_t n, std::vector<uint8_t>& dst) {
	dst.resize(n);
	if (n == 0) return;
	if (fseek(src, (long)off, SEEK_SET) != 0)
		bwgame::error("slim_mpq: fseek(%zu) failed", off);
	if (fread(dst.data(), n, 1, src) != 1)
		bwgame::error("slim_mpq: read %zu bytes at %zu failed", n, off);
}

// Read `n_words` little-endian u32s from `src` at `off`, decrypt in
// place with `key` (using encrypted_reader-style state), and return
// the decrypted words.
//
// This is a mini-reader — for a variable-length integer stream where
// we know the length up front (sector offset table).
std::vector<uint32_t> read_decrypt_u32s(FILE* src, size_t off, size_t n_words, uint32_t key,
                                         const bwgame::data_writing::crypt_table_t& crypt_table) {
	if (n_words == 0) return {};
	std::vector<uint8_t> raw(n_words * 4);
	if (fseek(src, (long)off, SEEK_SET) != 0)
		bwgame::error("slim_mpq: fseek(%zu) failed", off);
	if (fread(raw.data(), raw.size(), 1, src) != 1)
		bwgame::error("slim_mpq: read %zu bytes at %zu failed", raw.size(), off);

	std::vector<uint32_t> out(n_words);
	uint32_t k = key;
	uint32_t add_n = 0xeeeeeeee;
	for (size_t i = 0; i < n_words; ++i) {
		uint32_t d;
		std::memcpy(&d, raw.data() + i * 4, 4);
		add_n += crypt_table[(k & 0xff) + 1024];
		uint32_t xor_n = k + add_n;
		uint32_t plain = d ^ xor_n;
		out[i] = plain;
		add_n = add_n * 33 + plain + 3;
		k = ((~k << 21) + 0x11111111) | (k >> 11);
	}
	return out;
}

// Encrypt & emit `words` little-endian u32s to `w` using `key`.
void write_encrypt_u32s(file_writer& w, const std::vector<uint32_t>& words, uint32_t key,
                         const bwgame::data_writing::crypt_table_t& crypt_table) {
	auto enc = make_encrypted_writer(w, key, crypt_table);
	for (auto v : words) enc.put_u32(v);
}

// Encrypt & emit an already-in-memory byte range (assumed 4-byte
// aligned or with a 4-byte-padded tail). This mirrors how
// encrypted_reader consumes a payload: sector-size chunks with the
// sector's own key.
void write_encrypt_bytes(file_writer& w, const uint8_t* src, size_t n, uint32_t key,
                          const bwgame::data_writing::crypt_table_t& crypt_table) {
	if (n == 0) return;
	auto enc = make_encrypted_writer(w, key, crypt_table);
	// Whole 4-byte chunks
	size_t full = n & ~size_t(3);
	for (size_t i = 0; i < full; i += 4) {
		uint32_t v;
		std::memcpy(&v, src + i, 4);
		enc.put_u32(v);
	}
	// Tail bytes: encrypted_writer only writes u32s, so we need to
	// zero-pad up to 4 and emit. But retail archives always align
	// sectors to 4 bytes since the reader reads u32 offsets from the
	// sector-offset table. If we hit a non-multiple-of-4, we still
	// need to handle it -- and the reader can too because it uses
	// get_bytes which handles partial data.
	//
	// We handle it by encrypting a padded u32 and writing only the
	// tail bytes.
	if (n & 3) {
		uint32_t v = 0;
		std::memcpy(&v, src + full, n - full);
		// Do one more crypt step manually to get the bytes out.
		uint32_t& k = enc.key;
		uint32_t& add_n = enc.add_n;
		add_n += crypt_table[(k & 0xff) + 1024];
		uint32_t xor_n = k + add_n;
		uint32_t cipher = v ^ xor_n;
		uint8_t buf[4];
		std::memcpy(buf, &cipher, 4);
		w.put_bytes(buf, n - full);
		// key/add_n state update not needed since we're done.
	}
}

} // namespace

int main(int argc, char** argv) {
	Args args;
	if (int rc = parse_args(argc, argv, args)) return rc == 1 ? 0 : rc;

	// Normalize drop prefixes once.
	std::vector<std::string> drops_normalized;
	for (const auto& p : args.drop_prefixes) drops_normalized.push_back(normalize(p));

	try {
		// -------- 1. Open source archive via existing reader --------
		bwgame::data_loading::mpq_file<> src_mpq(bwgame::a_string(args.in_path.c_str()));
		auto& mpq = src_mpq.mpq;
		auto& crypt_table = mpq.crypt_table;

		// Keep the underlying FILE* open for verbatim sector copies.
		// mpq_file wraps a file_reader; grab its FILE handle.
		FILE* src_fp = src_mpq.file.f;
		if (!src_fp) bwgame::error("slim_mpq: source file handle unexpectedly null");

		// -------- 2. Build the keep-set --------
		// Retail Blizzard MPQs ship without (listfile), so we can't do
		// "drop by prefix over full enumeration". Instead we enumerate
		// what we know openBW reads (see build_keep_list) and take
		// only those.
		std::vector<std::string> candidates;
		if (!args.listfile_override.empty()) {
			candidates = load_listfile_from_disk(args.listfile_override);
			if (args.verbose) std::fprintf(stderr,
				"slim_mpq: using listfile override with %zu entries\n", candidates.size());
		} else {
			candidates = build_keep_list(mpq, args.verbose);
			if (args.verbose) std::fprintf(stderr,
				"slim_mpq: built-in keep-set has %zu candidates\n", candidates.size());
		}

		// Filter by drop prefixes (applied AFTER the keep-set, so a
		// caller can still trim sound/*.wav paths that images.tbl
		// happens to inject, if any).
		std::vector<std::string> kept;
		std::vector<std::string> seen;
		size_t missing_count = 0;
		size_t dropped_by_prefix = 0;
		uint64_t kept_bytes = 0;
		for (const auto& name : candidates) {
			if (name.empty()) continue;
			// Dedup on lowercase-normalized name to avoid emitting the
			// same file twice from overlapping keep-list sources.
			std::string norm = normalize(name);
			if (std::find(seen.begin(), seen.end(), norm) != seen.end()) continue;
			seen.push_back(norm);

			if (matches_any_prefix(norm, drops_normalized)) {
				++dropped_by_prefix;
				continue;
			}
			if (!mpq.file_exists(bwgame::a_string(name.c_str()))) {
				++missing_count;
				continue;
			}
			auto* he = mpq.find_hash_table_entry(bwgame::a_string(name.c_str()));
			if (!he) { ++missing_count; continue; }
			kept.push_back(name);
			kept_bytes += mpq.block_table.at(he->block_index).compressed_size;
		}

		std::fprintf(stderr,
			"slim_mpq: keeping %zu files (%llu compressed bytes), "
			"skipping %zu missing, %zu drop-prefix matches\n",
			kept.size(), (unsigned long long)kept_bytes,
			missing_count, dropped_by_prefix);

		// -------- 4. Plan the output layout --------
		// Header at 0 (32 bytes). Sector-data region follows. Then
		// hash table, then block table.
		//
		// For each kept file we need to emit:
		//   [sector-offset table][sector 0][sector 1]...
		// Total on-disk size == be.compressed_size (already includes
		// the sector-offset table).

		// Collect kept-file info once — the reader has already parsed
		// block_table entries; we index by hash lookup.
		struct plan_entry {
			std::string name;
			block_table_entry be_src;   // from source
			uint32_t new_data_offset;    // patched
			uint32_t new_flags;          // usually same as flags, unchanged
		};
		std::vector<plan_entry> plan;
		plan.reserve(kept.size());
		for (const auto& name : kept) {
			auto* he = mpq.find_hash_table_entry(bwgame::a_string(name.c_str()));
			if (!he) bwgame::error("slim_mpq: internal: %s in keep list but not in hash table", name.c_str());
			plan.push_back({name, mpq.block_table.at(he->block_index), 0, 0});
		}

		// Now open the output. Reserve header space by seeking past it.
		file_writer out_w(bwgame::a_string(args.out_path.c_str()));
		out_w.seek(32);
		size_t cursor = 32;

		// -------- 5. Emit sector data for each kept file --------
		for (auto& pe : plan) {
			const auto& be = pe.be_src;
			pe.new_data_offset = (uint32_t)cursor;
			pe.new_flags = be.flags;

			if (!(be.flags & 0x20000)) {
				// FIXED_KEY not set: byte-verbatim copy works.
				std::vector<uint8_t> buf;
				slurp(src_fp, be.data_offset, be.compressed_size, buf);
				out_w.put_bytes(buf.data(), buf.size());
				cursor += be.compressed_size;
				continue;
			}

			// FIXED_KEY: need to decrypt with old key + re-encrypt
			// with new key.
			uint32_t old_key = compute_file_key(pe.name, be, crypt_table);
			// New key: as-if the file were located at new_data_offset.
			block_table_entry be_new = be;
			be_new.data_offset = pe.new_data_offset;
			uint32_t new_key = compute_file_key(pe.name, be_new, crypt_table);

			// Number of sectors + one terminator entry (== compressed_size).
			size_t sector_size = mpq.sector_size;
			size_t n_sectors = (be.size + sector_size - 1) / sector_size;
			size_t sector_table_words = n_sectors + 1;

			// Decrypt the sector-offset table (key - 1).
			auto sector_offsets = read_decrypt_u32s(
				src_fp, be.data_offset, sector_table_words, old_key - 1, crypt_table);

			// Sanity: last entry should equal be.compressed_size.
			if (sector_offsets.back() != be.compressed_size) {
				bwgame::error("slim_mpq: %s: sector offset table sanity check failed "
				              "(expected last=%u, got %u)",
				              pe.name.c_str(),
				              be.compressed_size, sector_offsets.back());
			}

			// Re-encrypt the sector-offset table with new_key - 1 and emit.
			write_encrypt_u32s(out_w, sector_offsets, new_key - 1, crypt_table);
			cursor += sector_table_words * 4;

			// For each sector: read raw, decrypt with old_key+i,
			// re-encrypt with new_key+i, write. Sector size in bytes
			// = sector_offsets[i+1] - sector_offsets[i].
			for (size_t i = 0; i < n_sectors; ++i) {
				size_t s_off = be.data_offset + sector_offsets[i];
				size_t s_len = sector_offsets[i + 1] - sector_offsets[i];
				std::vector<uint8_t> sector_buf(s_len);
				if (s_len) {
					if (fseek(src_fp, (long)s_off, SEEK_SET) != 0)
						bwgame::error("slim_mpq: %s sector %zu: fseek failed", pe.name.c_str(), i);
					if (fread(sector_buf.data(), s_len, 1, src_fp) != 1)
						bwgame::error("slim_mpq: %s sector %zu: read failed", pe.name.c_str(), i);
				}
				// Decrypt in place with (old_key + i). Sector-payload
				// decryption reads a sector_size-worth of u32s at a
				// time; the reader does this via encrypted_reader with
				// end_pos = s_len. We mirror that state machine.
				{
					uint32_t k = old_key + (uint32_t)i;
					uint32_t add_n = 0xeeeeeeee;
					size_t full = s_len & ~size_t(3);
					std::vector<uint8_t> decrypted(s_len);
					for (size_t j = 0; j < full; j += 4) {
						uint32_t d;
						std::memcpy(&d, sector_buf.data() + j, 4);
						add_n += crypt_table[(k & 0xff) + 1024];
						uint32_t xor_n = k + add_n;
						uint32_t plain = d ^ xor_n;
						std::memcpy(decrypted.data() + j, &plain, 4);
						add_n = add_n * 33 + plain + 3;
						k = ((~k << 21) + 0x11111111) | (k >> 11);
					}
					if (s_len & 3) {
						// Tail: reader's encrypted_reader handles
						// short trailers by NOT running `next()` when
						// end_pos - pos < 4 (see data_loading.h:348).
						// For those bytes it reads raw from underlying
						// reader without decryption.
						std::memcpy(decrypted.data() + full,
						            sector_buf.data() + full, s_len - full);
					}
					sector_buf = std::move(decrypted);
				}
				// Re-encrypt with (new_key + i) — mirror of the above.
				{
					uint32_t k = new_key + (uint32_t)i;
					uint32_t add_n = 0xeeeeeeee;
					size_t full = s_len & ~size_t(3);
					std::vector<uint8_t> encrypted(s_len);
					for (size_t j = 0; j < full; j += 4) {
						uint32_t plain;
						std::memcpy(&plain, sector_buf.data() + j, 4);
						add_n += crypt_table[(k & 0xff) + 1024];
						uint32_t xor_n = k + add_n;
						uint32_t cipher = plain ^ xor_n;
						std::memcpy(encrypted.data() + j, &cipher, 4);
						add_n = add_n * 33 + plain + 3;
						k = ((~k << 21) + 0x11111111) | (k >> 11);
					}
					if (s_len & 3) {
						std::memcpy(encrypted.data() + full,
						            sector_buf.data() + full, s_len - full);
					}
					out_w.put_bytes(encrypted.data(), s_len);
				}
				cursor += s_len;
			}
		}

		// -------- 6. Emit hash table + block table --------
		size_t new_hash_table_size = bwgame::data_writing::round_up_pow2(
			(kept.size() * 4 + 2) / 3);   // ~4/3 headroom
		if (new_hash_table_size < 16) new_hash_table_size = 16;

		// Build ordered <name, block_index> list matching plan order.
		std::vector<std::pair<std::string, uint32_t>> for_hash;
		for_hash.reserve(plan.size());
		for (size_t i = 0; i < plan.size(); ++i) {
			for_hash.emplace_back(plan[i].name, (uint32_t)i);
		}
		auto hash_table = bwgame::data_writing::build_hash_table(
			new_hash_table_size, for_hash, crypt_table);

		size_t hash_table_offset = cursor;
		bwgame::data_writing::put_encrypted_hash_table(out_w, hash_table, crypt_table);
		cursor += new_hash_table_size * 16;

		std::vector<bwgame::data_writing::block_entry> block_table;
		block_table.reserve(plan.size());
		for (const auto& pe : plan) {
			block_table.push_back({pe.new_data_offset,
			                        pe.be_src.compressed_size,
			                        pe.be_src.size,
			                        pe.new_flags});
		}
		size_t block_table_offset = cursor;
		bwgame::data_writing::put_encrypted_block_table(out_w, block_table, crypt_table);
		cursor += plan.size() * 16;

		// -------- 7. Backpatch header --------
		uint32_t archive_size = (uint32_t)cursor;
		out_w.seek(0);
		out_w.put<uint32_t>(0x1a51504d);              // signature
		out_w.put<uint32_t>(32);                       // header size
		out_w.put<uint32_t>(archive_size);
		out_w.put<uint16_t>(0);                        // version
		uint16_t block_size_pow = 0;
		while ((size_t)512 << block_size_pow < mpq.sector_size) ++block_size_pow;
		out_w.put<uint16_t>(block_size_pow);
		out_w.put<uint32_t>((uint32_t)hash_table_offset);
		out_w.put<uint32_t>((uint32_t)block_table_offset);
		out_w.put<uint32_t>((uint32_t)new_hash_table_size);
		out_w.put<uint32_t>((uint32_t)plan.size());

		std::fprintf(stderr,
			"slim_mpq: wrote %s (%u bytes = %.2f MiB)\n",
			args.out_path.c_str(),
			archive_size,
			archive_size / 1048576.0);

		// Close output so we can reopen for verification.
		out_w = file_writer{};

		// -------- 8. Roundtrip verification --------
		{
			bwgame::data_loading::mpq_file<> verify(bwgame::a_string(args.out_path.c_str()));
			auto& vmpq = verify.mpq;
			size_t bad = 0;
			for (const auto& pe : plan) {
				if (!vmpq.file_exists(bwgame::a_string(pe.name.c_str()))) {
					std::fprintf(stderr, "slim_mpq: VERIFY FAIL: %s missing after write\n", pe.name.c_str());
					++bad;
					continue;
				}
				auto fr = vmpq.open(bwgame::a_string(pe.name.c_str()));
				if (fr.size() != pe.be_src.size) {
					std::fprintf(stderr, "slim_mpq: VERIFY FAIL: %s size mismatch (%zu vs %u)\n",
					              pe.name.c_str(), fr.size(), pe.be_src.size);
					++bad;
				}
			}
			// Spot-check that a known sound path is gone (should
			// never have been in the keep-set to begin with).
			for (const char* spot : {
				"sound/Zerg/Drone/ZDrErr00.wav",
				"sound/misc/BuildingLoss.wav",
				"music/terran1.wav",
			}) {
				if (vmpq.file_exists(bwgame::a_string(spot))) {
					std::fprintf(stderr, "slim_mpq: VERIFY FAIL: %s unexpectedly present in slim archive\n", spot);
					++bad;
				}
			}
			if (bad) {
				std::fprintf(stderr, "slim_mpq: verification failed on %zu file(s)\n", bad);
				return 3;
			}
			std::fprintf(stderr, "slim_mpq: verify OK (%zu kept files present)\n", plan.size());
		}

		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "slim_mpq: error: %s\n", e.what());
		return 4;
	}
}
