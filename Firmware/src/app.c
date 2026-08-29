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
#include "drivers/8258/watchdog.h"

// 硬件看门狗超时(ms):须大于最长阻塞操作 —— EPD 全刷 WaitBusy 3000ms +
// 开销 ≈3.2s,单次上传块 <0.5s。设 10s 留足裕量,卡死时最多 10s 自动复位。
// 深度保留睡眠时 16M 计时器停止,timer2 计数冻结,不会睡眠中误复位;
// 唤醒后 main_loop 立即喂狗。
#define WD_TIMEOUT_MS 10000

RAM uint8_t battery_level;
RAM uint16_t battery_mv;
RAM int16_t temperature;
RAM uint32_t last_activity_tick;  // reset on user action; used for sleep delay

// 休眠决策诊断:每 10 秒在 BLE 日志里上报"为何未休眠",便于定位
// "阅读界面不进休眠"问题(mode/conn/upload/idle/超时/EPD/按键)。
#define SLP_DIAG_MS 10000
RAM static uint32_t slp_diag_tick = 0;

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

    // 硬件看门狗:10s 超时,固件卡死(渲染/上传/任意路径)自动复位回时钟。
    // 深度保留睡眠期间 16M 计时器冻结,不会误复位;唤醒后 main_loop 喂狗。
    wd_set_interval_ms(WD_TIMEOUT_MS, CLOCK_16M_SYS_TIMER_CLK_1MS);
    wd_start();
}

_attribute_ram_code_ void user_init_deepRetn(void)
{ // after sleep this will get executed
    blc_ll_initBasicMCU();
    rf_set_power_level_index(RF_POWER_P3p01dBm);
    blc_ll_recoverDeepRetention();
}

_attribute_ram_code_ void main_loop(void)
{
    wd_clear();  // 喂看门狗(一次寄存器写);深度保留睡眠期间计数冻结,唤醒后立即续喂
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
        /* This branch forces SUSPEND_DISABLE so the MCU never suspends while
         * the panel is refreshing.  Do NOT touch the BLE stack's wake-up
         * source here: the stack needs its default (timer + pad) so its
         * ADV-driven low-power cycle keeps working after the refresh. */
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
            /* 锁屏轮询唤醒:锁屏时广播已停止(ebook_handle_lock 里关掉),
             * 唤醒不能再靠广播事件 —— 这里显式挂一个 32k 应用唤醒定时器,
             * 每 LOCK_POLL_MS 醒来一次轮询按钮/保持时钟,与 BLE 状态彻底
             * 解耦(即使 BLE 被用户关掉,锁屏也绝不会睡死)。每一轮都重新
             * 武装,下一次 blt_sdk_main_loop 的睡眠就会以它为唤醒源。 */
            bls_pm_setAppWakeupLowPower(
                clock_time() + (uint32_t)LOCK_POLL_MS * CLOCK_16M_SYS_TIMER_CLK_1MS, 1);

            /* Locked: sleep only when the EPD is idle and no button is held.
             * While a button is held we keep SUSPEND_DISABLE so the 10 ms
             * button scan can process the press (long-press F -> unlock).
             * The sleep itself is handed to the BLE stack (timer-driven
             * deep-retention): the MCU sleeps between poll wakes and keeps
             * advertising OFF while locked.  No GPIO pad wake-ups are armed --
             * the analog pulls die in deep retention and the floating pins
             * would fire spurious wakes (the old self-wake loop). */
            uint8_t held = !gpio_read(BTN_FRONT_PIN) ||
                           !gpio_read(BTN_LEFT_PIN)  ||
                           !gpio_read(BTN_RIGHT_PIN);
            allow_sleep = !held && !epd_update_state;
        }
        else {
            /* 非锁屏:不需要轮询唤醒定时器,关掉它。 */
            bls_pm_setAppWakeupLowPower(0, 0);

            if (ble_get_connected() && (ebook_ble_is_uploading() || ble_get_ota_started())) {
            /* BLE 连接中且正在上传(书籍/字库)或 OTA 升级:绝不在此时自动锁屏
             * —— 锁屏处理会触发一次全刷,抢占与外部 Flash 共享的 SPI 总线,
             * 破坏进行中的上传;OTA 走 ota_started 标志(非 ebook 上传标志),
             * 也要一并拦截,否则 MTU-23 下几分钟的上传途中设备会中途锁屏。
             * 空闲连接(未上传)则照常超时自动锁屏,保证阅读界面也能休眠。 */
            allow_sleep = 0;
        }
        else if (idle_ticks < (uint32_t)timeout_s * 1000 * CLOCK_16M_SYS_TIMER_CLK_1MS) {
            allow_sleep = 0;                  // active within timeout -> awake
        }
        else {
            /* Idle timeout expired.  Enter the LOCK (screen-saver) screen so
             * the user wakes into a sane state instead of being stuck inside
             * e.g. the Settings menu.  ebook_handle_lock() is a no-op when
             * already locked, and converts the current mode (clock / reading /
             * settings / about / select) to LOCK while remembering the
             * previous one for resume after wake.  The actual SUSPEND happens
             * on the NEXT main-loop pass when eb_mode == LOCK (case above),
             * once the lock-render refresh has settled. */
            ebook_handle_lock();
            /* Never sleep on THIS pass: the lock render starts a full EPD
             * refresh, and suspending mid-refresh leaves the panel stuck
             * (BUSY low -> epd_update_state=1 forever -> every later redraw
             * skipped).  Wait for the refresh to finish first. */
            allow_sleep = !epd_update_state;
            }
        }

        if (allow_sleep)
        {
            /* Re-arm the BLE stack's low-power mask (ADV/connection
             * retention).  blt_sdk_main_loop then sleeps between advertising
             * events and wakes to transmit, so the locked device keeps
             * advertising (discoverable) at low power.  Never enter sleep
             * directly here: a raw cpu_sleep_wakeup bypasses the stack's
             * power-management bookkeeping and permanently wedges the ADV
             * scheduler (device becomes undiscoverable). */
            blt_pm_proc();
        }
        else
        {
            /* Fully awake: keep the BLE radio + CPU running, ignore sleep. */
            bls_pm_setSuspendMask(SUSPEND_DISABLE);
        }

        /* 诊断:每 10 秒上报一次休眠决策因子(未休眠时)。 */
        if (!allow_sleep &&
            (int32_t)(clock_time() - slp_diag_tick) >=
                (int32_t)(SLP_DIAG_MS * CLOCK_16M_SYS_TIMER_CLK_1MS)) {
            slp_diag_tick = clock_time();
            uint8_t held = !gpio_read(BTN_FRONT_PIN) ||
                           !gpio_read(BTN_LEFT_PIN)  ||
                           !gpio_read(BTN_RIGHT_PIN);
            char sb[72];
            sprintf(sb, "SLP: mode=%d conn=%d up=%d idle=%us to=%us epd=%d held=%d",
                    (int)eb_mode, ble_get_connected() ? 1 : 0,
                    ebook_ble_is_uploading() ? 1 : 0,
                    (unsigned)(idle_ticks / (CLOCK_16M_SYS_TIMER_CLK_1MS * 1000)),
                    (unsigned)timeout_s, (int)epd_update_state, held ? 1 : 0);
            ble_log(sb);
        }
    }
}
