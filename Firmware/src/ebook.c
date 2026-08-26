#include <stdint.h>
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"

#include "main.h"
#include "ext_flash.h"
#include "epd.h"
#include "OneBitDisplay.h"
#include "flash.h"
#include "ble.h"
#include "ebook.h"
#include "ebook_layout.h"
#include "battery.h"
#include "epd_refresh.h"
#include "buttons.h"

#ifndef PROGMEM
#define PROGMEM
#endif
#define memcpy_P memcpy

extern const uint8_t Dialog_plain_16Bitmaps[];
extern const GFXglyph Dialog_plain_16Glyphs[];
extern const GFXfont Dialog_plain_16;

extern uint8_t epd_temp[];
extern uint8_t epd_buffer[];
extern OBDISP obd;

extern RAM uint16_t battery_mv;
extern void epd_display(struct date_time _time, uint16_t battery_mv, int16_t temperature, uint8_t full_or_partial);
extern struct date_time get_time(void);

RAM ebook_state_t ebook_state;
uint8_t ebook_read_buf[EB_READ_BUF_SIZE];
RAM eb_mode_t eb_mode = EB_MODE_CLOCK;
RAM uint8_t eb_selected = 0;
// Mode we came from before entering LOCK, so unlock restores it (e.g. reading
// -> lock -> unlock -> back to reading, not always back to clock).
RAM eb_mode_t eb_prev_mode = EB_MODE_CLOCK;

// Pending render flag: set when mode changes to SELECT/READ but EPD is busy.
// The main loop checks this and renders when EPD becomes available.
RAM static uint8_t render_pending = 0;

// ===================== Catalog operations =====================

void ebook_catalog_init(void)
{
	if (!ext_flash_is_safe()) return;
	ext_flash_init();
	uint32_t magic;
	ext_flash_read(EB_CATALOG_ADDR, 4, (uint8_t *)&magic);
	if (magic != EB_CATALOG_MAGIC) {
		// Maybe the SPI wasn't ready. Retry once.
		ext_flash_init();
		ext_flash_read(EB_CATALOG_ADDR, 4, (uint8_t *)&magic);
	}
	if (magic != EB_CATALOG_MAGIC) {
		// Check if font or book data exists on the flash.
		// If so, only repair the magic header (don't erase!).
		uint8_t fb, bb;
		ext_flash_read(EB_FONT_ADDR, 1, &fb);
		ext_flash_read(EB_BOOKS_ADDR, 1, &bb);
		if (fb != 0xFF || bb != 0xFF) {
			// Data exists!  Write just the header bytes.
			// (NOR flash can page-program to erased (=0xFF) area.)
			uint8_t hdr[8];
			hdr[0] = 'B'; hdr[1] = 'O'; hdr[2] = 'O'; hdr[3] = 'K';
			hdr[4] = 0x01;
			hdr[5] = 0; // font flag stays 0 (will be recovered by font_installed)
			hdr[6] = 0; // book count stays 0 (will be rebuilt by scan?)
			hdr[7] = 0;
			ext_flash_page_program(EB_CATALOG_ADDR, 8, hdr);
		} else {
			// Truly empty flash: do full init.
			ext_flash_sector_erase(EB_CATALOG_ADDR);
			uint8_t hdr[8] = {0};
			hdr[0] = 'B'; hdr[1] = 'O'; hdr[2] = 'O'; hdr[3] = 'K';
			hdr[4] = 0x01;
			hdr[5] = 0;
			hdr[6] = 0;
			ext_flash_page_program(EB_CATALOG_ADDR, 8, hdr);
		}
	}
	ebook_catalog_reclaim_stale();
}

static uint8_t catalog_entry_is_live(uint8_t flags, uint8_t idx)
{
	if (flags == EB_FLAG_DONE)
		return 1;
	if (flags == EB_FLAG_UPLOADING && ebook_ble_upload_owns_slot(idx))
		return 1;
	return 0;
}

static void catalog_build_entry(uint8_t *entry, uint8_t flags, const char *title,
                                uint32_t start, uint32_t len, uint8_t enc)
{
	int i;
	for (i = 0; i < EB_ENTRY_SIZE; i++) entry[i] = 0;
	entry[EB_ENT_OFF_FLAGS] = flags;
	if (title) {
		int tlen = 0;
		while (title[tlen] && tlen < 19) tlen++;
		for (i = 0; i < tlen; i++)
			entry[EB_ENT_OFF_TITLE + i] = (uint8_t)title[i];
	}
	entry[EB_ENT_OFF_START]   = start & 0xFF;
	entry[EB_ENT_OFF_START+1] = (start >> 8) & 0xFF;
	entry[EB_ENT_OFF_START+2] = (start >> 16) & 0xFF;
	entry[EB_ENT_OFF_START+3] = (start >> 24) & 0xFF;
	entry[EB_ENT_OFF_LEN]     = len & 0xFF;
	entry[EB_ENT_OFF_LEN+1]   = (len >> 8) & 0xFF;
	entry[EB_ENT_OFF_LEN+2]   = (len >> 16) & 0xFF;
	entry[EB_ENT_OFF_LEN+3]   = (len >> 24) & 0xFF;
	entry[EB_ENT_OFF_ENC]     = enc;
}

// Erase the catalog sector and rewrite only finished books (plus an optional
// new/updated entry).  Drops DELETED slots, abandoned UPLOADING slots, and
// any corrupted intermediate flag bytes so ghost entries stop blocking uploads.
static void catalog_rewrite_sector(int8_t insert_idx, uint8_t flags, const char *title,
                                   uint32_t start, uint32_t len, uint8_t enc)
{
	uint8_t entries[EB_MAX_BOOKS][EB_ENTRY_SIZE];
	uint8_t hdr[8];
	int i;

	ext_flash_read(EB_CATALOG_ADDR, 8, hdr);
	for (i = 0; i < EB_MAX_BOOKS; i++) {
		uint32_t entry_addr = EB_CATALOG_ADDR + 0x08 + i * EB_ENTRY_SIZE;
		ext_flash_read(entry_addr, EB_ENTRY_SIZE, entries[i]);
	}

	ext_flash_sector_erase(EB_CATALOG_ADDR);

	if (hdr[0] != 'B' || hdr[1] != 'O' || hdr[2] != 'O' || hdr[3] != 'K') {
		hdr[0] = 'B'; hdr[1] = 'O'; hdr[2] = 'O'; hdr[3] = 'K';
		hdr[4] = 0x01;
		hdr[5] = 0;
		hdr[6] = 0;
		hdr[7] = 0;
	}
	ext_flash_page_program(EB_CATALOG_ADDR, 8, hdr);

	for (i = 0; i < EB_MAX_BOOKS; i++) {
		uint32_t eaddr = EB_CATALOG_ADDR + 0x08 + i * EB_ENTRY_SIZE;
		if (insert_idx >= 0 && i == insert_idx) {
			uint8_t entry[EB_ENTRY_SIZE];
			catalog_build_entry(entry, flags, title, start, len, enc);
			ext_flash_page_program(eaddr, EB_ENTRY_SIZE, entry);
		} else if (catalog_entry_is_live(entries[i][EB_ENT_OFF_FLAGS], (uint8_t)i)) {
			ext_flash_page_program(eaddr, EB_ENTRY_SIZE, entries[i]);
		}
	}
}

static int catalog_wait_flash(int wait_us)
{
	while (!ext_flash_is_safe() && wait_us > 0) { WaitUs(1000); wait_us -= 1000; }
	return ext_flash_is_safe();
}

static uint8_t catalog_count_done(void)
{
	uint8_t n = 0;

	for (int i = 0; i < EB_MAX_BOOKS; i++) {
		uint8_t flags;
		ext_flash_read(EB_CATALOG_ADDR + 0x08 + i * EB_ENTRY_SIZE, 1, &flags);
		if (flags == EB_FLAG_DONE)
			n++;
	}
	return n;
}

