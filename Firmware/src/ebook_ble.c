#include <stdint.h>
#include <stdio.h>
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"

#include "ext_flash.h"
#include "ble.h"
#include "ebook.h"
#include "flash.h"
#include "main.h"   // app_config.h -> FW_VERSION_*
#include "OneBitDisplay.h"   // OBDISP type for the debug draw handler

// Upload state (active only during upload)
static struct {
	uint8_t  active;        // 0=idle, 1=book, 2=font
	uint8_t  book_idx;
	uint32_t write_offset;  // current flash write position
	uint32_t total_len;     // expected total bytes
	uint32_t written;       // bytes written so far
	uint32_t next_erase;    // next sector boundary to erase
} upload;

// Exposed for app.c to suppress clock refresh / auto-lock during active upload
uint8_t ebook_ble_is_uploading(void)
{
	return upload.active;
}

// Called from the BLE disconnect callback so a dropped connection mid-upload
// does not leave upload.active set forever (which would permanently suppress
// the clock refresh and auto-lock, since app.c keys both off this flag).
void ebook_ble_reset_upload(void)
{
	memset(&upload, 0, sizeof(upload));
	ebook_catalog_reclaim_stale();
}

uint8_t ebook_ble_upload_owns_slot(uint8_t book_idx)
{
	return upload.active != 0 && upload.book_idx < EB_MAX_BOOKS &&
	       upload.book_idx == book_idx;
}

static void notify_epd(uint8_t *data, int len)
{
	bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, data, len);
}

// Erase sectors as needed for the write range [offset, offset+len)
static void erase_ahead(uint32_t offset, uint16_t len)
{
	uint32_t end = offset + len;
	while (upload.next_erase < end) {
		ext_flash_sector_erase(upload.next_erase);
		upload.next_erase += EB_SECTOR_SIZE;
	}
}

// Write data to flash, handling page-alignment.
// Supports arbitrary len by looping over page-sized chunks.
static void flash_write(uint32_t addr, uint16_t len, const uint8_t *data)
{
	if (len == 0) return;

	uint16_t offset = 0;
	while (offset < len) {
		uint16_t page_off = (addr + offset) % EB_PAGE_SIZE;
		uint16_t chunk = EB_PAGE_SIZE - page_off;
		if (chunk > len - offset) chunk = len - offset;
		ext_flash_page_program(addr + offset, chunk, data + offset);
		offset += chunk;
	}
}

#define UPLOAD_STAGING_SIZE 256
#define UPLOAD_NOTIFY_STEP  256

static uint8_t staging[UPLOAD_STAGING_SIZE];
static uint16_t staging_len;
static uint32_t notify_watermark;

static void upload_staging_reset(void)
{
	staging_len = 0;
	notify_watermark = 0;
}

static void upload_notify_progress(uint8_t cmd)
{
	uint8_t resp[4] = {cmd, 0, 0, 0};
	resp[1] = (upload.written >> 16) & 0xFF;
	resp[2] = (upload.written >> 8) & 0xFF;
	resp[3] = upload.written & 0xFF;
	notify_epd(resp, 4);
	notify_watermark = upload.written;
}

static void upload_flush_staging(void)
{
	if (!staging_len)
		return;

	erase_ahead(upload.write_offset, staging_len);
	flash_write(upload.write_offset, staging_len, staging);
	upload.write_offset += staging_len;
	upload.written += staging_len;
	staging_len = 0;

	if (upload.written - notify_watermark >= UPLOAD_NOTIFY_STEP)
		upload_notify_progress(upload.active == 2 ? 0x17 : 0x11);
}

static void upload_feed_data(uint8_t *data, uint16_t data_len)
{
	while (data_len > 0) {
		uint16_t space = UPLOAD_STAGING_SIZE - staging_len;
		uint16_t n = data_len < space ? data_len : space;
		memcpy(staging + staging_len, data, n);
		staging_len += n;
		data += n;
		data_len -= n;
		if (staging_len >= UPLOAD_STAGING_SIZE)
			upload_flush_staging();
	}
}

/* Log ATT MTU + connection interval.  conn_interval_next is BLE N*1.25ms;
 * conn_interval is an internal tick/us value — do NOT multiply it by 1.25. */
