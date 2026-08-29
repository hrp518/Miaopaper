#include <stdint.h>
#include "tl_common.h"
#include "main.h"
#include "epd.h"
#include "epd_spi.h"
#include "epd_bw_213.h"
#include "OneBitDisplay.h"
#include "drivers.h"
#include "stack/ble/ble.h"

/*
 * GDEQ0213B74 — sequences from GDEQ0213B74_Arduino/GDEQ0213B74_Arduino.ino
 *
 * fp=1  EPD_HW_Init + write 0x24+0x26 + EPD_Update (0xF7) + DeepSleep
 * fp=0  wake + write 0x24 only + EPD_Part_Update (0xFF), no RESET/SWR/deep-sleep
 */

#define EPD_BW_213_WIDTH  128
#define EPD_BW_213_HEIGHT 250
#define EPD_BW_213_SIZE   4000

uint8_t epd_flip_h = 1;
uint8_t epd_flip_v = 1;
uint8_t epd_byte_flip = 0;

_attribute_ram_code_ void EPD_BW_213_WakeOnly(void);

static void EPD_BW_213_WaitBusy(int max_ms)
{
	EPD_CheckStatus(max_ms);
}

/* EPD_HW_Init register block (lines 177-209), shared by full + partial. */
static void EPD_BW_213_ApplyGeometry(void)
{
	EPD_WriteCmd(0x01);
	EPD_WriteData(0xF9);
	EPD_WriteData(0x00);
	EPD_WriteData(0x00);

	EPD_WriteCmd(0x11);
	EPD_WriteData(0x01);

	EPD_WriteCmd(0x44);
	EPD_WriteData(0x00);
	EPD_WriteData(0x0F);

	EPD_WriteCmd(0x45);
	EPD_WriteData(0xF9);
	EPD_WriteData(0x00);
	EPD_WriteData(0x00);
	EPD_WriteData(0x00);

	EPD_WriteCmd(0x3C);
	EPD_WriteData(0x05);

	EPD_WriteCmd(0x21);
	EPD_WriteData(0x00);
	EPD_WriteData(0x80);
}

static void EPD_BW_213_SetRAMCursor(void)
{
	EPD_WriteCmd(0x4E);
	EPD_WriteData(0x00);
	EPD_WriteCmd(0x4F);
	EPD_WriteData(0xF9);
	EPD_WriteData(0x00);
}

_attribute_ram_code_ void EPD_BW_213_HW_Init(void)
{
	gpio_write(EPD_RESET, 0);
	WaitMs(10);
	gpio_write(EPD_RESET, 1);
	WaitMs(10);

	EPD_BW_213_WaitBusy(100);

	EPD_WriteCmd(0x12);
	EPD_BW_213_WaitBusy(100);

	EPD_BW_213_ApplyGeometry();

	EPD_WriteCmd(0x18);
	EPD_WriteData(0x80);

	EPD_BW_213_SetRAMCursor();
	EPD_BW_213_WaitBusy(100);
}

_attribute_ram_code_ void EPD_BW_213_Full_Update(void)
{
	EPD_WriteCmd(0x22);
	EPD_WriteData(0xF7);
	EPD_WriteCmd(0x20);
	EPD_BW_213_WaitBusy(3000);
}

_attribute_ram_code_ void EPD_BW_213_Part_Update(void)
{
	EPD_WriteCmd(0x22);
	EPD_WriteData(0xFF);
	EPD_WriteCmd(0x20);
	EPD_BW_213_WaitBusy(3000);
}

_attribute_ram_code_ void EPD_BW_213_DeepSleep(void)
{
	EPD_WriteCmd(0x10);
	EPD_WriteData(0x01);
	WaitMs(100);
}

_attribute_ram_code_ uint8_t EPD_BW_213_read_temp(void)
{
	EPD_WriteCmd(0x18);
	EPD_WriteData(0x80);
	EPD_WriteCmd(0x22);
	EPD_WriteData(0xB1);
	EPD_WriteCmd(0x20);
	EPD_BW_213_WaitBusy(500);
	return 25;
}