/* Any slot that is not a finished book and not the active upload target is free. */
static int8_t catalog_scan_free_slot(void)
{
	for (int i = 0; i < EB_MAX_BOOKS; i++) {
		uint8_t flags;
		ext_flash_read(EB_CATALOG_ADDR + 0x08 + i * EB_ENTRY_SIZE, 1, &flags);
		if (!catalog_entry_is_live(flags, (uint8_t)i))
			return i;
	}
	return -1;
}

void ebook_catalog_compact(void)
{
	if (!catalog_wait_flash(600000)) return;
	ext_flash_init();
	catalog_rewrite_sector(-1, 0, NULL, 0, 0, 0);
}

static void catalog_reclaim_internal(int wait_us)
{
	if (!catalog_wait_flash(wait_us)) return;
	ext_flash_init();
	for (int i = 0; i < EB_MAX_BOOKS; i++) {
		uint8_t flags;
		ext_flash_read(EB_CATALOG_ADDR + 0x08 + i * EB_ENTRY_SIZE, 1, &flags);
		if (catalog_entry_is_live(flags, (uint8_t)i))
			continue;
		if (flags == EB_FLAG_EMPTY)
			continue;
		/* DELETED, stale UPLOADING, or corrupted flag — compact once. */
		catalog_rewrite_sector(-1, 0, NULL, 0, 0, 0);
		return;
	}
	/* Fewer than 8 finished books but no reusable slot -> force compact. */
	if (catalog_count_done() < EB_MAX_BOOKS && catalog_scan_free_slot() < 0)
		catalog_rewrite_sector(-1, 0, NULL, 0, 0, 0);
}

void ebook_catalog_reclaim_stale(void)
{
	catalog_reclaim_internal(600000);
}

void ebook_catalog_reclaim_quick(void)
{
	catalog_reclaim_internal(200000);
}

int8_t ebook_catalog_find_free_slot(void)
{
	if (!catalog_wait_flash(600000)) return -1;
	ext_flash_init();
	for (int pass = 0; pass < 3; pass++) {
		ebook_catalog_reclaim_stale();
		int8_t slot = catalog_scan_free_slot();
		if (slot >= 0) return slot;
		if (catalog_count_done() >= EB_MAX_BOOKS)
			break;
		catalog_rewrite_sector(-1, 0, NULL, 0, 0, 0);
	}
	return -1;
}

uint8_t ebook_catalog_delete_book(uint8_t book_idx)
{
	uint8_t entries[EB_MAX_BOOKS][EB_ENTRY_SIZE];
	uint8_t hdr[8];
	int i;

	if (book_idx >= EB_MAX_BOOKS) return 1;
	if (!catalog_wait_flash(600000)) return 2;
	ext_flash_init();

	ext_flash_read(EB_CATALOG_ADDR, 8, hdr);
	for (i = 0; i < EB_MAX_BOOKS; i++) {
		uint32_t entry_addr = EB_CATALOG_ADDR + 0x08 + i * EB_ENTRY_SIZE;
		ext_flash_read(entry_addr, EB_ENTRY_SIZE, entries[i]);
	}

	ext_flash_sector_erase(EB_CATALOG_ADDR);

	if (hdr[0] != 'B' || hdr[1] != 'O' || hdr[2] != 'O' || hdr[3] != 'K') {
		hdr[0] = 'B'; hdr[1] = 'O'; hdr[2] = 'O'; hdr[3] = 'K';
		hdr[4] = 0x01;
		hdr[5] = 0;
		hdr[6] = 0;
		hdr[7] = 0;
	}
	ext_flash_page_program(EB_CATALOG_ADDR, 8, hdr);

	for (i = 0; i < EB_MAX_BOOKS; i++) {
		if (i == (int)book_idx) continue;
		if (entries[i][EB_ENT_OFF_FLAGS] != EB_FLAG_DONE) continue;
		uint32_t eaddr = EB_CATALOG_ADDR + 0x08 + i * EB_ENTRY_SIZE;
		ext_flash_page_program(eaddr, EB_ENTRY_SIZE, entries[i]);
	}
	return 0;
}

void ebook_catalog_set_font(uint8_t installed)
{
	if (!ext_flash_is_safe()) return;
	ext_flash_init();
	uint8_t val = installed ? 1 : 0;
	ext_flash_page_program(EB_CATALOG_ADDR + EB_OFF_FONT_INST, 1, &val);
}

uint8_t ebook_catalog_font_installed(void)
{
	if (!ext_flash_is_safe()) return 0;
	ext_flash_init();
	uint8_t val;
	ext_flash_read(EB_CATALOG_ADDR + EB_OFF_FONT_INST, 1, &val);
	if (val) return 1;

	// Catalog says no, but actual font data may still exist (e.g.
	// firmware update erased only the catalog header).  Check
	// the first byte of the font area.  Erased flash = 0xFF.
	uint8_t fb;
	ext_flash_read(EB_FONT_ADDR, 1, &fb);
	if (fb != 0xFF) {
		// Font data exists on flash, repair the catalog flag
		uint8_t one = 1;
		ext_flash_page_program(EB_CATALOG_ADDR + EB_OFF_FONT_INST, 1, &one);
		return 1;
	}
	return 0;
}

uint8_t ebook_catalog_read(uint8_t book_idx, uint32_t *start, uint32_t *len, uint8_t *enc, char *title)
{
	if (book_idx >= EB_MAX_BOOKS) return 0xFF;
	if (!ext_flash_is_safe()) return 0xFE;
	ext_flash_init();

	uint32_t entry_addr = EB_CATALOG_ADDR + 0x08 + book_idx * EB_ENTRY_SIZE;
	uint8_t entry[EB_ENTRY_SIZE];
	ext_flash_read(entry_addr, EB_ENTRY_SIZE, entry);

	if (entry[EB_ENT_OFF_FLAGS] != EB_FLAG_DONE)
		return entry[EB_ENT_OFF_FLAGS];

	if (title) {
		int i;
		for (i = 0; i < 20; i++) {
			title[i] = entry[EB_ENT_OFF_TITLE + i];
			if (title[i] == 0) break;
		}
		title[i] = 0;
	}
	if (start) *start = entry[EB_ENT_OFF_START] |
	                     ((uint32_t)entry[EB_ENT_OFF_START+1] << 8) |
	                     ((uint32_t)entry[EB_ENT_OFF_START+2] << 16) |
	                     ((uint32_t)entry[EB_ENT_OFF_START+3] << 24);
	if (len) *len = entry[EB_ENT_OFF_LEN] |
	                ((uint32_t)entry[EB_ENT_OFF_LEN+1] << 8) |
	                ((uint32_t)entry[EB_ENT_OFF_LEN+2] << 16) |
	                ((uint32_t)entry[EB_ENT_OFF_LEN+3] << 24);
	if (enc) *enc = entry[EB_ENT_OFF_ENC];
	return 0;
}

/* List catalog slots that hold a finished book. Returns count; fills slots[0..n-1]. */
static uint8_t ebook_catalog_list_done(uint8_t *slots)
{
	uint8_t n = 0;

	if (!ext_flash_is_safe()) return 0;
	ext_flash_init();
	for (int i = 0; i < EB_MAX_BOOKS; i++) {
		uint8_t flags;
		ext_flash_read(EB_CATALOG_ADDR + 0x08 + i * EB_ENTRY_SIZE, 1, &flags);
		if (flags == EB_FLAG_DONE) {
			if (slots) slots[n] = (uint8_t)i;
			n++;
		}
	}
	return n;
}

uint8_t ebook_get_book_count(void)
{
	return ebook_catalog_list_done(NULL);
}

