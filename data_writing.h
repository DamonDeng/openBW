// Writer counterpart to data_loading.h. Used by tools/slim_mpq.cpp
// to emit slim MPQ archives (with unwanted files removed) that the
// existing bwgame::data_loading::mpq_archive_reader can read
// unchanged.
//
// Design notes:
//  - We do NOT re-compress sector data. Compressed bytes are copied
//    from the source archive verbatim, so no PKWARE Implode / zlib /
//    Huffman encoder is needed.
//  - We DO have to re-encrypt sector data (and the per-file sector-
//    offset table) for files whose block-table flags include
//    0x20000 ("FIXED_KEY"). Those files derive their encryption key
//    from `file_key + data_offset`, and we're relocating them to
//    new offsets in the slim archive. The `encrypted_writer` below
//    is the symmetric partner of `data_loading::encrypted_reader`.
//  - No compression/re-encoding means the slim MPQ ends up slightly
//    less dense than a StormLib-recompressed archive would be, but
//    correctness is straightforward.

#ifndef BWGAME_DATA_WRITING_H
#define BWGAME_DATA_WRITING_H

#include "data_loading.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace bwgame {
namespace data_writing {

using data_loading::crypt_table_t;
using data_loading::get_crypt_table;
using data_loading::string_hash;
// error() and a_string live directly in the bwgame:: namespace (see
// util.h); we're inside bwgame::data_writing so lookup finds them
// implicitly for unqualified use.

// Same as data_loading::default_little_endian: MPQ is a Windows
// format so on-disk multibyte fields are always little-endian.
static const bool default_little_endian = true;

// Native filesystem writer, symmetric with data_loading::file_reader.
struct file_writer {
	a_string filename;
	FILE* f = nullptr;

	file_writer() = default;
	explicit file_writer(a_string filename) { open(std::move(filename)); }
	~file_writer() { if (f) fclose(f); }

	file_writer(const file_writer&) = delete;
	file_writer& operator=(const file_writer&) = delete;
	file_writer(file_writer&& n) { f = n.f; n.f = nullptr; filename = std::move(n.filename); }
	file_writer& operator=(file_writer&& n) {
		std::swap(f, n.f);
		std::swap(filename, n.filename);
		return *this;
	}

	void open(a_string filename_) {
		if (f) fclose(f);
		f = fopen(filename_.c_str(), "wb+");
		if (!f) error("file_writer: failed to open %s for writing", filename_.c_str());
		filename = std::move(filename_);
	}

	void put_bytes(const uint8_t* src, size_t n) {
		if (n && fwrite(src, n, 1, f) != 1) {
			error("file_writer: %s: write error", filename);
		}
	}

	template<typename T, bool little_endian = default_little_endian>
	void put(T value) {
		uint8_t buf[sizeof(T)];
		data_loading::set_value_at<little_endian>(buf, value);
		put_bytes(buf, sizeof(T));
	}

	void seek(size_t offset) {
		if ((size_t)(long)offset != offset || fseek(f, (long)offset, SEEK_SET)) {
			error("file_writer: %s: failed to seek to offset %zu", filename, offset);
		}
	}

	size_t tell() const {
		return (size_t)ftell(f);
	}
};

// Symmetric partner of data_loading::encrypted_reader.
//
// Encryption is a 32-bit-block XOR stream cipher whose key state
// depends on the *plaintext* value at each step (see reader:
// `add_n = add_n * 33 + data + 3;` where `data` is the decrypted
// plaintext). The writer mirrors this: emit ciphertext =
// plaintext ^ (key + add_n), then advance state using plaintext.
//
// Only 4-byte-aligned writes are supported. That covers everything
// MPQ needs (hash table, block table, sector-offset table all in
// 4-byte units) and matches how the reader consumes data.
template<typename base_writer_T, bool default_little_endian = true>
struct encrypted_writer {
	base_writer_T& writer;
	uint32_t key;
	uint32_t add_n = 0xeeeeeeee;
	const crypt_table_t& crypt_table;

	encrypted_writer(base_writer_T& writer, uint32_t key, const crypt_table_t& crypt_table)
		: writer(writer), key(key), crypt_table(crypt_table) {}

	// Encrypt one 32-bit plaintext word and emit it.
	void put_u32(uint32_t plaintext) {
		add_n += crypt_table[(key & 0xff) + 1024];
		uint32_t xor_n = key + add_n;
		uint32_t cipher = plaintext ^ xor_n;
		writer.template put<uint32_t, true>(cipher);
		add_n = add_n * 33 + plaintext + 3;
		key = ((~key << 21) + 0x11111111) | (key >> 11);
	}