static void upload_log_link_diag(const char *tag)
{
	extern _attribute_aligned_(4) st_ll_conn_slave_t bltc;
	u16 mtu = blc_att_getEffectiveMtuSize(0);
	u16 iv = bltc.conn_interval_next;
	char msg[56];

	if (iv >= 6 && iv <= 0x0C80) {
		sprintf(msg, "%s mtu=%u interval=%u.%02ums", tag, mtu,
		        (unsigned)(iv * 125 / 100), (unsigned)(iv * 125 % 100));
	} else {
		u32 us = bltc.conn_interval;
		sprintf(msg, "%s mtu=%u interval=%lu.%03lums (~internal)", tag, mtu,
		        (unsigned long)(us / 1000), (unsigned long)(us % 1000));
	}
	ble_log(msg);
}

// Find an empty, deleted, or abandoned UPLOADING catalog slot
static int8_t find_free_slot(void)
{
	return ebook_catalog_find_free_slot();
}

// Mark an upload as in-progress so main_loop stops starting new EPD refreshes
// (which would seize the shared SPI bus), then wait up to ~600ms for any
// refresh that is ALREADY running to settle.  Returns 1 if the bus is safe,
// 0 if still busy after the grace window (caller replies 0xFE so the host
// can retry).  Setting upload.active FIRST is what prevents the race where a
// per-minute clock refresh starts between the begin arriving and the safety
// check.
static uint8_t upload_begin_quiesce(uint8_t mode)
{
	memset(&upload, 0, sizeof(upload));
	upload_staging_reset();
	upload.active = mode;
	upload.book_idx = 0xFF;
	// Drain any in-flight EPD refresh (typical DU ~1s, full GC ~3s -- we can't
	// block that long in the BLE callback, so cap the wait and let the host
	// retry on a true long refresh).
	int wait_us = 600000;
	while (!ext_flash_is_safe() && wait_us > 0) { WaitUs(1000); wait_us -= 1000; }
	if (!ext_flash_is_safe()) {
		upload.active = 0;   // not actually starting; release the lock
		return 0;
	}
	ext_flash_init();
	return 1;
}

static void handle_book_begin(uint8_t *payload, unsigned int payload_len)
{
	uint8_t resp[3] = {0x10, 0xFF, 0};
	if (payload_len < 8) { notify_epd(resp, 3); return; }

	uint8_t title_len = payload[1];
	if (title_len > 19) title_len = 19;
	char title[20] = {0};
	if (title_len > 0 && payload_len >= 2 + title_len)
		memcpy(title, &payload[2], title_len);

	int data_off = 2 + title_len;
	if (payload_len < data_off + 4) { resp[1] = 0xFD; notify_epd(resp, 3); return; }

	uint32_t total_len = ((uint32_t)payload[data_off] << 16) |
	                     ((uint32_t)payload[data_off+1] << 8) |
	                     payload[data_off+2];
	uint8_t encoding = payload[data_off + 3];

	// Take the upload lock + wait for any in-flight refresh BEFORE touching the
	// catalog/flash.  On 0xFE the host retries -- far better than silently
	// racing the SPI bus.
	if (!upload_begin_quiesce(1)) { resp[1] = 0xFE; notify_epd(resp, 3); return; }

	// Find free slot
	int8_t slot = find_free_slot();
	if (slot < 0) {
		upload.active = 0;
		resp[1] = ext_flash_is_safe() ? 0x01 : 0xFE;
		notify_epd(resp, 3);
		return;
	}

	// Find free flash space
	uint32_t start = ebook_find_free_space();
	if (start + total_len > EB_FLASH_END) { upload.active = 0; resp[1] = 0x02; notify_epd(resp, 3); return; }

	// Initialize upload state (upload.active already set to 1 by quiesce)
	upload.book_idx = slot;
	upload.write_offset = start;
	upload.total_len = total_len;
	upload.next_erase = start; // will erase on first write

	// Write catalog entry as "uploading"
	if (ebook_catalog_write_entry(slot, EB_FLAG_UPLOADING, title, start, total_len, encoding) != 0) {
		resp[1] = 0xFE; // EPD became busy between check and write
		notify_epd(resp, 3);
		memset(&upload, 0, sizeof(upload));
		return;
	}

	// Request the fastest connection interval for the bulk data upload (same
	// reason as handle_font_begin): a large interval makes a multi-MB book take
	// many minutes.  Report the negotiated MTU + interval for diagnostics.
	ble_set_connection_speed(6);
	upload_log_link_diag("BBEGIN");

	resp[1] = 0x00;
	resp[2] = slot;
	notify_epd(resp, 3);
}