uint32_t ebook_find_free_space(void)
{
	uint32_t max_end = EB_BOOKS_ADDR;
	for (int i = 0; i < EB_MAX_BOOKS; i++) {
		uint32_t start, len;
		uint8_t flags;
		if (!ext_flash_is_safe()) return EB_BOOKS_ADDR;
		ext_flash_init();
		uint32_t entry_addr = EB_CATALOG_ADDR + 0x08 + i * EB_ENTRY_SIZE;
		uint8_t entry[EB_ENTRY_SIZE];
		ext_flash_read(entry_addr, EB_ENTRY_SIZE, entry);
		flags = entry[EB_ENT_OFF_FLAGS];
		if (flags == EB_FLAG_DONE ||
		    (flags == EB_FLAG_UPLOADING && ebook_ble_upload_owns_slot((uint8_t)i))) {
			start = entry[EB_ENT_OFF_START] |
			        ((uint32_t)entry[EB_ENT_OFF_START+1] << 8) |
			        ((uint32_t)entry[EB_ENT_OFF_START+2] << 16) |
			        ((uint32_t)entry[EB_ENT_OFF_START+3] << 24);
			len = entry[EB_ENT_OFF_LEN] |
			      ((uint32_t)entry[EB_ENT_OFF_LEN+1] << 8) |
			      ((uint32_t)entry[EB_ENT_OFF_LEN+2] << 16) |
			      ((uint32_t)entry[EB_ENT_OFF_LEN+3] << 24);
			uint32_t end = ((start + len) + EB_SECTOR_SIZE - 1) & ~(EB_SECTOR_SIZE - 1);
			if (end > max_end) max_end = end;
		}
	}
	return max_end;
}

// Rewrite the catalog sector when reusing a slot whose body still has
// programmed 0-bits (DELETED / stale UPLOADING / corrupted metadata).
static void catalog_rewrite_with_entry(uint8_t book_idx, uint8_t flags, const char *title,
                                        uint32_t start, uint32_t len, uint8_t enc)
{
	catalog_rewrite_sector((int8_t)book_idx, flags, title, start, len, enc);
}

uint8_t ebook_catalog_write_entry(uint8_t book_idx, uint8_t flags, const char *title,
                                   uint32_t start, uint32_t len, uint8_t enc)
{
	if (book_idx >= EB_MAX_BOOKS) return 1;
	// Wait for EPD refresh to finish (up to 200ms)
	{
		int wait_us = 200000;
		while (!ext_flash_is_safe() && wait_us > 0) { WaitUs(1000); wait_us -= 1000; }
		if (!ext_flash_is_safe()) return 2;
	}

	ext_flash_init();

	// Only a fully erased slot (0xFF) can be page-programmed in place.
	// DELETED / UPLOADING / corrupted entries need a sector rewrite.
	uint8_t old_flags;
	ext_flash_read(EB_CATALOG_ADDR + 0x08 + book_idx * EB_ENTRY_SIZE, 1, &old_flags);
	if (old_flags != EB_FLAG_EMPTY) {
		catalog_rewrite_with_entry(book_idx, flags, title, start, len, enc);
		return 0;
	}

	// Normal case: slot is erased (0xFF / EMPTY). Direct write works.
	uint8_t entry[EB_ENTRY_SIZE];
	int i;
	for (i = 0; i < EB_ENTRY_SIZE; i++) entry[i] = 0;
	entry[EB_ENT_OFF_FLAGS] = flags;
	if (title) {
		int tlen = 0;
		while (title[tlen] && tlen < 19) tlen++;
		for (i = 0; i < tlen; i++)
			entry[EB_ENT_OFF_TITLE + i] = (uint8_t)title[i];
	}
	entry[EB_ENT_OFF_START]   = start & 0xFF;
	entry[EB_ENT_OFF_START+1] = (start >> 8) & 0xFF;
	entry[EB_ENT_OFF_START+2] = (start >> 16) & 0xFF;
	entry[EB_ENT_OFF_START+3] = (start >> 24) & 0xFF;
	entry[EB_ENT_OFF_LEN]     = len & 0xFF;
	entry[EB_ENT_OFF_LEN+1]   = (len >> 8) & 0xFF;
	entry[EB_ENT_OFF_LEN+2]   = (len >> 16) & 0xFF;
	entry[EB_ENT_OFF_LEN+3]   = (len >> 24) & 0xFF;
	entry[EB_ENT_OFF_ENC]     = enc;

	uint32_t addr = EB_CATALOG_ADDR + 0x08 + book_idx * EB_ENTRY_SIZE;
	ext_flash_page_program(addr, EB_ENTRY_SIZE, entry);
	return 0;
}

void ebook_catalog_set_flag(uint8_t book_idx, uint8_t flags)
{
	if (book_idx >= EB_MAX_BOOKS) return;
	// Wait for EPD refresh to finish (up to 200ms)
	int wait_us = 200000;
	while (!ext_flash_is_safe() && wait_us > 0) { WaitUs(1000); wait_us -= 1000; }
	if (!ext_flash_is_safe()) return;
	ext_flash_init();
	uint8_t addr_off = 0x08 + book_idx * EB_ENTRY_SIZE + EB_ENT_OFF_FLAGS;
	ext_flash_page_program(EB_CATALOG_ADDR + addr_off, 1, &flags);
}

// ===================== Page-turn history + backward pagination =====================
// PREV used to keep a single prev_char_pos snapshot with 0 doubling as "no
// history", so a second consecutive prev (or a prev right after opening a
// book at a saved position) restored position 0 and jumped to the start of
// the text.  Navigation now uses a small stack of the page starts actually
// shown this session; when the stack is empty (just opened at a restored
// position), the previous page start is derived by layout instead.

#define EB_PAGE_HIST_DEPTH 16
// The stack lives in NORMAL RAM, not retention RAM: the retention area is
// completely full (adding these 65 bytes made the linker overlay .text),
// so they do not fit there.  Deep retention sleep therefore leaves the
// contents garbage; a magic word detects that and drops the history --
// PREV then falls back to backward layout, which is exact for any position
// produced by forward paging anyway.
static uint32_t eb_page_hist[EB_PAGE_HIST_DEPTH];
static uint8_t  eb_page_hist_len;
static uint32_t eb_page_hist_magic;
#define EB_PAGE_HIST_MAGIC 0xEB11EB11u

// Scratch window for the backward layout search.  Plain RAM is fine: it is
// only used synchronously inside one ebook_prev_page() call.
static uint8_t eb_back_win[EB_PAGE_BACK_MAX + EB_READ_BUF_SIZE];

static void eb_hist_reset(void)
{
	eb_page_hist_magic = EB_PAGE_HIST_MAGIC;
	eb_page_hist_len = 0;
}

// Discard garbage history after a deep-sleep wake (magic mismatch).
static void eb_hist_check_wake(void)
{
	if (eb_page_hist_magic != EB_PAGE_HIST_MAGIC)
		eb_hist_reset();
}

static void eb_hist_push(uint32_t pos)
{
	eb_hist_check_wake();
	if (eb_page_hist_len >= EB_PAGE_HIST_DEPTH) {
		// Full: drop the oldest entry.
		for (int i = 1; i < EB_PAGE_HIST_DEPTH; i++)
			eb_page_hist[i - 1] = eb_page_hist[i];
		eb_page_hist_len = EB_PAGE_HIST_DEPTH - 1;
	}
	eb_page_hist[eb_page_hist_len++] = pos;
}

static void eb_fill_from_flash(uint32_t off, uint32_t len, uint8_t *dst)
{
	ext_flash_read(ebook_state.book_start + off, (uint16_t)len, dst);
}

static uint32_t find_prev_page_start(uint32_t S)
{
	return eb_find_prev_page_start(S, ebook_state.book_len, ebook_state.encoding,
	                               eb_fill_from_flash, eb_back_win, sizeof(eb_back_win));
}

// ===================== Init / Open / Close =====================

