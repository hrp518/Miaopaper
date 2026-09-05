#include <stdint.h>
#include "tl_common.h"
#include "main.h"
#include "drivers.h"
#include "drivers/8258/pm.h"
#include "stack/ble/ble.h"
#include "stack/ble/ll/ll_pm.h"

#include "ble.h"        // ble_get_connected()
#include "ebook.h"      // eb_mode
#include "etime.h"      // current_unix_time
#include "ext_flash.h"
#include "buttons.h"    // BTN_*_PIN(取证读数字电平)
#include "sleep_log.h"

// etime.c 定义但未在 etime.h 声明:墙上时钟(unix 秒),retention RAM,
// 跨 deep retention 保留;冷启动重置为 2026-07-11(日志里 00:00 后的时间即"未校时")。
extern RAM uint32_t current_unix_time;

// ---- 记录字节布局(手动打包,16 字节,避免 tc32 结构体对齐问题) ----
// b0 magic=0xC7 | b1 type | b2 a | b3 b | b4 c
// b5 eb_mode | b6 ble_connected | b7 crc = xor(b0..b6, b8..b15)
// b8..b11 wall = current_unix_time (LE) | b12..b15 tick = clock_time() (LE)

#define SLP_MAGIC 0xC7

RAM uint8_t slp_last_sleep_mode = 0xFF;

RAM static uint8_t  wake_capture_pending = 0;   // 1=retention唤醒 2=suspend唤醒
RAM static uint32_t slp_last_log_sec = 0;       // SLP/WAKE 5s 节流(墙上秒,回绕安全)
RAM static uint32_t retick_last_sec = 0;        // R 心跳 60s 节流(独立!与 S 共用会让
                                                // S 每 5s 顶掉基准,R 永不写 —— 踩过)
RAM static uint16_t append_idx = 0;             // 下一条写入的记录序号(0..255)
RAM static uint8_t  append_idx_valid = 0;       // 冷启动后扫描过 Flash?

static uint8_t scan_buf[256];   // 扫描/整理用,非保留区(冷启动会重建)

#define SLP_MIN_INTERVAL_MS 5000

static uint8_t rec_crc(const uint8_t *r)
{
    uint8_t c = 0;
    int i;
    for (i = 0; i < 8; i++) {   // b0..b7(crc 位先按 0 累计)
        if (i != 7) c ^= r[i];
    }
    for (i = 8; i < 16; i++) c ^= r[i];
    return c;
}

