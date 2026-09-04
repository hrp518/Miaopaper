// Battery SOC measurement (the only surviving part of the old
// battery-scan diagnostics): every 60s drive PB1 HIGH and read the PB5
// differential, cell voltage = mV x 2, SOC from the 14s table.
// The BLE-triggered sweeps (0xB2/B5/B6/B7/B8/B9) were removed to fit the
// 128KB OTA image limit.

#include <stdint.h>
#include <stdio.h>
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"

#include "battery_scan.h"
#include "battery.h"

// PB5 read configured to match the unknown-firmware register dump verbatim.
// This is a quirk configuration: +AIN = B5P, -AIN = VBAT (i.e. differential
// of PB5 vs the chip's own VDD).  The result is V_PB5 - V_VBAT, not an
// absolute voltage.  We use it as-is to mirror what some external firmware
// does, so behaviour can be cross-checked against that dump.
static uint16_t sample_pb5_quirk_mv(void)
{
	adc_power_on_sar_adc(0);
	adc_set_sample_clk(5);
	adc_set_left_right_gain_bias(GAIN_STAGE_BIAS_PER100, GAIN_STAGE_BIAS_PER100);
	adc_set_chn_enable_and_max_state_cnt(ADC_MISC_CHN, 2);
	adc_set_state_length(240, 0, 10);
    gpio_setup_up_down_resistor(GPIO_PB5, 0);
	// Leave PB5 as-is (no drive, normal ADC input).  Skip gpio_setup_up_down
	// so the floating state from the user/board is preserved.

	// Differential mode, RES14 with the differential sign bit enabled
	// (BIT(13) = sign), matching the unknown-firmware dump.
	analog_write(anareg_adc_res_m, RES14 | FLD_ADC_EN_DIFF_CHN_M);

	// Quirk: +AIN = B5P (0x6), -AIN = VBAT (0xF).  This is NOT the SDK
	// standard path (which uses -AIN = GND).  It produces V_PB5 - V_VBAT.
	analog_write(areg_adc_ain_chn_misc, (B5P << 4) | VBAT);

	// VBAT divider kept OFF (BIT_RNG(2,3) = 0) but BIT(4) = 1 to match the
	// unknown dump byte 0x10.  This is a reserved bit, harmless but copied
	// verbatim so the configuration is identical.
	analog_write(areg_adc_vref_vbat_div, 0x10);

	adc_set_ref_voltage(ADC_MISC_CHN, ADC_VREF_1P2V);   // sets 0xFA bias too
	adc_set_tsample_cycle_chn_misc(SAMPLING_CYCLES_6);
	adc_set_ain_pre_scaler(ADC_PRESCALER_1F8);         // 1/8 -> 0xFA = 0xFD
	adc_power_on_sar_adc(1);

	// Average 10 reads.  Leave ADC powered on after sampling (matches the
	// unknown firmware behaviour where 0xFC stays at 0xC5 across dumps).
	uint32_t sum = 0;
	for (int k = 0; k < 10; k++)
		sum += (uint16_t)adc_sample_and_get_result();
	return (uint16_t)(sum / 10u);
}

// ------------------------------------------
// 全局电池状态
// ------------------------------------------
uint16_t measured_batt_mv  = 0;   // 14串电池总电压 (mV)
uint8_t  measured_batt_soc = 0;   // 电量 0~100

// 每秒调用, 每 60 秒测量一次: 驱动 PB1 高, 测 PB5 差分,
// 单电池电压 = PB5_差分_mV × 2, 直接查表得 SOC.
// 开机后第一次 tick 立刻触发（cnt 初始=60）。
void battery_check_tick(void)
{
	static uint16_t cnt = 60;   // start at 60 so first call triggers immediately
	if (cnt < 60) { cnt++; return; }
	cnt = 1;                    // next measurement in 60 seconds

	// 驱动 PB1 = HIGH
	gpio_set_func(GPIO_PB1, AS_GPIO);
	gpio_set_output_en(GPIO_PB1, 1);
	gpio_set_data_strength(GPIO_PB1, 1);
	gpio_write(GPIO_PB1, 1);
	sleep_us(100);  // 等待稳定

	// 测 PB5 差分 (B5P - VBAT, 1/8预分频, RES14 差分)
	uint16_t pb5_mv = sample_pb5_quirk_mv();

	// 单电池电压 = PB5 × 2 (用户经验公式), 直接查 SOC 表（不用除14）
	uint32_t cell_mv = (uint32_t)pb5_mv * 2u;

	measured_batt_mv  = (uint16_t)cell_mv;
	measured_batt_soc = get_battery_soc_14s(cell_mv);

	// 拉低 PB1 减少发热
	gpio_write(GPIO_PB1, 0);
	gpio_set_output_en(GPIO_PB1, 0);
}
