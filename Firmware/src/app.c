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

// EPD 刷新完成沿检测 + 完成时刻:超级省电断电前要求完成后静置 2s,
// 避免"GC 刚刷完就断电"的面板异常(见 locked 分支 super 门槛)。
RAM static uint8_t  prev_epd_state = 0;
RAM static uint32_t epd_done_tick = 0;

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
    lock_hold_until = clock_time() + (uint32_t)LOCK_HOLD_MS * CLOCK_16M_SYS_TIMER_CLK_1MS;
    /* 静默唤醒(超级省电):不渲染提示 GC —— 屏上本就是锁屏画面,双击照常解锁。
     * 提示仅普通(非超级省电)锁屏的单击时显示。 */
    if (ss_is_maintenance()) return;
    lock_hint_tick = clock_time() + (uint32_t)DCLK_CONFIRM_MS * CLOCK_16M_SYS_TIMER_CLK_1MS;
}

// 休眠决策诊断:每 10 秒在 BLE 日志里上报"为何未休眠",便于定位
// "阅读界面不进休眠"问题(mode/conn/upload/idle/超时/EPD/按键)。
#define SLP_DIAG_MS 10000
RAM static uint32_t slp_diag_tick = 0;

// 超级省电 pad 唤醒:首个 main_loop 里直接解锁(user_init_normal 里置位,
// 那时 irq 还没开,渲染/写 Flash 放到 main_loop 上下文做)。
RAM uint8_t super_pad_unlock = 0;

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

    /* 超级省电深睡唤醒(冷启动):pad 唤醒(真按键)→ 直接解锁。此前设计是
     * "静默回锁屏 + 10s 观察窗",实测有个致命 UX 问题:唤醒那一击(click1)
     * 落在冷启动期间,lwbtn 看不到它;用户双击的第二击(click2)到达时只是
     * 一次"单击"→ 不解锁;设备又不渲染不广播 → 看起来就是"双击唤不醒"。
     * 每次唤醒=冷启动,所以双击的第一击永远被吞,第二击永远是单击 ——
     * 只有用户隔几秒再补一个完整双击才能解锁(SLPLOG 里 W,W,U 的实测序列)。
     * 现在:pad 唤醒 = 明确的按键意图,首个 main_loop 直接解锁回时钟屏
     * (屏幕刷新即反馈);非 pad 唤醒(毛刺)仍走静默锁屏+重睡,零开销。
     *
     * 注:0x44[3] 必须在下面 W1C 清除之前读 —— pm_is_deepPadWakeup() 不可靠
     * (cpu_wakeup_init 要求 pad/timer 状态位恰为 0x08 才置位,组合态会漏判)。
     * 毛刺误唤醒的代价 = 一次解锁全刷 + 60s 后重新入睡,实测 16h 深睡零毛刺
     * (16us 滤波位 0x26[3] 不能用:会触发驱动入睡守卫的硬复位循环,见
     * super 分支注释),可接受。 */
    if (ss_flags & SS_FLAG_WAS_SUPER) {
        uint8_t pad_wake = (analog_read(0x44) & BIT(3)) ? 1 : 0;
        eb_prev_mode = EB_MODE_CLOCK;
        eb_mode = EB_MODE_LOCK;
        ss_set_maintenance(1);
        lock_hold_until = clock_time() +
            (uint32_t)10000 * CLOCK_16M_SYS_TIMER_CLK_1MS;
        /* 关键:冷启动 init_ble 会无条件重开广播 —— 超级省电锁屏必须立刻
         * 关掉,否则每次唤醒都有 10s 可连接窗口(手机一连接就常驻唤醒)。
         * 解锁路径 ebook_handle_unlock 会按设置恢复广播。 */
        ble_set_advertising(0);
        /* W1C:写 1 清除 pad 唤醒状态锁存(驱动 suspend 入口同样写 0x44=0x0F) */
        analog_write(0x44, BIT(3));
        if (pad_wake)
            super_pad_unlock = 1;   // 延迟到首个 main_loop(irq 已开)再解锁
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
    // 注意:不再因超级省电唤醒跳过 —— 此前 maintenance 跳过让所有 super
    // 唤醒(pad/复位/看门狗)在日志里隐形,SLPLOG 只剩 W/U 凭空出现,
    // 无法区分"按键没唤醒"和"唤醒了但没记录"。现在每次冷启动一条,
    // a=1 即 pad 唤醒,a=0 即复位/断电/看门狗。
    slp_log_boot();

    // 超级省电唤醒的锁屏渲染在 user_init_normal 内同步完成:以此为 2s 静置窗
    // 起点(超级省电断电前要求 GC 完成后静置 2s,见 locked 分支 super 门槛)。
    epd_done_tick = clock_time();
    prev_epd_state = 1;
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

// ===================== 直入 0x80 全深睡(绕过驱动守卫) =====================
// cpu_sleep_wakeup(0x80,...) 的守卫(反汇编 0x1e4:0x44[3]=1 → 13ms 后写
// 0x6f=0x20 复位)被驱动自身在"W1C 之后、检查点之前"的寄存器写入副作用
// 误触发(运行取证:引脚电平干净、五态 bisect 全不锁存,但 S 同秒必 B),
// 布防了 pad 唤醒脚的应用从这条路永远进不去。本函数逐条复刻驱动
// cpu_sleep_wakeup_32k_rc 的 DEEPSLEEP 序列(0x54~0x1de),仅删去守卫,
// 末尾调用 SDK 公开的 sleep_start()(pm.h 声明)。tick=0(无定时唤醒)、
// 内部 32k RC、wakeup_src=PM_WAKEUP_PAD|BIT(3)(0x26=0x18:PAD+16us 滤波,
// 直写路径不再经过守卫,滤波位可以安全启用)。不返回;返回=拒睡。
static void direct_deep_sleep(uint8_t wake_src)
{
    if (func_before_suspend && func_before_suspend() == 0)
        return;                                    // 栈钩子拒绝(驱动 0x66 同款)
    /* 驱动 0x54~0x98 的早起簿记(此前漏复刻,start_suspend 依赖这些状态):
     * pm_enter 标志消费清零 + tick 账本更新。 */
    *(volatile uint8_t *)0x00800643 = 0;           // 0x54~0x5c
    tick_cur += 0x230;                             // 0x84~0x90
    tick_32k_cur = cpu_get_32k_tick();             // 0x92~0x98
    pm_timRecover.recover_flag = 0;                // 驱动 0x9a
    *(volatile uint8_t *)0x00800066 = DEEPSLEEP_MODE;  // 睡眠模式 stash(驱动 0xd8)
    analog_write(0x26, wake_src & 0xFF);           // 0xc4: 唤醒源使能
    analog_write(0x44, 0x0F);                      // 0xd0: W1C 唤醒状态
    analog_write(0x04, 0x48);                      // 0xe2
    analog_write(0x7e, 0);                         // 0xea
    analog_write(0x2b, 0x5e);                      // 0x100
    analog_write(0x2c, 0x9e);                      // 0x106: 无 TIMER 唤醒位
    analog_write(0x07, (analog_read(0x07) & ~7) | 4);  // 0x116
    analog_write(0x7f, 1);                         // 0x32e
    /* 32k RC 校准(驱动 0x13e~0x176 同款公式) */
    unsigned short calib = tick_32k_calib ? tick_32k_calib : 32768;
    unsigned int half = calib / 2;
    analog_write(0x20, (uint8_t)(127 - (unsigned)((0xFA00u + half) / calib)));
    analog_write(0x1f, (uint8_t)(0u - (unsigned)((0x1F400u + half) / calib)));
    /* 32k 唤醒比较:绝对远未来。驱动公式在 tick_cur 较大时会算出"≈现在"
     * 的比较值 → 入睡 ~1s 后 32k 假唤醒(实测复位周期恒 ~1s)。定时唤醒本
     * 就被 0x26[6]=0 禁用,这里只求比较值永不命中:tick_32k_cur + 0x6亿 ≈
     * 13.7 小时 @32k。 */
    unsigned int cmp = tick_32k_cur + 0x60000000u;
    /* 数字域杂散唤醒源:0x6e 复位默认 0x1f(I2C/SPI/USB host 等全使能)。
     * 按键唤醒走模拟域(0x26/0x27~0x2a),与 0x6e 无关 → 全关。 */
    REG_ADDR8(0x6e) = 0x00;
    *(volatile uint8_t *)0x0080074C = 0x2C;        // 0x1a4
    *(volatile unsigned int *)0x00800754 = cmp;    // 0x1aa
    *(volatile uint8_t *)0x0080074F = 8;           // 0x1b4: 触发 32k 域写
    for (volatile unsigned int t = 0; t < 2000000u; t++)   // 0x1d6: 同步等待,
        if (*(volatile uint8_t *)0x0080074F & 8) break;    // 带上限防挂死
    *(volatile uint8_t *)0x0080074C = 0x20;        // 0x1de
    /* 0x1e4 守卫:已删 —— 引脚电平已被运行取证证明干净,守卫属误报 */
    /* 看门狗实测(c=01 复位):wd_stop 拦不住已启动的 WD(telink 惯例:一次
     * 使能不能关)。把捕获推到 14 位上限(~268s @16M tick;若入睡后按 32k
     * 计数则 ~71 分钟)。两种结果都能定位:复位时间跟着变 → CPU 卡在
     * start_suspend(醒着卡死);不再复位且按键可醒 → 真睡成。 */
    wd_set_interval_ms(268000, CLOCK_16M_SYS_TIMER_CLK_1MS);
    wd_clear();
    wd_stop();
    /* 判别器(手册:0x35~0x39 深睡保留,看门狗/软件复位清除):睡前写魔数,
     * 开机 B 记录读回 —— 0x5A=真睡了被唤醒(复位即深睡唤醒);清零=没睡,
     * 是狗/软件复位。c=01 无法区分这两种,这个可以。 */
    analog_write(0x35, 0x5A);
    sleep_start();      // SDK 公开入口;0.80 真睡不返回,start_suspend 卡住则不返回
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
    // 会让节流误判"不足 60s"吞掉记录(实测 R 只出第一条)。顺序此前写反过,
    // GPT 报告指出后核实修正。
    handler_time();
    slp_log_wake_capture();
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

    /* 超级省电 pad 唤醒的延迟解锁(见 user_init_normal):放在电池采样后,
     * 唤醒首页的电池图标不会显示 0;irq 已开,与 B' 唤醒的双击解锁走完全
     * 相同的 ebook_handle_unlock 路径 —— 恢复广播、回时钟屏(全刷=用户
     * 可见反馈)、记 U。 */
    if (super_pad_unlock) {
        super_pad_unlock = 0;
        ebook_handle_unlock();
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
    /* 刷新完成沿(1→0)打时间戳:超级省电断电前要求完成后静置 2s(面板波形
     * 收尾/电荷稳定),避免"GC 刚完就断电"观感与面板异常。 */
    uint8_t epd_busy = epd_state_handler();  // EPD refresh in progress -> keep awake
    if (prev_epd_state && !epd_busy)
        epd_done_tick = clock_time();
    prev_epd_state = (epd_busy != 0);

    if (epd_busy)  // EPD refresh in progress -> keep awake
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
            // 三按钮布防:cpu_set_gpio_wakeup(pin, Level_Low, 1) 一次搞定
            // per-pin 使能(0x27+port)与极性(0x21+port)。
            // !!布防只做一次!! 运行取证(2026-09-05 SLP_T_PADPROBE)实锤:
            // 对 0x28/0x29 使能寄存器的每一次写入都会让 0x44[3] 毛刺置位
            // (probe 五态全 0=引脚电平干净;恢复使能后立即读=0x88)。此前
            // 每个 main_loop 周期重复布防 → bit3 永远是 1 → 0x80 入睡守卫
            // 必拒睡复位(super sleep 从未睡成)。
            // 以硬件实际状态为准(cpu_wakeup_init 在每次启动/retention 唤醒
            // 都会清 0x27~0x2a,清过就重布防),不用 RAM 标志:
            if (!(analog_read(0x28) & 0x10) || !(analog_read(0x29) & 0x11)) {
                cpu_set_gpio_wakeup(BTN_FRONT_PIN, Level_Low, 1);
                cpu_set_gpio_wakeup(BTN_LEFT_PIN,  Level_Low, 1);
                cpu_set_gpio_wakeup(BTN_RIGHT_PIN, Level_Low, 1);
                analog_write(0x44, 0x0F);   // 洗掉布防写入自产的毛刺
            }
            else if (analog_read(0x44) & BIT(3)) {
                analog_write(0x44, 0x0F);   // 运行中若再置位,入睡前洗掉
            }
            /* 唤醒源寄存器(0x26)不再在本分支反复写! 运行取证:? a=50 实测
             * LOCK 分支每轮 setWakeupSource(PAD|TIMER) 把 0x26 写成 0x50:
             * ① 留下 TIMER 唤醒使能与 super 纯 PAD 入睡打架;② 每轮重写
             * 0x26 = 持续制造 0x44[3] 毛刺。栈的唤醒源设置移到 B' 分支
             * (super=true 时永远不会走到那里)。 */
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
            else if (lock_hold_until &&
                     (int32_t)(clock_time() - lock_hold_until) < 0)
                allow_sleep = 0;   // 唤醒后逗留观察期:先保持唤醒
                /* !!必须先判 lock_hold_until!=0!! clock_time() 32位@16MHz 每
                 * 268s 回绕,开机 134~268s 期间 (int32_t)(tick-0) 为负 ——
                 * 旧写法在从未设置过逗留期(=0)的手动锁屏下,负半周里"逗留期"
                 * 凭空生效,睡眠被整体封死(实测 00:25:57 锁屏 129 秒无入睡
                 * 尝试,解锁时刻恰为负半周结束点)。lock_hint_tick 同款防法。 */
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
                /* 唤醒源唯一 = PAD(规范要求)。wakeup_tick = 0:全深睡下
                 * 不使用定时唤醒(数据手册:全深睡仅外部 pad 唤醒)。
                 *
                 * !!不要给 src 塞 afe_0x26[3](16us 滤波位)!! 驱动入睡时执行
                 * analog_write(0x26, src&0xFF)(反汇编核实),曾借道 src|BIT(3)
                 * 想恢复抗抖滤波 —— 实测(2026-09-05 20:37 SLPLOG)0x26[3]=1
                 * 会让 0x44[3] pad 唤醒状态在驱动的入睡检查点被锁存,驱动随即
                 * 走"pad 已在唤醒电平"守卫:soft_reboot_dly13ms(13ms) + 写
                 * 复位寄存器 0x6f=0x20(反汇编 0x442 处)→ 硬复位。表现为
                 * S a=80 → 同秒 B a=00(b=0 无唤醒源)→ 10s 观察窗 → 再试的
                 * 无限重启循环,0x26=0x10(纯 PAD)则实测可稳定睡 16h。
                 * LOCK 分支里对 0x26 的 |=0x08 同样无效(驱动整写覆盖),
                 * 滤波只能放弃;实测 16h 深睡零毛刺唤醒,可接受。
                 * pad 唤醒状态锁存位(analog 0x44 bit3)在冷启动钩子中已清零;
                 * 若入睡瞬间恰有按键按住(0x44[3] 被硬件置回),同一守卫会
                 * 复位而不睡 —— 重启路径回到这里,松手后自然入睡,自愈。 */
                /* pad 唤醒运行时取证(一次定位 0x44[3] 锁存源,布局见
                 * docs/ENGINEERING_NOTES.md):寄存器转储 + 逐脚 bisect。
                 * 探测自身恢复原值,不影响本次入睡。 */
                slp_log_pad_forensics();
                slp_log_super_enter(PM_WAKEUP_PAD);
                ss_stash(SS_FLAG_WAS_SUPER);
                slp_last_sleep_mode = 0x80;
                ext_flash_deep_power_down();
                /* 直入 0x80(见 direct_deep_sleep):cpu_sleep_wakeup 的守卫被
                 * 驱动自身写入副作用误触发(S 同秒必 B,取证已证引脚干净),
                 * 改为逐条复刻驱动序列并删守卫,调 SDK 公开的 sleep_start。
                 * 拒睡返回时重开狗 + 记拒绝 → B' 兜底。 */
                direct_deep_sleep(PM_WAKEUP_PAD | BIT(3));
                wd_start();
                slp_log_super_reject(PM_WAKEUP_PAD);
            }
            else if (eb_mode == EB_MODE_LOCK && !ble_get_connected())
            {
                /* B'(无 Super 时):广播保留+2s 间隔 → retention(~5µA)。
                 * 栈的唤醒源在此设置(原 LOCK 分支每轮写,现移到只服务 B' 的
                 * 这里,super 分支不再被 0x26=0x50 反复改写)。 */
                bls_pm_setWakeupSource(PM_WAKEUP_PAD | PM_WAKEUP_TIMER);
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
            /* 未睡原因取证(锁屏+断连却没睡):门状态记入 SLPLOG,30s 节流。
             * 此前该状态只在连接时打印,锁屏断连后是盲区(实测 129 秒无入睡
             * 尝试且无任何记录)。 */
            if (eb_mode == EB_MODE_LOCK && !ble_get_connected()) {
                uint8_t f = 0;
                if (!gpio_read(BTN_FRONT_PIN) || !gpio_read(BTN_LEFT_PIN) ||
                    !gpio_read(BTN_RIGHT_PIN)) f |= 0x01;      // held
                if (epd_update_state) f |= 0x02;               // epd busy
                if ((int32_t)(clock_time() - lock_hold_until) < 0) f |= 0x04;
                if (ble_hold) f |= 0x08;
                slp_log_wait_gate(f, epd_update_state, analog_read(0x26));
            }
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
