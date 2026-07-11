#pragma once
#include <stdint.h>
#include "drivers.h"

// Active-low: pressed = 0, released = 1
#define BTN_FRONT_PIN  GPIO_PB4
#define BTN_LEFT_PIN   GPIO_PC4
#define BTN_RIGHT_PIN  GPIO_PC0

typedef enum {
    BTN_NONE   = 0,
    BTN_FRONT  = 1,
    BTN_LEFT   = 2,
    BTN_RIGHT  = 3,
} btn_id_t;

static inline uint8_t btn_read_front(void) { return gpio_read(BTN_FRONT_PIN); }
static inline uint8_t btn_read_left(void)  { return gpio_read(BTN_LEFT_PIN); }
static inline uint8_t btn_read_right(void) { return gpio_read(BTN_RIGHT_PIN); }

// --- Charge status pin (PC1) ---
// Confirmed by testing: PC1 high-Z input, LOW = charging, HIGH = not charging.
#define CHARGE_STATUS_PIN  GPIO_PC1

static inline uint8_t is_charging(void) { return gpio_read(CHARGE_STATUS_PIN) ? 0 : 1; }

// Init PC1 as high-Z input for charge-status reading.
static inline void charge_status_init(void)
{
    gpio_set_func(CHARGE_STATUS_PIN, AS_GPIO);
    gpio_set_output_en(CHARGE_STATUS_PIN, 0);
    gpio_set_input_en(CHARGE_STATUS_PIN, 1);
    gpio_setup_up_down_resistor(CHARGE_STATUS_PIN, 0);  // no pull = high-Z
}
