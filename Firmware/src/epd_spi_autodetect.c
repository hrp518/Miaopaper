#include <stdint.h>
#include <stdio.h>
#include "main.h"
#include "epd.h"
#include "epd_spi.h"
#include "epd_spi_autodetect.h"
#include "drivers.h"
#include "ble.h"

// 26 candidate pins. PA0/PA1/PB7 are SAFE-RESERVED (EPD_BUSY/RESET/DC).
// PA5/PA6/PA7/PE* are physical conflicts (USB/SWS/missing on QFN32).
static const struct { uint32_t pin; const char *name; } k_spi_candidates[] = {
    {GPIO_PA2,"PA2"},{GPIO_PA3,"PA3"},{GPIO_PA4,"PA4"},
    {GPIO_PB0,"PB0"},{GPIO_PB1,"PB1"},{GPIO_PB2,"PB2"},{GPIO_PB3,"PB3"},
    {GPIO_PB4,"PB4"},{GPIO_PB5,"PB5"},{GPIO_PB6,"PB6"},
    {GPIO_PC0,"PC0"},{GPIO_PC1,"PC1"},{GPIO_PC2,"PC2"},{GPIO_PC3,"PC3"},
    {GPIO_PC4,"PC4"},{GPIO_PC5,"PC5"},{GPIO_PC6,"PC6"},{GPIO_PC7,"PC7"},
    {GPIO_PD0,"PD0"},{GPIO_PD1,"PD1"},{GPIO_PD2,"PD2"},{GPIO_PD3,"PD3"},
    {GPIO_PD4,"PD4"},{GPIO_PD5,"PD5"},{GPIO_PD6,"PD6"},{GPIO_PD7,"PD7"},
};
#define SPI_CAND_COUNT (sizeof(k_spi_candidates)/sizeof(k_spi_candidates[0]))

static inline void pin_high(uint32_t pin)
{
    gpio_set_func(pin, AS_GPIO);
    gpio_set_output_en(pin, 1);
    gpio_set_input_en(pin, 0);
    gpio_set_data_strength(pin, 1);
    gpio_setup_up_down_resistor(pin, PM_PIN_PULLUP_10K);
    gpio_write(pin, 1);
}

static inline void pin_low(uint32_t pin)
{
    gpio_set_func(pin, AS_GPIO);
    gpio_set_output_en(pin, 1);
    gpio_set_input_en(pin, 0);
    gpio_set_data_strength(pin, 1);
    gpio_write(pin, 0);
}

// MOSI line: caller drives bit value; this just configures as output.
static inline void pin_out(uint32_t pin)
{
    gpio_set_func(pin, AS_GPIO);
    gpio_set_output_en(pin, 1);
    gpio_set_input_en(pin, 0);
    gpio_set_data_strength(pin, 1);
    gpio_setup_up_down_resistor(pin, PM_PIN_PULLUP_10K);
}

static inline void pin_in_release(uint32_t pin)
{
    gpio_set_func(pin, AS_GPIO);
    gpio_set_output_en(pin, 0);
    gpio_set_input_en(pin, 1);
    gpio_setup_up_down_resistor(pin, PM_PIN_PULLUP_1M);
}

// Bit-bang write 8 bits to MOSI under CLK, MSB first.
static void bb_write(uint32_t mosi, uint32_t clk, uint8_t val)
{
    WaitUs(10);
    for (int i = 0; i < 8; i++) {
        gpio_write(clk, 0);
        gpio_write(mosi, (val & 0x80) ? 1 : 0);
        val <<= 1;
        gpio_write(clk, 1);
    }
}

// Bit-bang read 8 bits from MISO (= MOSI line reused as input).
static uint8_t bb_read(uint32_t miso, uint32_t clk)
{
    uint8_t v = 0;
    gpio_set_output_en(miso, 0);
    gpio_set_input_en(miso, 1);
    WaitUs(10);
    for (int i = 0; i < 8; i++) {
        gpio_write(clk, 0);
        gpio_write(clk, 1);
        v <<= 1;
        if (gpio_read(miso)) v |= 1;
    }
    gpio_set_output_en(miso, 1);
    gpio_set_input_en(miso, 0);
    return v;
}

