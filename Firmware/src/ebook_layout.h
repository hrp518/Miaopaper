#pragma once

// Pure text-layout / pagination helpers.
//
// This header is deliberately free of SDK dependencies: it only needs the
// constants from ebook.h and the Dialog_plain_16 glyph widths, so the exact
// same code can be compiled both into the firmware (ebook.c) and into the
// host-side pagination test (tools/test_prev_page.py mirrors these
// functions 1:1 -- keep them in sync when editing).

#include <stdint.h>
#include "ebook.h"

#ifndef EB_LAYOUT_HOST_TEST
#include "OneBitDisplay.h"
#endif

extern const GFXglyph Dialog_plain_16Glyphs[];

// ===================== Line breaking =====================

static int find_line_break_ascii(const uint8_t *buf, int len, int max_w)
{
	int x = 0, last_space = -1;
	for (int i = 0; i < len; i++) {
		if (buf[i] == '\n') return i + 1;
		if (buf[i] == '\r') continue;
		uint8_t c = buf[i];
		if (c < 0x20 || c > 0x7E) c = ' ';
		int cw = Dialog_plain_16Glyphs[c - 0x20].xAdvance;
		if (c == ' ') last_space = i;
		if (x + cw > max_w) {
			return (last_space >= 0) ? last_space + 1 : i;
		}
		x += cw;
	}
	return len;
}

static int find_line_break_gb2312(const uint8_t *buf, int len, int max_w)
{
	int x = 0, i = 0;
	while (i < len) {
		if (buf[i] == '\n') return i + 1;
		if (buf[i] == '\r') { i++; continue; }

		int cw, cb;
		if (buf[i] >= 0xA1 && i + 1 < len && buf[i+1] >= 0xA1) {
			cw = EB_HZ_CHAR_W; cb = 2;
		} else {
			cw = EB_HZ_CHAR_W; cb = 1;   // ASCII rendered full-width (see render_page_gb2312)
		}
		if (x + cw > max_w) return i;
		x += cw;
		i += cb;
	}
	return len;
}

// ===================== Page layout (measure only) =====================

// A RAM window over part of the book.  win[0] is the book byte at offset
// win_base; win_end is the first offset past the window.  Used so the
// backward-pagination search below can lay out candidate pages without
// touching the (slow, bit-banged, EPD-shared) SPI flash.
typedef struct {
	const uint8_t *win;
	uint32_t win_base;
	uint32_t win_end;
	uint32_t book_len;
	uint8_t  encoding;   // EB_ENC_*
} eb_layout_t;

// Where the page that starts at `pos` ends (== where the next page starts).
// Mirrors render_page_ascii()/render_page_gb2312() line for line -- same
// buffer chunking, same caps, same end-of-book break -- but reads from the
// RAM window and draws nothing.  For any pos < win_end the caps resolve
// exactly like the real render path, so page boundaries measured here match
// the rendered ones bit for bit.
static uint32_t eb_page_end(const eb_layout_t *l, uint32_t pos)
{
	int line = 0;
	while (line < EB_LINES_PER_PAGE && pos < l->book_len) {
		int to_read = EB_READ_BUF_SIZE;
		if (pos + to_read > l->book_len) to_read = l->book_len - pos;
		if (pos + to_read > l->win_end) to_read = l->win_end - pos;
		if (to_read <= 0) break;
		int brk = (l->encoding == EB_ENC_GB2312)
			? find_line_break_gb2312(l->win + (pos - l->win_base), to_read, EB_MAX_LINE_W)
			: find_line_break_ascii(l->win + (pos - l->win_base), to_read, EB_MAX_LINE_W);
		pos += brk;
		line++;
		if (brk >= to_read && to_read < EB_READ_BUF_SIZE) break;
	}
	return pos;
}

// ===================== Backward pagination =====================

// Fill dst[0..len-1] with the book bytes at offsets [off, off+len).
typedef void (*eb_fill_win_t)(uint32_t off, uint32_t len, uint8_t *dst);

// Find the start offset of the page that precedes the page starting at S.
//
// A page consumes at most EB_LINES_PER_PAGE lines and a line at most
// EB_READ_BUF_SIZE bytes (find_line_break_* never consumes more than it was
// given), so the previous page start lies within EB_PAGE_BACK_MAX bytes
// before S.  The window also includes bytes past S because line-break
// decisions for a line ending at S may need the first byte AT S (the
// overflow check reads the overflowing character itself).
//
// When the window reaches the start of the book (S <= EB_PAGE_BACK_MAX) the
// canonical page chain is computable exactly: walk pages forward from 0 and
// return the last page that starts before S.  If S is a real page start
// that is exactly the previous page; otherwise it is the page CONTAINING
// byte S-1, which is the natural step back for a mid-page position (e.g.
// one saved by older firmware).
//
// Deeper in the book the chain from 0 is not reachable, so scan candidates
// upwards from the window start for the closest one whose page ends exactly
// at S.  Direction matters: a page that starts INSIDE another page can
// never end before it, so any match above the true previous-page start is a
// "chopped" page whose first line lost its head -- it would SKIP visible
// text.  Matches below it merely fold zero-width ('\r') or re-wrapped
// prefix bytes in, so the lowest match always shows a superset of the true
// previous page.  With no exact match at all, fall back to the page that
// contains byte S-1, same as above.
static uint32_t eb_find_prev_page_start(uint32_t S, uint32_t book_len, uint8_t encoding,
                                        eb_fill_win_t fill, uint8_t *win, uint32_t win_size)
{
	if (S == 0) return 0;

	uint32_t lo = (S > EB_PAGE_BACK_MAX) ? S - EB_PAGE_BACK_MAX : 0;
	uint32_t ahead = (book_len - S > EB_READ_BUF_SIZE) ? EB_READ_BUF_SIZE : book_len - S;
	uint32_t win_end = S + ahead;
	uint32_t win_len = win_end - lo;
	if (win_len > win_size) return lo;   // caller buffer too small (defensive)

	fill(lo, win_len, win);

	eb_layout_t l;
	l.win = win;
	l.win_base = lo;
	l.win_end = win_end;
	l.book_len = book_len;
	l.encoding = encoding;

	if (lo == 0) {
		// Walk the canonical chain from the book start: the last page
		// starting before S is either the exact previous page (S on a
		// page boundary) or the page containing S-1.
		uint32_t p = 0;
		while (p < S) {
			uint32_t nxt = eb_page_end(&l, p);
			if (nxt >= S) return p;
			p = nxt;
		}
		return p;   // defensive: nxt reaches >= S before p can reach S
	}

	for (uint32_t c = lo; c < S; c++) {
		if (eb_page_end(&l, c) == S)
			return c;
	}

	// No exact match: S is mid-page.  Step forward from the window start
	// until the page would reach past S; that page contains byte S-1.
	uint32_t p = lo;
	while (p < S) {
		uint32_t nxt = eb_page_end(&l, p);
		if (nxt >= S) break;
		p = nxt;
	}
	return p;
}
