// font_bitmap.h — tiny 5x8 bitmap font for HUD numeric readouts.
//
// openBW ships no text renderer of its own; retail BW's font/font*.fnt
// decoder was never ported (drawTextScreen and friends in
// mini-openbwapi are {} no-ops for BWAPI ABI compat only). This
// header provides the minimum needed to blit resource counters
// ("250", "12/20") into the HUD:
//
//   * `glyph_5x8` — one entry per ASCII code; 5 columns each 8 bits
//     packed into one byte (bit y = pixel at (col, y)). 0 = pixel
//     off, 1 = pixel on. Non-populated characters are zero-filled.
//   * `draw_text_indexed` — paints into an indexed-8 palette buffer.
//     Used by the SDL/WASM observers (shared framebuffer format).
//   * `draw_text_rgba`    — paints into a 32-bit ARGB (Qt QImage
//     Format_ARGB32) buffer. Used by the Qt observer's HUD.
//
// Only digits 0-9, '/', space, and '+' are populated for now — that
// covers "12345", "18/20", and future "+250" / signed diffs.
//
// C++14-compatible (no inline variables, no constexpr IIFEs); this
// header must be safe to include from the SDL observer path as well
// as the Qt path.
//
// Header-only, no state, no globals beyond the const glyph table.

#ifndef BW_HUD_FONT_BITMAP_H
#define BW_HUD_FONT_BITMAP_H

#include <cstdint>
#include <cstddef>

namespace bw_hud_font {

// A single glyph: 5 columns, each 8 rows packed into a byte
// (bit position = y). Row 7 stays zero on all glyphs to leave a
// gutter for a 1-px drop shadow.
struct glyph_t {
	uint8_t col[5];
};

// Portable glyph-table initializer: a helper function populates a
// function-local static on first call and returns a pointer to it.
// Avoids lambda-in-static-array-init compile issues on some
// compilers (clang C++14 without lambda-constexpr).
namespace detail {

	inline const glyph_t* build_glyph_table() {
		static glyph_t t[128] = {};
		auto set = [](glyph_t& g, const char* r0, const char* r1,
		              const char* r2, const char* r3, const char* r4,
		              const char* r5, const char* r6) {
			const char* rows[7] = {r0, r1, r2, r3, r4, r5, r6};
			for (int c = 0; c < 5; ++c) {
				uint8_t b = 0;
				for (int r = 0; r < 7; ++r) {
					if (rows[r][c] == '#') b |= (uint8_t)(1u << r);
				}
				g.col[c] = b;
			}
		};
		set(t['0'], ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###.");
		set(t['1'], "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###.");
		set(t['2'], ".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####");
		set(t['3'], ".###.", "#...#", "....#", "..##.", "....#", "#...#", ".###.");
		set(t['4'], "...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#.");
		set(t['5'], "#####", "#....", "####.", "....#", "....#", "#...#", ".###.");
		set(t['6'], ".###.", "#....", "#....", "####.", "#...#", "#...#", ".###.");
		set(t['7'], "#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#...");
		set(t['8'], ".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###.");
		set(t['9'], ".###.", "#...#", "#...#", ".####", "....#", "....#", ".###.");
		set(t['/'], "....#", "....#", "...#.", "..#..", ".#...", "#....", "#....");
		set(t['+'], ".....", "..#..", "..#..", "#####", "..#..", "..#..", ".....");
		return t;
	}

	inline const glyph_t* glyph_table() {
		static const glyph_t* p = build_glyph_table();
		return p;
	}

} // namespace detail

// Advance width per glyph (5 lit cols + 1 gap = 6 px).
static const int glyph_advance    = 6;
static const int glyph_height     = 7;   // rows used by digits
static const int glyph_row_count  = 8;   // total logical rows

// -------------------------------------------------------------------
// draw_text_indexed — palette-indexed 8-bit target
//   dst      : pointer to the top-left pixel of the target buffer
//   pitch    : stride in BYTES between rows (usually target_width)
//   x, y     : top-left of the first glyph
//   text     : null-terminated C string
//   fg       : palette index to write for lit pixels
//   shadow   : palette index for the 1-px drop shadow; 0 disables
//   clip_w, clip_h: bounds of `dst` in pixels
// -------------------------------------------------------------------
inline void draw_text_indexed(uint8_t* dst, size_t pitch,
                              int x, int y, const char* text,
                              uint8_t fg, uint8_t shadow,
                              int clip_w, int clip_h) {
	if (!dst || !text) return;
	const glyph_t* table = detail::glyph_table();
	int cursor = x;
	for (const char* p = text; *p; ++p) {
		unsigned char ch = (unsigned char)*p;
		const glyph_t& g = table[ch];
		for (int c = 0; c < 5; ++c) {
			uint8_t bits = g.col[c];
			if (!bits) continue;
			for (int r = 0; r < 8; ++r) {
				if (!(bits & (1u << r))) continue;
				int px = cursor + c;
				int py = y + r;
				if (shadow != 0) {
					int sx = px + 1, sy = py + 1;
					if (sx >= 0 && sy >= 0 && sx < clip_w && sy < clip_h) {
						dst[(size_t)sy * pitch + (size_t)sx] = shadow;
					}
				}
				if (px >= 0 && py >= 0 && px < clip_w && py < clip_h) {
					dst[(size_t)py * pitch + (size_t)px] = fg;
				}
			}
		}
		cursor += glyph_advance;
	}
}

// -------------------------------------------------------------------
// draw_text_rgba — 32-bit ARGB target (Qt QImage::Format_ARGB32)
//   dst  : pointer to first pixel; each pixel is 4 bytes.
//   pitch: stride in BYTES between rows (usually target_width * 4).
//   fg   : 32-bit AARRGGBB. Alpha in top byte.
//   shadow: 32-bit AARRGGBB for the drop shadow; 0 disables.
// -------------------------------------------------------------------
inline void draw_text_rgba(uint8_t* dst, size_t pitch,
                           int x, int y, const char* text,
                           uint32_t fg, uint32_t shadow,
                           int clip_w, int clip_h) {
	if (!dst || !text) return;
	const glyph_t* table = detail::glyph_table();
	int cursor = x;
	auto put_px = [&](int px, int py, uint32_t argb) {
		if (px < 0 || py < 0 || px >= clip_w || py >= clip_h) return;
		uint8_t* p = dst + (size_t)py * pitch + (size_t)px * 4;
		// Qt Format_ARGB32 memory layout on little-endian is BGRA.
		p[0] = (uint8_t)(argb & 0xff);         // B
		p[1] = (uint8_t)((argb >> 8) & 0xff);  // G
		p[2] = (uint8_t)((argb >> 16) & 0xff); // R
		p[3] = (uint8_t)((argb >> 24) & 0xff); // A
	};
	for (const char* p = text; *p; ++p) {
		unsigned char ch = (unsigned char)*p;
		const glyph_t& g = table[ch];
		for (int c = 0; c < 5; ++c) {
			uint8_t bits = g.col[c];
			if (!bits) continue;
			for (int r = 0; r < 8; ++r) {
				if (!(bits & (1u << r))) continue;
				int px = cursor + c;
				int py = y + r;
				if (shadow != 0) put_px(px + 1, py + 1, shadow);
				put_px(px, py, fg);
			}
		}
		cursor += glyph_advance;
	}
}

// Compute the advance width of a string in pixels (for right-align).
inline int measure_text(const char* text) {
	int n = 0;
	for (const char* p = text; *p; ++p) ++n;
	return n * glyph_advance;
}

} // namespace bw_hud_font

#endif // BW_HUD_FONT_BITMAP_H
