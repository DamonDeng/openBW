"""Pure-Python MPQ + BW-CHK parser (partial: PKWARE decoder WIP).

STATUS 2026-07-20:
  * MPQ layer works: header, encrypted hash/block tables, file
    lookup, encrypted single-unit and multi-sector payload decrypt.
  * CHK walker works: DIM/ERA/UNIT/SIDE tag extraction.
  * zlib-compressed CHK sectors work (mask 0x02).
  * PKWARE Implode decode (mask 0x08) is 95% correct but has an
    off-by-a-few-bits bug that surfaces mid-stream around bit 5000
    on Blizzard-shipped maps. See tests/PKWARE_status.md for
    diagnostic output (chunk tags "VER ", "IVER", "IVE2", "VCOD"
    all decode correctly for the first ~36 bytes before the bug
    hits).
  * For the moment, this parser is best used for maps compressed
    with zlib (mask 0x02) only. Blizzard-shipped .scm/.scx use
    PKWARE mask 0x08 which will raise ValueError.

For static-map extraction of the shipped Blizzard maps, use
tools/static_map_parser.cpp (build with cmake -DOPENBW_BUILD_TOOLS=ON,
then run ./build_tools/tools/openbw_static_map_parser). That tool
reuses the engine's MPQ + CHK + tileset pipeline via
game_load_functions::load_map_file, so its output is definitionally
identical to what the runtime sees -- no PKWARE bug to work around.
"""

from __future__ import annotations

import os
import struct
from pathlib import Path


# --------------------------------------------------------------------
# BW type ids we care about at parse time. Keep this list here so
# map_parser is standalone (no dependency on python_agent.enums).
# --------------------------------------------------------------------

MINERAL_TYPES = (176, 177, 178)     # Resource_Mineral_Field_Type_1/2/3
GEYSER_TYPE = 188                    # Resource_Vespene_Geyser
START_LOCATION_TYPE = 214            # Special_Start_Location


# --------------------------------------------------------------------
# MPQ crypto (Blizzard's Storm library scheme).
#
# Reimplemented from
# https://github.com/ladislav-zezula/StormLib/blob/master/src/StormCommon.cpp
# and cross-checked against openbw's `data_loading.h`.
# --------------------------------------------------------------------


def _build_crypt_table() -> list[int]:
    """Build the 0x500-entry crypt table. Cached at module load."""
    table = [0] * 0x500
    seed = 0x00100001
    for i in range(0x100):
        for j in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            t1 = (seed & 0xFFFF) << 0x10
            seed = (seed * 125 + 3) % 0x2AAAAB
            t2 = seed & 0xFFFF
            table[i + 0x100 * j] = (t1 | t2) & 0xFFFFFFFF
    return table


_CRYPT_TABLE = _build_crypt_table()


def _hash_string(s: str, hash_type: int) -> int:
    """Compute MPQ string hash. hash_type:
      0 = table offset, 1 = hash A, 2 = hash B, 3 = decryption key."""
    seed1 = 0x7FED7FED
    seed2 = 0xEEEEEEEE
    for ch in s.upper():
        c = ord(ch)
        if c == ord('/'):
            c = ord('\\')
        seed1 = (_CRYPT_TABLE[(hash_type << 8) + c] ^ ((seed1 + seed2) & 0xFFFFFFFF)) & 0xFFFFFFFF
        seed2 = (c + seed1 + seed2 + ((seed2 << 5) & 0xFFFFFFFF) + 3) & 0xFFFFFFFF
    return seed1


