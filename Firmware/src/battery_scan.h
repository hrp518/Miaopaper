#pragma once
#include <stdint.h>

// Battery-level pin auto-detection helper.
//
// The TLSR8258 ADC is only available on PB0..PB7, PC4, PC5 (see
// components/drivers/8258/adc.c ADC_GPIO_tab).  The current battery read
// path in battery.c samples PB7, which is shared with the EPD/external
// flash MOSI line, so the reading is only valid when the SPI bus is idle.
//
// This module adds a BLE-driven sweep (cmd 0xB2) that cycles through every
// ADC-capable GPIO that is *not* already used by the running firmware, plus
// the on-chip VBAT direct channel as a reference, and reports each reading
// back over the RxTx/OTA notify path so the host can pick the pin that
// actually carries the battery voltage.

// BLE 0xB2 entry: advance to the next candidate source, sample it once and
// push the result via ble_log().  No return value; the result string format
// is documented in battery_scan.c.
void battery_scan_next(void);

// Reset the sweep cursor to the beginning of the candidate list.
void battery_scan_reset(void);

// BLE 0xB5: sample the battery once on PB5 and notify the result.
// Output line: "BAT:mv=<dddd> lvl=<n>"  (mv in millivolts, lvl 0..100)
void battery_read_and_notify(void);

// BLE 0xB6: one-shot sweep of the 5 ADC-capable pins that are NOT already
// used by another function on this board (PB0, PB2, PB3, PB5, PC5).  Pins
// that are wired to the EPD/flash SPI bus or the buttons (PB1, PB4, PB6,
// PB7, PC4) are deliberately excluded -- reconfiguring them would break
// those functions.  Each pin is sampled several times and the median is
// reported.  Output is one line per pin:
//   "BS:PB0 mv=<dddd> n=<ok/low/grounded>"  ...  then "BS:END"
void battery_scan_all(void);

// BLE 0xB7: toggle digital-level monitoring of the 5 free pins (PB0/PB2/
// PB3/PB5/PC5).  When enabled, each main_loop tick reads their digital
// level and, on any change, notifies a line like
//   "DG:DIFF PB0=0->1 PC5=1->0"
// so you can watch which pin flips when you plug in / unplug USB power
// (i.e. find the charge-detect pin).  Call again to disable.
void digital_scan_set_enabled(uint8_t en);
uint8_t digital_scan_is_enabled(void);

// Called from main_loop every ~1s; no-op when monitoring is off.
void digital_scan_tick(void);

// BLE 0xB8: toggle continuous ADC scan mode.  When enabled, every main_loop
// tick samples the 6 ADC pins believed to be free for battery probing
// (PB0, PB1, PB2, PB3, PB5, PC5) using 5-sample median for noise rejection.
// The other 4 ADC pins stay off-limits:
//   PB4 FRONT button (1M pull-up) - kills button detection
//   PC4 LEFT  button (1M pull-up) - same
//   PB6 ext-Flash MISO            - corrupts running flash reads
//   PB7 EPD/Flash MOSI (shared)   - corrupts SPI byte stream
//
// Note: PB1 was originally assumed to be the EPD power MOSFET gate, but a
// manual probe (BLE 0xC0) showed the screen stays on whether PB1 is driven
// LOW, HIGH, or high-Z.  PB1 was therefore also wired up as UART TX
// (init_uart) and reconfigured by the EPD power-gate macros; both uses have
// since been removed (the UART was a dead init, the gate was dead code that
// only fought the other users of PB1).  PB1 is now a free ADC input and is
// included in the scan as a candidate for locating the battery divider pin.
//
// Output is three notify lines per tick, two entries each:
//   "ADC1:PB0=3232 PB1=3232"
//   "ADC2:PB2= 538 PB3=2480"
//   "ADC3:PB5=3802 PC5=3482"
//
// Values are MILLIVOLTS, not raw codes.  Conversion lives inside
// adc_sample_and_get_result(): mV = raw * Vref * 8 / 0x2000 with
// Vref=1175 mV (default 1.2 V bandgap), 1/8 pre-scaler, 14-bit raw
// masked to 13 bits (BIT(13) is treated as sign bit in differential mode
// and dropped).  Maximum return is ~9396 mV; readings near 9000 usually
// indicate a transient during sampling, not a 9 V battery.
//
// Call again to disable.
void adc_scan_set_enabled(uint8_t en);
uint8_t adc_scan_is_enabled(void);

// Called from main_loop every ~1s; no-op when not enabled.
void adc_scan_tick(void);

// BLE 0xB9: toggle continuous chip-supply (VDD) monitor using the on-chip
// VBAT/3 channel -- no GPIO involved.  Reports the rail that actually powers
// the chip, which is the battery voltage when the cell feeds VDD directly.
// Once per ~1s tick it pushes one line:
//   "VDD:<dddd>"          (millivolts, e.g. "VDD:3021")
// This bypasses every external-pin uncertainty and is the ground-truth
// reference for the GPIO ADC scan (0xB8).  Call again to disable.
//
// Config: internal VBAT positive input, 1/3 VBAT divider, 1.2 V bandgap
// reference, 1:1 pre-scale (NOT 1/8 -- VDD/3 already fits the 1.2 V window,
// and the old read_vbat_direct() that also enabled 1/8 pre-scale was double-
// attenuated and read ~0).
void vdd_scan_set_enabled(uint8_t en);
uint8_t vdd_scan_is_enabled(void);

// Called from main_loop every ~1s; no-op when not enabled.
void vdd_scan_tick(void);

// ------------------------------------------
// 电池电压测量 (PB1驱动高 → PB5差分测压)
// ------------------------------------------
// 每分钟拉高 PB1, 测 PB5 差分电压, 计算电池电压和 SOC
// 电压 = PB5_mV × 2 (14串总电压), SOC 查 3.7V 锂电池表
// 测量完拉低 PB1 以减少发热.
extern uint16_t measured_batt_mv;      // 14串电池总电压 (mV)
extern uint8_t  measured_batt_soc;     // 电量百分比 0~100
void battery_check_tick(void);