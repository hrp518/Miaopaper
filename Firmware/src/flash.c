#include <stdint.h>
#include "tl_common.h"
#include "main.h"
#include "drivers.h"
#include "vendor/common/user_config.h"
#include "app_config.h"
#include "drivers/8258/gpio_8258.h"

#include "flash.h"

#define MAGIC_WORD 0xABCFF125

// Selectable idle-sleep timeouts (seconds), indexed by settings.sleep_timeout_idx.
const uint16_t g_sleep_timeout_s[SLEEP_TIMEOUT_COUNT] = {15, 30, 60, 120};
const uint16_t g_epd_gc_interval[EPD_GC_INTERVAL_COUNT] = {10, 20, 50, 100};

RAM settings_struct settings;

void init_flash(void)
{
	flash_read_page(0x78100, sizeof(settings), (uint8_t *)&settings);

	if ((settings.magic != MAGIC_WORD) | (settings.crc != get_crc()) | (settings.len != sizeof(settings)))
	{
		reset_settings_to_default();
		save_settings_to_flash();
	}
}

void reset_settings_to_default(void)
{
	settings.magic = MAGIC_WORD;
	settings.len = sizeof(settings_struct);

	settings.temp_C_or_F = false;
	settings.advertising_temp_C_or_F = false;
	settings.blinking_smiley = false;
	settings.comfort_smiley = true;
	settings.show_batt_enabled = true;
	settings.advertising_interval = 6;
	settings.measure_interval = 10;
	settings.temp_offset = 0;
	settings.temp_alarm_point = 5;
	settings.epd_model = 0; // 0 = auto detect
	settings.ebook_active = 0;
	settings.ebook_book_idx = 0;
	settings.ebook_char_pos = 0;
	settings.ebook_prev_char_pos = 0;
	settings.ble_enabled = 1;        // BLE on by default
	settings.sleep_timeout_idx = 2;  // 60s by default
	settings.lock_read_enabled = 0;  // 阅读锁屏模式默认关: 锁屏仍显示屏保图
	settings.epd_gc_interval_idx = 0; // GC every 10 partial refreshes
	settings.super_sleep = 0;        // 超级省电默认关
}

void save_settings_to_flash(void)
{
	settings.crc = get_crc();
	flash_erase_sector(0x78100);
	flash_write_page(0x78100, sizeof(settings_struct), (uint8_t *)&settings);
}

uint8_t get_crc(void)
{
	uint8_t temp_crc = 0x00;

	for (int i = 0; i < sizeof(settings_struct) - 1; i++) // Iterate over everything expect the last value as it is CRC itself
	{
		temp_crc = temp_crc ^ ((uint8_t *)&settings)[i];
	}
	return temp_crc;
}