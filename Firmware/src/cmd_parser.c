#include <stdint.h>
#include <stdio.h>
#include "main.h"
#include "epd.h"
#include "epd_bw_213.h"
#include "tl_common.h"
#include "stack/ble/ble.h"
#include "vendor/common/blt_common.h"

#include "etime.h"
#include "flash.h"
#include "ext_flash.h"
#include "epd_bw_213.h"
#include "sleep_log.h"

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
	else if(inData == 0xF6){// Dump sleep-diagnostic log (newest first) from ext flash 0x7FD000
		slp_log_dump();
	}
	else if(inData == 0xF7){// Clear sleep-diagnostic log (erase sector 0x7FD000)
		slp_log_clear();
	}
	else if(inData == 0xF2){// Set byte_flip (FixBuffer byte reverse)
		epd_byte_flip = req->dat[1] ? 1 : 0;
		set_EPD_wait_flush();
	}

}
