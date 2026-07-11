#pragma once

#include <stdint.h>

uint16_t get_battery_mv(void);
uint8_t get_battery_level(uint16_t battery_mv);
uint16_t get_temperature_c(void);

// SOC = 100% - (PB5_voltage / 1V), integer math.  Returns [0, 100].
//   mv = 3000 -> SOC = 97
//   mv = 3500 -> SOC = 97
//   mv = 4200 -> SOC = 96
uint8_t get_battery_soc(uint16_t battery_mv);

// 单节 3.7V 锂电池电压→电量查表 (用户提供的 14串数据除14).
// cell_mv = 单节电池电压(mV), 返回电量百分比 0~100.
uint8_t get_battery_soc_14s(uint32_t cell_mv);

/* UI-facing SOC: same source as the clock face (measured_batt_soc). */
uint8_t ui_get_battery_pct(void);
void ui_format_battery(char *buf, int buf_len);