void ebook_init(void)
{
	ebook_catalog_init();
	/* Boot always lands on the clock screen (this is an e-reader: the user
	 * presses a button to enter a book).  We do NOT auto-resume reading even
	 * if settings.ebook_active was left set by a previous session.  The
	 * per-book reading position is still preserved in the external-flash
	 * progress ring, so re-opening the same book later resumes correctly. */
	memset(&ebook_state, 0, sizeof(ebook_state));
	eb_hist_reset();
	/* Remember which book + position was last read, purely so that the next
	 * ebook_open() (or any code that reads ebook_state) is consistent, but
	 * keep eb_mode on the clock. */
	if (settings.ebook_active) {
		ebook_state.active = 0;  // not reading yet
		ebook_state.book_idx = settings.ebook_book_idx;
		uint32_t pos = settings.ebook_char_pos;
		ebook_progress_load(ebook_state.book_idx, &pos);
		settings.ebook_char_pos = pos;
		/* Mark inactive so a crash/loop does not surprise the user by jumping
		 * into a book; opening the book sets it active again. */
		settings.ebook_active = 0;
	}
	eb_mode = EB_MODE_CLOCK;
}

void ebook_open(uint8_t book_idx)
{
	uint32_t start, len;
	uint8_t enc;
	if (ebook_catalog_read(book_idx, &start, &len, &enc, NULL) != 0) return;

	ebook_state.active = 1;
	ebook_state.book_idx = book_idx;
	ebook_state.char_pos = 0;
	ebook_state.prev_char_pos = 0;
	ebook_state.book_start = start;
	ebook_state.book_len = len;
	ebook_state.encoding = enc;
	eb_mode = EB_MODE_READ;
	epd_refresh_scene_enter(EPD_RF_SCENE_READ);

	settings.ebook_active = 1;
	settings.ebook_book_idx = book_idx;
	// Restore the saved reading position for this book (external flash ring).
	// Falls back to 0 (start) if no progress record exists yet.
	{
		uint32_t pos = 0;
		if (ebook_progress_load(book_idx, &pos)) {
			ebook_state.char_pos = pos;
		} else {
			ebook_state.char_pos = 0;
		}
		ebook_state.prev_char_pos = 0;
		settings.ebook_char_pos = ebook_state.char_pos;
		settings.ebook_prev_char_pos = 0;
	}
	// Fresh session on this book: no back-navigation history.  PREV from the
	// restored position will derive the previous page by layout.
	eb_hist_reset();
	save_settings_to_flash();

	ebook_display_current_page();
}

void ebook_close(void)
{
	ebook_state.active = 0;
	eb_mode = EB_MODE_CLOCK;
	set_EPD_wait_flush();
	epd_update(get_time(), battery_mv, 0);  // refresh clock
}

// Long-press PC0 in read mode: save and exit
void ebook_exit_to_clock(void)
{
	settings.ebook_active = 0;
	settings.ebook_char_pos = ebook_state.char_pos;
	settings.ebook_prev_char_pos = ebook_state.prev_char_pos;
	save_settings_to_flash();
	// Also persist to the per-book progress ring so re-opening lands here.
	ebook_progress_save(ebook_state.book_idx, ebook_state.char_pos);
	ebook_state.active = 0;
	eb_mode = EB_MODE_CLOCK;
	// Trigger clock display on next main_loop iteration
	set_EPD_wait_flush();
	epd_update(get_time(), battery_mv, 0);
}

void ebook_next_book(void)
{
	for (int i = 1; i <= EB_MAX_BOOKS; i++) {
		uint8_t idx = (ebook_state.book_idx + i) % EB_MAX_BOOKS;
		uint32_t start, len;
		uint8_t enc;
		if (ebook_catalog_read(idx, &start, &len, &enc, NULL) == 0) {
			ebook_state.book_idx = idx;
			ebook_state.char_pos = 0;
			ebook_state.prev_char_pos = 0;
			ebook_state.book_start = start;
			ebook_state.book_len = len;
			ebook_state.encoding = enc;
			eb_hist_reset();
			ebook_display_current_page();
			return;
		}
	}
}

// ===================== HZK16 Chinese rendering (from external flash) =====================

static void draw_hz_char(uint8_t high, uint8_t low, int x, int y)
{
	if (!ebook_catalog_font_installed()) return;
	uint8_t section = high - 0xA0;
	uint8_t position = low - 0xA0;
	if (section < 1 || section > 87 || position < 1 || position > 94) return;

	uint32_t offset = ((uint32_t)(section - 1) * 94 + (position - 1)) * EB_HZ_CHAR_BYTES;
	uint8_t bitmap[EB_HZ_CHAR_BYTES];
	ext_flash_read(EB_FONT_ADDR + offset, EB_HZ_CHAR_BYTES, bitmap);

	for (int row = 0; row < EB_HZ_CHAR_H; row++) {
		uint16_t bits = ((uint16_t)bitmap[row * 2] << 8) | bitmap[row * 2 + 1];
		int sy = y + row;
		if (sy < 0 || sy >= EB_DISP_H) continue;
		uint8_t mask = 1 << (sy & 7);
		uint8_t *base = &epd_temp[(sy >> 3) * EB_DISP_W + x];
		for (int col = 0; col < EB_HZ_CHAR_W; col++) {
			if (x + col >= EB_DISP_W) break;
			if (bits & (0x8000 >> col))
				base[col] |= mask;
		}
	}
}

// ===================== Line breaking =====================
// find_line_break_ascii()/find_line_break_gb2312() live in ebook_layout.h
// (shared with the host-side pagination test).

// ===================== Title rendering (ASCII or GB2312) =====================

static int dialog16_text_width(const char *s)
{
	GFXfont font;
	GFXglyph glyph;

	memcpy_P(&font, &Dialog_plain_16, sizeof(font));
	int w = 0;
	for (int i = 0; s[i]; i++) {
		uint8_t c = (uint8_t)s[i];
		if (c < font.first || c > font.last)
			continue;
		memcpy_P(&glyph, &font.glyph[c - font.first], sizeof(glyph));
		w += glyph.xAdvance;
	}
	return w;
}

static void ebook_commit_display(epd_rf_scene_t scene, uint8_t allow_partial)
{
	uint8_t mode = epd_refresh_pick(scene, allow_partial);
	char dbuff[32];
	sprintf(dbuff, "ebook EPD mode=%d", mode);
	ble_log(dbuff);
	EPD_Display(epd_buffer, NULL, EB_DISP_W * EB_DISP_H / 8, mode);
	if (mode == 1)
		epd_partial_ready = 1;
}

static void draw_title_string(const char *title, uint8_t enc, int x, int baseline, int max_w)
{
	const uint8_t *p = (const uint8_t *)title;

	/* Older uploads may have GB2312 bytes but enc=ASCII in catalog. */
	if (enc != EB_ENC_GB2312) {
		for (int i = 0; p[i]; i++) {
			if (p[i] >= 0xA1) { enc = EB_ENC_GB2312; break; }
		}
	}

	if (enc == EB_ENC_GB2312) {
		int cx = x;
		int y_hz = EB_HZ_Y_FROM_BASELINE(baseline);
		for (int i = 0; p[i] && cx - x < max_w; ) {
			if (p[i] >= 0xA1 && p[i + 1] >= 0xA1) {
				if (cx + EB_HZ_CHAR_W > x + max_w) break;
				draw_hz_char(p[i], p[i + 1], cx, y_hz);
				cx += EB_HZ_CHAR_W;
				i += 2;
			} else {
				/* Full-width advance, same reason as render_page_gb2312: the
				 * HZK16 A3xx letters occupy the whole 16 px cell. */
				if (cx + EB_HZ_CHAR_W > x + max_w) break;
				uint8_t c = p[i++];
				if (c == '\r' || c == '\n') continue;
				if (c < 0x20 || c > 0x7E) c = ' ';
				draw_hz_char(0xA3, c + 0x80, cx, y_hz);
				cx += EB_HZ_CHAR_W;
			}
		}
	} else {
		char clipped[21];
		GFXfont font;
		GFXglyph glyph;
		int ci = 0;
		int cx = 0;

		memcpy_P(&font, &Dialog_plain_16, sizeof(font));
		for (int i = 0; title[i] && ci < (int)sizeof(clipped) - 1; i++) {
			uint8_t c = (uint8_t)title[i];
			int adv = EB_ASCII_W;

			if (c >= font.first && c <= font.last) {
				memcpy_P(&glyph, &font.glyph[c - font.first], sizeof(glyph));
				adv = glyph.xAdvance;
			}
			if (cx + adv > max_w)
				break;
			clipped[ci++] = (char)c;
			cx += adv;
		}
		clipped[ci] = 0;
		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, x, baseline, clipped, 1);
	}
}

