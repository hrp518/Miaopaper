/*
 * ebook_buttons.c - Button handling via LwBTN library.
 *
 * Hardware (confirmed): buttons active-LOW.
 *   gpio_read() = nonzero when RELEASED, = 0 when PRESSED.
 *   get_state returns (gpio_read ? 0 : 1): pressed -> 1 (active).
 *
 * Event mapping (read mode):
 *   F  single click : next page      F  double click : previous page
 *   L  single click : previous page
 *   R  single click : next page
 *   Long press R    : enter/exit select menu or reading
 *   Long press F    : lock / unlock (wake)
 */
#include <stdint.h>
#include "tl_common.h"
#include "drivers.h"
#include "app_config.h"
#include "stack/ble/ble.h"
#include "vendor/common/blt_common.h"
#include "buttons.h"
#include "ebook_buttons.h"
#include "ebook.h"
#include "ble.h"
#include "lwbtn.h"

#define LONG_PRESS_MS          1000
#define LONG_PRESS_KEEPALIVES  (LONG_PRESS_MS / LWBTN_CFG_TIME_KEEPALIVE_PERIOD)

typedef enum {
    BTN_ID_FRONT = 0,
    BTN_ID_LEFT,
    BTN_ID_RIGHT,
} btn_id_e;

typedef struct {
    btn_id_e id;
    uint32_t pin;
} btn_arg_t;

static btn_arg_t btn_args[3] = {
    {BTN_ID_FRONT, BTN_FRONT_PIN},
    {BTN_ID_LEFT,  BTN_LEFT_PIN},
    {BTN_ID_RIGHT, BTN_RIGHT_PIN},
};

extern RAM uint32_t last_activity_tick;  // in app.c for sleep delay

/* All button state lives in retention RAM so it survives deep-sleep.
 * This means we do NOT need to re-init buttons after wake-up. */
RAM lwbtn_btn_t lwbtns[3];
RAM lwbtn_t     my_group;
RAM uint8_t     long_fired[3];
RAM uint32_t    btn_last_10ms;
RAM uint16_t    btn_reinit_count;

/* ===================== Mode action handlers ===================== */

static void on_click_front(void)
{
    if (eb_mode == EB_MODE_LOCK) ebook_handle_unlock();
    else if (eb_mode == EB_MODE_SELECT) ebook_select_confirm();
    else if (eb_mode == EB_MODE_SETTINGS) ebook_settings_change();
    else if (eb_mode == EB_MODE_ABOUT) ebook_exit_about();
    else if (eb_mode == EB_MODE_READ) ebook_next_page();
}

static void on_dblclick_front(void)
{
    if (eb_mode == EB_MODE_READ) ebook_prev_page();
}

/* Per the UI spec, ONLY the F button supports double-click to reverse the
 * page direction.  L and R are single-click-only.
 * On the CLOCK screen a short press is the entry point to the menus:
 *   L -> settings, R -> book list.  (Long-press was unreliable on L.)
 * On the ABOUT screen L/R flip pages; F returns to the settings menu. */
static void on_click_left(void)
{
    if (eb_mode == EB_MODE_SELECT) ebook_select_up();
    else if (eb_mode == EB_MODE_SETTINGS) ebook_settings_up();
    else if (eb_mode == EB_MODE_ABOUT) ebook_about_prev();   // page prev
    else if (eb_mode == EB_MODE_READ) ebook_prev_page();
    else if (eb_mode == EB_MODE_CLOCK) ebook_enter_settings();
}

static void on_click_right(void)
{
    if (eb_mode == EB_MODE_SELECT) ebook_select_down();
    else if (eb_mode == EB_MODE_SETTINGS) ebook_settings_down();
    else if (eb_mode == EB_MODE_ABOUT) ebook_about_next();    // page next
    else if (eb_mode == EB_MODE_READ) ebook_next_page();
    else if (eb_mode == EB_MODE_CLOCK) ebook_enter_select();
}

/* ===================== LwBTN callbacks ===================== */

static uint8_t btn_get_state(lwbtn_t *lwobj, lwbtn_btn_t *btn)
{
    (void)lwobj;
    btn_arg_t *a = (btn_arg_t *)btn->arg;
    return gpio_read(a->pin) ? 0 : 1;
}

