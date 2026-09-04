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
#include "ext_flash.h"
#include "ota.h"
#include "epd.h"
#include "etime.h"
#include "bart_tif.h"
#include "ebook.h"
#include "ebook_buttons.h"
#include "buttons.h"
#include "charge_state.h"
#include "battery_scan.h"
#include "sleep_log.h"
#include "super_sleep.h"

// ebook.c/etime.c 内部变量(未在头文件声明)
extern RAM uint8_t eb_prev_mode;
extern RAM uint32_t current_unix_time;
#include "drivers/8258/watchdog.h"
#include "drivers/8258/pm.h"

// 硬件看门狗超时(ms):须大于最长阻塞操作 —— EPD 全刷 WaitBusy 3000ms +
// 开销 ≈3.2s,单次上传块 <0.5s。设 10s 留足裕量,卡死时最多 10s 自动复位。
// 深度保留睡眠时 16M 计时器停止,timer2 计数冻结,不会睡眠中误复位;
// 唤醒后 main_loop 立即喂狗。
#define WD_TIMEOUT_MS 10000

RAM uint8_t battery_level;
RAM uint16_t battery_mv;
RAM int16_t temperature;
RAM uint32_t last_activity_tick;  // reset on user action; used for sleep delay
// 深睡 pad 唤醒观察:lock_wake_pending 由 user_init_deepRetn 置位;唤醒后设备
// 逗留 LOCK_HOLD_MS(期间允许双击解锁 + GC 全刷显示 "Double Click to Unlock"),
// 超时未解锁(单击/毛刺)则重新深睡。用于观察是否毛刺触发唤醒。
RAM static uint8_t lock_wake_pending = 0;
RAM static uint32_t lock_hint_tick = 0;
RAM static uint32_t lock_hold_until = 0;
#define LOCK_HOLD_MS 3000
#define DCLK_CONFIRM_MS 450   // 稍大于双击窗口(LWBTN 350ms)

// BLE 数据活动保持:任何发往 OTA 特性(0x331f)的写(书籍/字库/OTA/命令)都会
// 重置该计时器,睡眠决策据此保持唤醒 —— 修复 OTA/上传中途被"空闲超时自动锁屏"
// 打断(设备在刷写途中进入深睡)的问题。锁屏分支不读 idle 计时,所以单独用
// last_ble_write_tick + 保持窗口来强制不睡。
#define BLE_WRITE_HOLD_MS 4000
RAM uint32_t last_ble_write_tick = 0;

// BLE 数据活动回调(由 otaWritePre / 其它写处理函数调用):重置空闲计时,让正在
// 上传/OTA 的设备永不被"空闲超时"误判为无人使用而自动锁屏+深睡。
_attribute_ram_code_ void app_mark_ble_activity(void)
{
    last_activity_tick = clock_time();
    last_ble_write_tick = clock_time();
}

// 锁屏下单点击 F:先挂一个"确认非双击"计时(稍大于双击窗口),期间若随后双击
// 则解锁并取消;超时未双击(确认为单点击)且仍锁屏,才 GC 全刷显示
// "Double Click to Unlock"。同时保持唤醒,避免提示还没显示设备就再睡。
void app_lock_observe(void)
{
    lock_hint_tick = clock_time() + (uint32_t)DCLK_CONFIRM_MS * CLOCK_16M_SYS_TIMER_CLK_1MS;
    lock_hold_until = clock_time() + (uint32_t)LOCK_HOLD_MS * CLOCK_16M_SYS_TIMER_CLK_1MS;
}

// 休眠决策诊断:每 10 秒在 BLE 日志里上报"为何未休眠",便于定位
// "阅读界面不进休眠"问题(mode/conn/upload/idle/超时/EPD/按键)。
#define SLP_DIAG_MS 10000
RAM static uint32_t slp_diag_tick = 0;

// Settings
extern settings_struct settings;

