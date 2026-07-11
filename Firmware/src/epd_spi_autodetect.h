#pragma once

// Enumerate which physical pin is wired to each of EPD's three SPI
// signals (CS, CLK, MOSI). Always restores main.h's original SPI pin
// configuration on exit so OTA and other features keep working.
void epd_spi_pin_autodetect(void);

// Brute-force all 7P6 = 5040 permutations of 7 candidate pins across
// the 6 EPD roles (CS/CLK/MOSI/RESET/DC/BUSY). Stops on the first
// permutation that returns a non-0xFF status. Restores main.h config
// on exit.
void epd_5040_perm_test(void);
