#include <stdint.h>
#include <stdio.h>
#include "main.h"
#include "epd.h"
#include "epd_spi.h"
#include "epd_autodetect.h"
#include "drivers.h"
#include "ble.h"
#include "drivers/8258/analog.h"

// Candidate pins: all GPIOs except the EPD roles the user has confirmed.
// EPD pins (RESET/DC/BUSY/CS/CLK/MOSI) MUST stay functional so that
// SPI can read a real panel response - testing them would break the test.
// Everything else (LED/UART/NFC/other unused) is project dead code or
// stale defaults and is fair game to probe.
// QFN32: PE0-PE3 not bonded. PA5/PA6 are USB, PA7 is SWS.
static const struct { uint32_t pin; const char *name; } k_pwr_candidates[] = {
    // PA: skip PA0(EPD_BUSY), PA1(EPD_RESET) - everything else probeable
    // (PA5/PA6 USB, PA7 SWS are physically unavailable - skipped)
    {GPIO_PA2,"PA2"},{GPIO_PA3,"PA3"},{GPIO_PA4,"PA4"},
    // PB: skip PB7(EPD_DC)
    {GPIO_PB0,"PB0"},{GPIO_PB1,"PB1"},{GPIO_PB2,"PB2"},{GPIO_PB3,"PB3"},
    {GPIO_PB4,"PB4"},{GPIO_PB5,"PB5"},{GPIO_PB6,"PB6"},
    // PC: all 8 probeable (NFC + PC5)
    {GPIO_PC0,"PC0"},{GPIO_PC1,"PC1"},{GPIO_PC2,"PC2"},{GPIO_PC3,"PC3"},
    {GPIO_PC4,"PC4"},{GPIO_PC5,"PC5"},{GPIO_PC6,"PC6"},{GPIO_PC7,"PC7"},
    // PD: skip PD3(EPD_CLK), PD4(EPD_MOSI), PD7(EPD_CS); PD2 LED still probeable
    {GPIO_PD0,"PD0"},{GPIO_PD1,"PD1"},
    {GPIO_PD2,"PD2"},
    {GPIO_PD5,"PD5"},{GPIO_PD6,"PD6"},
};
#define CAND_COUNT (sizeof(k_pwr_candidates)/sizeof(k_pwr_candidates[0]))
#define CAND_COUNT (sizeof(k_pwr_candidates)/sizeof(k_pwr_candidates[0]))

// Run full EPD init sequence (mirrors 0xE3 epd_debug_init) and return
// the second SPI read value (0x2F Read RAM). Non-0xFF means the panel
// is powered and responding.
static uint8_t run_full_epd_init_test(const char *pin_name, int state)
{
    char buff[64];

    sprintf(buff, "Try %s=%d init...", pin_name, state);
    ble_log(buff);
    sleep_ms(30);

    EPD_init();

    ble_log("HW reset start");
    gpio_write(EPD_RESET, 0);
    WaitMs(20);
    gpio_write(EPD_RESET, 1);
    WaitMs(100);

    sprintf(buff, "HW reset: BUSY=%d", gpio_read(EPD_BUSY));
    ble_log(buff);
    sleep_ms(30);

    ble_log("Send SWRST 0x12");
    EPD_WriteCmd(0x12);
    WaitMs(200);

    sprintf(buff, "SWRST: BUSY=%d", gpio_read(EPD_BUSY));
    ble_log(buff);
    sleep_ms(30);

    ble_log("SPI read test...");
    EPD_WriteCmd(0x0F);
    WaitMs(10);
    uint8_t spi1 = EPD_SPI_read();
    sprintf(buff, "SPI read: 0x%02X", spi1);
    ble_log(buff);
    sleep_ms(30);

    EPD_WriteCmd(0x2F);
    WaitMs(10);
    uint8_t spi2 = EPD_SPI_read();
    sprintf(buff, "SPI read2: 0x%02X", spi2);
    ble_log(buff);
    sleep_ms(30);

    return spi2;
}

void epd_auto_detect_power_pin(void)
{
    char buff[48];

    ble_log("=== EPD PWR Auto-detect start ===");
    sleep_ms(30);

    for (int i = 0; i < CAND_COUNT; i++) {
        for (int state = 1; state >= 0; state--) {
            uint32_t pin = k_pwr_candidates[i].pin;

            gpio_set_func(pin, AS_GPIO);
            gpio_set_output_en(pin, 1);
            gpio_set_input_en(pin, 0);
            gpio_set_data_strength(pin, 1);
            gpio_setup_up_down_resistor(pin, PM_PIN_PULLUP_10K);
            gpio_write(pin, state);
            sleep_ms(50);

            uint8_t v = run_full_epd_init_test(k_pwr_candidates[i].name, state);

            if (v != 0xFF) {
                sprintf(buff, "FOUND PWR on %s (state=%d, spi2=0x%02X)",
                        k_pwr_candidates[i].name, state, v);
                ble_log(buff);
                sleep_ms(20);
                ble_log("=== EPD PWR Auto-detect end ===");
                return;
            }
        }
    }

    ble_log("No pin responded - MOS control not on tested GPIOs");
    ble_log("=== EPD PWR Auto-detect end ===");
}