static void EPD_BW_213_WriteFrame(unsigned char *image, int size, uint8_t ram_target)
{
	EPD_WriteCmd(ram_target);
	int row_bytes = EPD_BW_213_WIDTH / 8;
	int start = epd_flip_h ? (size - row_bytes) : 0;
	int end = epd_flip_h ? -row_bytes : size;
	int step = epd_flip_h ? -row_bytes : row_bytes;
	int i;

	for (i = start; i != end; i += step) {
		int j;
		if (epd_flip_v) {
			for (j = row_bytes - 1; j >= 0; j--) {
				uint8_t b = image[i + j];
				b = ((b >> 1) & 0x55) | ((b << 1) & 0xAA);
				b = ((b >> 2) & 0x33) | ((b << 2) & 0xCC);
				b = ((b >> 4) & 0x0F) | ((b << 4) & 0xF0);
				EPD_WriteData(b);
			}
		} else {
			for (j = 0; j < row_bytes; j++)
				EPD_WriteData(image[i + j]);
		}
	}
}

uint8_t EPD_BW_213_Display(unsigned char *image, int size, uint8_t full_or_partial)
{
	if (full_or_partial == 1) {
		/* SetRAMValue_BaseMap style: 0x24+0x26 match, then GC refresh. */
		EPD_BW_213_HW_Init();
		EPD_BW_213_SetRAMCursor();
		EPD_BW_213_WriteFrame(image, size, 0x24);
		EPD_BW_213_SetRAMCursor();
		EPD_BW_213_WriteFrame(image, size, 0x26);
		EPD_BW_213_Full_Update();
		EPD_BW_213_DeepSleep();
	} else {
		/*
		 * Partial: no GPIO/SW reset — preserves 0x26 base map from last full.
		 * Re-apply the same geometry as full refresh to keep Y aligned.
		 */
		EPD_BW_213_WakeOnly();
		EPD_BW_213_ApplyGeometry();
		EPD_BW_213_SetRAMCursor();
		EPD_BW_213_WriteFrame(image, size, 0x24);
		EPD_BW_213_Part_Update();
	}

	return 0;
}

_attribute_ram_code_ void EPD_BW_213_WakeOnly(void)
{
	EPD_WriteCmd(0x00);
	EPD_BW_213_WaitBusy(100);
}

_attribute_ram_code_ void EPD_BW_213_set_sleep(void)
{
	EPD_BW_213_DeepSleep();
}

_attribute_ram_code_ void EPD_BW_213_SetBaseMap(unsigned char *image, int size)
{
	EPD_BW_213_HW_Init();
	EPD_BW_213_SetRAMCursor();
	EPD_BW_213_WriteFrame(image, size, 0x24);
	EPD_BW_213_SetRAMCursor();
	EPD_BW_213_WriteFrame(image, size, 0x26);
	EPD_BW_213_Full_Update();
}

/* epd_test_image.inc / epd_show_test_image 已移除(BLE 调试命令 0xEE):
 * 4KB rodata,固件需保持在 128KB OTA 暂存区上限内。 */

extern uint8_t epd_buffer[epd_buffer_size];
extern uint8_t epd_temp[epd_buffer_size];
extern OBDISP obd;

void epd_resolution_test(void)
{
	ble_log("ResTest: start");
	sleep_ms(15);

	memset(epd_temp, 0xFF, 4000);

	for (int byte_row = 0; byte_row < 16; byte_row++) {
		for (int pixel_row = 0; pixel_row < 8; pixel_row++) {
			int y = byte_row * 8 + pixel_row;
			if (y % 10 == 0) {
				for (int x = 0; x < 250; x++)
					epd_temp[byte_row * 250 + x] &= ~(0x80 >> pixel_row);
			}
			epd_temp[byte_row * 250 + 0] &= ~(0x80 >> pixel_row);
		}
	}

	for (int y = 0; y < 128; y++) {
		int byte_row = y / 8;
		int pixel_row = y % 8;
		uint8_t mask = ~(0x80 >> pixel_row);
		int x = 10 + y;
		if (x < 245)
			epd_temp[byte_row * 250 + x] &= mask;
	}

	FixBuffer(epd_temp, epd_buffer, 250, 128);
	EPD_BW_213_Display(epd_buffer, 4000, 1);
	ble_log("ResTest: done");
}
