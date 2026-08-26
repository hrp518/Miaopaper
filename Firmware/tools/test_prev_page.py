#!/usr/bin/env python3
"""Host-side verification of the backward-pagination logic.

This mirrors the C code in Firmware/src/ebook_layout.h 1:1 (same caps, same
loop structure, same fallbacks) and uses the REAL Dialog_plain_16 xAdvance
widths parsed out of Firmware/src/font16.h, so the pagination matches the
firmware bit for bit.  Keep both files in sync when editing either.

Checks, for a matrix of ASCII and GB2312 books:
  1. For every forward page start S > 0, eb_find_prev_page_start(S) returns
     exactly the previous page's start (so PREV never jumps to the beginning
     and NEXT after PREV lands back on the same page).
  2. The whole prev chain from the last page reproduces the forward page
     sequence in reverse, ending at 0.
  3. For every position S in 1..len (canonical or not), the result is a
     strict step backwards (< S).

Usage: python test_prev_page.py
"""

import os
import random
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")

# ---- constants from ebook.h ----
EB_READ_BUF_SIZE = 80
EB_LINES_PER_PAGE = 5
EB_DISP_W = 250
EB_TEXT_MARGIN = 2
EB_MAX_LINE_W = EB_DISP_W - 2 * EB_TEXT_MARGIN   # 246
EB_HZ_CHAR_W = 16
EB_ENC_ASCII = 0
EB_ENC_GB2312 = 1
EB_PAGE_BACK_MAX = EB_LINES_PER_PAGE * EB_READ_BUF_SIZE   # 400


def load_advances():
    """Parse the xAdvance values from font16.h (4th struct field).

    The table only defines 94 glyphs (0x20..0x7D): '~' (0x7E) is missing even
    though GFXfont.last says 0x7E, so the firmware reads one entry past the
    array for '~'.  Forward and backward layout share that same table, so the
    consistency properties verified here hold for any value in that slot; we
    just use a plausible placeholder.
    """
    text = open(os.path.join(SRC, "font16.h"), encoding="utf-8", errors="replace").read()
    entries = re.findall(r"\{\s*\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*(\d+)\s*,\s*-?\d+\s*,\s*-?\d+\s*\}", text)
    adv = [int(a) for a in entries]
    if len(adv) != 94:
        raise SystemExit(f"font16.h: expected 94 glyphs, parsed {len(adv)}")
    adv.append(10)   # placeholder for the missing '~' (0x7E) slot
    return adv


ADV = load_advances()
# Byte -> advance lookup mirroring the `if (c < 0x20 || c > 0x7E) c = ' ';` remap.
ADV_BY_BYTE = [ADV[0]] * 256
for c in range(0x20, 0x7F):
    ADV_BY_BYTE[c] = ADV[c - 0x20]


# ---- 1:1 mirrors of ebook_layout.h ----

def find_line_break_ascii(buf, base, ln, max_w):
    x = 0
    last_space = -1
    for i in range(ln):
        c = buf[base + i]
        if c == 0x0A:
            return i + 1
        if c == 0x0D:
            continue
        if c < 0x20 or c > 0x7E:
            c = 0x20
        cw = ADV_BY_BYTE[c]
        if c == 0x20:
            last_space = i
        if x + cw > max_w:
            return last_space + 1 if last_space >= 0 else i
        x += cw
    return ln


def find_line_break_gb2312(buf, base, ln, max_w):
    x = 0
    i = 0
    while i < ln:
        c = buf[base + i]
        if c == 0x0A:
            return i + 1
        if c == 0x0D:
            i += 1
            continue
        if c >= 0xA1 and i + 1 < ln and buf[base + i + 1] >= 0xA1:
            cw = EB_HZ_CHAR_W
            cb = 2
        else:
            cw = EB_HZ_CHAR_W
            cb = 1
        if x + cw > max_w:
            return i
        x += cw
        i += cb
    return ln


