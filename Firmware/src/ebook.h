#pragma once
#include <stdint.h>

// --- External flash layout ---
#define EB_CATALOG_ADDR   0x000000
#define EB_FONT_ADDR      0x001000
#define EB_BOOKS_ADDR     0x041000

#define EB_CATALOG_MAGIC  0x424F4F4B
#define EB_MAX_BOOKS      8
#define EB_SECTOR_SIZE    4096
#define EB_PAGE_SIZE      256
#define EB_FLASH_END      0x800000
#define EB_LOCK_IMG_ADDR  0x7FF000  // lock-screen image: 4000 bytes, just below flash end
#define EB_LOCK_IMG_FLAG_OFFSET  0x0FF0  // valid flag within the same sector (after 4000 byte image, 96 bytes spare)
#define EB_LOCK_IMG_FLAG_VALUE   0xAA    // written at end of lock-img upload to mark valid

// Per-book reading-progress storage (external NOR flash, append-only ring).
// Lives in the 4 KB sector just below the lock image.  Book data grows up
// from 0x041000 and is capped at ~7 MB, so it never reaches here.
//   Layout: a stream of 8-byte records, appended one per save.
//     [magic=0x5A][book_idx][char_pos b0..b3][crc xor]
//   Reads scan for the newest valid record for a given book.  When the
//   sector fills up, it is erased and only the latest record per book is
//   rewritten (erase amortised over ~512 saves -> flash-friendly).
#define EB_PROG_ADDR      0x7FE000
#define EB_PROG_REC_SIZE  8
#define EB_PROG_MAX_RECS  (EB_SECTOR_SIZE / EB_PROG_REC_SIZE)  // 512
#define EB_PROG_MAGIC     0x5A

// Catalog header
#define EB_OFF_FONT_INST  0x05
#define EB_OFF_BOOK_CNT   0x06

// Book entry (32 bytes)
#define EB_ENTRY_SIZE     32
#define EB_ENT_OFF_FLAGS  0x00
#define EB_ENT_OFF_TITLE  0x01
#define EB_ENT_OFF_START  0x15
#define EB_ENT_OFF_LEN    0x19
#define EB_ENT_OFF_ENC    0x1D

// NOR flash flag scheme: all transitions only clear bits (1→0),
// never set bits (0→1). Program can only clear bits.
//   EMPTY=0xFF (erased) → UPLOADING=0xFE → DONE=0xFC → DELETED=0x00
// When reusing a DELETED slot, the entire catalog sector is erased and
// all non-deleted entries are rewritten (done in ebook_catalog_write_entry).
#define EB_FLAG_EMPTY     0xFF
#define EB_FLAG_UPLOADING 0xFE
#define EB_FLAG_DONE      0xFC
#define EB_FLAG_DELETED   0x00

#define EB_ENC_ASCII      0
#define EB_ENC_GB2312     1

// Display parameters
#define EB_DISP_W         250
#define EB_DISP_H         128
#define EB_STATUS_H       22   // status text baseline (do not move up: y<6 is off-screen)
#define EB_DIVIDER_Y1     27   // below 16px title/HZ glyphs (baseline 22 -> top ~10, bottom ~26)
#define EB_DIVIDER_Y2     28
#define EB_BAR_TEXT_Y     22   // status-bar text baseline (all screens)
#define EB_MENU_BASELINE0 (EB_DIVIDER_Y2 + 16)  // 44 - first menu row baseline
#define EB_MENU_BASELINE(row) (EB_MENU_BASELINE0 + (row) * EB_LINE_H_ASCII)
#define EB_HZ_Y_FROM_BASELINE(baseline) ((baseline) - 12)
#define EB_CONTENT_TOP    (EB_DIVIDER_Y2 + 2)   // 30 - first GB2312 text row (top)
#define EB_READ_ASCII_BASELINE0 (EB_DIVIDER_Y2 + 15)  // 43 - first ASCII line baseline
#define EB_READ_ASCII_BASELINE(row) (EB_READ_ASCII_BASELINE0 + (row) * EB_LINE_H_ASCII)
#define EB_FOOTER_TEXT_Y  (EB_DISP_H - 6)
/* Status bar: reserve right column for battery ("100%+" = 63px in Dialog_plain_16). */
#define EB_BATT_TEXT_W_MAX  68
#define EB_BATT_AREA_W      EB_BATT_TEXT_W_MAX
#define EB_BATT_X0          (EB_DISP_W - EB_TEXT_MARGIN - EB_BATT_AREA_W)
#define EB_TITLE_MAX_W      (EB_BATT_X0 - EB_TEXT_MARGIN)
/* Clock face (scene 2) — derived from EB_DIVIDER_Y2 so bar/divider stay aligned */
#define EB_CLOCK_TIME_X        40
#define EB_CLOCK_TIME_Y        (EB_DIVIDER_Y2 + 42)   /* 70 */
#define EB_CLOCK_DIV_MID_Y1    (EB_DIVIDER_Y2 + 46)   /* 74 */
#define EB_CLOCK_DIV_MID_Y2    (EB_DIVIDER_Y2 + 48)   /* 76 */
#define EB_CLOCK_INFO_Y        (EB_DIVIDER_Y2 + 64)   /* 92 */
#define EB_CLOCK_DIV_BOT_Y1    (EB_DIVIDER_Y2 + 70)   /* 98 */
#define EB_CLOCK_DIV_BOT_Y2    (EB_DIVIDER_Y2 + 72)   /* 100 */
#define EB_CLOCK_DATE_Y        (EB_DIVIDER_Y2 + 88)   /* 116 */
#define EB_CLOCK_WEEK_Y        (EB_DIVIDER_Y2 + 92)   /* 120 */
#define EB_TEXT_MARGIN    2
#define EB_MAX_LINE_W     (EB_DISP_W - 2 * EB_TEXT_MARGIN)
#define EB_LINES_PER_PAGE 5   // 5 lines fit in y<=~121 without clipping
#define EB_HZ_CHAR_W      16
#define EB_HZ_CHAR_H      16
#define EB_HZ_CHAR_BYTES  32
#define EB_ASCII_W        8
#define EB_LINE_H_ASCII   19
#define EB_LINE_H_HZ      18