// ===================== Page rendering =====================

static void draw_status_bar(void)
{
	char title[21];
	uint8_t enc;
	ebook_catalog_read(ebook_state.book_idx, NULL, NULL, &enc, title);

	draw_title_string(title, enc, EB_TEXT_MARGIN, EB_BAR_TEXT_Y, EB_TITLE_MAX_W);

	char bbuf[8];
	ui_format_battery(bbuf, sizeof(bbuf));
	{
		int bw = dialog16_text_width(bbuf);
		int bx = EB_DISP_W - EB_TEXT_MARGIN - bw;
		if (bx < EB_BATT_X0)
			bx = EB_BATT_X0;
		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16,
		                     bx, EB_BAR_TEXT_Y, bbuf, 1);
	}

	obdRectangle(&obd, 0, EB_DIVIDER_Y1, EB_DISP_W - 1, EB_DIVIDER_Y2, 1, 1);
}

static uint32_t render_page_ascii(uint32_t pos)
{
	int line = 0;

	while (line < EB_LINES_PER_PAGE && pos < ebook_state.book_len) {
		int to_read = EB_READ_BUF_SIZE;
		if (pos + to_read > ebook_state.book_len)
			to_read = ebook_state.book_len - pos;
		ext_flash_read(ebook_state.book_start + pos, to_read, ebook_read_buf);

		int brk = find_line_break_ascii(ebook_read_buf, to_read, EB_MAX_LINE_W);

		char linebuf[EB_READ_BUF_SIZE + 1];
		int copy = brk < EB_READ_BUF_SIZE ? brk : EB_READ_BUF_SIZE;
		int i;
		for (i = 0; i < copy; i++) linebuf[i] = (char)ebook_read_buf[i];
		linebuf[copy] = 0;

		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16,
		                     EB_TEXT_MARGIN, EB_READ_ASCII_BASELINE(line), linebuf, 1);

		pos += brk;
		line++;

		if (brk >= to_read && to_read < EB_READ_BUF_SIZE) break;
	}
	return pos;
}

static uint32_t render_page_gb2312(uint32_t pos)
{
	int y = EB_CONTENT_TOP;
	int line = 0;

	while (line < EB_LINES_PER_PAGE && pos < ebook_state.book_len) {
		int to_read = EB_READ_BUF_SIZE;
		if (pos + to_read > ebook_state.book_len)
			to_read = ebook_state.book_len - pos;
		ext_flash_read(ebook_state.book_start + pos, to_read, ebook_read_buf);

		int brk = find_line_break_gb2312(ebook_read_buf, to_read, EB_MAX_LINE_W);

		int x = EB_TEXT_MARGIN;
		int i = 0;
		while (i < brk && x < EB_DISP_W) {
			if (ebook_read_buf[i] >= 0xA1 && i + 1 < brk && ebook_read_buf[i+1] >= 0xA1) {
				if (x + EB_HZ_CHAR_W <= EB_MAX_LINE_W) {
					draw_hz_char(ebook_read_buf[i], ebook_read_buf[i+1], x, y);
				}
				x += EB_HZ_CHAR_W;
				i += 2;
			} else {
				/* Half-width (8 px) advance overlapped the next glyph: the
				 * HZK16 A3xx "full-width ASCII" letters span the whole 16 px
				 * cell (their right stroke reaches columns 9..13), so drawing
				 * the next char at x+8 pressed it onto the previous letter
				 * (e.g. "N" + CJK "去").  Advance a full cell like the
				 * surrounding characters -- no overlap, consistent spacing. */
				uint8_t c = ebook_read_buf[i];
				if (c == '\r' || c == '\n') { i++; continue; }
				if (c < 0x20 || c > 0x7E) c = ' ';
				if (x + EB_HZ_CHAR_W <= EB_MAX_LINE_W)
					draw_hz_char(0xA3, c + 0x80, x, y);
				x += EB_HZ_CHAR_W;
				i++;
			}
		}

		pos += brk;
		y += EB_LINE_H_HZ;
		line++;

		if (brk >= to_read && to_read < EB_READ_BUF_SIZE) break;
	}
	return pos;
}

void ebook_display_current_page(void)
{
	if (!ebook_state.active || !ext_flash_is_safe()) return;
	if (epd_update_state) { render_pending = 1; return; }

	epd_clear();
	obdCreateVirtualDisplay(&obd, EB_DISP_W, EB_DISP_H, epd_temp);
	obdFill(&obd, 0, 0);
	ext_flash_init();

	draw_status_bar();

	if (ebook_state.encoding == EB_ENC_GB2312) {
		render_page_gb2312(ebook_state.char_pos);
	} else {
		render_page_ascii(ebook_state.char_pos);
	}

	FixBuffer(epd_temp, epd_buffer, EB_DISP_W, EB_DISP_H);
	ebook_commit_display(EPD_RF_SCENE_READ, 1);
	render_pending = 0;
}

void ebook_next_page(void)
{
	if (!ebook_state.active || !ext_flash_is_safe()) return;
	if (epd_update_state) { render_pending = 1; return; }

	// Compute next page start WITHOUT rendering yet
	uint32_t next_pos;
	if (ebook_state.encoding == EB_ENC_GB2312) {
		next_pos = render_page_gb2312(ebook_state.char_pos);
	} else {
		next_pos = render_page_ascii(ebook_state.char_pos);
	}

	if (next_pos >= ebook_state.book_len) {
		// Already at last page: just re-render current
		ebook_display_current_page();
		return;
	}

	// Advance to next page NOW, before visible render
	eb_hist_push(ebook_state.char_pos);
	ebook_state.prev_char_pos = ebook_state.char_pos;   // legacy settings mirror
	ebook_state.char_pos = next_pos;

	// Persist to the flash-friendly progress ring (amortised erase, so this
	// is safe to call on every page turn) + mirror into settings for the
	// legacy close/exit path.
	settings.ebook_char_pos = ebook_state.char_pos;
	settings.ebook_prev_char_pos = ebook_state.prev_char_pos;
	ebook_progress_save(ebook_state.book_idx, ebook_state.char_pos);

	// Display from new position
	ebook_display_current_page();
}

void ebook_prev_page(void)
{
	if (!ebook_state.active) return;
	if (ebook_state.char_pos == 0) return;   // already on the very first page
	if (!ext_flash_is_safe()) return;        // EPD refresh owns the SPI bus
	eb_hist_check_wake();

	uint32_t back;
	if (eb_page_hist_len > 0) {
		// Exact page we came from: pop the history stack.
		back = eb_page_hist[--eb_page_hist_len];
	} else {
		// No in-session history (e.g. just opened the book at a saved
		// position): derive the previous page start by layout.
		back = find_prev_page_start(ebook_state.char_pos);
		if (back >= ebook_state.char_pos) return;   // defensive: must step back
	}

	ebook_state.char_pos = back;
	// Legacy single-step mirror (settings compatibility only; real back
	// navigation state lives in eb_page_hist).
	ebook_state.prev_char_pos = (eb_page_hist_len > 0) ? eb_page_hist[eb_page_hist_len - 1] : 0;
	settings.ebook_char_pos = ebook_state.char_pos;
	settings.ebook_prev_char_pos = ebook_state.prev_char_pos;
	ebook_progress_save(ebook_state.book_idx, ebook_state.char_pos);

	ebook_display_current_page();
}

// ===================== Lock screen =====================