_attribute_ram_code_ void user_init_normal(void)
{                            // this will get executed one time after power up
    random_generator_init(); // must
    ext_flash_boot_resync(); // 全深睡唤醒后 Flash 可能仍在 0xB9:同步状态
    init_time();
    /* 超级省电唤醒恢复:读 ext flash stash,把墙上时钟恢复到"入睡时刻"。
     * 必须在 init_time 之后(init_time 会重置时钟)、其余初始化之前。 */
    uint8_t ss_flags = ss_boot_restore();
    init_ble();
    init_flash();
    ebook_init();
    ebook_buttons_init();
    charge_status_init();   // PC1 high-Z input for charge-status reading

    /* 超级省电深睡唤醒(冷启动),按唤醒来源分流:
     * - pad(真按键):渲染锁屏 + 10s 操作窗,单击/双击解锁照常;
     * - timer(180s 维护唤醒):时钟已推进、stash 已刷新,直接回锁屏不渲染
     *   (电子纸画面本来就在),几个主循环后自动再睡 —— 屏幕完全无感。 */
    if (ss_flags & SS_FLAG_WAS_SUPER) {
        if (pm_is_deepPadWakeup()) {
            ebook_handle_lock();
            lock_hold_until = clock_time() +
                (uint32_t)10000 * CLOCK_16M_SYS_TIMER_CLK_1MS;
        } else {
            eb_prev_mode = EB_MODE_CLOCK;
            eb_mode = EB_MODE_LOCK;      // 不渲染:维护唤醒,屏幕保持原样
            ss_set_maintenance(1);
        }
    }

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

    // 冷启动记录(含 pad 唤醒标志/唤醒源):冷启动本身 = full deep sleep、
    // 断电或看门狗复位 —— retention 唤醒不会走到这里。
    if (!ss_is_maintenance())
        slp_log_boot();   // 维护性唤醒(180s)不记 B,防日志刷屏
}

_attribute_ram_code_ void user_init_deepRetn(void)
{ // after sleep this will get executed
    blc_ll_initBasicMCU();
    rf_set_power_level_index(RF_POWER_P3p01dBm);
    /* retention 唤醒统一走库的睡醒对账(清 bltPm 标志 + 补写 LL 寄存器)。
     * 锁屏深睡由"方案B':广播保留+间隔拉长"实现(ebook_handle_lock),栈自己
     * 进 retention,无需任何栈外重建。 */
    blc_ll_recoverDeepRetention();
    /* 关键:广播 retention tick(锁屏 B' 下每 2s 一次)也走这里,但只有
     * pad 唤醒才是用户按键。若无条件置 lock_wake_pending,每个广播 tick 都会
     * 触发"双击解锁观察"(逗留 3s + 450ms 后 GC 全刷提示),设备永远醒着反复
     * 刷 "Double Click to Unlock" —— B' 实测踩坑。 */
    if (pm_is_deepPadWakeup()) {
        lock_wake_pending = 1;
        // 休眠诊断:只对真按键(pad)唤醒做记录标记(不做 IO)。
        slp_log_arm_wake_capture();
    } else {
        // 非 pad 的 retention 唤醒(广播 tick 等):记 60s 节流心跳,证明
        // 栈确实在 retention 里循环(每 2s 广播 tick 都会重跑这里)。
        slp_log_arm_retick();
    }
}