// Run one full SW-reset + status read using caller-supplied SPI pins.
// Always restores EPD_RESET / EPD_DC / EPD_BUSY themselves.
static uint8_t try_spi(uint32_t cs, uint32_t clk, uint32_t mosi)
{
    pin_high(cs);
    pin_low(clk);
    pin_high(mosi);

    // Hardware reset pulse on EPD_RESET (PA1 - always uses main.h pin)
    gpio_write(EPD_RESET, 0);
    WaitMs(5);
    gpio_write(EPD_RESET, 1);
    WaitMs(30);

    // Select chip
    gpio_write(cs, 0);
    // DC = command mode
    gpio_write(EPD_DC, 0);
    bb_write(mosi, clk, 0x12);  // SW reset
    gpio_write(cs, 1);
    WaitMs(50);

    // Read status 0x0F
    gpio_write(cs, 0);
    gpio_write(EPD_DC, 0);
    bb_write(mosi, clk, 0x0F);
    gpio_write(EPD_DC, 1);
    uint8_t v = bb_read(mosi, clk);
    gpio_write(cs, 1);

    return v;
}

// Restore the three EPD SPI pins to their main.h state.
static void restore_main_spi_pins(void)
{
    // CS
    gpio_set_func(EPD_CS, AS_GPIO);
    gpio_set_output_en(EPD_CS, 1);
    gpio_set_input_en(EPD_CS, 0);
    gpio_setup_up_down_resistor(EPD_CS, PM_PIN_PULLUP_1M);
    // CLK
    gpio_set_func(EPD_CLK, AS_GPIO);
    gpio_set_output_en(EPD_CLK, 1);
    gpio_set_input_en(EPD_CLK, 0);
    gpio_setup_up_down_resistor(EPD_CLK, PM_PIN_PULLUP_1M);
    // MOSI
    gpio_set_func(EPD_MOSI, AS_GPIO);
    gpio_set_output_en(EPD_MOSI, 1);
    gpio_set_input_en(EPD_MOSI, 0);
    gpio_setup_up_down_resistor(EPD_MOSI, PM_PIN_PULLUP_1M);
    // DC / RESET / BUSY in case any got disturbed
    gpio_set_func(EPD_DC, AS_GPIO);
    gpio_set_output_en(EPD_DC, 1);
    gpio_set_input_en(EPD_DC, 0);
    gpio_setup_up_down_resistor(EPD_DC, PM_PIN_PULLUP_1M);

    gpio_set_func(EPD_RESET, AS_GPIO);
    gpio_set_output_en(EPD_RESET, 1);
    gpio_set_input_en(EPD_RESET, 0);
    gpio_setup_up_down_resistor(EPD_RESET, PM_PIN_PULLUP_1M);

    gpio_set_func(EPD_BUSY, AS_GPIO);
    gpio_set_output_en(EPD_BUSY, 0);
    gpio_set_input_en(EPD_BUSY, 1);
    gpio_setup_up_down_resistor(EPD_BUSY, PM_PIN_PULLUP_1M);
}