// DCDC voltage sweep (analog reg 0x0c). Default 0xc4 = 1.8V, 0xc6 = 1.9V.
// Higher values raise voltage but affect whole chip - use with caution.
void epd_auto_detect_dcdc(void)
{
    char buff[48];
    const uint8_t dcdc_values[] = {0xc4, 0xc6, 0xc8, 0xca, 0xcc, 0xce, 0xff};
    const int n = sizeof(dcdc_values)/sizeof(dcdc_values[0]);

    uint8_t orig = analog_read(0x0c);
    sprintf(buff, "DCDC orig=0x%02X, sweeping...", orig);
    ble_log(buff);
    sleep_ms(30);

    for (int i = 0; i < n; i++) {
        analog_write(0x0c, dcdc_values[i]);
        sleep_ms(50);

        sprintf(buff, "DCDC=0x%02X try...", dcdc_values[i]);
        ble_log(buff);
        sleep_ms(30);

        uint8_t v = run_full_epd_init_test("DCDC", dcdc_values[i]);

        if (v != 0xFF) {
            sprintf(buff, "FOUND DCDC=0x%02X, spi2=0x%02X", dcdc_values[i], v);
            ble_log(buff);
            sprintf(buff, "Leaving DCDC at 0x%02X (was 0x%02X)", dcdc_values[i], orig);
            ble_log(buff);
            return;
        }
    }

    sprintf(buff, "No DCDC value responded, restoring 0x%02X", orig);
    ble_log(buff);
    analog_write(0x0c, orig);
}

// Toggle a single analog register bit and test EPD SPI.
// en=1 to set the bit, en=0 to clear. On hit, leaves the bit set.
void epd_auto_detect_ana_bit(unsigned char addr, unsigned char bit, unsigned char en)
{
    char buff[48];
    uint8_t orig = analog_read(addr);
    uint8_t newval = en ? (orig | bit) : (orig & ~bit);

    sprintf(buff, "ANA 0x%02X bit 0x%02X -> %d (0x%02X)",
            addr, bit, en, newval);
    ble_log(buff);
    sleep_ms(30);

    analog_write(addr, newval);
    sleep_ms(50);

    uint8_t v = run_full_epd_init_test("ANA", newval);

    if (v != 0xFF) {
        sprintf(buff, "FOUND ANA 0x%02X bit 0x%02X en=%d, spi2=0x%02X",
                addr, bit, en, v);
        ble_log(buff);
        return;
    }

    sprintf(buff, "ANA 0x%02X bit 0x%02X en=%d no hit, restoring 0x%02X",
            addr, bit, en, orig);
    ble_log(buff);
    analog_write(addr, orig);
}

// 3.3V-domain pin pull control. Per register_8258.h:1279, the 3.3V analog
// regs 0x0E..0x15 control per-pin 3.3V pull resistors:
//   0x0E=PA0-3, 0x0F=PA4-7, 0x10=PB0-3, 0x11=PB4-7
//   0x12=PC0-3, 0x13=PC4-7, 0x14=PD0-3, 0x15=PD4-7
// Each bit = one pin's 3.3V pull enable. This is the real on-board power
// control (e.g. PC5 EPD_ENABLE via MOSFET). The 1.8V PM_PIN_PULLUP used
// in run_full_epd_init_test does NOT affect this 3.3V domain.
void epd_auto_detect_33v_pull(unsigned char addr, unsigned char mask, unsigned char en)
{
    char buff[48];
    uint8_t orig = analog_read(addr);
    uint8_t newval = en ? (orig | mask) : (orig & ~mask);

    sprintf(buff, "33V pull 0x%02X mask 0x%02X -> %d (0x%02X, was 0x%02X)",
            addr, mask, en, newval, orig);
    ble_log(buff);
    sleep_ms(30);

    analog_write(addr, newval);
    sleep_ms(50);

    uint8_t v = run_full_epd_init_test("33V", newval);

    if (v != 0xFF) {
        sprintf(buff, "FOUND 33V pull 0x%02X mask 0x%02X en=%d, spi2=0x%02X",
                addr, mask, en, v);
        ble_log(buff);
        return;
    }

    sprintf(buff, "33V pull 0x%02X mask 0x%02X en=%d no hit, restoring 0x%02X",
            addr, mask, en, orig);
    ble_log(buff);
    analog_write(addr, orig);
}