_attribute_ram_code_ void main_loop(void)
{
    wd_clear();  // 喂看门狗(一次寄存器写);深度保留睡眠期间计数冻结,唤醒后立即续喂
    blt_sdk_main_loop();
    ble_link_maintenance_tick();
    // 休眠诊断:deep retention 唤醒后的首个 main_loop,补写 WAKE 记录(内部有
    // 待处理标志 + pad/连接 状态门控,空闲时只是一次 RAM 读)。
    // 休眠诊断:唤醒记录(R/W)必须在 handler_time 之后 —— 它内部用
    // current_unix_time 节流,而唤醒首轮该值还是睡前旧值(未补算),提前调用
    // 会让节流误判"不足 60s"吞掉记录(实测 R 只出第一条)。
    slp_log_wake_capture();
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
        if (!ebook_ble_is_uploading() && !ble_get_ota_started())
            epd_update(get_time(), battery_mv, temperature);
    } else if (eb_mode == EB_MODE_LOCK) {
        ebook_button_tick();  // only scan buttons, no screen update
        /* 深睡 pad 唤醒观察:唤醒后若未在逗留期内双击解锁(单击/毛刺),GC 全刷
         * 显示 "Double Click to Unlock",便于观察是否毛刺触发唤醒。锁屏电源管理
         * 分支负责设置 lock_hint_tick,这里到点只负责渲染。 */
        if (lock_hint_tick && (int32_t)(clock_time() - lock_hint_tick) >= 0 &&
            eb_mode == EB_MODE_LOCK) {
            lock_hint_tick = 0;
            ebook_render_lock_hint();
        }
    } else {
        ebook_button_tick();
        // 离开锁屏:取消待显示的唤醒提示,避免下次锁屏误触发
        lock_hint_tick = 0;
        lock_wake_pending = 0;
        lock_hold_until = 0;
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
        charge_state_tick();
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
        /* BLE 数据活动保持:最近收到 BLE 写(OTA/书籍/字库/命令)则强制不睡 ——
         * 即使锁屏分支(不读 idle 计时)也会被这条覆盖,确保刷写途中不深睡。 */
        uint8_t ble_hold = (int32_t)(clock_time() - last_ble_write_tick) <
                           (int32_t)(BLE_WRITE_HOLD_MS * CLOCK_16M_SYS_TIMER_CLK_1MS);
        /* Idle timeout comes from the persistent settings (see flash.h /
         * app_config.h g_sleep_timeout_s). Default index 2 = 60 s. */
        uint16_t timeout_s = g_sleep_timeout_s[settings.sleep_timeout_idx];

        if (eb_mode == EB_MODE_LOCK) {
            /* 真深睡 GPIO 唤醒:锁屏后 MCU 深度保留睡眠,按 F/L/R(active-low)
             * 拉低引脚 -> cpu_set_gpio_wakeup(Level_Low) 触发 pad 唤醒,不做 1s
             * 轮询。上拉(1M)由 btn_reconfig_gpio 设置并保留(模拟寄存器在深睡
             * 不清除,见 SDK gpio_init(anaRes_init_en=0))。唤醒后 lwbtn 处理
             * 按键(单/长按 F -> ebook_handle_unlock)。 */
            uint8_t held = !gpio_read(BTN_FRONT_PIN) ||
                           !gpio_read(BTN_LEFT_PIN)  ||
                           !gpio_read(BTN_RIGHT_PIN);
            // 三个按钮都使能低电平 pad 唤醒
            cpu_set_gpio_wakeup(BTN_FRONT_PIN, Level_Low, 1);
            cpu_set_gpio_wakeup(BTN_LEFT_PIN,  Level_Low, 1);
            cpu_set_gpio_wakeup(BTN_RIGHT_PIN, Level_Low, 1);
            // 数据手册 2.7.2:深睡 IO 唤醒的 afe 寄存器(确保 SDK 驱动没漏设,
            // 尤其 afe_0x26[3]=1 的 16us 抗抖滤波,防止按钮脚毛刺引发乱唤醒):
            //   afe_0x26[4]=IO唤醒使能, afe_0x26[3]=16us filter
            //   afe_0x28[4]=PB4(F)使能, afe_0x29[0][4]=PC0(R)/PC4(L)使能
            //   afe_0x22[4]=PB4极性(0=低电平), afe_0x23[0][4]=PC0/PC4极性
            {
                unsigned char wk = analog_read(0x26);
                wk |= 0x10;   // [4] IO(pad) 唤醒使能
                wk |= 0x08;   // [3] 16us 抗抖滤波
                analog_write(0x26, wk);
                analog_write(0x28, analog_read(0x28) | 0x10);            // PB4(F)
                analog_write(0x29, analog_read(0x29) | 0x01 | 0x10);     // PC0(R) + PC4(L)
                analog_write(0x22, analog_read(0x22) & ~0x10);           // PB4 极性=低
                analog_write(0x23, analog_read(0x23) & ~(0x01 | 0x10));  // PC0/PC4 极性=低
            }
            /* 唤醒源 = pad + timer(两个位都必须有 —— "有时候咋按都不开"的
             * 根因修复):timer 供栈的 2s 广播 tick 在 retention 中自醒;pad 供
             * 按键秒醒。历史踩坑:旧代码只给 PAD(剥掉 timer → 锁屏即断连);
             * 改方案A时整行删除(只剩库默认 timer → 深睡中按键完全无效)。 */
            bls_pm_setWakeupSource(PM_WAKEUP_PAD | PM_WAKEUP_TIMER);
            /* 方案A(唯一路径):锁屏广播保持开启(见 ebook_handle_lock)→ 栈在
             * 1s 广播间隔间走 retention(掩码 DEEPSLEEP_RETENTION_ADV),~5-10µA
             * 且手机可随时连。按钮 pad 唤醒寄存器(cpu_set_gpio_wakeup + afe
             * 0x22/0x23/0x26/0x28/0x29)照常布好,深睡中按键秒醒。 */
            /* 深睡 pad 唤醒观察:唤醒后设一个"逗留期",期间允许双击解锁,并且
             * 若未解锁(单击/毛刺)则 GC 全刷显示 "Double Click to Unlock"。
             * 关键:毛刺唤醒时通常无按键按住,若不逗留设备会立刻再睡,提示
             * 来不及显示 —— 所以唤醒后先保持唤醒 LOCK_HOLD_MS。 */
            if (lock_wake_pending) {
                lock_wake_pending = 0;
                lock_hold_until = clock_time() + (uint32_t)LOCK_HOLD_MS * CLOCK_16M_SYS_TIMER_CLK_1MS;
                lock_hint_tick = clock_time() + (uint32_t)DCLK_CONFIRM_MS * CLOCK_16M_SYS_TIMER_CLK_1MS;
            }
            /* 关键:即使处于锁屏(mode=4),若正在 OTA / 上传书籍字体,也绝不能
             * 深睡 —— 否则 pad 唤醒后上传中断(真深睡后不再每 1s 醒来处理,OTA
             * 传不完)。另加"唤醒后逗留期"保持唤醒观察。 */
            if (ble_get_connected() && (ebook_ble_is_uploading() || ble_get_ota_started()))
                allow_sleep = 0;
            else if ((int32_t)(clock_time() - lock_hold_until) < 0)
                allow_sleep = 0;   // 唤醒后逗留观察期:先保持唤醒
            else
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

        /* BLE 数据活动保持:最近有 BLE 写(含 OTA/上传)则强制不睡,覆盖上面的
         * mode/idle 判定 —— 刷写途中绝不能被自动锁屏+深睡打断。 */
        if (ble_hold)
            allow_sleep = 0;

        if (allow_sleep)
        {
            /* 超级省电(settings.super_sleep):锁屏+未连接 → 全深睡 0x80
             * (~2.5µA)。时钟不走(醒来=入睡时刻+维护推进,连网页可校准);
             * 唤醒=冷启动,init_ble 全量重建,蓝牙必然可用。
             * 关键:wakeup_tick 必须是真实未来时刻且唤醒源必须含 TIMER 位 ——
             * 否则 32k 闹钟保持 0,入睡瞬间到期 → 无限软重启循环(实测踩坑,
             * 表现为锁屏每 10s 原地 GC)。周期 180s(16M tick 上限 268s)。
             * 被拒返回则落到 blt_pm_proc 走 B' 兜底。 */
            uint8_t super = (eb_mode == EB_MODE_LOCK && !ble_get_connected() &&
                             settings.super_sleep);
            if (super)
            {
                if (!ss_is_maintenance())
                    save_settings_to_flash();  // 固化阅读位置/模式(仅真入睡,防内部Flash磨损)
                else
                    current_unix_time += SS_MAINT_PERIOD_S;  // 维护唤醒:时钟推进
                ss_stash(SS_FLAG_WAS_SUPER);
                slp_last_sleep_mode = 0x80;
                ext_flash_deep_power_down();
                cpu_sleep_wakeup(DEEPSLEEP_MODE,
                                 PM_WAKEUP_PAD | PM_WAKEUP_TIMER,
                                 clock_time() +
                                 (uint32_t)SS_MAINT_PERIOD_S * CLOCK_16M_SYS_TIMER_CLK_1S);
                /* 正常不返回(唤醒=软重启);返回=入睡被拒 → B' 兜底 */
            }
            else if (eb_mode == EB_MODE_LOCK && !ble_get_connected())
            {
                /* B'(无 Super 时):广播保留+2s 间隔 → retention(~5µA) */
                slp_log_sleep(1, bls_pm_getSuspendMask());
                ext_flash_deep_power_down();
            }
            else
            {
                ext_flash_deep_power_down();
            }
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
            char sb[88];
            sprintf(sb, "SLP: mode=%d conn=%d up=%d ota=%d hold=%d idle=%us to=%us epd=%d",
                    (int)eb_mode, ble_get_connected() ? 1 : 0,
                    ebook_ble_is_uploading() ? 1 : 0, ble_get_ota_started() ? 1 : 0,
                    ble_hold ? 1 : 0,
                    (unsigned)(idle_ticks / (CLOCK_16M_SYS_TIMER_CLK_1MS * 1000)),
                    (unsigned)timeout_s, (int)epd_update_state);
            ble_log(sb);
        }
    }
}