static void handle_book_data(uint8_t *payload, unsigned int payload_len)
{
	uint8_t resp[4] = {0x11, 0, 0, 0};
	if (upload.active != 1 || payload_len < 2) { notify_epd(resp, 4); return; }

	uint16_t data_len = payload_len - 1;
	uint8_t *data = &payload[1];

	upload_feed_data(data, data_len);
	if (upload.written - notify_watermark >= UPLOAD_NOTIFY_STEP)
		upload_notify_progress(0x11);
}

static void handle_book_end(uint8_t *payload, unsigned int payload_len)
{
	uint8_t resp[6] = {0x12, 0xFF, 0, 0, 0, 0};
	if (upload.active != 1) { notify_epd(resp, 6); return; }

	upload_flush_staging();
	upload_notify_progress(0x11);

	ext_flash_init();
	// Wait for EPD refresh to finish (up to 200ms) before writing flag.
	// handle_book_begin already checked ext_flash_is_safe(), but between then
	// and now the main_loop may have kicked off an epd_update() which sets
	// epd_update_state=1, causing ebook_catalog_set_flag to silently return.
	// The catalog entry was already written as EB_FLAG_UPLOADING during begin,
	// so we must ensure the DONE flag actually sticks.
	uint32_t actual = upload.written;
	ebook_catalog_set_flag(upload.book_idx, EB_FLAG_DONE);

	// ble_set_connection_speed(200); // disabled: keep stable interval, avoid notify drops

	resp[1] = 0x00;
	resp[2] = upload.book_idx;
	resp[3] = (actual >> 16) & 0xFF;
	resp[4] = (actual >> 8) & 0xFF;
	resp[5] = actual & 0xFF;
	notify_epd(resp, 6);

	memset(&upload, 0, sizeof(upload));
}

static void handle_book_delete(uint8_t *payload, unsigned int payload_len)
{
	uint8_t resp[2] = {0x13, 0xFF};
	if (payload_len < 2) { notify_epd(resp, 2); return; }

	uint8_t idx = payload[1];
	if (idx >= EB_MAX_BOOKS) { notify_epd(resp, 2); return; }

	ebook_catalog_delete_book(idx);
	// Reset the saved reading position for this slot so a newly-uploaded book
	// at the same index starts from page 0 instead of the old book's position.
	ebook_progress_save(idx, 0);
	resp[1] = 0x00;
	notify_epd(resp, 2);
}

static void handle_status(void)
{
	// 0x19: [font][book_count][lock_img][att_mtu]
	uint8_t resp[5] = {0x19, 0, 0, 0, 0};

	if (!ext_flash_is_safe()) { notify_epd(resp, 5); return; }
	ext_flash_init();

	// Font: double check
	uint8_t font_flag = ebook_catalog_font_installed();
	uint8_t font_first;
	ext_flash_read(EB_FONT_ADDR, 1, &font_first);
	resp[1] = (font_flag && font_first != 0xFF) ? 1 : 0;

	// Book count
	uint8_t count = 0;
	for (int i = 0; i < EB_MAX_BOOKS; i++) {
		uint32_t entry_addr = EB_CATALOG_ADDR + 0x08 + i * EB_ENTRY_SIZE;
		uint8_t entry[EB_ENTRY_SIZE];
		ext_flash_read(entry_addr, EB_ENTRY_SIZE, entry);
		if (entry[EB_ENT_OFF_FLAGS] == EB_FLAG_DONE)
			count++;
	}
	resp[2] = count;

	// Lock image status (valid flag at EB_LOCK_IMG_FLAG_OFFSET)
	uint8_t lock_flag;
	ext_flash_read(EB_LOCK_IMG_ADDR + EB_LOCK_IMG_FLAG_OFFSET, 1, &lock_flag);
	resp[3] = (lock_flag == EB_LOCK_IMG_FLAG_VALUE) ? 1 : 0;
	resp[4] = (uint8_t)ble_get_effective_mtu();

	notify_epd(resp, 5);
}

