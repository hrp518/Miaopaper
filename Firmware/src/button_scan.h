#pragma once
#include <stdint.h>

void button_scan_init(void);
void button_scan_tick(void);
void button_scan_set_enabled(uint8_t en);
uint8_t button_scan_is_enabled(void);