void epd_spi_pin_autodetect(void)
{
    char buff[64];

    ble_log("=== EPD SPI Pin Auto-detect start ===");
    sleep_ms(30);

    // Show starting config
    sprintf(buff, "Start: CS=%s CLK=%s MOSI=%s (DC=%s RST=%s)",
            "PD7", "PD3", "PD4", "PB7", "PA1");
    ble_log(buff);
    sleep_ms(30);

    // ---------- Round 1: find MOSI ----------
    // CS and CLK fixed at main.h; MOSI swept through candidates.
    uint32_t found_mosi = 0;
    ble_log("Round 1: probe MOSI (CS=PD7 CLK=PD3)");
    sleep_ms(30);
    for (int i = 0; i < SPI_CAND_COUNT; i++) {
        uint32_t m = k_spi_candidates[i].pin;
        pin_out(m);
        uint8_t v = try_spi(EPD_CS, EPD_CLK, m);
        sprintf(buff, "R1 MOSI=%s -> 0x%02X", k_spi_candidates[i].name, v);
        ble_log(buff);
        sleep_ms(40);
        if (v != 0xFF) {
            found_mosi = m;
            sprintf(buff, "R1 >>> MOSI found: %s", k_spi_candidates[i].name);
            ble_log(buff);
            break;
        }
    }
    ble_log("R1 done");
    if (!found_mosi) {
        ble_log("Round 1: no MOSI pin responded");
        ble_log("Restoring main.h SPI config");
        restore_main_spi_pins();
        // Release any candidate pins we touched
        for (int i = 0; i < SPI_CAND_COUNT; i++) pin_in_release(k_spi_candidates[i].pin);
        ble_log("=== EPD SPI Pin Auto-detect end (no hit) ===");
        return;
    }

    // ---------- Round 2: find CLK ----------
    // CS fixed, MOSI=found, CLK swept.
    uint32_t found_clk = 0;
    ble_log("Round 2: probe CLK (CS=PD7, MOSI=found)");
    sleep_ms(30);
    for (int i = 0; i < SPI_CAND_COUNT; i++) {
        uint32_t c = k_spi_candidates[i].pin;
        pin_low(c);
        uint8_t v = try_spi(EPD_CS, c, found_mosi);
        sprintf(buff, "R2 CLK=%s -> 0x%02X", k_spi_candidates[i].name, v);
        ble_log(buff);
        sleep_ms(40);
        if (v != 0xFF) {
            found_clk = c;
            sprintf(buff, "R2 >>> CLK found: %s", k_spi_candidates[i].name);
            ble_log(buff);
            break;
        }
    }
    ble_log("R2 done");
    if (!found_clk) {
        ble_log("Round 2: no CLK pin responded (MOSI confirmed)");
        sprintf(buff, "Partial result: MOSI confirmed at one pin, CLK=PD3 was wrong");
        ble_log(buff);
        restore_main_spi_pins();
        for (int i = 0; i < SPI_CAND_COUNT; i++) pin_in_release(k_spi_candidates[i].pin);
        ble_log("=== EPD SPI Pin Auto-detect end ===");
        return;
    }

    // ---------- Round 3: find CS ----------
    // MOSI + CLK fixed, CS swept.
    uint32_t found_cs = 0;
    ble_log("Round 3: probe CS (MOSI+CLK found, CS=PD7 was wrong)");
    sleep_ms(30);
    for (int i = 0; i < SPI_CAND_COUNT; i++) {
        uint32_t c = k_spi_candidates[i].pin;
        pin_high(c);
        uint8_t v = try_spi(c, found_clk, found_mosi);
        sprintf(buff, "R3 CS=%s -> 0x%02X", k_spi_candidates[i].name, v);
        ble_log(buff);
        sleep_ms(40);
        if (v != 0xFF) {
            found_cs = c;
            sprintf(buff, "R3 >>> CS found: %s", k_spi_candidates[i].name);
            ble_log(buff);
            break;
        }
    }
    ble_log("R3 done");
    if (!found_cs) {
        ble_log("Round 3: no CS pin responded (MOSI+CLK confirmed)");
        sprintf(buff, "Partial result: MOSI+CLK confirmed, CS=PD7 was wrong");
        ble_log(buff);
        restore_main_spi_pins();
        for (int i = 0; i < SPI_CAND_COUNT; i++) pin_in_release(k_spi_candidates[i].pin);
        ble_log("=== EPD SPI Pin Auto-detect end ===");
        return;
    }

    sprintf(buff, "FOUND EPD SPI: MOSI=??? CLK=??? CS=???");
    // We don't have names of found_*; just say "FOUND" since we logged them above.
    ble_log("FOUND all three EPD SPI pins - see logs above for each");
    restore_main_spi_pins();
    for (int i = 0; i < SPI_CAND_COUNT; i++) pin_in_release(k_spi_candidates[i].pin);
    ble_log("=== EPD SPI Pin Auto-detect end ===");
}

// ============================================================================
// 5040-permutation brute-force: assign 6 EPD roles to 6 of 7 candidate pins
// ============================================================================

static const uint32_t k_perm_pins[7] = {
    GPIO_PA0, GPIO_PA1, GPIO_PB7, GPIO_PD3, GPIO_PD4, GPIO_PD7, GPIO_PC4
};
static const char * const k_perm_names[7] = {
    "PA0", "PA1", "PB7", "PD3", "PD4", "PD7", "PC4"
};

static inline void cfg_out_high(uint32_t pin)
{
    gpio_set_func(pin, AS_GPIO);
    gpio_set_output_en(pin, 1);
    gpio_set_input_en(pin, 0);
    gpio_set_data_strength(pin, 1);
    gpio_setup_up_down_resistor(pin, PM_PIN_PULLUP_10K);
    gpio_write(pin, 1);
}

