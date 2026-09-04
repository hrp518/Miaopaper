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

// ===================== Button/charge level monitor (BLE 0x46) =====================
// 诊断用:每当任一引脚电平变化,上报一行当前所有按钮+充电引脚电平。配合
// 按下某个按钮,观察其他引脚是否跟着变 —— 确认按钮独立 / 有无浮空或共享。
static uint8_t btnlvl_mon = 0;
static uint32_t btnlvl_next = 0;
static uint8_t btnlvl_last[4];   // F,L,R,CHG
static uint16_t btnlvl_left = 0; // remaining ticks (auto-stop)

void btn_level_monitor_start(void)
{
    btnlvl_last[0] = gpio_read(GPIO_PB4) ? 1 : 0;  // F
    btnlvl_last[1] = gpio_read(GPIO_PC4) ? 1 : 0;  // L
    btnlvl_last[2] = gpio_read(GPIO_PC0) ? 1 : 0;  // R
    btnlvl_last[3] = gpio_read(GPIO_PC1) ? 1 : 0;  // CHG
    btnlvl_mon = 1;
    btnlvl_left = 2400;   // ~2 minutes at 50ms
    btnlvl_next = clock_time() + 50ULL * CLOCK_16M_SYS_TIMER_CLK_1MS;
    char b[80];
    sprintf(b, "BTNLVL: ON  F=%d L=%d R=%d CHG=%d (active-low: 0=pressed)  CHG: 0=charging",
            btnlvl_last[0], btnlvl_last[1], btnlvl_last[2], btnlvl_last[3]);
    ble_log(b);
}

void btn_level_monitor_stop(void)
{
    if (btnlvl_mon) { btnlvl_mon = 0; ble_log("BTNLVL: OFF"); }
}

void btn_level_monitor_tick(void)
{
    if (!btnlvl_mon) return;
    if ((int32_t)(clock_time() - btnlvl_next) < 0) return;
    btnlvl_next = clock_time() + 50ULL * CLOCK_16M_SYS_TIMER_CLK_1MS;
    if (btnlvl_left) btnlvl_left--;

    uint8_t f = gpio_read(GPIO_PB4) ? 1 : 0;
    uint8_t l = gpio_read(GPIO_PC4) ? 1 : 0;
    uint8_t r = gpio_read(GPIO_PC0) ? 1 : 0;
    uint8_t c = gpio_read(GPIO_PC1) ? 1 : 0;

    if (f != btnlvl_last[0] || l != btnlvl_last[1] || r != btnlvl_last[2] || c != btnlvl_last[3]) {
        char b[64];
        sprintf(b, "BTNLVL: F=%d L=%d R=%d CHG=%d", f, l, r, c);
        ble_log(b);
        btnlvl_last[0] = f; btnlvl_last[1] = l; btnlvl_last[2] = r; btnlvl_last[3] = c;
    }
    if (btnlvl_left == 0) { btnlvl_mon = 0; ble_log("BTNLVL: timeout OFF"); }
}

// ===================== Free/unused GPIO change monitor (BLE 0x48) =====================
// 诊断:监控所有未被任何外设占用的 GPIO 引脚电平,按某个按钮(F/L/R)时看
// 是否有空闲脚跟着变化 —— 用于发现浮空/共用走线耦合(会影响 GPIO 唤醒)。
// 每个空闲脚配弱上拉(1M)稳定读取:真正浮空的脚读 1;若按下按钮把某脚耦合
// 拉低,会看到 1->0 变化。
static const uint32_t free_gpio_pins[] = {
    // PA 剩余 (PA0/PA1 归 EPD BUSY/RESET)
    GPIO_PA2, GPIO_PA3, GPIO_PA4, GPIO_PA5, GPIO_PA6, GPIO_PA7,
    // PB 剩余 (PB4 按钮, PB6 flash MISO, PB7 EPD/flash MOSI)
    GPIO_PB0, GPIO_PB1, GPIO_PB2, GPIO_PB3,
    // PC 剩余 (PC0/PC1/PC4 按键/充电)
    GPIO_PC2, GPIO_PC3, GPIO_PC5, GPIO_PC6, GPIO_PC7,
    // PD 剩余 (PD2/3/4/7 归 flash/EPD SPI)
    GPIO_PD0, GPIO_PD1, GPIO_PD5, GPIO_PD6,
};
#define FREE_GPIO_COUNT (sizeof(free_gpio_pins)/sizeof(free_gpio_pins[0]))
static const char *const free_gpio_names[] = {
    "PA2","PA3","PA4","PA5","PA6","PA7",
    "PB0","PB1","PB2","PB3",
    "PC2","PC3","PC5","PC6","PC7",
    "PD0","PD1","PD5","PD6",
};

static uint8_t freegpio_mon = 0;
static uint32_t freegpio_next = 0;
static uint8_t freegpio_last[FREE_GPIO_COUNT];

void free_gpio_monitor_start(void)
{
    for (int i = 0; i < (int)FREE_GPIO_COUNT; i++) {
        gpio_set_func(free_gpio_pins[i], AS_GPIO);
        gpio_set_output_en(free_gpio_pins[i], 0);
        gpio_set_input_en(free_gpio_pins[i], 1);
        // 弱上拉:让真正浮空/未接的脚读 1,耦合拉低会看到变化
        gpio_setup_up_down_resistor(free_gpio_pins[i], PM_PIN_PULLUP_1M);
        freegpio_last[i] = gpio_read(free_gpio_pins[i]) ? 1 : 0;
    }
    freegpio_mon = 1;
    freegpio_next = clock_time() + 50ULL * CLOCK_16M_SYS_TIMER_CLK_1MS;
    // 只报变化 pin:不打印冗长基线,按下按钮时若有任何空闲脚电平变化才输出
    // (日志须纯 ASCII —— 网页只显示 ASCII 行,中文会被过滤看不到)
    ble_log("FREEGPIO: ON (press buttons; only changed pins are reported)");
}

void free_gpio_monitor_stop(void) { if (freegpio_mon) { freegpio_mon = 0; ble_log("FREEGPIO: OFF"); } }

void free_gpio_monitor_tick(void)
{
    if (!freegpio_mon) return;
    if ((int32_t)(clock_time() - freegpio_next) < 0) return;
    freegpio_next = clock_time() + 50ULL * CLOCK_16M_SYS_TIMER_CLK_1MS;

    uint8_t cur[FREE_GPIO_COUNT];
    int changed = 0;
    for (int i = 0; i < (int)FREE_GPIO_COUNT; i++) {
        cur[i] = gpio_read(free_gpio_pins[i]) ? 1 : 0;
        if (cur[i] != freegpio_last[i]) changed = 1;
    }
    if (!changed) return;

    char b[200];
    int pos = 0;
    append(b, &pos, sizeof(b), "FREEGPIO: ");
    int any = 0;
    for (int i = 0; i < (int)FREE_GPIO_COUNT; i++) {
        if (cur[i] != freegpio_last[i]) {
            if (any) append(b, &pos, sizeof(b), " ");
            append(b, &pos, sizeof(b), free_gpio_names[i]);
            b[pos++] = '=';
            b[pos++] = '0' + freegpio_last[i];
            append(b, &pos, sizeof(b), "->");
            b[pos++] = '0' + cur[i];
            any = 1;
        }
        freegpio_last[i] = cur[i];
    }
    b[pos] = 0;
    ble_log(b);
}
