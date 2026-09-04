#pragma once

// Battery SOC measurement (PB1-driven / PB5-differential quirk path).
// The old BLE-triggered pin sweeps were removed to fit the 128KB image.

extern uint16_t measured_batt_mv;      // 14串电池总电压 (mV)
extern uint8_t  measured_batt_soc;     // 电量百分比 0~100
void battery_check_tick(void);