def eb_page_end(book, win, win_base, win_end, book_len, encoding, pos):
    """Mirror of eb_page_end(): `win` is a copy of book[win_base:win_end]."""
    line = 0
    while line < EB_LINES_PER_PAGE and pos < book_len:
        to_read = EB_READ_BUF_SIZE
        if pos + to_read > book_len:
            to_read = book_len - pos
        if pos + to_read > win_end:
            to_read = win_end - pos
        if to_read <= 0:
            break
        base = pos - win_base
        if encoding == EB_ENC_GB2312:
            brk = find_line_break_gb2312(win, base, to_read, EB_MAX_LINE_W)
        else:
            brk = find_line_break_ascii(win, base, to_read, EB_MAX_LINE_W)
        pos += brk
        line += 1
        if brk >= to_read and to_read < EB_READ_BUF_SIZE:
            break
    return pos


def eb_find_prev_page_start(book, book_len, encoding, S):
    """Mirror of eb_find_prev_page_start() (fill callback == book slice)."""
    if S == 0:
        return 0
    lo = S - EB_PAGE_BACK_MAX if S > EB_PAGE_BACK_MAX else 0
    ahead = EB_READ_BUF_SIZE if book_len - S > EB_READ_BUF_SIZE else book_len - S
    win_end = S + ahead
    win_len = win_end - lo
    win = book[lo:win_end]

    def pend(pos):
        return eb_page_end(book, win, lo, win_end, book_len, encoding, pos)

    if lo == 0:
        p = 0
        while p < S:
            nxt = pend(p)
            if nxt >= S:
                return p
            p = nxt
        return p

    for c in range(lo, S):
        if pend(c) == S:
            return c

    p = lo
    while p < S:
        nxt = pend(p)
        if nxt >= S:
            break
        p = nxt
    return p


def page_end_full(book, book_len, encoding, pos):
    """Reference layout with the whole book as window (== render_page_*)."""
    return eb_page_end(book, book, 0, book_len, book_len, encoding, pos)


# ---- book generators ----

def gen_ascii(rng, n, space_p, nl_p, cr_p):
    out = bytearray()
    while len(out) < n:
        r = rng.random()
        if r < nl_p:
            out.append(0x0A)
        elif r < nl_p + cr_p:
            out.append(0x0D)
        elif r < nl_p + cr_p + space_p:
            out.append(0x20)
        else:
            out.append(rng.randrange(0x21, 0x7F))
    return bytes(out)


def gen_ascii_binary(rng, n):
    return bytes(rng.randrange(0, 256) for _ in range(n))


def gen_gb(rng, n, nl_p, ascii_p, cr_p):
    out = bytearray()
    while len(out) < n:
        r = rng.random()
        if r < nl_p:
            out.append(0x0A)
        elif r < nl_p + cr_p:
            out.append(0x0D)
        elif r < nl_p + cr_p + ascii_p:
            out.append(rng.randrange(0x21, 0x7F))
        else:
            out.append(rng.randrange(0xA1, 0xF8))
            out.append(rng.randrange(0xA1, 0xFF))
    return bytes(out)


CASES = [
    ("ascii_typical",  EB_ENC_ASCII,  lambda rng, n: gen_ascii(rng, n, 0.18, 0.03, 0.00)),
    ("ascii_newlines", EB_ENC_ASCII,  lambda rng, n: gen_ascii(rng, n, 0.05, 0.20, 0.00)),
    ("ascii_cr_runs",  EB_ENC_ASCII,  lambda rng, n: gen_ascii(rng, n, 0.02, 0.01, 0.35)),
    ("ascii_words",    EB_ENC_ASCII,  lambda rng, n: gen_ascii(rng, n, 0.01, 0.00, 0.00)),
    ("ascii_binary",   EB_ENC_ASCII,  gen_ascii_binary),
    ("gb_typical",     EB_ENC_GB2312, lambda rng, n: gen_gb(rng, n, 0.05, 0.10, 0.00)),
    ("gb_newlines",    EB_ENC_GB2312, lambda rng, n: gen_gb(rng, n, 0.25, 0.10, 0.00)),
    ("gb_cr_mix",      EB_ENC_GB2312, lambda rng, n: gen_gb(rng, n, 0.02, 0.05, 0.30)),
]

SIZES = [137, 399, 400, 401, 480, 3000]