static void handle_catalog_compact(uint8_t *payload, unsigned int payload_len)
{
	(void)payload;
	(void)payload_len;
	uint8_t resp[2] = {0x1A, 0xFF};
	ebook_catalog_reclaim_stale();
	resp[1] = 0x00;
	notify_epd(resp, 2);
}

static void handle_book_list(uint8_t *payload, unsigned int payload_len)
{
	// Wait for EPD refresh to finish before reading catalog.
	int wait_us = 200000;
	while (!ext_flash_is_safe() && wait_us > 0) { WaitUs(1000); wait_us -= 1000; }
	if (!ext_flash_is_safe())
	{
		uint8_t busy[2] = {0x14, 0}; // send count=0 so HTML resolves; addLog will show busy below
		notify_epd(busy, 2);
		return;
	}
	ext_flash_init();
	ebook_catalog_reclaim_quick();

	uint8_t count_resp[2] = {0x14, 0};
	uint8_t count = 0;

	for (int i = 0; i < EB_MAX_BOOKS; i++) {
		uint32_t entry_addr = EB_CATALOG_ADDR + 0x08 + i * EB_ENTRY_SIZE;
		uint8_t entry[EB_ENTRY_SIZE];
		ext_flash_read(entry_addr, EB_ENTRY_SIZE, entry);
		if (entry[EB_ENT_OFF_FLAGS] == EB_FLAG_DONE)
			count++;
	}

	count_resp[1] = count;
	notify_epd(count_resp, 2);

	// Send each book entry (small gap avoids shallow BLE notify queue drops)
	for (int i = 0; i < EB_MAX_BOOKS; i++) {
		uint32_t entry_addr = EB_CATALOG_ADDR + 0x08 + i * EB_ENTRY_SIZE;
		uint8_t entry[EB_ENTRY_SIZE];
		ext_flash_read(entry_addr, EB_ENTRY_SIZE, entry);
		if (entry[EB_ENT_OFF_FLAGS] != EB_FLAG_DONE) continue;

		uint8_t resp[26];
		resp[0] = 0x15;
		resp[1] = i;
		memcpy(&resp[2], &entry[EB_ENT_OFF_TITLE], 20);
		uint32_t len = entry[EB_ENT_OFF_LEN] |
		               ((uint32_t)entry[EB_ENT_OFF_LEN+1] << 8) |
		               ((uint32_t)entry[EB_ENT_OFF_LEN+2] << 16);
		resp[22] = (len >> 16) & 0xFF;  // BE MSB first
		resp[23] = (len >> 8) & 0xFF;
		resp[24] = len & 0xFF;
		resp[25] = entry[EB_ENT_OFF_ENC];
		notify_epd(resp, 26);
		WaitMs(15);
	}
}

// --- Lock screen image upload ---

static void handle_lock_img_begin(uint8_t *payload, unsigned int payload_len)
{
	uint8_t resp[2] = {0x30, 0xFF};
	if (payload_len < 4) { notify_epd(resp, 2); return; }

	uint32_t total = ((uint32_t)payload[1] << 16) |
	                 ((uint32_t)payload[2] << 8) |
	                 payload[3];
	if (total != 4000) { resp[1] = 0xFD; notify_epd(resp, 2); return; }

	// The external SPI flash shares CLK/MOSI with the EPD, so wait for any
	// in-progress EPD refresh to finish before touching flash.  The old code
	// failed immediately on a busy EPD and silently skipped the erase, which
	// left the validity flag erased -> "no lock image" after every upload
	// that overlapped a refresh.
	int wait_us = 50000;
	while (!ext_flash_is_safe() && wait_us > 0) { WaitUs(1000); wait_us -= 1000; }
	if (!ext_flash_is_safe()) { resp[1] = 0xFE; notify_epd(resp, 2); return; }

	ext_flash_init();
	ext_flash_sector_erase(EB_LOCK_IMG_ADDR);

	resp[1] = 0x00;
	notify_epd(resp, 2);
}