def _decrypt_dwords(data: bytes, key: int) -> bytes:
    """Decrypt N * 4 bytes with the MPQ table cipher. Returns bytes."""
    n = len(data) // 4
    out = bytearray()
    seed = 0xEEEEEEEE
    for i in range(n):
        seed = (seed + _CRYPT_TABLE[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        v = int.from_bytes(data[i * 4 : i * 4 + 4], "little", signed=False)
        v = (v ^ ((key + seed) & 0xFFFFFFFF)) & 0xFFFFFFFF
        key = ((~key << 0x15) + 0x11111111) & 0xFFFFFFFF | ((key >> 0x0B) & 0xFFFFFFFF)
        seed = (v + seed + (seed << 5) + 3) & 0xFFFFFFFF
        out.extend(v.to_bytes(4, "little", signed=False))
    return bytes(out)


# --------------------------------------------------------------------
# MPQ archive walker.
# --------------------------------------------------------------------


MPQ_SIGNATURE = 0x1A51504D  # b"MPQ\x1A"

# Hash table entry (16 bytes): hashA (u32) | hashB (u32) | locale (u16)
# | platform (u16) | block_index (u32). block_index == 0xFFFFFFFF means
# "empty slot", 0xFFFFFFFE means "deleted".
_HASH_TABLE_ENTRY_FMT = "<IIHHI"

# Block table entry (16 bytes): file_pos (u32) | c_size (u32) | u_size
# (u32) | flags (u32).
_BLOCK_TABLE_ENTRY_FMT = "<IIII"

MPQ_FILE_EXISTS  = 0x80000000
MPQ_FILE_ENCRYPTED = 0x00010000
MPQ_FILE_FIX_KEY = 0x00020000
MPQ_FILE_SINGLE_UNIT = 0x01000000
MPQ_FILE_COMPRESS = 0x00000200


def _find_file(mpq_bytes: bytes, archive_offset: int,
               hash_table: bytes, block_table: bytes,
               filename: str) -> tuple[int, int, int, int]:
    """Return (file_pos_abs, c_size, u_size, flags) for `filename`
    or raise KeyError if not found."""
    hash_slot_count = len(hash_table) // 16
    h_start = _hash_string(filename, 0) % hash_slot_count
    h_a = _hash_string(filename, 1)
    h_b = _hash_string(filename, 2)

    i = h_start
    while True:
        entry = struct.unpack_from(_HASH_TABLE_ENTRY_FMT, hash_table, i * 16)
        e_hashA, e_hashB, _locale, _platform, block_index = entry
        if block_index == 0xFFFFFFFF:
            raise KeyError(f"file not found in MPQ: {filename}")
        if e_hashA == h_a and e_hashB == h_b and block_index != 0xFFFFFFFE:
            # Found the block.
            file_pos, c_size, u_size, flags = struct.unpack_from(
                _BLOCK_TABLE_ENTRY_FMT, block_table, block_index * 16)
            return archive_offset + file_pos, c_size, u_size, flags
        i = (i + 1) % hash_slot_count
        if i == h_start:
            raise KeyError(f"file not found in MPQ (hash-loop): {filename}")


def _open_mpq(path: str | Path) -> tuple[bytes, int, bytes, bytes, int]:
    """Read the whole file and return
    (raw_bytes, archive_offset, hash_table_plain, block_table_plain,
     sector_size)."""
    raw = Path(path).read_bytes()
    # Some maps have a "user data" header before the MPQ header.
    # The MPQ signature can appear at offset 0, 512, or anywhere on
    # a 512-byte boundary. Scan.
    archive_offset = -1
    for off in range(0, min(len(raw), 4096), 512):
        if len(raw) - off < 32:
            continue
        sig = int.from_bytes(raw[off:off + 4], "little", signed=False)
        if sig == MPQ_SIGNATURE:
            archive_offset = off
            break
    if archive_offset < 0:
        raise ValueError(f"MPQ signature not found in {path}")

    hdr = raw[archive_offset:archive_offset + 32]
    (_sig, _header_size, _archive_size, _format_version,
     _block_size_shift,
     hash_off, block_off,
     hash_size, block_size,
     ) = struct.unpack("<IIIHHIIII", hdr)
    sector_size = 512 << _block_size_shift

    hash_start = archive_offset + hash_off
    block_start = archive_offset + block_off

    hash_enc = raw[hash_start:hash_start + hash_size * 16]
    block_enc = raw[block_start:block_start + block_size * 16]

    hash_key = _hash_string("(hash table)", 3)
    block_key = _hash_string("(block table)", 3)
    hash_plain = _decrypt_dwords(hash_enc, hash_key)
    block_plain = _decrypt_dwords(block_enc, block_key)

    return raw, archive_offset, hash_plain, block_plain, sector_size


def _compute_file_key(filename: str, file_pos_rel: int,
                      c_size: int, flags: int) -> int:
    """Compute the MPQ file-payload encryption key. The base key is
    the hash of the LAST path component (`scenario.chk`, not the
    full `staredit\\scenario.chk`). If MPQ_FILE_FIX_KEY is set the
    key is further mixed with file position + uncompressed size —
    every retail Blizzard map has FIX_KEY set."""
    # Last path component.
    last = filename.split("\\")[-1].split("/")[-1]
    key = _hash_string(last, 3)
    if flags & MPQ_FILE_FIX_KEY:
        # Storm uses (file_key + file_pos) XOR file_size for the
        # adjusted key. file_pos here is the offset RELATIVE to
        # the archive start.
        key = ((key + file_pos_rel) ^ c_size) & 0xFFFFFFFF
    return key


def _read_scenario_chk(path: str | Path) -> bytes:
    """Extract the raw CHK bytes for staredit\\scenario.chk from
    the MPQ. Handles single-unit files (compressed or not), which
    is the shape every Blizzard-shipped .scm uses. Multi-sector
    files (large user maps) are not handled here."""
    raw, archive_offset, hash_t, block_t, sector_size = _open_mpq(path)
    filename = "staredit\\scenario.chk"
    file_pos_abs, c_size, u_size, flags = _find_file(
        raw, archive_offset, hash_t, block_t, filename)
    if not (flags & MPQ_FILE_EXISTS):
        raise ValueError(f"scenario.chk not present in MPQ: {path}")

    file_pos_rel = file_pos_abs - archive_offset
    key = None
    if flags & MPQ_FILE_ENCRYPTED:
        key = _compute_file_key(filename, file_pos_rel, c_size, flags)

    if flags & MPQ_FILE_SINGLE_UNIT:
        # Whole file is one blob. Decrypt (sector index 0), then
        # optionally decompress.
        data = raw[file_pos_abs:file_pos_abs + c_size]
        if key is not None:
            data = _decrypt_dwords(data, key)
        return _maybe_decompress_sector(data, c_size, u_size, flags, path)

    # Multi-sector layout.
    n_full_sectors = u_size // sector_size
    last_sector_u_size = u_size - n_full_sectors * sector_size
    n_sectors = n_full_sectors + (1 if last_sector_u_size > 0 else 0)
    # +1 for the trailing sentinel offset (end of last sector).
    n_offsets = n_sectors + 1
    offsets_bytes_len = n_offsets * 4
    offsets_enc = raw[file_pos_abs : file_pos_abs + offsets_bytes_len]
    if key is not None:
        offsets_plain = _decrypt_dwords(offsets_enc, (key - 1) & 0xFFFFFFFF)
    else:
        offsets_plain = offsets_enc
    offsets = [
        int.from_bytes(offsets_plain[i * 4 : i * 4 + 4], "little", signed=False)
        for i in range(n_offsets)
    ]

    out = bytearray()
    for i in range(n_sectors):
        sec_start = file_pos_abs + offsets[i]
        sec_end = file_pos_abs + offsets[i + 1]
        sec = raw[sec_start:sec_end]
        if key is not None:
            sec = _decrypt_dwords(sec, (key + i) & 0xFFFFFFFF)
        # Uncompressed size of this sector.
        this_u_size = (last_sector_u_size
                       if (i == n_sectors - 1 and last_sector_u_size > 0)
                       else sector_size)
        sec_c_size = offsets[i + 1] - offsets[i]
        out.extend(_maybe_decompress_sector(
            bytes(sec), sec_c_size, this_u_size, flags, path))
    return bytes(out)


def _maybe_decompress_sector(data: bytes, c_size: int, u_size: int,
                             flags: int, path) -> bytes:
    """Apply the MPQ_FILE_COMPRESS mask to a decrypted sector."""
    if not (flags & MPQ_FILE_COMPRESS):
        return data
    if c_size == u_size:
        return data
    mask = data[0]
    payload = data[1:]
    if mask == 0x02:
        import zlib
        return zlib.decompress(payload)
    elif mask == 0x08:
        # PKWARE Implode -- Blizzard maps use this for
        # scenario.chk sectors. Pure-Python decoder below.
        return _pkware_explode(payload, u_size, path)
    else:
        raise ValueError(
            f"CHK uses unknown compression mask 0x{mask:02x}: {path}")


# --------------------------------------------------------------------
# PKWARE Data Compression Library ("Implode") decoder.
#
# Ported from ZeroMemory's `explode.c` in StormLib
# (https://github.com/ladislav-zezula/StormLib, MIT-licensed
# public-domain equivalent). The reference implementation lives
# in `src/pklib/explode.c`; this Python port follows the same
# algorithm and table names for auditability.
#
# The stream starts with 2 header bytes: lit_type (0=raw literals,
# 1=fixed-Huffman literals) and dict_size_bits (4, 5, or 6). After
# that, bits are read LSB-first: 0 = literal byte, 1 = (length,
# distance) copy pair. Length codes are variable-length prefix
# codes stored in `DistBits`/`DistCode` and `LenBits`/`LenCode`;
# StormLib expands them into 256-entry direct lookup tables at
# init time. That's exactly what we do here.
# --------------------------------------------------------------------


class _BitReader:
    """LSB-first bit reader with peek()."""
    __slots__ = ("_buf", "_bit_pos", "_end_bit_pos")

    def __init__(self, data: bytes) -> None:
        self._buf = data
        self._bit_pos = 0
        self._end_bit_pos = len(data) * 8

    def peek(self, n: int) -> int:
        """Peek the next n bits without consuming."""
        v = 0
        for i in range(n):
            bit_index = self._bit_pos + i
            if bit_index >= self._end_bit_pos:
                break
            byte_index = bit_index >> 3
            bit = (self._buf[byte_index] >> (bit_index & 7)) & 1
            v |= bit << i
        return v

    def consume(self, n: int) -> None:
        self._bit_pos += n

    def read(self, n: int) -> int:
        v = self.peek(n)
        self._bit_pos += n
        return v


# StormLib source-of-truth tables (from src/pklib/explode.c).

# Table 1: distance code bit lengths (64 entries -- code index -> #bits).
_DIST_BITS = [
    0x02, 0x04, 0x04, 0x05, 0x05, 0x05, 0x05, 0x06,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
]

# Table 2: distance codes (64 entries).
_DIST_CODE = [
    0x03, 0x0D, 0x05, 0x19, 0x09, 0x11, 0x01, 0x3E,
    0x1E, 0x2E, 0x0E, 0x36, 0x16, 0x26, 0x06, 0x7C,
    0x3C, 0x5C, 0x1C, 0x6C, 0x2C, 0x4C, 0x0C, 0x78,
    0x38, 0x58, 0x18, 0x68, 0x28, 0x48, 0x08, 0xF0,
    0x70, 0xB0, 0x30, 0xD0, 0x50, 0x90, 0x10, 0xE0,
    0x60, 0xA0, 0x20, 0xC0, 0x40, 0x80, 0x00,
    # StormLib table has 64 entries; last 17 are unused (only 47
    # legal distance codes) so we pad with zeros to keep index math
    # consistent.
] + [0] * 17

# Table 3: length code bit lengths (16 entries -- code index -> #bits).
# Source: StormLib src/pklib/explode.c `LenBits[0x10]`.
_LEN_BITS = [
    3, 2, 3, 3, 4, 4, 4, 5, 5, 6, 7, 7, 8, 8, 9, 9,
]

# Table 4: length codes (16 entries).
# Source: StormLib src/pklib/explode.c `LenCode[0x10]`.
_LEN_CODE = [
    5, 3, 1, 6, 10, 2, 12, 20, 4, 24, 8, 48, 16, 32, 64, 0,
]

# Table 5: how many extra bits to read after each length code.
# Source: StormLib src/pklib/explode.c `ExLenBits[0x10]`.
_EX_LEN_BITS = [
    0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8,
]

# Table 6: base length values (before extra bits are added).
# Source: StormLib src/pklib/explode.c `LenBase[0x10]`.
# Note LenBase[0] = 2 (the minimum copy length), and index order
# matches the order of LenBits/LenCode.
_LEN_BASE = [
    0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0008, 0x0009,
    0x000A, 0x000C, 0x0010, 0x0018, 0x0028, 0x0048, 0x0088, 0x0108,
]


def _bit_reverse(v: int, n_bits: int) -> int:
    r = 0
    for i in range(n_bits):
        if v & (1 << i):
            r |= 1 << (n_bits - 1 - i)
    return r


def _build_lookup_table(code_lengths: list[int], codes: list[int],
                        max_bits: int) -> list[int]:
    """Build a 2**max_bits-entry lookup table for a prefix-code
    set. Each entry stores the code INDEX (0..len-1).

    In PKWARE's tables the numeric `code` value already matches
    the LSB-first bit accumulation you'd see when reading n bits
    from the stream. So no reversal is needed: every entry whose
    LOW n bits equal `code` gets index i (the high bits are
    "don't care" and will be consumed by the next code)."""
    size = 1 << max_bits
    table = [0xFF] * size
    for i, (n, c) in enumerate(zip(code_lengths, codes)):
        for j in range(size):
            if (j & ((1 << n) - 1)) == c:
                if table[j] == 0xFF:
                    table[j] = i
    return table


# StormLib uses 256-entry tables (peek at 8 bits). Length codes
# fit in <=8 bits after reversal; distance codes are up to 8 bits
# too. Fits perfectly.
_LEN_TABLE = _build_lookup_table(_LEN_BITS, _LEN_CODE, 8)
_DIST_TABLE = _build_lookup_table(_DIST_BITS[:47], _DIST_CODE[:47], 8)


def _pkware_explode(data: bytes, expected_u_size: int, path) -> bytes:
    """Decompress a PKWARE Implode stream. Returns u_size bytes."""
    if len(data) < 3:
        raise ValueError(f"PKWARE stream too short: {path}")
    lit_mode = data[0]
    dict_shift = data[1]
    if dict_shift not in (4, 5, 6):
        raise ValueError(f"PKWARE invalid dict_shift={dict_shift}: {path}")
    if lit_mode not in (0, 1):
        raise ValueError(f"PKWARE invalid lit_mode={lit_mode}: {path}")
    if lit_mode == 1:
        raise ValueError(
            f"PKWARE lit_mode=1 (custom Huffman) not implemented: {path}")

    br = _BitReader(data[2:])
    out = bytearray()

    while True:
        is_copy = br.read(1)
        if is_copy == 1:
            # Length code: peek 8 bits, look up.
            peek = br.peek(8)
            len_idx = _LEN_TABLE[peek]
            if len_idx == 0xFF:
                raise ValueError(
                    f"PKWARE bad length code at bit {br._bit_pos}: {path}")
            br.consume(_LEN_BITS[len_idx])
            length = _LEN_BASE[len_idx]
            extra = _EX_LEN_BITS[len_idx]
            if extra > 0:
                length += br.read(extra)
            if length == 519:  # end-of-stream sentinel
                break
            # Distance: peek 8 bits, look up.
            peek = br.peek(8)
            dist_idx = _DIST_TABLE[peek]
            if dist_idx == 0xFF:
                raise ValueError(
                    f"PKWARE bad distance code at bit {br._bit_pos}: {path}")
            br.consume(_DIST_BITS[dist_idx])
            # StormLib: for length == 2, use only 2 low bits of the
            # distance regardless of dict_shift; otherwise use
            # dict_shift low bits. The shift used to combine the
            # distance-code index with the low bits also switches:
            # length == 2 uses shift 2, else dict_shift.
            if length == 2:
                dist_low = br.read(2)
                distance = (dist_idx << 2) | dist_low
            else:
                dist_low = br.read(dict_shift)
                distance = (dist_idx << dict_shift) | dist_low
            distance += 1
            start = len(out) - distance
            if start < 0:
                raise ValueError(
                    f"PKWARE back-ref before start: dist={distance} "
                    f"out_len={len(out)} path={path}")
            # Copy byte-by-byte since src and dst can overlap.
            for i in range(length):
                out.append(out[start + i])
        else:
            byte = br.read(8)
            out.append(byte)
        if len(out) >= expected_u_size:
            break

    return bytes(out[:expected_u_size])


# --------------------------------------------------------------------
# CHK chunk walker.
# --------------------------------------------------------------------


def _walk_chk(chk: bytes) -> dict[str, list[bytes]]:
    """Return a dict mapping 4-byte tag -> list of chunk payloads
    (multiple chunks with the same tag can appear; retail BW
    processes them in stream order). We return them all so the
    caller can pick the last / most-recent write, matching engine
    behaviour."""
    out: dict[str, list[bytes]] = {}
    pos = 0
    while pos + 8 <= len(chk):
        tag = chk[pos:pos + 4].decode("ascii", errors="replace")
        size = int.from_bytes(chk[pos + 4:pos + 8], "little", signed=True)
        pos += 8
        # Some (bad?) chunks report negative sizes; treat as 0.
        if size < 0:
            size = 0
        # Clamp to remaining bytes.
        end = min(pos + size, len(chk))
        out.setdefault(tag, []).append(chk[pos:end])
        pos = end
    return out


# --------------------------------------------------------------------
# Public API.
# --------------------------------------------------------------------


def parse_scm(path: str | Path) -> dict:
    """Parse a Blizzard-shipped .scm/.scx and return a static-data
    dict. See module docstring for the shape.

    Raises ValueError on unparseable files (usually because the
    map uses a compression variant we haven't implemented)."""
    p = Path(path)
    chk = _read_scenario_chk(p)
    chunks = _walk_chk(chk)

    if "DIM " not in chunks:
        raise ValueError(f"CHK missing DIM chunk: {p}")
    if "ERA " not in chunks:
        raise ValueError(f"CHK missing ERA chunk: {p}")

    dim = chunks["DIM "][-1]      # Last write wins.
    if len(dim) < 4:
        raise ValueError(f"DIM chunk too short: {p}")
    tile_w = int.from_bytes(dim[0:2], "little", signed=False)
    tile_h = int.from_bytes(dim[2:4], "little", signed=False)

    era = chunks["ERA "][-1]
    if len(era) < 2:
        raise ValueError(f"ERA chunk too short: {p}")
    tileset = int.from_bytes(era[0:2], "little", signed=False) % 8

    minerals: list[tuple[int, int, int]] = []
    geysers: list[tuple[int, int]] = []
    starts: list[tuple[int, int, int]] = []

    # UNIT chunks — 36 bytes per entry. Multiple UNIT chunks can
    # appear; consume all of them.
    for unit_chunk in chunks.get("UNIT", []):
        n = len(unit_chunk) // 36
        for i in range(n):
            entry = unit_chunk[i * 36 : (i + 1) * 36]
            # See openbw bwgame.h line 21557 for the layout.
            #   u32 id, u16 x, u16 y, u16 type, u16 link, u16 valid_flags,
            #   u16 valid_props, u8 owner, u8 hp%, u8 shield%, u8 energy%,
            #   u32 resources, u16 units_in_hangar, u16 flags,
            #   u32 unused, u32 related_unit_id.
            (_uid, x, y, type_id, _link, _valid_flags, _valid_props,
             owner, _hp, _shield, _energy,
             _resources, _hangar, _flags,
             _unused, _related) = struct.unpack(
                "<IHHHHHHBBBBIHHII", entry)
            if type_id == 0xFFFF:
                continue
            if type_id in MINERAL_TYPES:
                minerals.append((x, y, type_id))
            elif type_id == GEYSER_TYPE:
                geysers.append((x, y))
            elif type_id == START_LOCATION_TYPE:
                # Slot info lives in `owner`.
                starts.append((owner, x, y))

    name = p.stem
    return {
        "name": name,
        "tile_w": tile_w,
        "tile_h": tile_h,
        "width": tile_w * 32,
        "height": tile_h * 32,
        "tileset": tileset,
        "minerals": minerals,
        "geysers": geysers,
        "start_locations": starts,
    }
