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
#define SLP_T_BOOT  1   // 冷启动(main()->user_init_normal)。a=pad唤醒标志 b=wakeup_src c=复位源取证
#define SLP_T_LOCK  2   // 加锁。a=eb_prev_mode
#define SLP_T_SLP   3   // 锁屏且未连接、即将 blt_pm_proc 深睡。a=上次SUSPEND_ENTER模式 b=唤醒源 c=掩码
#define SLP_T_WAKE  4   // 唤醒。a=1 retention唤醒(pad标志) / a=2 suspend唤醒; c=睡前的模式字节
#define SLP_T_DISC  5   // BLE 断连。a=terminate reason
#define SLP_T_UNLK  6   // 解锁
#define SLP_T_RETICK 8  // retention 心跳:非 pad 的 retention 唤醒(广播 tick 等),
                        // 60s 节流记一条 —— 出现即证明栈在 retention 里循环睡眠。
#define SLP_T_PADCFG 9   // 运行时取证:pad 唤醒寄存器原始值(见 slp_log_pad_forensics)
#define SLP_T_PADPROBE 10 // 运行时取证:逐脚 bisect 定位 0x44[3] 锁存源
#define SLP_T_WAITGATE 11  // 未睡原因:锁屏+断连却未入睡的门状态(30s 节流)
#define SLP_T_ELAPSED 12   // 真实断电时长:开机时 32k tick 差值/32768 = 秒
#define SLP_T_PADLATCH 13  // 单脚稳态诊断:布防单脚→W1C→等500ms→读0x44(bit3 锁存? )

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
// 超级省电(0x80)入睡前调用一次(无节流):a=0x80, b=布防的唤醒源,
// c=入睡瞬间 0x44 原始值(bit3=1 → 驱动守卫将拒睡并复位)。
void slp_log_super_enter(uint8_t armed_src);
// cpu_sleep_wakeup(0x80) 意外返回(拒睡)时记一条:c=0xFF 标识。
void slp_log_super_reject(uint8_t armed_src);
// pad 唤醒运行时取证(超级省电入睡前调用):寄存器原始值转储 + 逐脚 bisect。
// 记录布局见 docs/ENGINEERING_NOTES.md "运行时取证"。
void slp_log_pad_forensics(void);
// 未睡原因记录(内部 30s 节流):flags 门位图见 sleep_log.c 注释
void slp_log_wait_gate(uint8_t flags, uint8_t epd_busy, uint8_t v26);
// 单脚稳态 pad 锁存诊断(进 LOCK 后跑一次):见 sleep_log.c
void slp_log_pad_latch_probe(void);
// 真实断电时长记录:开机调用(读 0x3a~0x3c 入睡时存的 32k tick,对当前 32k
// tick 求差)。32k 在深睡期间持续走(POR 不清 0x3a~0x3c),与复位类型无关。
void slp_log_elapsed(void);
// 直接返回真实断电秒数(32k 差值;不记日志)。
uint32_t slp_elapsed_sec(void);
// 入睡前把当前 32k tick 低 24 位存入 0x3a~0x3c(slp_log_elapsed 读回)。
void slp_stamp_sleep_32k(void);
void slp_log_boot(void);            // user_init_normal 末尾调;含复位源取证(b/c)
void slp_log_lock(uint8_t prev_mode);
void slp_log_unlock(void);
void slp_log_disc(uint8_t reason);

void slp_log_dump(void);            // BLE 0xF6:notify 最近记录(最新在前)
void slp_log_clear(void);           // BLE 0xF7:擦除日志扇区

// 最近一次栈实际进入的睡眠模式(0xFF=本次开机栈从未睡过)。retention RAM,
// 跨 deep retention 保留;冷启动重置为 0xFF(本身就是证据:真掉过电)。
extern _attribute_data_retention_ uint8_t slp_last_sleep_mode;
