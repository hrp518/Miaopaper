#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Convert WenQuanYi Bitmap Song (12pt, 16x16) BDF into two font files:

  1) HZK16_87_wqy.bin   - GB2312 87x94 area, byte-compatible with the
                          existing HZK16_87.bin (same offsets, same 32
                          bytes/char, 16x16, MSB-first rows).  Drop-in
                          replacement that upgrades the glyph artwork.
  2) WQY_GBK16.bin       - GBK extension "big font": a sparse dictionary of
                          every GBK double-byte code point NOT renderable by
                          the GB2312 fast path (traditional Chinese, rare
                          simplified, extra symbols).  Format:

                          header (32 bytes):
                            [0:4]  magic  "WQY1"
                            [4]    version = 1
                            [5]    glyph_w = 16
                            [6]    glyph_h = 16
                            [7]    glyph_bytes = 32
                            [8:12] count (u32 LE)
                            [12:16] table_offset (u32 LE)
                            [16:20] glyph_offset (u32 LE)
                            [20:32] reserved (0)
                          table: count x 4 bytes
                            [gbk_code u16 LE][glyph_index u16 LE]
                            (sorted by gbk_code; glyph_index*32 = byte
                             offset of the 32-byte bitmap in the glyph blob)
                          blob: count x 32 bytes (HZK16 row format)