void ebook_render_lock(void)
{
	uint8_t valid = 0;
	if (!ext_flash_is_safe()) {
		/* The EPD shares CLK/MOSI with the external flash.  Rendering now
		 * would skip the image check (drawing the text fallback) and stack
		 * a second refresh on a busy panel; retry from main_loop instead. */
		render_pending = 1;
		return;
	}

	ext_flash_init();
	ext_flash_read(EB_LOCK_IMG_ADDR + EB_LOCK_IMG_FLAG_OFFSET, 1, &valid);

	if (valid == EB_LOCK_IMG_FLAG_VALUE) {
		// Has lock image: display it FULL-SCREEN (no white bar, no text overlay).
		// The uploaded image already is the complete 250x128 frame, so read it
		// straight into epd_temp and transform to epd_buffer.  Previously a
		// "white bar + Hold F to wake" overlay was drawn on top, which produced
		// a visible bar in the bottom-right and obscured the image.
		ext_flash_read(EB_LOCK_IMG_ADDR, 4000, epd_temp);
		FixBuffer(epd_temp, epd_buffer, EB_DISP_W, EB_DISP_H);
	} else {
		// No image: centered text
		epd_clear();
		obdCreateVirtualDisplay(&obd, EB_DISP_W, EB_DISP_H, epd_temp);
		obdFill(&obd, 0, 0);
		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 60, 50, "Locked", 1);
		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 30, 72, "Hold F to wake", 1);
		FixBuffer(epd_temp, epd_buffer, EB_DISP_W, EB_DISP_H);
	}

	EPD_Display(epd_buffer, NULL, EB_DISP_W * EB_DISP_H / 8, 1);  // lock: full refresh
	epd_partial_ready = 1;
}

void ebook_handle_lock(void)
{
	if (eb_mode == EB_MODE_LOCK) return;   // already locked
	// Lock is allowed from ANY screen (clock, reading, settings, ...), not
	// just the clock, so the user can put the device to sleep while reading.
	eb_prev_mode = eb_mode;               // remember where we came from
	eb_mode = EB_MODE_LOCK;
	{	// Debug: did the lock actually run, and does the panel state allow it?
		char lk[48];
		sprintf(lk, "LK:LOCK prev=%d st=%d", eb_prev_mode, epd_update_state);
		ble_log(lk);
	}
	/* Lock screen: scene change, force full refresh.  (ebook_render_lock
	 * already passes full=1; clearing the flag keeps the state consistent so
	 * the following unlock re-establishes a clean base map.) */
	epd_partial_ready = 0;
	/* Defer while the EPD is mid-refresh: the SPI bus is shared with the
	 * external flash, so rendering now would skip the lock image (leaving a
	 * "Locked / Hold F to wake" text screen despite a stored image) and start
	 * a second refresh on a busy panel.  A long-press F can land at any time,
	 * including during the per-minute clock refresh. */
	if (epd_update_state) {
		render_pending = 1;
		return;
	}
	ebook_render_lock();
}

void ebook_handle_unlock(void)
{
	if (eb_mode != EB_MODE_LOCK) return;
	// Restore the screen the user was on before locking.  If it was a
	// menu (SETTINGS/SELECT/ABOUT), always go back to CLOCK on unlock --
	// users don't expect to land on a settings screen after lock+unlock.
	eb_mode = eb_prev_mode;
	if (eb_mode == EB_MODE_SETTINGS || eb_mode == EB_MODE_SELECT || eb_mode == EB_MODE_ABOUT) {
		eb_mode = EB_MODE_CLOCK;
	}
	{
		// Debug: did the long-press unlock fire, and is the EPD available
		// (st=0) for the restore redraw, or wedged (st=1, screens freezes)?
		char lk[48];
		sprintf(lk, "LK:UNLK mode=%d p=%d st=%d", eb_mode, epd_partial_ready, epd_update_state);
		ble_log(lk);
	}
	if (eb_mode == EB_MODE_READ) {
		if (!settings.epd_partial_enabled)
			epd_partial_ready = 0;
		render_pending = 1;
		ebook_display_current_page();
	} else {
		// Everything else drops back to the clock (settings/about don't need
		// to survive sleep).
		eb_mode = EB_MODE_CLOCK;
		set_EPD_wait_flush();
		epd_update(get_time(), battery_mv, 0);
	}
}

// ===================== Select menu rendering =====================

void ebook_render_select(void)
{
	if (!ext_flash_is_safe()) { render_pending = 1; return; }
	if (epd_update_state) { render_pending = 1; return; }

	epd_clear();
	obdCreateVirtualDisplay(&obd, EB_DISP_W, EB_DISP_H, epd_temp);
	obdFill(&obd, 0, 0);
	ext_flash_init();

	uint8_t slots[EB_MAX_BOOKS];
	uint8_t cnt = ebook_catalog_list_done(slots);
	if (eb_selected > cnt) eb_selected = cnt;

	// Status bar
	char hdr[24];
	int n = 0;
	hdr[n++] = '(';
	hdr[n++] = '0' + cnt;
	hdr[n++] = ' ';
	hdr[n++] = 'b'; hdr[n++] = 'o'; hdr[n++] = 'o'; hdr[n++] = 'k'; hdr[n++] = ')';
	hdr[n] = 0;
	obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 4, EB_BAR_TEXT_Y, hdr, 1);
	obdRectangle(&obd, 0, EB_DIVIDER_Y1, EB_DISP_W - 1, EB_DIVIDER_Y2, 1, 1);

	// eb_selected 0..cnt-1 = book list item, eb_selected == cnt = "Back to Clock"
	int total = cnt + 1;
	int vis = (total < 4) ? total : 4;
	int scroll = eb_selected;
	if (scroll > total - vis) scroll = total - vis;
	if (scroll < 0) scroll = 0;

	for (int line = 0; line < vis; line++) {
		int sel = scroll + line;
		int baseline = EB_MENU_BASELINE(line);

		if (sel == cnt) {
			char item[16];
			item[0] = (sel == eb_selected) ? '>' : ' ';
			memcpy(item + 1, "Back to Clock", 14);
			item[15] = 0;
			obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16,
			                     4, baseline, item, 1);
		} else {
			char title[21] = {0};
			uint8_t enc;
			uint8_t slot = slots[sel];
			if (ebook_catalog_read(slot, NULL, NULL, &enc, title) != 0) continue;

			char arrow[2];
			arrow[0] = (sel == eb_selected) ? '>' : ' ';
			arrow[1] = 0;
			obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16,
			                     4, baseline, arrow, 1);
			draw_title_string(title, enc, 12, baseline, EB_DISP_W - 16);
		}
	}

	FixBuffer(epd_temp, epd_buffer, EB_DISP_W, EB_DISP_H);
	ebook_commit_display(EPD_RF_SCENE_MENU, 1);
	render_pending = 0;
}

// ===================== Book list (select) entry =====================

// Enter the book list from the clock screen.  Shared by the short-press-R
// and long-press-R handlers so the entry path is identical.
void ebook_enter_select(void)
{
	if (eb_mode != EB_MODE_CLOCK) return;
	eb_mode = EB_MODE_SELECT;
	eb_selected = 0;
	render_pending = 1;
	epd_refresh_scene_enter(EPD_RF_SCENE_MENU);
	ebook_render_select();
}

// ===================== Long-press PC0 entry handler =====================

void ebook_handle_long_right(void)
{
	if (eb_mode == EB_MODE_CLOCK) {
		// Enter select mode (also reachable via short-press R)
		ebook_enter_select();
	} else if (eb_mode == EB_MODE_SELECT) {
		// Long press in select: go back to clock
		eb_mode = EB_MODE_CLOCK;
		set_EPD_wait_flush();
		epd_update(get_time(), battery_mv, 0);
	} else if (eb_mode == EB_MODE_READ) {
		// Save and exit
		ebook_exit_to_clock();
	}
}

// Called by button handler
void ebook_select_confirm(void)
{
	if (eb_mode != EB_MODE_SELECT) return;
	uint8_t slots[EB_MAX_BOOKS];
	uint8_t cnt = ebook_catalog_list_done(slots);
	if (eb_selected < cnt) {
		render_pending = 1;
		ebook_open(slots[eb_selected]);
	} else {
		// Back to clock
		eb_mode = EB_MODE_CLOCK;
		set_EPD_wait_flush();
		epd_update(get_time(), battery_mv, 0);
	}
}