// 追加一条记录。所有公共入口最终走到这里。
static void slp_rec(uint8_t type, uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t r[SLP_LOG_REC_SIZE];

    if (!ext_flash_is_safe())       // EPD 刷新中,总线共享,跳过(诊断日志允许丢)
        return;
    ext_flash_init();               // 幂等:确保 Flash 引脚已配置(冷启动早期写也安全)

    r[0] = SLP_MAGIC;
    r[1] = type;
    r[2] = a;
    r[3] = b;
    r[4] = c;
    r[5] = (uint8_t)eb_mode;
    r[6] = ble_get_connected() ? 1 : 0;

    uint32_t wall = current_unix_time;
    uint32_t tick = clock_time();
    r[8]  = (uint8_t)(wall);
    r[9]  = (uint8_t)(wall >> 8);
    r[10] = (uint8_t)(wall >> 16);
    r[11] = (uint8_t)(wall >> 24);
    r[12] = (uint8_t)(tick);
    r[13] = (uint8_t)(tick >> 8);
    r[14] = (uint8_t)(tick >> 16);
    r[15] = (uint8_t)(tick >> 24);
    r[7] = rec_crc(r);

    if (!append_idx_valid) {
        // 冷启动后第一次写:按 256B 块扫描扇区,找到第一条空记录(magic=0xFF)。
        uint16_t off;
        append_idx = SLP_LOG_MAX_RECS;
        for (off = 0; off < 4096; off += 256) {
            ext_flash_read(SLP_LOG_ADDR + off, 256, scan_buf);
            int i;
            for (i = 0; i < 256; i += SLP_LOG_REC_SIZE) {
                if (scan_buf[i] == 0xFF) {
                    append_idx = (off + i) / SLP_LOG_REC_SIZE;
                    goto scanned;
                }
                if (scan_buf[i] != SLP_MAGIC)
                    goto scanned;   // 数据异常,别覆写,直接按满处理
            }
        }
scanned:
        append_idx_valid = 1;
    }

    if (append_idx >= SLP_LOG_MAX_RECS) {
        // 扇区满:保留最近 KEEP 条,擦除后写回,从 KEEP 继续。
        uint16_t start = (SLP_LOG_MAX_RECS - SLP_LOG_KEEP_ON_FULL) * SLP_LOG_REC_SIZE;
        ext_flash_read(SLP_LOG_ADDR + start, SLP_LOG_KEEP_ON_FULL * SLP_LOG_REC_SIZE, scan_buf);
        ext_flash_sector_erase(SLP_LOG_ADDR);
        ext_flash_page_program(SLP_LOG_ADDR, SLP_LOG_KEEP_ON_FULL * SLP_LOG_REC_SIZE, scan_buf);
        append_idx = SLP_LOG_KEEP_ON_FULL;
    }

    // 写入 + 读回校验(一次重试)。SPI 比特位拍期间偶发坏位曾产生坏记录。
    for (int attempt = 0; attempt < 2; attempt++) {
        ext_flash_page_program(SLP_LOG_ADDR + append_idx * SLP_LOG_REC_SIZE,
                               SLP_LOG_REC_SIZE, r);
        uint8_t chk[SLP_LOG_REC_SIZE];
        ext_flash_read(SLP_LOG_ADDR + append_idx * SLP_LOG_REC_SIZE,
                       SLP_LOG_REC_SIZE, chk);
        if (chk[0] == SLP_MAGIC && chk[7] == rec_crc(chk))
            break;
    }
    append_idx++;
}

// ---- 公共入口 ----

void slp_log_boot(void)
{
    append_idx_valid = 0;           // 冷启动:RAM 缓存不可信,强制重扫
    slp_last_sleep_mode = 0xFF;
    /* 复位源取证:
     * b = cpu_wakeup_init 存的 0x44 原始字节([3:0] 唤醒源,bit6=wd_status
     *      镜像,bit7=dcdc_rdy —— 此前 &0x0F 掩码把 bit6 看门狗证据抹掉了)。
     * c = 数字域复位标志(数据手册 5.1.6:看门狗复位自动置 0x72[0],写 1 清除):
     *      bit0 = 0x72[0] wd 复位标志;bit1 = TMR_STA 的 WD 标志;bit2 = 0x44[6]。
     * 一次启动即可区分:看门狗复位(c≠0)/ pad 真唤醒(b=0x08)/ 驱动守卫复位
     * (入睡瞬间 0x44[3]=1,见 S a=80 的 c)/ 普通上电(全 0)。 */
    uint8_t raw44 = (uint8_t)pm_get_wakeup_src();
    uint8_t rst = REG_ADDR8(0x72) & 0x01;                    // wd 复位标志
    REG_ADDR8(0x72) = 0x01;                                  // W1C,防粘滞污染后续判读
    if (reg_tmr_sta & FLD_TMR_STA_WD) rst |= 0x02;
    if (raw44 & BIT(6)) rst |= 0x04;
    /* bit3 = 0x35 魔数判别:direct_deep_sleep 睡前写 0x5A;0x35 深睡保留、
     * 看门狗/软件复位清除 —— 开机读到 0x5A 即"真睡了被唤醒",清零即"没睡"。 */
    if (analog_read(0x35) == 0x5A) rst |= 0x08;
    analog_write(0x35, 0x00);   // 消费掉,防污染下一次判读
    slp_rec(SLP_T_BOOT, pm_is_deepPadWakeup() ? 1 : 0, raw44, rst);
}

void slp_log_lock(uint8_t prev_mode)          { slp_rec(SLP_T_LOCK, prev_mode, 0, 0); }
void slp_log_unlock(void)                     { slp_rec(SLP_T_UNLK, 0, 0, 0); }
void slp_log_disc(uint8_t reason)             { slp_rec(SLP_T_DISC, reason, 0, 0); }