	// Encrypt a 4-byte-aligned byte range. `n` must be a multiple
	// of 4. Bytes are interpreted little-endian to match reader.
	void put_bytes(const uint8_t* src, size_t n) {
		if (n & 3) error("encrypted_writer: put_bytes size %zu not a multiple of 4", n);
		for (size_t i = 0; i < n; i += 4) {
			uint32_t v = data_loading::value_at<uint32_t, true>(src + i);
			put_u32(v);
		}
	}
};

template<typename base_writer_T>
auto make_encrypted_writer(base_writer_T& writer, uint32_t key, const crypt_table_t& crypt_table) {
	return encrypted_writer<base_writer_T>(writer, key, crypt_table);
}

// -----------------------------------------------------------------
// MPQ layout emitters.
//
// The MPQ hash table is `hash_table_size` slots of 16 bytes:
//   uint32 hash1, uint32 hash2, uint16 locale, uint16 platform,
//   uint32 block_index
// It's encrypted with key = string_hash("(hash table)", 3).
//
// The block table is `block_table_size` entries of 16 bytes:
//   uint32 data_offset, uint32 compressed_size, uint32 size,
//   uint32 flags
// It's encrypted with key = string_hash("(block table)", 3).
//
// These emitters take plain in-memory arrays of the same struct
// types the reader parses into (hash_table_entry,
// block_table_entry) and stream them through encrypted_writer.
// -----------------------------------------------------------------

struct hash_slot {
	uint32_t hash1;
	uint32_t hash2;
	uint16_t locale;
	uint16_t platform;
	uint32_t block_index;
};

struct block_entry {
	uint32_t data_offset;
	uint32_t compressed_size;
	uint32_t size;
	uint32_t flags;
};

// Tombstone constants — see mpq_archive_reader::find_hash_table_entry
// at data_loading.h:1224. block_index==0xffffffff terminates a probe
// walk; 0xfffffffe means "slot was deleted but keep probing." We use
// 0xffffffff for empty slots.
static constexpr uint32_t HASH_EMPTY = 0xffffffffu;

// Round up to the next power of two, min 4 (MPQ archives seem to use
// >= 4). Used to size the fresh hash table.
static inline size_t round_up_pow2(size_t n) {
	size_t r = 4;
	while (r < n) r <<= 1;
	return r;
}

// Build a hash table given a list of (filename, block_index) pairs.
// Handles linear probing on hash1-collision. Returns a vector sized
// to `hash_table_size`, with unused slots as tombstones.
inline std::vector<hash_slot> build_hash_table(
	size_t hash_table_size,
	const std::vector<std::pair<std::string, uint32_t>>& entries,
	const crypt_table_t& crypt_table
) {
	std::vector<hash_slot> table(hash_table_size, hash_slot{
		0xffffffffu, 0xffffffffu, 0xffffu, 0xffffu, HASH_EMPTY
	});
	for (const auto& entry : entries) {
		const std::string& name = entry.first;
		uint32_t block_index = entry.second;
		uint32_t hash0 = string_hash(name.c_str(), 0, crypt_table);
		uint32_t hash1 = string_hash(name.c_str(), 1, crypt_table);
		uint32_t hash2 = string_hash(name.c_str(), 2, crypt_table);
		size_t initial = hash0 % hash_table_size;
		size_t idx = initial;
		bool placed = false;
		do {
			if (table[idx].block_index == HASH_EMPTY) {
				table[idx] = {hash1, hash2, 0, 0, block_index};
				placed = true;
				break;
			}
			++idx;
			if (idx == hash_table_size) idx = 0;
		} while (idx != initial);
		if (!placed) error("build_hash_table: table full (%zu slots, entry %s)",
		                    hash_table_size, name.c_str());
	}
	return table;
}

// Emit the hash table to writer, encrypted with the retail key.
template<typename base_writer_T>
inline void put_encrypted_hash_table(
	base_writer_T& writer,
	const std::vector<hash_slot>& table,
	const crypt_table_t& crypt_table
) {
	uint32_t key = string_hash("(hash table)", 3, crypt_table);
	auto enc = make_encrypted_writer(writer, key, crypt_table);
	for (const auto& s : table) {
		enc.put_u32(s.hash1);
		enc.put_u32(s.hash2);
		// locale + platform packed into one u32
		enc.put_u32(((uint32_t)s.platform << 16) | s.locale);
		enc.put_u32(s.block_index);
	}
}

// Emit the block table to writer, encrypted with the retail key.
template<typename base_writer_T>
inline void put_encrypted_block_table(
	base_writer_T& writer,
	const std::vector<block_entry>& entries,
	const crypt_table_t& crypt_table
) {
	uint32_t key = string_hash("(block table)", 3, crypt_table);
	auto enc = make_encrypted_writer(writer, key, crypt_table);
	for (const auto& e : entries) {
		enc.put_u32(e.data_offset);
		enc.put_u32(e.compressed_size);
		enc.put_u32(e.size);
		enc.put_u32(e.flags);
	}
}

} // namespace data_writing
} // namespace bwgame

#endif // BWGAME_DATA_WRITING_H