static void handle_lock_img_data(uint8_t *payload, unsigned int payload_len)
{
	uint8_t resp[2] = {0x31, 0xFF};
	if (payload_len < 3) { notify_epd(resp, 2); return; }

	uint16_t offset = ((uint16_t)payload[1] << 8) | payload[2];
	uint16_t dlen = payload_len - 3;
	if (offset + dlen > 4000) { notify_epd(resp, 2); return; }

	// Wait for the shared EPD bus to be free (see handle_lock_img_begin).
	int wait_us = 50000;
	while (!ext_flash_is_safe() && wait_us > 0) { WaitUs(1000); wait_us -= 1000; }
	if (!ext_flash_is_safe()) { notify_epd(resp, 2); return; }

	ext_flash_init();
	ext_flash_page_program(EB_LOCK_IMG_ADDR + offset, dlen, &payload[3]);

	resp[1] = 0x00;
	notify_epd(resp, 2);
}

static void handle_lock_img_end(uint8_t *payload, unsigned int payload_len)
{
	uint8_t resp[2] = {0x32, 0xFF};
	// Wait for the shared EPD bus, then write the validity flag.
	int wait_us = 50000;
	while (!ext_flash_is_safe() && wait_us > 0) { WaitUs(1000); wait_us -= 1000; }
	if (!ext_flash_is_safe()) { notify_epd(resp, 2); return; }

	ext_flash_init();
	uint8_t valid = EB_LOCK_IMG_FLAG_VALUE;
	ext_flash_page_program(EB_LOCK_IMG_ADDR + EB_LOCK_IMG_FLAG_OFFSET, 1, &valid);
	resp[1] = 0x00;
	notify_epd(resp, 2);
}

// 0x33: Read the lock image back (4000 bytes) so the web page can preview the
// stored image after reconnect.  Streams the image in ~180-byte notify chunks:
// each chunk = [0x33][off_hi][off_lo][data...].  A trailing [0x33][0xFF][0xFF]
// marks end of stream.  If no valid image, replies [0x33][0xFE] (absent).
//
// IMPORTANT: do NOT declare a 4000-byte stack buffer here - the Telink MCU has
// very little RAM and that would overflow the stack and crash the handler
// (which is exactly why the web page saw 0/4000).  Read each chunk straight
// from flash into the small 180-byte packet buffer.
static void handle_lock_img_read(uint8_t *payload, unsigned int payload_len)
{
	// Two modes:
	//   [0x33]                         -> legacy "read all" stream (kept for
	//                                    compatibility but drops packets on a
	//                                    shallow notify queue -- see below).
	//   [0x33][off_hi][off_lo]         -> request-RESPONSE: read ONE 160-byte
	//                                    chunk at the given offset and reply
	//                                    with a single notification.  The web
	//                                    page drives one chunk at a time and
	//                                    awaits each reply, so no packet is ever
	//                                    dropped by the BLE notify queue.
	// The request-response mode is what the web page uses now; the legacy
	// blast-stream reliably lost ~3 of 23 packets because
	// bls_att_pushNotifyData silently drops when the TX notify queue is full.

	uint8_t resp[2] = {0x33, 0xFE};

	// Wait briefly for the shared EPD bus before reading the flag/image.
	int wait_us = 50000;
	while (!ext_flash_is_safe() && wait_us > 0) { WaitUs(1000); wait_us -= 1000; }
	if (!ext_flash_is_safe()) { notify_epd(resp, 2); return; }

	ext_flash_init();

	// Request-response mode: read one chunk at a given offset.
	if (payload_len >= 3) {
		uint16_t off = ((uint16_t)payload[1] << 8) | payload[2];
		if (off >= 4000) {
			// Out of range -> treat as end-of-stream marker.
			uint8_t end[3] = {0x33, 0xFF, 0xFF};
			notify_epd(end, 3);
			return;
		}
		uint16_t dlen = 4000 - off;
		if (dlen > 160) dlen = 160;   // 3-byte header + 160 = 163 < MTU
		uint8_t pkt[180];
		pkt[0] = 0x33;
		pkt[1] = (off >> 8) & 0xFF;
		pkt[2] = off & 0xFF;
		ext_flash_read(EB_LOCK_IMG_ADDR + off, dlen, &pkt[3]);
		notify_epd(pkt, 3 + dlen);
		return;
	}

	// Legacy "read all" mode (0x33 with no offset).  Streams the whole image
	// in one shot; kept for backward compatibility but unreliable.
	uint8_t hb[2] = {0x33, 0xFB};
	notify_epd(hb, 2);

	uint8_t valid = 0;
	ext_flash_read(EB_LOCK_IMG_ADDR + EB_LOCK_IMG_FLAG_OFFSET, 1, &valid);
	uint8_t flg[3] = {0x33, 0xFC, valid};
	notify_epd(flg, 3);

	if (valid != EB_LOCK_IMG_FLAG_VALUE) { notify_epd(resp, 2); return; }

	uint8_t pkt[180];
	uint16_t off = 0;
	while (off < 4000) {
		uint16_t dlen = 4000 - off;
		if (dlen > 176) dlen = 176;  // 3-byte header + 176 = 179 < MTU
		pkt[0] = 0x33;
		pkt[1] = (off >> 8) & 0xFF;
		pkt[2] = off & 0xFF;
		ext_flash_read(EB_LOCK_IMG_ADDR + off, dlen, &pkt[3]);
		notify_epd(pkt, 3 + dlen);
		WaitMs(20);
		off += dlen;
	}
	pkt[0] = 0x33; pkt[1] = 0xFF; pkt[2] = 0xFF;
	notify_epd(pkt, 3);
	WaitMs(20);
}