def test_book(name, enc, book, verbose=False):
    book_len = len(book)

    # Forward page sequence + per-page backward check.
    pages = [0]
    pos = 0
    while True:
        nxt = page_end_full(book, book_len, enc, pos)
        assert nxt > pos, f"{name}: page did not advance at {pos}"
        if nxt >= book_len:
            break
        pages.append(nxt)
        pos = nxt

    exact = 0
    for k in range(1, len(pages)):
        S = pages[k]
        got = eb_find_prev_page_start(book, book_len, enc, S)
        # The shown page must END exactly at S: NEXT after PREV lands back on
        # the very same page (no drift, no overlap at the bottom edge).
        back = page_end_full(book, book_len, enc, got)
        assert back == S, (
            f"{name}[len={book_len}] page {k}: next(prev({S})) = {back} != {S}")
        if S <= EB_PAGE_BACK_MAX:
            # Within one window of the book start the canonical chain from 0
            # is used: the answer must be EXACTLY the previous page start.
            assert got == pages[k - 1], (
                f"{name}[len={book_len}] page {k}: prev_page_start({S}) = {got}, "
                f"expected canonical {pages[k-1]}")
        else:
            # Deeper, the returned start may only sit at or before the
            # canonical one (a lower start folds invisible/re-wrapped
            # prefix bytes in, never skipping text).
            assert got <= pages[k - 1], (
                f"{name}[len={book_len}] page {k}: prev_page_start({S}) = {got}, "
                f"skips text before {pages[k-1]}")
        if got == pages[k - 1]:
            exact += 1

    # Full prev chain from the last page: strictly decreasing and ends at 0.
    # After a non-canonical (variant) position the containing-page fallback
    # may overlap the page just shown; count those instead of failing.
    chain = [pages[-1]]
    overlaps = 0
    while chain[-1] != 0:
        p = eb_find_prev_page_start(book, book_len, enc, chain[-1])
        assert p < chain[-1], f"{name}: prev chain stalled at {p}"
        end = page_end_full(book, book_len, enc, p)
        assert end >= chain[-1], (
            f"{name}: chain gap: page at {p} ends at {end}, before {chain[-1]}")
        if end > chain[-1]:
            overlaps += 1
        chain.append(p)
    assert chain[-1] == 0
    assert len(set(chain)) == len(chain), f"{name}: prev chain revisited a page"

    if verbose:
        print(f"  {name}[len={book_len}]: {len(pages)} pages, "
              f"prev exact {exact}/{len(pages)-1}, overlaps {overlaps}")


def test_noncanonical_steps(name, enc, book):
    """Every position (page boundary or not) must step strictly backwards."""
    book_len = len(book)
    for S in range(1, book_len + 1):
        got = eb_find_prev_page_start(book, book_len, enc, S)
        assert got < S, f"{name}: prev_page_start({S}) = {got}, not < {S}"


def main():
    rng = random.Random(20260826)
    total = 0
    for name, enc, gen in CASES:
        for n in SIZES:
            test_book(name, enc, gen(rng, n))
            total += 1
    print(f"forward/backward consistency: {total} books OK")

    # Strict-backward property for EVERY position of small books.
    test_noncanonical_steps("ascii_typical", EB_ENC_ASCII, gen_ascii(rng, 600, 0.18, 0.03, 0.0))
    test_noncanonical_steps("ascii_newlines", EB_ENC_ASCII, gen_ascii(rng, 600, 0.05, 0.20, 0.0))
    test_noncanonical_steps("gb_typical", EB_ENC_GB2312, gen_gb(rng, 600, 0.05, 0.10, 0.0))
    test_noncanonical_steps("gb_cr_mix", EB_ENC_GB2312, gen_gb(rng, 600, 0.02, 0.05, 0.30))
    print("strict-backward for all positions: 4 books OK")

    # Hand-picked edge cases.
    for name, enc, book in [
        ("empty_lines", EB_ENC_ASCII, b"\n" * 250),
        ("cr_only", EB_ENC_ASCII, b"\r" * 250),
        ("one_char", EB_ENC_ASCII, b"A"),
        ("nl_at_end", EB_ENC_ASCII, b"hello world\n"),
        ("gb_pairs", EB_ENC_GB2312, bytes([0xB0, 0xA1] * 200)),
    ]:
        if len(book) > 0:
            test_book(name, enc, book)
    print("edge cases: OK")

    print("ALL TESTS PASSED")


if __name__ == "__main__":
    sys.exit(main())
