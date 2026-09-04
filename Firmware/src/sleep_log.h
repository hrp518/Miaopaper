#pragma once
#include <stdint.h>

// ===================== 休眠诊断日志(sleep log) =====================
// 目的:回答"锁屏后设备到底进没进 deep retention 深度保留睡眠"。
// 原理:把每次加锁/准备入睡/唤醒/断连事件按 16 字节一条追加写到外置 Flash
// 扇区 0x7FD000(GBK 字库上限 0x700000 之上、阅读进度扇区 0x7FE000 之下,
// 4KB 专用,不与任何现有数据冲突)。读回时:
//   - 出现 WAKE 记录 且 pm 报告 pad 唤醒  -> 之前睡的是 deep retention;
//   - 出现 BOOT 记录(冷启动)             -> 之前是 full deep sleep / 断电 / 看门狗;
//   - 只有 SLP 记录却从无 WAKE/BOOT       -> 根本没睡(blt_pm_proc 拒睡)或普通
//     suspend(suspend 醒来不重启 main,SUSPEND_ENTER 回调里的模式字节=0x00 可证)。
// 读出:BLE 终端发 0xF6(读最近 80 条),0xF7 清空。也可用 0xF4 直接裸读 0x7FD000。

#define SLP_LOG_ADDR          0x7FD000
#define SLP_LOG_REC_SIZE      16
#define SLP_LOG_MAX_RECS      (4096 / SLP_LOG_REC_SIZE)   // 256
#define SLP_LOG_KEEP_ON_FULL  32    // 扇区写满时整理后保留的最近条数

// record type 字节
#define SLP_T_BOOT  1   // 冷启动(main()->user_init_normal)。a=pad唤醒标志 b=wakeup_src c=suspend掩码
#define SLP_T_LOCK  2   // 加锁。a=eb_prev_mode
#define SLP_T_SLP   3   // 锁屏且未连接、即将 blt_pm_proc 深睡。a=上次SUSPEND_ENTER模式 b=唤醒源 c=掩码
#define SLP_T_WAKE  4   // 唤醒。a=1 retention唤醒(pad标志) / a=2 suspend唤醒; c=睡前的模式字节
#define SLP_T_DISC  5   // BLE 断连。a=terminate reason
#define SLP_T_UNLK  6   // 解锁
#define SLP_T_RETICK 8  // retention 心跳:非 pad 的 retention 唤醒(广播 tick 等),
                        // 60s 节流记一条 —— 出现即证明栈在 retention 里循环睡眠。

// 在 init_ble 里注册到 BLT_EV_FLAG_SUSPEND_ENTER。SDK 约定 p[0] 是栈即将进入的
// 睡眠模式(0x00=suspend, 0x07=deep retention 32k)。此回调每次 conn/adv 事件间
// 都会触发,必须最短:只写一个 RAM 变量,绝不做 Flash IO / ble_log。
// 注意:实测本 SDK 该事件可能从不触发(slp_last_sleep_mode 恒 0xFF),判读别依赖 a。
void slp_log_suspend_enter(uint8_t e, uint8_t *p, int n);
// 注册到 BLT_EV_FLAG_SUSPEND_EXIT(ble.c 的 user_set_rf_power 里调):
// 标记一次 suspend 唤醒(类型 W a=2)。只置 RAM 标志,不做 IO。
void slp_log_suspend_exit(void);

// user_init_deepRetn 调:pad 唤醒(真按键)→ 记 W a=1。
void slp_log_arm_wake_capture(void);
// user_init_deepRetn 调:非 pad 唤醒(广播 tick)→ 记 R 心跳(60s 节流)。
void slp_log_arm_retick(void);
// main_loop 里 blt_sdk_main_loop() 之后调:处理待记录标志(内部有节流/门控)。
void slp_log_wake_capture(void);

// 锁屏、未连接、即将调 blt_pm_proc 深睡前调用(内部 5s 节流)。
void slp_log_sleep(uint8_t armed_wake_src, uint8_t suspend_mask);
void slp_log_boot(void);            // user_init_normal 末尾调
void slp_log_lock(uint8_t prev_mode);
void slp_log_unlock(void);
void slp_log_disc(uint8_t reason);

void slp_log_dump(void);            // BLE 0xF6:notify 最近记录(最新在前)
void slp_log_clear(void);           // BLE 0xF7:擦除日志扇区

// 最近一次栈实际进入的睡眠模式(0xFF=本次开机栈从未睡过)。retention RAM,
// 跨 deep retention 保留;冷启动重置为 0xFF(本身就是证据:真掉过电)。
extern _attribute_data_retention_ uint8_t slp_last_sleep_mode;
