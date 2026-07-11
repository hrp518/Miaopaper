#pragma once

extern uint8_t epd_flip_h;
extern uint8_t epd_flip_v;
extern uint8_t epd_byte_flip;
uint8_t EPD_BW_213_read_temp(void);
uint8_t EPD_BW_213_Display(unsigned char *image, int size, uint8_t full_or_partial);
void EPD_BW_213_WakeOnly(void);
void EPD_BW_213_set_sleep(void);
void epd_show_test_image(void);
void EPD_BW_213_HW_Init(void);
void EPD_BW_213_Part_Update(void);
void EPD_BW_213_Full_Update(void);
void EPD_BW_213_SetBaseMap(unsigned char *image, int size);
void EPD_BW_213_DeepSleep(void);
void epd_resolution_test(void);