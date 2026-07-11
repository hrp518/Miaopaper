#pragma once

typedef struct Settings_struct
{
	uint32_t magic;
	uint32_t len;
	uint8_t temp_C_or_F;
	uint8_t advertising_temp_C_or_F;
	uint8_t blinking_smiley;
	uint8_t comfort_smiley;
	uint8_t show_batt_enabled;
	uint8_t advertising_interval;//advise new values - multiply by 10 for value
	uint8_t measure_interval;//time = loop interval * factor (def: about 7 * X)
	int8_t temp_offset;
	uint8_t temp_alarm_point;//divide by ten for value
	uint8_t epd_model;// EPD model: 0=auto detect, 1=BW213, 2=BWR213, 3=BWR154, 4=213ICE, 5=BWR296
	uint8_t ebook_active;
	uint8_t ebook_book_idx;
	uint32_t ebook_char_pos;
	uint32_t ebook_prev_char_pos;
	uint8_t ble_enabled;// BLE advertising: 1=on (default), 0=off. Power-on always advertises so the device stays reachable; turning it off takes effect at runtime only.
	uint8_t sleep_timeout_idx;// index into g_sleep_timeout_s: 0=15s, 1=30s, 2=60s, 3=120s (default 2)
	uint8_t epd_partial_enabled;// 0=scene switch full + else partial; 1=all partial + GC every N
	uint8_t epd_gc_interval_idx;// index into g_epd_gc_interval: 0=10,1=20,2=50,3=100
	uint8_t crc;// Needs to be at the last position otherwise the settings can not be validated on next boot!!!!
} settings_struct;

extern settings_struct settings;

void init_flash(void);
void reset_settings_to_default(void);
void save_settings_to_flash(void);
uint8_t get_crc(void);