void ebook_select_up(void)
{
	if (eb_mode != EB_MODE_SELECT) return;
	uint8_t cnt = ebook_get_book_count();
	if (eb_selected > 0) {
		eb_selected--;
		render_pending = 1;
		ebook_render_select();
	} else {
		// At top: wrap to bottom (last book).
		eb_selected = cnt;
		render_pending = 1;
		ebook_render_select();
	}
}

void ebook_select_down(void)
{
	if (eb_mode != EB_MODE_SELECT) return;
	uint8_t cnt = ebook_get_book_count();
	if (eb_selected < cnt) {
		eb_selected++;
		render_pending = 1;
		ebook_render_select();
	} else {
		// At bottom: wrap to top.
		eb_selected = 0;
		render_pending = 1;
		ebook_render_select();
	}
}

// ===================== Settings menu =====================
// Entered by long-pressing LEFT from the clock screen. Lets the user toggle
// Bluetooth, pick the idle-sleep timeout and view the firmware version.
// Navigation uses partial refresh (0xFF) after the entry full refresh.

#define EB_SET_ITEM_COUNT 6   // BT / Sleep / Partial / GC / About / Exit
RAM static uint8_t eb_set_selected = 0;

static void ebook_render_settings(void)
{
	// CRITICAL: bind the obd virtual display to epd_temp BEFORE any draw call.
	// Without this obdFill/obdWriteStringCustom scribble into whatever buffer
	// pointer the obd struct currently holds (garbage / wrong memory),
	// which crashed the settings screen on entry.
	epd_clear();
	obdCreateVirtualDisplay(&obd, EB_DISP_W, EB_DISP_H, epd_temp);
	obdFill(&obd, 0, 0); // clear epd_temp virtual display
	char item[28];

	// Title bar
	obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16,
	                     4, EB_BAR_TEXT_Y, "Settings", 1);
	obdRectangle(&obd, 0, EB_DIVIDER_Y1, EB_DISP_W - 1, EB_DIVIDER_Y2, 1, 1);

	int vis = (EB_SET_ITEM_COUNT < 4) ? EB_SET_ITEM_COUNT : 4;
	int scroll = eb_set_selected;
	if (scroll > EB_SET_ITEM_COUNT - vis) scroll = EB_SET_ITEM_COUNT - vis;
	if (scroll < 0) scroll = 0;

	for (int line = 0; line < vis; line++) {
		int idx = scroll + line;
		int baseline = EB_MENU_BASELINE(line);
		item[0] = (idx == eb_set_selected) ? '>' : ' ';
		switch (idx) {
		case 0:
			strcpy(item + 1, settings.ble_enabled ? "BT: On" : "BT: Off");
			break;
		case 1: {
			uint8_t si = settings.sleep_timeout_idx;
			if (si >= SLEEP_TIMEOUT_COUNT) si = 2;
			sprintf(item + 1, "Sleep: %ds", g_sleep_timeout_s[si]);
			break;
		}
		case 2:
			sprintf(item + 1, "GPart: %s",
			        settings.epd_partial_enabled ? "On" : "Off");
			break;
		case 3: {
			uint8_t gi = settings.epd_gc_interval_idx;
			if (gi >= EPD_GC_INTERVAL_COUNT) gi = 0;
			sprintf(item + 1, "GC: %u", (unsigned)g_epd_gc_interval[gi]);
			break;
		}
		case 4:
			strcpy(item + 1, "About");
			break;
		case 5:
			strcpy(item + 1, "Exit");
			break;
		default:
			item[1] = 0;
			break;
		}
		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16,
		                     4, baseline, item, 1);
	}

	FixBuffer(epd_temp, epd_buffer, EB_DISP_W, EB_DISP_H);
	ebook_commit_display(EPD_RF_SCENE_MENU, 1);
	render_pending = 0;
}

void ebook_enter_settings(void)
{
	eb_mode = EB_MODE_SETTINGS;
	eb_set_selected = 0;
	epd_refresh_scene_enter(EPD_RF_SCENE_MENU);
	render_pending = 1;
	ebook_render_settings();
}

void ebook_exit_settings(void)
{
	/* Settings -> clock: scene change, full refresh. */
	eb_mode = EB_MODE_CLOCK;
	set_EPD_wait_flush();
	epd_update(get_time(), battery_mv, 0);
}

void ebook_settings_up(void)
{
	if (eb_set_selected > 0) {
		eb_set_selected--;
		render_pending = 1;
		ebook_render_settings();
	} else {
		// At top: wrap to bottom.
		eb_set_selected = EB_SET_ITEM_COUNT - 1;
		render_pending = 1;
		ebook_render_settings();
	}
}

void ebook_settings_down(void)
{
	if (eb_set_selected < EB_SET_ITEM_COUNT - 1) {
		eb_set_selected++;
		render_pending = 1;
		ebook_render_settings();
	} else {
		// At bottom: wrap to top.
		eb_set_selected = 0;
		render_pending = 1;
		ebook_render_settings();
	}
}

void ebook_settings_change(void)
{
	switch (eb_set_selected) {
	case 0: // toggle Bluetooth
		settings.ble_enabled = !settings.ble_enabled;
		ble_set_advertising(settings.ble_enabled);
		save_settings_to_flash();
		ebook_render_settings();
		break;
	case 1: // cycle sleep timeout
		settings.sleep_timeout_idx =
			(settings.sleep_timeout_idx + 1) % SLEEP_TIMEOUT_COUNT;
		save_settings_to_flash();
		ebook_render_settings();
		break;
	case 2: // toggle global partial refresh
		settings.epd_partial_enabled = !settings.epd_partial_enabled;
		save_settings_to_flash();
		ebook_render_settings();
		break;
	case 3: // cycle GC full-refresh interval
		settings.epd_gc_interval_idx =
			(settings.epd_gc_interval_idx + 1) % EPD_GC_INTERVAL_COUNT;
		save_settings_to_flash();
		ebook_render_settings();
		break;
	case 4: // About
		ebook_enter_about();
		break;
	case 5: // Exit
		ebook_exit_settings();
		break;
	default:
		break;
	}
}

// ===================== About screen =====================
// Page 1: FW version + build info
// Page 2: author/contact + avatar (top-right)
// LEFT/RIGHT: page navigation.  FRONT: back to settings.

#include "avatar_data.inc"

static uint8_t about_page = 0;   // 0 = page1, 1 = page2

// OR avatar (OBD/epd_temp format) before FixBuffer -- same path as lock image.
static void overlay_avatar_obd(void)
{
	for (int i = 0; i < 4000; i++)
		epd_temp[i] |= avatar_obd_buf[i];
}

static void ebook_render_about(void)
{
	epd_clear();
	obdCreateVirtualDisplay(&obd, EB_DISP_W, EB_DISP_H, epd_temp);
	obdFill(&obd, 0, 0);

	obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16,
	                     4, EB_BAR_TEXT_Y, "About", 1);
	obdRectangle(&obd, 0, EB_DIVIDER_Y1, EB_DISP_W - 1, EB_DIVIDER_Y2, 1, 1);
	char line[40];

	if (about_page == 0) {
		int tx = 4;
		int ty = EB_MENU_BASELINE0;
		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, tx, ty, "MiaoPaper", 1);
		ty += EB_LINE_H_ASCII;
		sprintf(line, "FW v%d.%d.%d", FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH);
		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, tx, ty, line, 1);

		sprintf(line, "Build:");
		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, tx, ty + EB_LINE_H_ASCII, line, 1);
		sprintf(line, "%s", BUILD_DATE);
		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, tx, ty + 2 * EB_LINE_H_ASCII, line, 1);
		sprintf(line, "%s", BUILD_TIME);
		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, tx, ty + 3 * EB_LINE_H_ASCII, line, 1);

		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16,
		                     4, EB_FOOTER_TEXT_Y, "R:pg2  F:back", 1);
	} else {
		int ty = EB_MENU_BASELINE0;
		sprintf(line, "Author: %s", AUTHOR_NAME);
		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 4, ty, line, 1);

		/* Email on its own line (no long label) so it fits 250px width. */
		ty += EB_LINE_H_ASCII + 6;
		sprintf(line, "%s", AUTHOR_EMAIL);
		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 4, ty, line, 1);

		ty += EB_LINE_H_ASCII;
		sprintf(line, "Bili: %s", AUTHOR_BILIBILI);
		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 4, ty, line, 1);

		ty += EB_LINE_H_ASCII;
		sprintf(line, "Git: %s", AUTHOR_GITHUB);
		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 4, ty, line, 1);

		overlay_avatar_obd();

		obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16,
		                     4, EB_FOOTER_TEXT_Y, "L:pg1  F:back", 1);
	}

	FixBuffer(epd_temp, epd_buffer, EB_DISP_W, EB_DISP_H);
	ebook_commit_display(EPD_RF_SCENE_MENU, 1);
	render_pending = 0;
}