static void btn_evt(lwbtn_t *lwobj, lwbtn_btn_t *btn, lwbtn_evt_t evt)
{
    (void)lwobj;
    btn_arg_t *a = (btn_arg_t *)btn->arg;
    uint8_t id = (uint8_t)a->id;
    static const char letter[3] = {'F', 'L', 'R'};

    switch (evt) {
    case LWBTN_EVT_ONPRESS:
        long_fired[id] = 0;
        last_activity_tick = clock_time();  // delay sleep
        break;

    case LWBTN_EVT_KEEPALIVE:
        if (!long_fired[id] && lwbtn_keepalive_get_count(btn) >= LONG_PRESS_KEEPALIVES) {
            long_fired[id] = 1;
            if (id == BTN_ID_RIGHT) {
                ble_log("BTN:RL");
                ebook_handle_long_right();
            } else if (id == BTN_ID_FRONT) {
                ble_log("BTN:FL");
                if (eb_mode == EB_MODE_LOCK) {
                    ebook_handle_unlock();
                } else {
                    ebook_handle_lock();
                }
            } else if (id == BTN_ID_LEFT) {
                /* Long-press LEFT toggles the settings menu: enter from the
                 * clock screen, or leave it (back to clock). */
                ble_log("BTN:LL");
                if (eb_mode == EB_MODE_CLOCK) {
                    ebook_enter_settings();
                } else if (eb_mode == EB_MODE_SETTINGS) {
                    ebook_exit_settings();
                }
            }
        }
        break;

    case LWBTN_EVT_ONCLICK: {
        /* If this press already fired a long-press action, ignore the
         * release-time CLICK entirely - otherwise long-press F (lock) is
         * immediately followed by unlock, or long-press R (menu) by a stray
         * page-turn, because the click handler runs on release. */
        if (long_fired[id]) {
            break;
        }
        uint8_t clicks = lwbtn_click_get_count(btn);
        char b[8];
        b[0]='B';b[1]='T';b[2]='N';b[3]=':';b[4]=letter[id];b[5]='0'+clicks;b[6]=0;
        ble_log(b);
        if (clicks >= 2) {
            /* Only the F button honours a double-click: reverse direction
             * (previous page) in read mode. */
            if (id == BTN_ID_FRONT) on_dblclick_front();
        } else {
            if (id == BTN_ID_FRONT) on_click_front();
            else if (id == BTN_ID_LEFT) on_click_left();
            else on_click_right();
        }
        break;
    }

    default:
        break;
    }
}

/* ===================== Public API ===================== */

/* Re-configure the 3 button GPIOs (func=input, 1M pullup) and release PC0
 * from I2C.  This must be called from main_loop context (awake), NEVER
 * from user_init_deepRetn() - the i2c/analog register writes there break
 * the BLE link recovery.
 *
 * Safe to call repeatedly; cheap enough to run every second. */
static void btn_reconfig_gpio(void)
{
    reg_i2c_mode &= ~FLD_I2C_MASTER_EN;
    reg_clk_en0 &= ~FLD_CLK0_I2C_EN;

    uint32_t pins[] = {BTN_FRONT_PIN, BTN_LEFT_PIN, BTN_RIGHT_PIN};
    for (int i = 0; i < 3; i++) {
        gpio_set_func(pins[i], AS_GPIO);
        gpio_set_output_en(pins[i], 0);
        gpio_set_input_en(pins[i], 1);
        gpio_setup_up_down_resistor(pins[i], PM_PIN_PULLUP_1M);
    }
}

void ebook_buttons_init(void)
{
    btn_reconfig_gpio();

    for (int i = 0; i < 3; i++) {
        lwbtns[i].arg = &btn_args[i];
        long_fired[i] = 0;
    }
    btn_last_10ms = 0;
    btn_reinit_count = 0;

    /* Manually populate the group fields.  lwbtn_init_ex() is not used
     * because its NULL-pointer check misfires on this compiler (tc32 gcc
     * 4.5.1) and returns 0 after zeroing our group. */
    memset(&my_group, 0, sizeof(my_group));
    my_group.btns = lwbtns;
    my_group.btns_cnt = 3;
    my_group.evt_fn = btn_evt;
    my_group.get_state_fn = btn_get_state;

    /* Register the 3 buttons as low-level GPIO wake-up sources so that
     * while a button is HELD down the CPU wakes immediately on every
     * connection-interval sleep and runs main_loop fast, letting lwbtn
     * detect the press/keep-alive quickly.  Polarity = Level_Low because
     * buttons are active-low. */
    cpu_set_gpio_wakeup(BTN_FRONT_PIN, Level_Low, 1);
    cpu_set_gpio_wakeup(BTN_LEFT_PIN,  Level_Low, 1);
    cpu_set_gpio_wakeup(BTN_RIGHT_PIN, Level_Low, 1);
    bls_pm_setWakeupSource(PM_WAKEUP_PAD);
}

void ebook_button_tick(void)
{
    uint32_t now = clock_time();
    if ((uint32_t)(now - btn_last_10ms) < 10 * CLOCK_SYS_CLOCK_1MS)
        return;
    btn_last_10ms = now;

    /* Deep retention sleep does NOT retain the analog GPIO config (1M pullup)
     * on TLSR825x, so the buttons misread after the first sleep cycle.
     * Re-apply it every ~1 second in main_loop context (awake = safe).
     * This is the same operation ED01 does and is proven to fix detection. */
    if (++btn_reinit_count >= 100) {
        btn_reinit_count = 0;
        btn_reconfig_gpio();
    }

    uint32_t mstime = now / CLOCK_SYS_CLOCK_1MS;
    lwbtn_process_ex(&my_group, mstime);
}