// 0x34: Read settings -> [0x34][ble_enabled][sleep_idx][fw_major][fw_minor][fw_patch]
static void handle_settings_read(uint8_t *payload, unsigned int payload_len)
{
	(void)payload; (void)payload_len;
	uint8_t resp[6];
	resp[0] = 0x34;
	resp[1] = settings.ble_enabled ? 1 : 0;
	resp[2] = settings.sleep_timeout_idx;
	resp[3] = FW_VERSION_MAJOR;
	resp[4] = FW_VERSION_MINOR;
	resp[5] = FW_VERSION_PATCH;
	notify_epd(resp, 6);
}

// 0x35: Write settings <- [0x35][ble_enabled][sleep_idx]
//   Applies immediately, persists to flash, replies [0x35][0x00].
static void handle_settings_write(uint8_t *payload, unsigned int payload_len)
{
	uint8_t resp[2] = {0x35, 0xFF};
	if (payload_len < 3) { notify_epd(resp, 2); return; }

	uint8_t ble = payload[1] ? 1 : 0;
	uint8_t si  = payload[2];
	if (si >= SLEEP_TIMEOUT_COUNT) si = 2;

	settings.ble_enabled = ble;
	settings.sleep_timeout_idx = si;
	// Apply BLE advertising change at runtime.
	ble_set_advertising(ble);
	save_settings_to_flash();

	resp[1] = 0x00;
	notify_epd(resp, 2);
}

// --- Font upload ---

static void handle_font_begin(uint8_t *payload, unsigned int payload_len)
{
	uint8_t resp[2] = {0x16, 0xFF};
	if (payload_len < 4) { notify_epd(resp, 2); return; }

	uint32_t total = ((uint32_t)payload[1] << 16) |
	                 ((uint32_t)payload[2] << 8) |
	                 payload[3];

	// Take the upload lock + drain any in-flight EPD refresh before touching
	// flash.  0xFE -> host retries (the old code failed instantly on a busy
	// EPD, which is exactly what made the upload abort at begin).
	if (!upload_begin_quiesce(2)) { resp[1] = 0xFE; notify_epd(resp, 2); return; }

	// upload.active already set to 2 (font) by quiesce
	upload.write_offset = EB_FONT_ADDR;
	upload.total_len = total;
	upload.next_erase = EB_FONT_ADDR;

	// Ask the central for the fastest connection interval (7.5 ms).  A large
	// interval is THE throughput killer for a 256 KB upload: at 250 ms the
	// whole font takes minutes.  The host may refuse and keep its own minimum,
	// but we must at least ask.  (Previously disabled to avoid notify drops
	// during a param update; that trade-off is not worth multi-minute uploads.)
	ble_set_connection_speed(6);
	upload_log_link_diag("FBEGIN");

	resp[1] = 0x00;
	notify_epd(resp, 2);
}