static inline void cfg_out_low(uint32_t pin)
{
    gpio_set_func(pin, AS_GPIO);
    gpio_set_output_en(pin, 1);
    gpio_set_input_en(pin, 0);
    gpio_set_data_strength(pin, 1);
    gpio_write(pin, 0);
}

static inline void cfg_out_bidir(uint32_t pin)
{
    gpio_set_func(pin, AS_GPIO);
    gpio_set_output_en(pin, 1);
    gpio_set_input_en(pin, 0);
    gpio_set_data_strength(pin, 1);
    gpio_setup_up_down_resistor(pin, PM_PIN_PULLUP_10K);
}

static inline void cfg_in_pullup(uint32_t pin)
{
    gpio_set_func(pin, AS_GPIO);
    gpio_set_output_en(pin, 0);
    gpio_set_input_en(pin, 1);
    gpio_setup_up_down_resistor(pin, PM_PIN_PULLUP_1M);
}

static uint8_t test_perm(uint32_t cs, uint32_t clk, uint32_t mosi,
                         uint32_t rst, uint32_t dc, uint32_t busy)
{
    cfg_out_high(cs);
    cfg_out_low(clk);
    cfg_out_bidir(mosi);
    cfg_out_high(rst);
    cfg_out_low(dc);
    cfg_in_pullup(busy);

    // HW reset pulse
    gpio_write(rst, 0); WaitMs(10);
    gpio_write(rst, 1); WaitMs(50);

    // SW reset 0x12
    gpio_write(cs, 0);
    gpio_write(dc, 0);
    bb_write(mosi, clk, 0x12);
    gpio_write(cs, 1);
    WaitMs(200);

    // Read status 0x0F
    gpio_write(cs, 0);
    gpio_write(dc, 0);
    bb_write(mosi, clk, 0x0F);
    gpio_write(dc, 1);
    uint8_t v = bb_read(mosi, clk);
    gpio_write(cs, 1);
    WaitMs(5);

    return v;
}

void epd_5040_perm_test(void)
{
    char buff[96];

    ble_log("=== 5040 Permutation Test start ===");
    sleep_ms(30);

    int idx = 0;
    for (int a = 0; a < 7; a++) {
        for (int b = 0; b < 7; b++) { if (b == a) continue;
            for (int c = 0; c < 7; c++) { if (c == a || c == b) continue;
                for (int d = 0; d < 7; d++) { if (d == a || d == b || d == c) continue;
                    for (int e = 0; e < 7; e++) { if (e == a || e == b || e == c || e == d) continue;
                        for (int f = 0; f < 7; f++) { if (f == a || f == b || f == c || f == d || f == e) continue;
                            idx++;
                            uint32_t cs   = k_perm_pins[a];
                            uint32_t clk  = k_perm_pins[b];
                            uint32_t mosi = k_perm_pins[c];
                            uint32_t rst  = k_perm_pins[d];
                            uint32_t dc   = k_perm_pins[e];
                            uint32_t busy = k_perm_pins[f];

                            uint8_t v = test_perm(cs, clk, mosi, rst, dc, busy);

                            if (v != 0xFF) {
                                sprintf(buff, "FOUND #%d: CS=%s CLK=%s MOSI=%s RST=%s DC=%s BUSY=%s stat=0x%02X",
                                        idx,
                                        k_perm_names[a], k_perm_names[b], k_perm_names[c],
                                        k_perm_names[d], k_perm_names[e], k_perm_names[f],
                                        v);
                                ble_log(buff);
                                sleep_ms(20);
                                ble_log("=== 5040 Permutation Test end ===");
                                restore_main_spi_pins();
                                for (int i = 0; i < 7; i++) cfg_in_pullup(k_perm_pins[i]);
                                return;
                            }

                            if ((idx % 50) == 0) {
                                sprintf(buff, "Progress: %d/5040", idx);
                                ble_log(buff);
                                sleep_ms(15);
                            }
                        }
                    }
                }
            }
        }
    }

    ble_log("No valid permutation found in 5040 tests");
    sleep_ms(20);
    ble_log("=== 5040 Permutation Test end ===");
    restore_main_spi_pins();
    for (int i = 0; i < 7; i++) cfg_in_pullup(k_perm_pins[i]);
}
