#pragma once
#include <stdint.h>

// ===================== 超级省电模式 (settings.super_sleep) =====================
// 锁屏 + 未连接时进入全深睡 DEEPSLEEP_MODE(0x80, ~0.4µA):时钟不走、RAM 全丢、
// 唤醒 = 冷启动(soft_reboot 路径,BLE 必然可用 —— 无栈状态残留问题)。
//
// 状态持久化:入睡前往 ext flash 0x7FC000(空闲扇区,字库之上、睡眠日志之下)
// 追加一条 stash 记录 {magic, 墙上秒, 标志, crc};冷启动读最新有效记录,
// 恢复 current_unix_time 到"入睡时刻"(睡眠时长本身丢失,时钟会慢,连网页可校准),
// 并回到锁屏界面。NOR 不能覆写 → 追加写满后压缩(保留最新几条)。
//
// 唤醒后给 10s 操作窗(复用 lock_hold_until 机制):窗内按键可解锁,超时自动
// 重新深睡。噪声误唤醒代价 ≈ 10s × 5mA ≈ 50µA·s,可忽略。

#define SS_ADDR       0x7FC000
#define SS_REC_SIZE   12
#define SS_MAX_RECS   (4096 / SS_REC_SIZE)   // 341
#define SS_KEEP_ON_FULL 8
#define SS_MAGIC      0x5B
#define SS_FLAG_WAS_SUPER 0x01

// 入睡(全深睡)前调用:写 stash 记录(内部会先 save_settings_to_flash 由调用方
// 负责;这里只写 stash)。flags 通常 = SS_FLAG_WAS_SUPER。
void ss_stash(uint8_t flags);

// 冷启动(user_init_normal,init_time 之后)调用:读最新有效 stash,若存在则
// 恢复 current_unix_time 并返回其 flags;无有效记录返回 0(正常上电)。
uint8_t ss_boot_restore(void);

// 本次上电是否为"超级省电深睡唤醒"(ss_boot_restore 恢复过):用于回锁屏 +
// 10s 重睡窗。
uint8_t ss_is_super_wake(void);
// 本次上电为超级省电的深睡唤醒(冷启动):app 据此跳过 B 记录和设置写盘,
// 锁屏分支静默回锁屏不渲染。
void ss_set_maintenance(uint8_t en);
uint8_t ss_is_maintenance(void);
