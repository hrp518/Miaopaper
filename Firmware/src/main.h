#pragma once
#include <stdint.h>
#include "app_config.h"
#include "app.h"
#include "battery.h"
#include "battery_scan.h"
#include "ble.h"
#include "cmd_parser.h"
#include "epd.h"
#include "flash.h"
#include "i2c.h"
#include "ota.h"
#include "uart.h"

#define EPD_RESET 	GPIO_PA1
#define EPD_DC 		GPIO_PD4
#define EPD_BUSY 	GPIO_PA0
#define EPD_CS		GPIO_PD3
#define EPD_CLK		GPIO_PD7
#define EPD_MOSI	GPIO_PB7



