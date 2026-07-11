#pragma once

void epd_auto_detect_power_pin(void);
void epd_auto_detect_dcdc(void);
void epd_auto_detect_ana_bit(unsigned char addr, unsigned char bit, unsigned char en);
void epd_auto_detect_33v_pull(unsigned char addr, unsigned char mask, unsigned char en);