Both files are intended for the MiaoPaper TLSR8258 e-reader firmware.
Font source: WenQuanYi Bitmap Song 1.0.0-RC1 (GPLv2 with font embedding
exception), see fonts/wqy-bitmapsong/COPYING.
"""

import os
import sys
import struct

FONT_DIR = os.path.dirname(os.path.abspath(__file__))
BDF_PATH = os.path.join(FONT_DIR, "wqy-bitmapsong", "wenquanyi_12pt.bdf")
OUT_HZK = os.path.join(FONT_DIR, "HZK16_87_wqy.bin")
OUT_GBK = os.path.join(FONT_DIR, "WQY_GBK16.bin")

CELL = 16          # 16x16 cell
ROW_BYTES = 2      # 2 bytes per row -> 32 bytes/char

# ---------------------------------------------------------------- BDF parsing

def parse_bdf(path):
    """Return {unicode_cp: 16-row list of 16-bit ints (MSB = leftmost px)}.
    Placement rule: cell row r corresponds to BDF y = r - 2, i.e. a CJK
    glyph with BBX 16 16 0 -2 (the wqy 12pt norm) fills rows 0..15 exactly.
    """
    glyphs = {}
    cur = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if line.startswith("STARTCHAR"):
                cur = {"cp": None, "w": 0, "h": 0, "xoff": 0, "yoff": 0, "rows": []}
            elif cur is not None:
                if line.startswith("ENCODING"):
                    cur["cp"] = int(line.split()[1])
                elif line.startswith("BBX"):
                    _, w, h, xo, yo = line.split()
                    cur["w"], cur["h"], cur["xoff"], cur["yoff"] = (
                        int(w), int(h), int(xo), int(yo))
                elif line.startswith("BITMAP"):
                    cur["in_bitmap"] = True
                elif cur.get("in_bitmap"):
                    if line == "ENDCHAR":
                        cp = cur["cp"]
                        if cp is not None and cp > 0:
                            bits = place_into_cell(cur)
                            if bits is not None:
                                glyphs[cp] = bits
                        cur = None
                    else:
                        cur["rows"].append(line)
    return glyphs


def place_into_cell(g):
    w, h, xoff, yoff = g["w"], g["h"], g["xoff"], g["yoff"]
    cell = [0] * CELL
    for row, hexstr in enumerate(g["rows"]):
        by = yoff + row            # BDF y of this bitmap row
        cr = by + 2                # cell row (cell row 0 == BDF y -2)
        if cr < 0 or cr >= CELL:
            continue
        v = int(hexstr, 16)
        nbytes = (w + 7) // 8
        # BDF row bytes are MSB-first; glyph pixel px sits at xoff+px
        for k in range(nbytes):
            byte = (v >> (8 * (nbytes - 1 - k))) & 0xFF
            for bit in range(8):
                px = k * 8 + bit
                if px >= w:
                    continue
                col = xoff + px
                if 0 <= col < CELL and (byte & (0x80 >> bit)):
                    cell[cr] |= 1 << (15 - col)
    return cell


# ------------------------------------------------------------- GBK utilities

def gbk_code(qb, tb):
    return (qb << 8) | tb


def gbk_to_unicode(qb, tb):
    try:
        return ord(bytes([qb, tb]).decode("gbk"))
    except (UnicodeDecodeError, ValueError):
        return None


def is_gb2312_fast_path(qb, tb):
    """Chars the firmware renders from the old HZK16_87.bin: GB2312 symbols
    (0xA1A1-0xA9FE) + GB2312 simplified hanzi (0xB0A1-0xF7FE)."""
    if tb < 0xA1 or tb > 0xFE:
        return False
    return (0xA1 <= qb <= 0xA9) or (0xB0 <= qb <= 0xF7)


def hzk_bytes(cell):
    out = bytearray(32)
    for r in range(CELL):
        out[r * 2] = (cell[r] >> 8) & 0xFF
        out[r * 2 + 1] = cell[r] & 0xFF
    return bytes(out)


# -------------------------------------------------------------------- output

def build_gb2312_file(glyphs):
    """87 x 94 cells, same layout as HZK16_87.bin."""
    data = bytearray(87 * 94 * 32)
    missing = 0
    for sec in range(1, 88):          # high byte 0xA0+sec
        for pos in range(1, 95):      # low byte 0xA0+pos
            qb, tb = 0xA0 + sec, 0xA0 + pos
            cp = gbk_to_unicode(qb, tb)
            if cp is not None and cp in glyphs:
                off = ((sec - 1) * 94 + (pos - 1)) * 32
                data[off:off + 32] = hzk_bytes(glyphs[cp])
            else:
                missing += 1
    with open(OUT_HZK, "wb") as f:
        f.write(data)
    return len(data), missing


def build_gbk_dict(glyphs):
    """Sparse dictionary of GBK codes NOT handled by the GB2312 fast path."""
    entries = []                      # (gbk_code, glyph_bytes)
    seen = set()
    for qb in range(0x81, 0xFF):
        for tb in range(0x40, 0xFF):
            if tb == 0x7F:
                continue
            if is_gb2312_fast_path(qb, tb):
                continue
            code = gbk_code(qb, tb)
            if code in seen:
                continue
            seen.add(code)
            cp = gbk_to_unicode(qb, tb)
            if cp is None or cp not in glyphs:
                continue
            entries.append((code, hzk_bytes(glyphs[cp])))

    entries.sort(key=lambda e: e[0])
    count = len(entries)
    header = bytearray(32)
    header[0:4] = b"WQY1"
    header[4] = 1
    header[5] = CELL
    header[6] = CELL
    header[7] = 32
    table_off = 32
    glyph_off = table_off + count * 4
    struct.pack_into("<I", header, 8, count)
    struct.pack_into("<I", header, 12, table_off)
    struct.pack_into("<I", header, 16, glyph_off)

    blob = bytearray()
    table = bytearray()
    for idx, (code, gb) in enumerate(entries):
        table += struct.pack("<HH", code, idx)
        blob += gb

    with open(OUT_GBK, "wb") as f:
        f.write(bytes(header))
        f.write(bytes(table))
        f.write(bytes(blob))
    return count, len(header) + len(table) + len(blob)


# ------------------------------------------------------------------ verify

def build_gbk_js_map():
    """Unicode -> GBK code point map for the web tool (traditional book
    upload).  Same shape as web_tools/js/gb2312_map.js."""
    pairs = []
    for cp in range(0x20, 0xFFFF):
        try:
            b = chr(cp).encode("gbk")
        except UnicodeEncodeError:
            continue
        if len(b) == 2:
            pairs.append((cp, (b[0] << 8) | b[1]))
    lines = []
    lines.append("// Auto-generated Unicode -> GBK map (MiaoPaper GBK 大字库 / 繁体书).")
    lines.append("var GBK_PAIRS=[")
    row = []
    for cp, gb in pairs:
        row.append("[%d,%d]" % (cp, gb))
        if len(row) == 20:
            lines.append(",".join(row) + ",")
            row = []
    if row:
        lines.append(",".join(row))
    lines.append("];")
    lines.append("var GBK_LOOKUP=null;")
    lines.append("function gbkLookupInit(){if(GBK_LOOKUP)return;GBK_LOOKUP=Object.create(null);"
                 "for(var i=0;i<GBK_PAIRS.length;i++){var p=GBK_PAIRS[i];GBK_LOOKUP[p[0]]=p[1];}}")
    lines.append("function gbkEncodeChar(cp){gbkLookupInit();var v=GBK_LOOKUP[cp];return v===undefined?null:v;}")
    lines.append("")
    js_path = os.path.join(FONT_DIR, "..", "web_tools", "js", "gbk_map.js")
    js_path = os.path.abspath(js_path)
    with open(js_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    return js_path, len(pairs)


def render_text(text, glyphs, enc):
    """Render a line of text to a PGM (16px tall per line) for eyeballing."""
    lines = []
    for ch in text:
        cp = ord(ch)
        bits = glyphs.get(cp)
        if bits is None:
            bits = [0] * CELL
        lines.append(bits)
    w = CELL * len(lines)
    img = bytearray(w * CELL)
    for ci, bits in enumerate(lines):
        for r in range(CELL):
            for c in range(CELL):
                if bits[r] & (1 << (15 - c)):
                    img[r * w + ci * CELL + c] = 255
    pgm = bytearray(b"P5\n%d %d\n255\n" % (w, CELL))
    pgm += img
    return bytes(pgm)


def main():
    if not os.path.exists(BDF_PATH):
        print("BDF not found:", BDF_PATH)
        sys.exit(1)

    print("Parsing", BDF_PATH, "...")
    glyphs = parse_bdf(BDF_PATH)
    print("  glyphs loaded:", len(glyphs))

    size, missing = build_gb2312_file(glyphs)
    print("HZK16_87_wqy.bin : %d bytes (GB2312 cells), %d undefined cells" % (size, missing))

    count, size = build_gbk_dict(glyphs)
    print("WQY_GBK16.bin    : %d bytes, %d GBK extension glyphs" % (size, count))

    js_path, js_count = build_gbk_js_map()
    print("gbk_map.js       : %d Unicode->GBK pairs -> %s" % (js_count, js_path))

    # Sample render for eyeballing
    samples = [
        ("simplified", "中文字库测试 简体中文阅读"),
        ("traditional", "繁體中文測試 讀一本傳統的書"),
    ]
    os.makedirs(os.path.join(FONT_DIR, "preview"), exist_ok=True)
    for name, text in samples:
        path = os.path.join(FONT_DIR, "preview", name + ".pgm")
        with open(path, "wb") as f:
            f.write(render_text(text, glyphs, "gbk"))
        print("  preview ->", path)


if __name__ == "__main__":
    main()
