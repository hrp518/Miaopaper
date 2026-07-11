#include <stdint.h>
#include <stdio.h>
#include "main.h"
#include "epd.h"
#include "epd_autodetect.h"
#include "epd_spi_autodetect.h"
#include "epd_bw_213.h"
#include "tl_common.h"
#include "stack/ble/ble.h"
#include "vendor/common/blt_common.h"

#include "etime.h"
#include "flash.h"
#include "button_scan.h"
#include "ext_flash.h"
#include "epd_bw_213.h"

#define testPin GPIO_PD3
void cmd_parser(void * p){
	rf_packet_att_data_t *req = (rf_packet_att_data_t*)p;
	uint8_t inData = req->dat[0];
	if(inData == 0xFF){
	gpio_set_func(testPin, AS_GPIO);
	gpio_set_output_en(testPin, 1);
	gpio_set_input_en(testPin, 0);
	sleep_ms(500);
	}else if(inData == 0xCC){
	gpio_set_func(GPIO_PD2, AS_GPIO);
	gpio_set_output_en(GPIO_PD2, 1);
	gpio_set_input_en(GPIO_PD2, 0);
	sleep_ms(500);
	}else if(inData == 0x0F){
		settings.advertising_temp_C_or_F = true;//Advertising Temp in F
	}else if(inData == 0x0C){
		settings.advertising_temp_C_or_F = false;//Advertising Temp in C
	}else if(inData == 0xB1){
		epd_display_char(req->dat[1]);
	}else if(inData == 0xB2){// Battery-cell pin auto-detect sweep: advance + sample once
		battery_scan_next();
	}else if(inData == 0xB5){// Read battery voltage on PB5 and notify
		battery_read_and_notify();
	}else if(inData == 0xB6){// One-shot sweep of all ADC pins, report each
		battery_scan_all();
	}else if(inData == 0xB7){// Toggle digital-level monitoring of free pins
		digital_scan_set_enabled(digital_scan_is_enabled() ? 0 : 1);
	}else if(inData == 0xB8){// Toggle continuous ADC scan (every 1s, all 10 ADC pins)
		adc_scan_set_enabled(adc_scan_is_enabled() ? 0 : 1);
	}else if(inData == 0xB9){// Toggle continuous chip-supply (VDD) monitor via internal VBAT/3 channel
		vdd_scan_set_enabled(vdd_scan_is_enabled() ? 0 : 1);
	}else if(inData == 0xB0){
		settings.show_batt_enabled = false;//Disable battery on LCD
	}else if(inData == 0xA0){
		settings.blinking_smiley = false;
		settings.comfort_smiley = false;
	}else if(inData == 0xA1){
		settings.blinking_smiley = false;
		settings.comfort_smiley = false;
	}else if(inData == 0xA2){
		settings.blinking_smiley = false;
		settings.comfort_smiley = false;
	}else if(inData == 0xA3){
		settings.blinking_smiley = false;
		settings.comfort_smiley = true; // Comfort Indicator
	}else if(inData == 0xAB){
		settings.blinking_smiley = true;//Smiley blinking
	}else if(inData == 0xFE){
		settings.advertising_interval = req->dat[1];//Set advertising interval with second byte, value*10second / 0=main_delay
	}else if(inData == 0xFA){
		settings.temp_offset = req->dat[1];//Set temp offset, -12,5 - +12,5 °C
	}else if(inData == 0xFC){
		settings.temp_alarm_point = req->dat[1];//Set temp alarm point value divided by 10 for temp in °C
		if(settings.temp_alarm_point==0)settings.temp_alarm_point = 1;
	}else if(inData == 0xDD){// Set time
		uint32_t new_time = (req->dat[1]<<24) +(req->dat[2]<<16) +(req->dat[3]<<8) +(req->dat[4]&0xff);
		set_time(new_time, (req->dat[5]<<8) + req->dat[6], req->dat[7], req->dat[8], req->dat[9]);
	}else if(inData == 0xDE){// Save settings in flash to default
		reset_settings_to_default();
		save_settings_to_flash();
	}else if(inData == 0xDF){// Save current settings in flash
		save_settings_to_flash();
	}
	else if(inData == 0xE0){// force set an EPD model, if it wasnt detect automatically correct
		set_EPD_model(req->dat[1]);
	}
	else if(inData == 0xE1){// force set an EPD scene
		set_EPD_scene(req->dat[1]);
	}
	else if(inData == 0xE2){// force set an EPD scene
		set_EPD_wait_flush();
	}
	else if(inData == 0xE3){// EPD debug init
		epd_debug_init();
	}
	else if(inData == 0xE4){// GPIO control: E4 + pin_idx + value (pin_idx: 0=PA7,1=PB5; value: 0=LOW,1=HIGH,2=READ)
	{
		char buff[32];
		uint8_t pin_idx = req->dat[1];
		uint8_t val = req->dat[2];
		uint32_t pin = (pin_idx == 0) ? GPIO_PA7 : GPIO_PB5;
		const char *pin_name = (pin_idx == 0) ? "PA7" : "PB5";
		if (val <= 1) {
			gpio_set_func(pin, AS_GPIO);
			gpio_set_output_en(pin, 1);
			gpio_set_input_en(pin, 0);
			gpio_write(pin, val);
			sprintf(buff, "GPIO %s=%d", pin_name, val);
		} else {
			gpio_set_func(pin, AS_GPIO);
			gpio_set_output_en(pin, 0);
			gpio_set_input_en(pin, 1);
			sprintf(buff, "GPIO %s READ=%d", pin_name, gpio_read(pin));
		}
		ble_log(buff);
	}
	}
	else if(inData == 0xE5){// EPD: partial init (prepare for local refresh)
		ext_flash_init();
		ble_log("EPD: Part Init");
		EPD_BW_213_HW_Init();
	}
	else if(inData == 0xE6){// EPD: partial refresh current buffer (0xFF LUT)
		ble_log("EPD: Part Update");
		EPD_BW_213_Part_Update();
		EPD_BW_213_DeepSleep();
	}
	else if(inData == 0xE7){// EPD: full init only (no display)
		ext_flash_init();
		ble_log("EPD: Init Only");
		EPD_BW_213_HW_Init();
	}
	else if(inData == 0xE8){// EPD: partial init + write current buffer as base + partial update
		ble_log("EPD: Part Init+Base+Update");
		{
			extern uint8_t epd_buffer[epd_buffer_size];
			EPD_BW_213_HW_Init();
			EPD_BW_213_SetBaseMap(epd_buffer, 4000);
		}
	}
	else if(inData == 0xE9){// Test analog 0x34 bit1 (USB power)
		epd_auto_detect_ana_bit(0x34, BIT(1), 1);
	}
	else if(inData == 0xEA){// Sweep 3.3V-domain pull (0x0E..0x15), all bits ON
		epd_auto_detect_33v_pull(0x0E, 0x0F, 1);  // PA0-3
		epd_auto_detect_33v_pull(0x0F, 0x0F, 1);  // PA4-7
		epd_auto_detect_33v_pull(0x10, 0x0F, 1);  // PB0-3
		epd_auto_detect_33v_pull(0x11, 0x0F, 1);  // PB4-7
		epd_auto_detect_33v_pull(0x12, 0x0F, 1);  // PC0-3
		epd_auto_detect_33v_pull(0x13, 0x0F, 1);  // PC4-7
		epd_auto_detect_33v_pull(0x14, 0x0F, 1);  // PD0-3
		epd_auto_detect_33v_pull(0x15, 0x0F, 1);  // PD4-7
	}
	else if(inData == 0xEB){// Enumerate which pin is EPD CS/CLK/MOSI
		epd_spi_pin_autodetect();
	}
	else if(inData == 0xEC){// 5040-permutation brute-force test of 7 pins across 6 EPD roles
		epd_5040_perm_test();
	}
	else if(inData == 0xEE){// Show test image (full refresh)
		epd_show_test_image();
	}
	else if(inData == 0xEF){// Show resolution test pattern
		epd_resolution_test();
	}
	else if(inData == 0xED){// GPIO button scan: 0=off, 1=on (re-arm via init each time)
		button_scan_set_enabled(req->dat[1] ? 1 : 0);
		ble_log(req->dat[1] ? "BTN SCAN ON" : "BTN SCAN OFF");
	}
	else if(inData == 0xF3){// Read external flash JEDEC ID
		if (!ext_flash_is_safe()) { ble_log("FLASH: EPD busy"); }
		else {
			ext_flash_init();
			uint8_t id[3];
			ext_flash_read_jedec_id(id);
			char buff[24];
			sprintf(buff, "JID %02X %02X %02X", id[0], id[1], id[2]);
			ble_log(buff);
		}
	}
	else if(inData == 0xF4){// Read external flash: F4 + addr_h + addr_m + addr_l + len
		if (!ext_flash_is_safe()) { ble_log("FLASH: EPD busy"); }
		else {
			uint32_t addr = ((uint32_t)req->dat[1] << 16) + (req->dat[2] << 8) + req->dat[3];
			uint8_t len = req->dat[4];
			if (len > 32) len = 32;
			ext_flash_init();
			uint8_t buf[32];
			ext_flash_read(addr, len, buf);
			char line[96];
			int pos = 0;
			for (int i = 0; i < len && pos < 90; i++)
				pos += sprintf(line + pos, "%02X ", buf[i]);
			ble_log(line);
		}
	}
	else if(inData == 0xF5){// Flash write self-test: erase a scratch sector, write a
		// known pattern, read it back, report match.  Uses the dedicated progress
		// sector 0x7FE000 (EB_PROG_ADDR), which is safe to clobber and lives on
		// the same 8 MB chip as the lock image.  This proves whether erase+program
		// actually work on this board -- if the read-back is all 0x00 or all 0xFF
		// after a "successful" write, the SPI bus / chip is not committing writes.
		if (!ext_flash_is_safe()) { ble_log("F5: EPD busy, retry"); }
		else {
			ext_flash_init();
			uint32_t addr = 0x7FE000;
			ble_log("F5: erase 0x7FE000...");
			ext_flash_sector_erase(addr);
			ble_log("F5: read-after-erase (expect all FF)...");
			uint8_t r[16];
			ext_flash_read(addr, 16, r);
			char line[96]; int pos = 0;
			for (int i = 0; i < 16 && pos < 90; i++) pos += sprintf(line+pos, "%02X ", r[i]);
			ble_log(line);
			uint8_t all_ff = 1; for (int i = 0; i < 16; i++) if (r[i] != 0xFF) all_ff = 0;
			ble_log(all_ff ? "F5: erase OK (FF)" : "F5: erase FAILED (not FF)");

			ble_log("F5: write pattern DE AD BE EF ...");
			uint8_t w[16] = {0xDE,0xAD,0xBE,0xEF,0x12,0x34,0x56,0x78,0x9A,0xBC,0xDE,0xF0,0x55,0xAA,0x00,0xFF};
			ext_flash_page_program(addr, 16, w);

			ble_log("F5: read-back (expect DE AD BE EF ...)");
			ext_flash_read(addr, 16, r);
			pos = 0; for (int i = 0; i < 16 && pos < 90; i++) pos += sprintf(line+pos, "%02X ", r[i]);
			ble_log(line);
			uint8_t match = 1; for (int i = 0; i < 16; i++) if (r[i] != w[i]) match = 0;
			ble_log(match ? "F5: WRITE OK (readback matches)" : "F5: WRITE FAILED (readback mismatch)");
		}
	}
	else if(inData == 0xF0){// Set flip_h (vertical flip)
		epd_flip_h = req->dat[1] ? 1 : 0;
		set_EPD_wait_flush();
	}
	else if(inData == 0xF1){// Set flip_v (horizontal flip)
		epd_flip_v = req->dat[1] ? 1 : 0;
		set_EPD_wait_flush();
	}
	else if(inData == 0xF2){// Set byte_flip (FixBuffer byte reverse)
		epd_byte_flip = req->dat[1] ? 1 : 0;
		set_EPD_wait_flush();
	}

}
