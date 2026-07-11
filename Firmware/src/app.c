#include <stdint.h>
#include <stdio.h>
#include "tl_common.h"
#include "app.h"
#include "main.h"
#include "drivers.h"
#include "stack/ble/ble.h"
#include "vendor/common/blt_common.h"

#include "battery.h"
#include "ble.h"
#include "flash.h"
#include "ota.h"
#include "epd.h"
#include "etime.h"
#include "bart_tif.h"
#include "button_scan.h"
#include "ebook.h"
#include "ebook_buttons.h"
#include "buttons.h"
#include "charge_state.h"
#include "battery_scan.h"

RAM uint8_t battery_level;
RAM uint16_t battery_mv;
RAM int16_t temperature;
RAM uint32_t last_activity_tick;  // reset on user action; used for sleep delay

// Settings
extern settings_struct settings;

_attribute_ram_code_ void user_init_normal(void)
{                            // this will get executed one time after power up
    random_generator_init(); // must
    init_time();
    init_ble();
    init_flash();
    ebook_init();
    ebook_buttons_init();
    charge_status_init();   // PC1 high-Z input for charge-status reading

    /* BLE advertising policy: a power cycle ALWAYS starts with advertising on
     * (init_ble does bls_ll_setAdvEnable(1)), so the device is reachable from
     * the web page even when the saved setting is "off".  If the saved setting
     * is off, turn advertising off now.  This lets the user re-enable BLE from
     * the web page after any reboot without bricking the device. */
    if (!settings.ble_enabled) {
        ble_set_advertising(0);
    }

    /* Seed the activity timer so the device does NOT immediately go to
     * sleep right after power-on.  last_activity_tick is in clock_time()
     * ticks; without this initialisation it would be 0 and the first
     * main_loop pass would consider the device "idle for >60s". */
    last_activity_tick = clock_time();

    // Force clock refresh on first main_loop to clear any stale EPD image.
    // We do NOT call set_EPD_wait_flush() here -- that triggers a full GC
    // (0xF7) which can leave a half-updated frame visible.  Instead rely on
    // the normal main_loop path which will do a partial refresh on the
    // first clock tick.  If the stale image still shows, the user can press
    // any button to force a refresh.
}

_attribute_ram_code_ void user_init_deepRetn(void)
{ // after sleep this will get executed
    blc_ll_initBasicMCU();
    rf_set_power_level_index(RF_POWER_P3p01dBm);
    blc_ll_recoverDeepRetention();
}