#define EB_READ_BUF_SIZE  80
// A page renders at most EB_LINES_PER_PAGE lines and each line consumes at
// most EB_READ_BUF_SIZE bytes (find_line_break_* never returns more than it
// was given), so no page can span more than this many bytes.  This bounds
// how far back the prev-page search (ebook_layout.h) needs to look.
#define EB_PAGE_BACK_MAX   (EB_LINES_PER_PAGE * EB_READ_BUF_SIZE)

// UI modes
typedef enum {
	EB_MODE_CLOCK = 0,
	EB_MODE_SELECT,
	EB_MODE_READ,
	EB_MODE_LOCK,
	EB_MODE_SETTINGS,
	EB_MODE_ABOUT,
	EB_MODE_DEBUG,    // resolution debug tool (BLE 0x40 draw line at Y)
} eb_mode_t;

typedef struct {
	uint8_t  active;
	uint8_t  book_idx;
	uint32_t char_pos;
	uint32_t prev_char_pos;
	uint32_t book_start;
	uint32_t book_len;
	uint8_t  encoding;
} ebook_state_t;

extern ebook_state_t ebook_state;
extern uint8_t ebook_read_buf[EB_READ_BUF_SIZE];
extern eb_mode_t eb_mode;
extern uint8_t eb_selected;

void ebook_init(void);
void ebook_open(uint8_t book_idx);
void ebook_close(void);
void ebook_next_page(void);
void ebook_prev_page(void);
void ebook_next_book(void);
void ebook_display_current_page(void);
void ebook_render_select(void);
void ebook_exit_to_clock(void);
uint8_t ebook_get_book_count(void);
// Book list (select) handlers
void ebook_enter_select(void);
void ebook_handle_long_right(void);
void ebook_handle_lock(void);
void ebook_handle_unlock(void);
void ebook_select_confirm(void);
void ebook_select_up(void);
void ebook_select_down(void);

// Settings menu (entered by long-pressing LEFT from the clock screen)
void ebook_enter_settings(void);
void ebook_exit_settings(void);
void ebook_settings_up(void);
void ebook_settings_down(void);
void ebook_settings_change(void);

// About screen (entered from the Settings menu)
void ebook_enter_about(void);
void ebook_exit_about(void);

uint8_t ebook_catalog_read(uint8_t book_idx, uint32_t *start, uint32_t *len, uint8_t *enc, char *title);
void ebook_catalog_reclaim_stale(void);
void ebook_catalog_reclaim_quick(void);
void ebook_catalog_compact(void);
int8_t ebook_catalog_find_free_slot(void);
uint8_t ebook_catalog_delete_book(uint8_t book_idx);
uint32_t ebook_find_free_space(void);
uint8_t ebook_catalog_write_entry(uint8_t book_idx, uint8_t flags, const char *title,
                                   uint32_t start, uint32_t len, uint8_t enc);
void ebook_catalog_set_flag(uint8_t book_idx, uint8_t flags);
void ebook_catalog_init(void);
void ebook_catalog_set_font(uint8_t installed);
uint8_t ebook_catalog_font_installed(void);

int ebook_ble_handle_command(uint8_t *payload, unsigned int payload_len);
uint8_t ebook_ble_is_uploading(void);
uint8_t ebook_ble_upload_owns_slot(uint8_t book_idx);
void ebook_ble_reset_upload(void);

// Check if there's a pending render (EPD was busy during mode change)
void ebook_check_pending_render(void);

// Per-book reading progress (stored in external flash, flash-friendly ring).
void ebook_progress_save(uint8_t book_idx, uint32_t char_pos);
uint8_t ebook_progress_load(uint8_t book_idx, uint32_t *pos);
