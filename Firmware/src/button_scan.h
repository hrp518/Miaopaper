#pragma once
#include <stdint.h>

void button_scan_init(void);
void button_scan_tick(void);
void button_scan_set_enabled(uint8_t en);
uint8_t button_scan_is_enabled(void);
// BLE 0x46 按钮/充电电平监视:启动后每个 ~50ms 读一次 F/L/R/CHG 引脚电平,
// 某引脚电平变化时 ble_log 上报一行 "BTNLVL: F=.. L=.. R=.. CHG=.."。
// 用于诊断:按下某个按钮时观察其他按钮/充电引脚电平是否跟着变
// (确认按钮独立、有无浮空或共享)。约 2 分钟或收到 0x47 停止。
void btn_level_monitor_start(void);
void btn_level_monitor_stop(void);
void btn_level_monitor_tick(void);
// BLE 0x48 空闲 GPIO 变化监视(推荐):扫描所有未被外设占用的空闲引脚
// (配弱上拉稳定读取)。按按钮(F/L/R)时若有任何空闲脚电平跟着变,
// 上报 "FREEGPIO: <pin>=old->new ..." —— 用于发现浮空/共用走线耦合。
// 约 2 分钟或收到 0x49 停止。
void free_gpio_monitor_start(void);
void free_gpio_monitor_stop(void);
void free_gpio_monitor_tick(void);