// 超级省电(0x80 全深睡)入睡前记一条:a=0x80 标识 super,b=实际布防的唤醒源,
// c=入睡瞬间 0x44 原始值(bit3=1 即 pad 电平仍激活 → 驱动守卫会拒绝入睡并复位,
// 这就是"每 10s 原地重启循环"的特征位)。此前的日志时间戳因时钟恢复到入睡时刻,
// 无法区分"睡了 0 秒被守卫复位"和"睡了 10 秒被看门狗复位" —— c 和 B 记录的
// 复位源取证合起来才能定位。
void slp_log_super_enter(uint8_t armed_src)
{
    slp_rec(SLP_T_SLP, 0x80, armed_src, analog_read(0x44));
}

// cpu_sleep_wakeup 意外返回(驱动拒睡):记 a=0x80 b=src c=0xFF 以便与入睡记录区分。
void slp_log_super_reject(uint8_t armed_src)
{
    slp_rec(SLP_T_SLP, 0x80, armed_src, 0xFF);
}

// ---- 未睡原因记录(锁屏+断连却未入睡时的门状态,30s 节流) ----
// 背景:00:25:57 锁屏 → 00:28:06 解锁,129 秒内无任何入睡尝试(无 S 记录),
// 而未睡诊断行只在蓝牙连接时输出 —— 锁屏断连后是日志盲区。
// 记录: a=门位图 b=epd_update_state c=0x26 实测值
//   a.bit0=held(按键按住) bit1=epd_busy bit2=lock_hold逗留
//   a.bit3=ble_hold(BLE写保持) —— 谁是拦截者一眼可见
static uint32_t waitgate_last_sec = 0;
void slp_log_wait_gate(uint8_t flags, uint8_t epd_busy, uint8_t v26)
{
    uint32_t now = current_unix_time;
    if (now - waitgate_last_sec < 30)
        return;
    waitgate_last_sec = now;
    slp_rec(SLP_T_WAITGATE, flags, epd_busy, v26);
}

// ---- pad 唤醒运行时取证(docs/ENGINEERING_NOTES.md "运行时取证") ----
// 背景:0x80 入睡前 0x44[3] 恒为 1 → 驱动守卫拒睡并复位(super sleep 从未
// 睡成)。静态审计(全反汇编)确认唤醒寄存器写入者仅 cpu_wakeup_init/
// cpu_set_gpio_wakeup/cpu_sleep_wakeup 三处,极性/上拉/输入使能静态上均
// 正确 —— 剩余疑点只能读回实机值分辨。
// PADCFG#1: a=0x26 b=0x21(PA极性) c=0x22(PB极性)
// PADCFG#2: a=0x23(PC极性) b=0x24(PD极性) c=0x27(PA使能)
// PADCFG#3: a=0x28(PB使能) b=0x29(PC使能) c=0x2a(PD使能)
// PADPROBE: a=位图 b=三键数字电平 c=探测后0x44
//   a.bit0=原配置 W1C→3ms→bit3重锁;bit1=全撤使能仍重锁;bit2=仅PB4;
//   a.bit3=仅PC0;bit4=仅PC4。b: bit0=F(PB4) bit1=L(PC4) bit2=R(PC0),1=松开。
void slp_log_pad_forensics(void)
{
    slp_rec(SLP_T_PADCFG, analog_read(0x26), analog_read(0x21), analog_read(0x22));
    slp_rec(SLP_T_PADCFG, analog_read(0x23), analog_read(0x24), analog_read(0x27));
    slp_rec(SLP_T_PADCFG, analog_read(0x28), analog_read(0x29), analog_read(0x2a));

    uint8_t save26 = analog_read(0x26);
    uint8_t en28 = analog_read(0x28), en29 = analog_read(0x29);
    /* 确认 IO 唤醒检测使能(0x26[4])打开,否则 bit3 无从锁存,探测无意义 */
    analog_write(0x26, save26 | 0x10);

    uint8_t probe = 0;
    analog_write(0x44, 0x0F); WaitMs(3);
    if (analog_read(0x44) & BIT(3)) probe |= 0x01;          // 原配置
    analog_write(0x28, en28 & ~0x10);                       // 撤 PB4
    analog_write(0x29, en29 & ~0x11);                       // 撤 PC0+PC4
    analog_write(0x44, 0x0F); WaitMs(3);
    if (analog_read(0x44) & BIT(3)) probe |= 0x02;          // 全撤
    analog_write(0x28, en28);                               // 仅 PB4
    analog_write(0x44, 0x0F); WaitMs(3);
    if (analog_read(0x44) & BIT(3)) probe |= 0x04;
    analog_write(0x28, en28 & ~0x10);                       // 仅 PC0
    analog_write(0x29, (en29 | 0x01) & ~0x10);
    analog_write(0x44, 0x0F); WaitMs(3);
    if (analog_read(0x44) & BIT(3)) probe |= 0x08;
    analog_write(0x29, (en29 | 0x10) & ~0x01);              // 仅 PC4
    analog_write(0x44, 0x0F); WaitMs(3);
    if (analog_read(0x44) & BIT(3)) probe |= 0x10;

    /* 恢复原值,不改变后续入睡路径的任何配置 */
    analog_write(0x28, en28);
    analog_write(0x29, en29);
    analog_write(0x26, save26);

    uint8_t lvl = 0;
    if (gpio_read(BTN_FRONT_PIN)) lvl |= 0x01;
    if (gpio_read(BTN_LEFT_PIN))  lvl |= 0x02;
    if (gpio_read(BTN_RIGHT_PIN)) lvl |= 0x04;
    slp_rec(SLP_T_PADPROBE, probe, lvl, analog_read(0x44));
}

