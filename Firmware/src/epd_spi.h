#pragma once

#include "main.h"

// PB1 used to be driven as an EPD power gate (EPD_PWR_GPIO_INIT configured it
// as a push-pull output pulled low).  Empirical testing (see battery_scan.h)
// showed the panel is unaffected by PB1 = LOW/HIGH/high-Z, so the "gate" was
// dead code that only fought other users of PB1 (formerly UART TX, now the
// ADC scan).  PB1 is released: the macros below are kept as no-ops so the
// call sites in epd.c compile unchanged, but they no longer touch PB1.
#define EPD_PWR_GPIO_INIT()  do { } while (0)
#define EPD_POWER_ON()       do { } while (0)
#define EPD_POWER_OFF()      do { } while (0)

#define EPD_ENABLE_WRITE_CMD() gpio_write(EPD_DC, 0)
#define EPD_ENABLE_WRITE_DATA() gpio_write(EPD_DC, 1)

#define EPD_IS_BUSY() (!gpio_read(EPD_BUSY))


void EPD_init(void);
void EPD_SPI_Write(unsigned char value);
uint8_t EPD_SPI_read(void);
void EPD_WriteCmd(unsigned char cmd);
void EPD_WriteData(unsigned char data);
void EPD_CheckStatus(int max_ms);
void EPD_CheckStatus_inverted(int max_ms);
void EPD_send_lut(uint8_t lut[], int len);
void EPD_send_empty_lut(uint8_t lut, int len);
void EPD_LoadImage(unsigned char *image, int size, uint8_t cmd);