_attribute_ram_code_ void main_loop(void)
{
    blt_sdk_main_loop();
    ble_link_maintenance_tick();
    handler_time();
    uint8_t flag = 0;
    if (time_reached_period(Timer_CH_1, 30))
    {
        // Battery is on PB5 (dedicated, not shared with SPI).  adc_base_init
        // briefly takes PB5 as a high-Z analog input for ~1ms; safe to do
        // any time.  Reject obviously-bad reads (<100mV) and keep last good.
        uint16_t mv = get_battery_mv();
        if (mv > 100)
            battery_mv = mv;
        battery_level = ui_get_battery_pct();
        temperature = get_temperature_c();
        set_adv_data(EPD_read_temp() * 10, battery_level, battery_mv);
        ble_send_battery(battery_level);
        ble_send_temp(EPD_read_temp() * 10);
        flag = 1;
    }
    if (!flag && time_reached_period(Timer_CH_3, 17)) {
        set_air_tag_adv_data();
    }

    if (eb_mode == EB_MODE_CLOCK) {
        ebook_button_tick();
        // Suppress the per-minute clock refresh while a book/font upload is in
        // progress: the external SPI flash shares CLK/MOSI with the EPD, and a
        // refresh mid-upload sets epd_update_state=1 which makes every
        // ext_flash_is_safe() check fail -> upload chunks silently dropped.
        if (!ebook_ble_is_uploading())
            epd_update(get_time(), battery_mv, temperature);
    } else if (eb_mode == EB_MODE_LOCK) {
        ebook_button_tick();  // only scan buttons, no screen update
    } else {
        ebook_button_tick();
    }

    // Check if there's a pending render (e.g., mode change during EPD busy)
    ebook_check_pending_render();

    // Every 3 seconds, send debug via OTA notify (confirmed working channel).
    // Suppressed during active book/font upload to avoid interfering with
    // ebook protocol responses on the same OTA_CMD_OUT_DP_H handle.
    if (ble_get_connected() && time_reached_period(Timer_CH_2, 3) && !ebook_ble_is_uploading())
    {
        uint8_t dbg[] = {'T', 'I', 'C', 'K'};
        bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, dbg, 4);
    }

    // Every 1 second, button pin scan (only emits if state changed or every 10s heartbeat)
    if (time_reached_period(Timer_CH_4, 1))
    {
        button_scan_tick();
        digital_scan_tick();
        charge_state_tick();
        adc_scan_tick();
        vdd_scan_tick();
        battery_check_tick();
    }

    // LED indicator removed (LED pins shared with Flash CS / EPD CS).

    /* ===================== Power management =====================
     * This is an e-reader, not a clock: the device must stay awake and
     * responsive whenever the user might interact with it.  It may only
     * enter low-power suspend when one of these is true:
     *   1. The EPD is mid-refresh  -> stay awake until the panel settles
     *      (handled first so it always wins).
     *   2. The user manually locked the screen (EB_MODE_LOCK) -> allow
     *      suspend; any of the 3 buttons wakes it.
     *   3. The user has been idle for > timeout -> auto-lock (screen saver)
     *      and allow suspend on the next pass (eb_mode == LOCK case above).
     * In every other situation we force SUSPEND_DISABLE so the device
     * keeps running and the screen / time / buttons stay live. */
    if (epd_state_handler())  // EPD refresh in progress -> keep awake
    {
        cpu_set_gpio_wakeup(EPD_BUSY, 1, 1);
        bls_pm_setWakeupSource(PM_WAKEUP_PAD);
        bls_pm_setSuspendMask(SUSPEND_DISABLE);
    }
    else
    {
        uint32_t idle_ticks = (uint32_t)(clock_time() - last_activity_tick);
        uint8_t  allow_sleep;
        /* Idle timeout comes from the persistent settings (see flash.h /
         * app_config.h g_sleep_timeout_s). Default index 2 = 60 s. */
        uint16_t timeout_s = g_sleep_timeout_s[settings.sleep_timeout_idx];

        if (eb_mode == EB_MODE_LOCK) {
            allow_sleep = 1;                  // (already) locked -> sleep now
        }
        else if (ble_get_connected()) {
            /* A BLE central is connected.  While connected we NEVER auto-lock:
             * the lock-screen handler triggers a full EPD refresh, which seizes
             * the SPI bus shared with the external flash and would corrupt any
             * in-flight upload (book/font/lock-image) -- the root cause of the
             * "EPD busy (0xFE)" upload failures.  The user can still lock
             * manually with a long-press of FRONT (ebook_handle_lock is called
             * directly from the button handler, not from here).  Stay awake so
             * the link stays responsive. */
            allow_sleep = 0;
        }
        else if (idle_ticks < (uint32_t)timeout_s * 1000 * CLOCK_16M_SYS_TIMER_CLK_1MS) {
            allow_sleep = 0;                  // active within timeout -> awake
        }
        else {
            /* Idle timeout expired (no BLE connection).  Enter the LOCK
             * (screen-saver) screen so the user wakes into a sane state instead
             * of being stuck inside e.g. the Settings menu.  ebook_handle_lock()
             * is a no-op when already locked, and converts the current mode
             * (clock / reading / settings / about / select) to LOCK while
             * remembering the previous one for resume after wake.  The actual
             * SUSPEND happens on the NEXT main-loop pass when eb_mode == LOCK
             * (case above). */
            ebook_handle_lock();
            allow_sleep = 1;
        }

        if (allow_sleep)
        {
            /* Configure the 3 active-low buttons as wake-up sources so a
             * press brings the device out of suspend. */
            cpu_set_gpio_wakeup(BTN_FRONT_PIN, Level_Low, 1);
            cpu_set_gpio_wakeup(BTN_LEFT_PIN,  Level_Low, 1);
            cpu_set_gpio_wakeup(BTN_RIGHT_PIN, Level_Low, 1);
            bls_pm_setWakeupSource(PM_WAKEUP_PAD);
            /* Let the BLE stack pick its normal suspend mask. */
            blt_pm_proc();
        }
        else
        {
            /* Fully awake: keep the BLE radio + CPU running, ignore sleep. */
            bls_pm_setSuspendMask(SUSPEND_DISABLE);
        }
    }
}