void slp_log_sleep(uint8_t armed_wake_src, uint8_t suspend_mask)
{
    // 节流用墙上秒(current_unix_time,retention RAM,跨睡眠推进)。
    // 教训:不要用 clock_time() 做 >秒级 节流 —— 16MHz 32位 tick 每 268 秒回绕,
    // int32 比较会把 [134s,268s] 的真实间隔误判成"不足 5s"而跳过(实测踩坑:
    // S 记录大面积缺失、首条迟到 89 秒)。
    uint32_t now_sec = current_unix_time;
    if (now_sec - slp_last_log_sec < 5)
        return;
    slp_last_log_sec = now_sec;
    slp_rec(SLP_T_SLP, slp_last_sleep_mode, armed_wake_src, suspend_mask);
}

_attribute_ram_code_ void slp_log_suspend_enter(uint8_t e, uint8_t *p, int n)
{
    (void)e; (void)n;
    // 每次栈入睡(含 conn 事件间)都触发,必须最短 —— 只记模式字节。
    // Telink SDK 约定:p 指向即将进入的睡眠模式(0x00=suspend,
    // 0x07=DEEPSLEEP_MODE_RET_SRAM_LOW32K)。
    if (p && n >= 1)
        slp_last_sleep_mode = p[0];
}

_attribute_ram_code_ void slp_log_suspend_exit(void)
{
    // SUSPEND_EXIT 事件(每次从 suspend 醒来都会调,含 conn 事件间):
    // 标记一次 suspend 唤醒。retention 唤醒不走这里(它重跑 main,走
    // user_init_deepRetn -> arm_wake_capture,两者在记录类型上可区分)。
    wake_capture_pending = 2;
}

_attribute_ram_code_ void slp_log_arm_wake_capture(void)
{
    wake_capture_pending = 1;
}

_attribute_ram_code_ void slp_log_arm_retick(void)
{
    wake_capture_pending = 3;
}