static void handle_font_data(uint8_t *payload, unsigned int payload_len)
{
	uint8_t resp[4] = {0x17, 0, 0, 0};
	if (upload.active != 2 || payload_len < 2) { notify_epd(resp, 4); return; }

	uint16_t data_len = payload_len - 1;
	uint8_t *data = &payload[1];

	upload_feed_data(data, data_len);
	if (upload.written - notify_watermark >= UPLOAD_NOTIFY_STEP)
		upload_notify_progress(0x17);
}

static void handle_font_end(uint8_t *payload, unsigned int payload_len)
{
	uint8_t resp[2] = {0x18, 0xFF};
	if (upload.active != 2) { notify_epd(resp, 2); return; }

	upload_flush_staging();
	upload_notify_progress(0x17);

	ebook_catalog_set_font(1);

	// ble_set_connection_speed(200); // disabled: avoid conn-param update side effects

	resp[1] = 0x00;
	notify_epd(resp, 2);

	memset(&upload, 0, sizeof(upload));
}

// --- Ebook control ---

static void handle_ebook_open(uint8_t *payload, unsigned int payload_len)
{
	uint8_t resp[2] = {0x20, 0xFF};
	if (payload_len < 2) { notify_epd(resp, 2); return; }

	ebook_open(payload[1]);
	if (ebook_state.active) {
		resp[1] = 0x00;
	}
	notify_epd(resp, 2);
}

static void handle_ebook_close(uint8_t *payload, unsigned int payload_len)
{
	ebook_close();
	uint8_t resp[1] = {0x21};
	notify_epd(resp, 1);
}

// ===================== Resolution debug tool (0x40 / 0x41) =====================
// 0x40: clear screen to white, draw a full-width horizontal BLACK line at the
//       given Y row, slow (full GC 0xF7) refresh.  Used to verify the panel
//       coordinate system / resolution from the web page.
// 0x41: exit debug mode, return to clock.
// Both expect buffers/globals that live in epd.c/obd.
extern uint8_t epd_temp[];
extern uint8_t epd_buffer[];
extern OBDISP obd;

static void handle_debug_draw(uint8_t *payload, unsigned int payload_len)
{
	uint8_t resp[3] = {0x40, 0xFF, 0};
	if (payload_len < 3) { notify_epd(resp, 2); return; }

	// Y as big-endian: [0x40][y_hi][y_lo]
	uint16_t y = ((uint16_t)payload[1] << 8) | payload[2];
	if (y > 127) y = 127;   // clamp to visible rows

	// Enter a dedicated mode so main_loop's clock refresh won't overwrite
	// our drawing, and ebook_check_pending_render won't re-render.
	eb_mode = EB_MODE_DEBUG;

	// White screen + draw the line on the OBD virtual buffer (epd_temp).
	epd_clear();
	obdCreateVirtualDisplay(&obd, 250, 128, epd_temp);
	obdFill(&obd, 0, 0);                       // white background (epd_temp 0 = white)
	obdRectangle(&obd, 0, y, 249, y, 1, 1);    // full-width black line (ucColor=1)

	// Convert OBD format -> epd_buffer and slow full refresh.
	FixBuffer(epd_temp, epd_buffer, 250, 128);
	EPD_Display(epd_buffer, NULL, 250 * 128 / 8, 1);   // 1 = full GC (0xF7)

	resp[1] = 0x00;
	resp[2] = (uint8_t)(y & 0xFF);   // echo the clamped Y back
	notify_epd(resp, 3);
}

static void handle_debug_exit(uint8_t *payload, unsigned int payload_len)
{
	(void)payload; (void)payload_len;
	// Back to clock; set_EPD_wait_flush forces a clean full redraw.
	eb_mode = EB_MODE_CLOCK;
	set_EPD_wait_flush();
	uint8_t resp[2] = {0x41, 0x00};
	notify_epd(resp, 2);
}

// ===================== PC1 charge-status pin test (0x42 / 0x43 / 0x44) =====
// Reconfigure PC1 and read its level, to characterise the charge-status pin.
//   0x42: PC1 weak pull-down  (100K to GND)  -> reply [0x42][level]
//   0x43: PC1 strong pull-down (driven LOW output) -> reply [0x43][level]
//   0x44: PC1 high-Z (no pull, input only)   -> reply [0x44][level]
// level = 0 (low) or 1 (high)
#include "drivers/8258/gpio_8258.h"