// Navigate About pages (called by LEFT/RIGHT in ABOUT mode).
void ebook_about_prev(void)
{
	if (eb_mode != EB_MODE_ABOUT) return;
	if (about_page == 0) about_page = 1; else about_page = 0;
	ebook_render_about();
}
void ebook_about_next(void)
{
	if (eb_mode != EB_MODE_ABOUT) return;
	if (about_page == 0) about_page = 1; else about_page = 0;
	ebook_render_about();
}

void ebook_enter_about(void)
{
	eb_mode = EB_MODE_ABOUT;
	about_page = 0;
	render_pending = 1;
	ebook_render_about();
}

void ebook_exit_about(void)
{
	eb_mode = EB_MODE_SETTINGS;
	render_pending = 1;
	ebook_render_settings();
}

// Called from main_loop to render when EPD becomes available after
// a mode change that was blocked by an ongoing EPD update.
void ebook_check_pending_render(void)
{
	if (!render_pending) return;
	if (epd_update_state) return;

	render_pending = 0;
	if (eb_mode == EB_MODE_SELECT) {
		ebook_render_select();
	} else if (eb_mode == EB_MODE_READ) {
		ebook_display_current_page();
	} else if (eb_mode == EB_MODE_SETTINGS) {
		ebook_render_settings();
	} else if (eb_mode == EB_MODE_ABOUT) {
		ebook_render_about();
	} else if (eb_mode == EB_MODE_LOCK) {
		ebook_render_lock();
	} else if (eb_mode == EB_MODE_CLOCK) {
		epd_update(get_time(), battery_mv, 0);
	}
}

// ===================== Per-book reading progress =====================
// Append-only ring of 8-byte records in the dedicated 4 KB sector
// EB_PROG_ADDR.  One append per save amortises the sector erase over
// ~512 saves, so frequent page-turn saves do not wear out the flash.
//
// Record: [magic=0x5A][book_idx][char_pos b0..b3][crc]
//   crc = xor of the first 6 bytes.
// An empty (erased) slot reads as 0xFF... so an unused sector has no valid
// records; prog_find_free returns the first all-0xFF 8-byte slot.

static uint8_t prog_crc(const uint8_t rec[EB_PROG_REC_SIZE])
{
	uint8_t c = 0;
	for (int i = 0; i < EB_PROG_REC_SIZE - 1; i++) c ^= rec[i];
	return c;
}

// Find the first erased (all-0xFF) 8-byte slot index, or -1 if sector full.
static int prog_find_free_slot(void)
{
	for (int i = 0; i < EB_PROG_MAX_RECS; i++) {
		uint8_t b[EB_PROG_REC_SIZE];
		ext_flash_read(EB_PROG_ADDR + (uint32_t)i * EB_PROG_REC_SIZE,
		               EB_PROG_REC_SIZE, b);
		uint8_t all_ff = 1;
		for (int j = 0; j < EB_PROG_REC_SIZE; j++) {
			if (b[j] != 0xFF) { all_ff = 0; break; }
		}
		if (all_ff) return i;
	}
	return -1;
}

// Compact the sector: keep only the newest valid record for each book,
// erase, then rewrite those records at the start.  Returns the next free
// slot index after compaction (0 if nothing to keep).
static int prog_compact(void)
{
	uint8_t keep[EB_MAX_BOOKS][EB_PROG_REC_SIZE];
	uint8_t have[EB_MAX_BOOKS] = {0};

	// Scan whole sector, remembering the last valid record per book.
	for (int i = 0; i < EB_PROG_MAX_RECS; i++) {
		uint8_t b[EB_PROG_REC_SIZE];
		ext_flash_read(EB_PROG_ADDR + (uint32_t)i * EB_PROG_REC_SIZE,
		               EB_PROG_REC_SIZE, b);
		if (b[0] != EB_PROG_MAGIC) continue;       // empty or stale
		if (b[EB_PROG_REC_SIZE - 1] != prog_crc(b)) continue; // corrupt
		uint8_t bk = b[1];
		if (bk >= EB_MAX_BOOKS) continue;
		memcpy(keep[bk], b, EB_PROG_REC_SIZE);
		have[bk] = 1;
	}

	ext_flash_sector_erase(EB_PROG_ADDR);

	int n = 0;
	for (int bk = 0; bk < EB_MAX_BOOKS; bk++) {
		if (have[bk]) {
			ext_flash_page_program(EB_PROG_ADDR + (uint32_t)n * EB_PROG_REC_SIZE,
			                       EB_PROG_REC_SIZE, keep[bk]);
			n++;
		}
	}
	return n;
}

// Save reading progress for the given book.  Called on page turns and exit.
void ebook_progress_save(uint8_t book_idx, uint32_t char_pos)
{
	if (!ext_flash_is_safe()) return;
	if (book_idx >= EB_MAX_BOOKS) return;
	ext_flash_init();

	uint8_t rec[EB_PROG_REC_SIZE];
	rec[0] = EB_PROG_MAGIC;
	rec[1] = book_idx;
	rec[2] = (char_pos      ) & 0xFF;
	rec[3] = (char_pos >>  8) & 0xFF;
	rec[4] = (char_pos >> 16) & 0xFF;
	rec[5] = (char_pos >> 24) & 0xFF;
	rec[6] = 0x00;
	rec[7] = prog_crc(rec);

	int slot = prog_find_free_slot();
	if (slot < 0) slot = prog_compact();   // full -> compact, reuse from 0
	if (slot < 0 || slot >= EB_PROG_MAX_RECS) return;

	ext_flash_page_program(EB_PROG_ADDR + (uint32_t)slot * EB_PROG_REC_SIZE,
	                       EB_PROG_REC_SIZE, rec);
}

// Read the most recent saved position for a book.  Returns 1 if found
// (writes *pos), 0 if no valid record exists (start of book).
uint8_t ebook_progress_load(uint8_t book_idx, uint32_t *pos)
{
	if (!ext_flash_is_safe() || book_idx >= EB_MAX_BOOKS) return 0;
	ext_flash_init();

	uint32_t found = 0;
	uint8_t found_any = 0;
	for (int i = 0; i < EB_PROG_MAX_RECS; i++) {
		uint8_t b[EB_PROG_REC_SIZE];
		ext_flash_read(EB_PROG_ADDR + (uint32_t)i * EB_PROG_REC_SIZE,
		               EB_PROG_REC_SIZE, b);
		if (b[0] != EB_PROG_MAGIC) {
			// First non-magic slot: everything after is erased/unused too,
			// so we can stop early.
			break;
		}
		if (b[EB_PROG_REC_SIZE - 1] != prog_crc(b)) continue;
		if (b[1] != book_idx) continue;
		uint32_t p = (uint32_t)b[2] | ((uint32_t)b[3] << 8) |
		             ((uint32_t)b[4] << 16) | ((uint32_t)b[5] << 24);
		found = p;
		found_any = 1;
	}
	if (found_any && pos) *pos = found;
	return found_any;
}