void slp_log_wake_capture(void)
{
    uint8_t pend = wake_capture_pending;
    if (!pend)
        return;
    wake_capture_pending = 0;
    if (pend == 1) {
        // pad 唤醒 = 真按键(user_init_deepRetn 只在 pad 时 arm)。
        slp_rec(SLP_T_WAKE, 1, (uint8_t)pm_get_wakeup_src() & 0x0F,
                slp_last_sleep_mode);
    } else if (pend == 3) {
        // retention 心跳:非 pad 唤醒。60s 节流,防刷 Flash。
        // a=pad标志 b=wakeup_src —— 若真实按键唤醒被误分到 else(pad 标志
        // 没置),b 里能看到 pad 位,据此诊断。
        uint32_t now_sec = current_unix_time;
        if (now_sec - retick_last_sec < 60)
            return;
        retick_last_sec = now_sec;
        slp_rec(SLP_T_RETICK, pm_is_deepPadWakeup() ? 1 : 0,
                (uint8_t)pm_get_wakeup_src() & 0x0F, 0);
    } else {
        // suspend 唤醒:只在未连接时记录(连接态每 7.5ms 一次,禁止写)。
        if (!ble_get_connected()) {
            uint32_t now_sec = current_unix_time;
            if (now_sec - slp_last_log_sec < 5)
                return;
            slp_last_log_sec = now_sec;
            slp_rec(SLP_T_WAKE, 2, 0, slp_last_sleep_mode);
        }
    }
}

// ---- 读出 ----

void slp_log_dump(void)
{
    char line[64];
    static const char tc[] = {0, 'B', 'L', 'S', 'W', 'D', 'U', 0, 'R'};
    uint8_t buf[SLP_LOG_REC_SIZE];
    uint16_t shown = 0;

    if (!ext_flash_is_safe()) { ble_log("SLPLOG: EPD busy"); return; }
    if (!append_idx_valid) {
        // 尚未扫描(刚冷启动还没写过):先扫出有效条数
        uint16_t off;
        for (off = 0; off < 4096; off += 256) {
            ext_flash_read(SLP_LOG_ADDR + off, 256, scan_buf);
            int i;
            for (i = 0; i < 256; i += SLP_LOG_REC_SIZE) {
                if (scan_buf[i] != SLP_MAGIC) { off = 4096; break; }
                append_idx++;
            }
        }
        append_idx_valid = 1;       // 注:此时 append_idx=总数,写入位置需重扫?
    }

    if (append_idx == 0) { ble_log("SLPLOG: empty"); return; }

    sprintf(line, "SLPLOG: %u recs, newest first", append_idx);
    ble_log(line);

    // 最新在前,最多 SLP_LOG_KEEP_ON_FULL*? 条 —— 上限 80 条,节奏 20ms 防notify溢出
    {
        int16_t i = (int16_t)append_idx - 1;
        for (; i >= 0 && shown < 80; i--, shown++) {
            ext_flash_read(SLP_LOG_ADDR + (uint16_t)i * SLP_LOG_REC_SIZE,
                           SLP_LOG_REC_SIZE, buf);
            if (buf[0] != SLP_MAGIC || buf[7] != rec_crc(buf)) {
                // 坏记录:报索引并跳过(不停止,后面的记录照样可读)
                char b[40];
                sprintf(b, "SLPLOG: bad rec #%d (%02X %02X %02X)",
                        (int)i, buf[0], buf[1], buf[2]);
                ble_log(b);
                WaitMs(20);
                continue;
            }
            uint32_t wall = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) |
                            ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
            // wall 是 unix 秒,转 HH:MM:SS(时:分:秒,日期略,保持行短)
            uint32_t sec_of_day = wall % 86400;
            char t = (buf[1] <= 8) ? tc[buf[1]] : '?';
            sprintf(line, "%c a=%02X b=%02X c=%02X m=%u bl=%u %02u:%02u:%02u",
                    t, buf[2], buf[3], buf[4], buf[5], buf[6],
                    (unsigned)(sec_of_day / 3600), (unsigned)((sec_of_day / 60) % 60),
                    (unsigned)(sec_of_day % 60));
            ble_log(line);
            WaitMs(20);
        }
    }
    ble_log("SLPLOG: end");
}

void slp_log_clear(void)
{
    if (!ext_flash_is_safe()) { ble_log("SLPLOG: EPD busy"); return; }
    ext_flash_sector_erase(SLP_LOG_ADDR);
    append_idx = 0;
    append_idx_valid = 1;
    ble_log("SLPLOG: cleared");
}