static void pc1_test(uint8_t cmd, GPIO_PullTypeDef pull, uint8_t drive_low)
{
	gpio_set_func(GPIO_PC1, AS_GPIO);
	gpio_set_input_en(GPIO_PC1, 1);

	if (drive_low) {
		// strong pull-down: drive output LOW
		gpio_set_output_en(GPIO_PC1, 1);
		gpio_write(GPIO_PC1, 0);
		gpio_setup_up_down_resistor(GPIO_PC1, 0);  // no pull while driven
		// let it settle, then switch back to input to read the real level
		sleep_us(100);
		gpio_set_output_en(GPIO_PC1, 0);
		gpio_setup_up_down_resistor(GPIO_PC1, pull);
		sleep_us(100);
	} else {
		gpio_set_output_en(GPIO_PC1, 0);
		gpio_setup_up_down_resistor(GPIO_PC1, pull);
		sleep_us(100);
	}

	uint8_t level = gpio_read(GPIO_PC1) ? 1 : 0;
	uint8_t resp[2] = {cmd, level};
	notify_epd(resp, 2);

	// Restore PC1 to high-Z input so subsequent is_charging() reads reflect
	// the real charge-status pin state (not the test pull/drive we just set).
	// Mirrors the static inline charge_status_init() in buttons.h -- copied
	// here because that helper is header-only and has no external symbol
	// that ebook_ble.c could link against.
	gpio_set_func(GPIO_PC1, AS_GPIO);
	gpio_set_output_en(GPIO_PC1, 0);
	gpio_set_input_en(GPIO_PC1, 1);
	gpio_setup_up_down_resistor(GPIO_PC1, 0);  // no pull = high-Z
}

static void handle_pc1_weak_pulldown(uint8_t *payload, unsigned int payload_len)
{
	(void)payload; (void)payload_len;
	pc1_test(0x42, PM_PIN_PULLDOWN_100K, 0);
}

static void handle_pc1_strong_pulldown(uint8_t *payload, unsigned int payload_len)
{
	(void)payload; (void)payload_len;
	pc1_test(0x43, 0, 1);  // drive low, then read
}

static void handle_pc1_hiz(uint8_t *payload, unsigned int payload_len)
{
	(void)payload; (void)payload_len;
	pc1_test(0x44, 0, 0);  // no pull, high-Z input
}

// --- Main dispatch ---

int ebook_ble_handle_command(uint8_t *payload, unsigned int payload_len)
{
	if (payload_len < 1) return 0;

	switch (payload[0]) {
	case 0x10: handle_book_begin(payload, payload_len); break;
	case 0x11: handle_book_data(payload, payload_len); break;
	case 0x12: handle_book_end(payload, payload_len); break;
	case 0x13: handle_book_delete(payload, payload_len); break;
	case 0x1A: handle_catalog_compact(payload, payload_len); break;
	case 0x14: handle_book_list(payload, payload_len); break;
	case 0x19: handle_status(); break;
	case 0x16: handle_font_begin(payload, payload_len); break;
	case 0x17: handle_font_data(payload, payload_len); break;
	case 0x18: handle_font_end(payload, payload_len); break;
	case 0x20: handle_ebook_open(payload, payload_len); break;
	case 0x21: handle_ebook_close(payload, payload_len); break;
	case 0x30: handle_lock_img_begin(payload, payload_len); break;
	case 0x31: handle_lock_img_data(payload, payload_len); break;
	case 0x32: handle_lock_img_end(payload, payload_len); break;
	case 0x33: handle_lock_img_read(payload, payload_len); break;
	case 0x34: handle_settings_read(payload, payload_len); break;
	case 0x35: handle_settings_write(payload, payload_len); break;
	case 0x40: handle_debug_draw(payload, payload_len); break;
	case 0x41: handle_debug_exit(payload, payload_len); break;
	case 0x42: handle_pc1_weak_pulldown(payload, payload_len); break;
	case 0x43: handle_pc1_strong_pulldown(payload, payload_len); break;
	case 0x44: handle_pc1_hiz(payload, payload_len); break;
	default: return 0;
	}
	return 1;
}
