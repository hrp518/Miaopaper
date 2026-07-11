#include <stdint.h>
#include <stdio.h>
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"
#include "vendor/common/blt_common.h"
#include "ble.h"
#include "main.h"
#include "epd.h"
#include "button_scan.h"

typedef struct {
    uint32_t pin;
    const char *name;
    uint8_t pull;  // PM_PIN_PULLUP_1M or PM_PIN_PULLDOWN_100K
} scan_pin_t;

// Only scan the 3 confirmed buttons (active-low):
//   FRONT = PB4, LEFT = PC4, RIGHT = PC0
static const scan_pin_t scan_pins[] = {
    {GPIO_PB4, "F", PM_PIN_PULLUP_1M},   // FRONT (front-facing button)
    {GPIO_PC4, "L", PM_PIN_PULLUP_1M},   // LEFT
    {GPIO_PC0, "R", PM_PIN_PULLUP_1M},   // RIGHT
};

#define SCAN_PIN_COUNT (sizeof(scan_pins)/sizeof(scan_pins[0]))

static uint8_t g_enabled = 0;
static uint8_t g_last_state[SCAN_PIN_COUNT];
static uint8_t g_initialized = 0;
static uint8_t g_tick_count = 0;

void button_scan_init(void)
{
    int i;
    // Force I2C controller off so PC0/PC1 (SDA/SCL) are released to GPIO.
    // Board has no NFC chip so we can shut i2c down for the duration of the scan.
    reg_i2c_mode &= ~FLD_I2C_MASTER_EN;
    reg_clk_en0 &= ~FLD_CLK0_I2C_EN;

    for (i = 0; i < SCAN_PIN_COUNT; i++)
    {
        gpio_set_func(scan_pins[i].pin, AS_GPIO);
        gpio_set_output_en(scan_pins[i].pin, 0);
        gpio_set_input_en(scan_pins[i].pin, 1);
        gpio_setup_up_down_resistor(scan_pins[i].pin, scan_pins[i].pull);
    }
    for (i = 0; i < SCAN_PIN_COUNT; i++)
    {
        g_last_state[i] = 0xFF; // sentinel: force first print
    }
    g_tick_count = 0;
    g_initialized = 1;
    ble_log("BS:L INIT 3 buttons (F=PB4 L=PC4 R=PC0)");
}

void button_scan_set_enabled(uint8_t en)
{
    g_enabled = en ? 1 : 0;
    if (g_enabled && !g_initialized)
    {
        button_scan_init();
    }
}

uint8_t button_scan_is_enabled(void)
{
    return g_enabled;
}

static void append(char *line, int *pos, int max, const char *s)
{
    while (*s && *pos < max - 1) line[(*pos)++] = *s++;
}

void button_scan_tick(void)
{
    if (!g_enabled || !g_initialized)
        return;

    if (!ble_get_connected())
        return;

    // Skip while EPD is mid-refresh (would race with SPI bus + power pin)
    if (epd_update_state)
        return;

    uint8_t cur[SCAN_PIN_COUNT];
    int i;
    uint8_t changed = 0;
    for (i = 0; i < SCAN_PIN_COUNT; i++)
    {
        cur[i] = gpio_read(scan_pins[i].pin) ? 1 : 0;
        if (cur[i] != g_last_state[i])
            changed = 1;
    }

    g_tick_count++;

    // Only emit on change (no heartbeat spam)
    if (!changed)
    {
        return;
    }

    // Build output: "BS:L DIFF name=0->1 ..."
    char line[80];
    int pos = 0;
    int max = sizeof(line);
    append(line, &pos, max, "BS:L DIFF ");
    int any = 0;
    for (i = 0; i < SCAN_PIN_COUNT; i++)
    {
        if (cur[i] != g_last_state[i])
        {
            if (any) append(line, &pos, max, " ");
            append(line, &pos, max, scan_pins[i].name);
            append(line, &pos, max, "=");
            line[pos++] = '0' + g_last_state[i];
            append(line, &pos, max, "->");
            line[pos++] = '0' + cur[i];
            any = 1;
        }
    }

    line[pos] = '\0';
    ble_log(line);

    for (i = 0; i < SCAN_PIN_COUNT; i++)
        g_last_state[i] = cur[i];
}
