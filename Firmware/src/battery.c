#include <stdint.h>
#include "tl_common.h"
#include "main.h"
#include "drivers.h"
#include "stack/ble/ble.h"

#include "battery.h"
#include "battery_scan.h"
#include "buttons.h"
#include "main.h"

extern RAM uint16_t battery_mv;

_attribute_ram_code_ void adc_init_firmware(ADC_InputPchTypeDef p_ain, ADC_InputNchTypeDef n_ain)
{
	adc_power_on_sar_adc(0);
	adc_set_sample_clk(5);
	adc_set_left_right_gain_bias(GAIN_STAGE_BIAS_PER100, GAIN_STAGE_BIAS_PER100);
	adc_set_chn_enable_and_max_state_cnt(ADC_MISC_CHN, 2);
	adc_set_state_length(240, 0, 10);
	analog_write(anareg_adc_res_m, RES14 | FLD_ADC_EN_DIFF_CHN_M);
	adc_set_ain_chn_misc(p_ain, n_ain);
	adc_set_input_mode_chn_misc(DIFFERENTIAL_MODE);
	adc_set_ref_voltage(ADC_MISC_CHN, ADC_VREF_1P2V);
	adc_set_tsample_cycle_chn_misc(SAMPLING_CYCLES_6);
	adc_set_ain_pre_scaler(ADC_PRESCALER_1);
	adc_power_on_sar_adc(1);
}

_attribute_ram_code_ uint16_t get_adc_reading(ADC_InputPchTypeDef p_ain, ADC_InputNchTypeDef n_ain)
{
	uint16_t temp;
	uint16_t adc_reading_temp;
	volatile unsigned int adc_dat_buf[8];
	int i, j;
	adc_init_firmware(p_ain, n_ain);
	adc_reset_adc_module();
	u32 t0 = clock_time();

	uint16_t adc_sample[8] = {0};
	u32 adc_result;
	for (i = 0; i < 8; i++)
	{
		adc_dat_buf[i] = 0;
	}
	while (!clock_time_exceed(t0, 25))
		;
	adc_config_misc_channel_buf((uint16_t *)adc_dat_buf, 8 << 2);
	dfifo_enable_dfifo2();

	for (i = 0; i < 8; i++)
	{
		while (!adc_dat_buf[i])
			;
		if (adc_dat_buf[i] & BIT(13))
		{
			adc_sample[i] = 0;
		}
		else
		{
			adc_sample[i] = ((uint16_t)adc_dat_buf[i] & 0x1FFF);
		}
		if (i)
		{
			if (adc_sample[i] < adc_sample[i - 1])
			{
				temp = adc_sample[i];
				adc_sample[i] = adc_sample[i - 1];
				for (j = i - 1; j >= 0 && adc_sample[j] > temp; j--)
				{
					adc_sample[j + 1] = adc_sample[j];
				}
				adc_sample[j + 1] = temp;
			}
		}
	}
	dfifo_disable_dfifo2();
	u32 adc_average = (adc_sample[2] + adc_sample[3] + adc_sample[4] + adc_sample[5]) / 4;
	adc_result = adc_average;
	adc_reading_temp = (adc_result * adc_vref_cfg.adc_vref) >> 10;

	adc_power_on_sar_adc(0);
	return adc_reading_temp;
}

_attribute_ram_code_ uint16_t get_battery_mv(void)
{
	adc_init();
	adc_base_init(GPIO_PB5);
	adc_power_on_sar_adc(1);
	return adc_sample_and_get_result();
}

uint8_t ui_get_battery_pct(void)
{
	if (measured_batt_soc > 0)
		return measured_batt_soc;
	if (measured_batt_mv > 0)
		return get_battery_soc_14s(measured_batt_mv);
	if (battery_mv > 100)
		return get_battery_soc_14s((uint32_t)battery_mv);
	return 0;
}

void ui_format_battery(char *buf, int buf_len)
{
	uint8_t pct = ui_get_battery_pct();
	int n = 0;

	if (pct > 100) pct = 100;
	if (buf_len < 2) { if (buf_len > 0) buf[0] = 0; return; }

	if (pct >= 100) {
		if (buf_len > n + 3) { buf[n++] = '1'; buf[n++] = '0'; buf[n++] = '0'; }
	} else if (pct >= 10) {
		if (buf_len > n + 2) { buf[n++] = '0' + (pct / 10); buf[n++] = '0' + (pct % 10); }
	} else {
		if (buf_len > n + 1) buf[n++] = '0' + pct;
	}
	if (buf_len > n + 1) buf[n++] = '%';
	if (is_charging() && buf_len > n + 1) buf[n++] = '+';
	buf[n] = 0;
}

_attribute_ram_code_ uint8_t get_battery_level(uint16_t mv)
{
	(void)mv;
	return ui_get_battery_pct();
}

_attribute_ram_code_ uint8_t get_battery_soc(uint16_t battery_mv)
{
	// SOC = 100% - (voltage / 1V), integer math.  battery_mv is in mV.
	//   mv = 3000 -> SOC = 100 - 3 = 97
	//   mv = 3500 -> SOC = 100 - 3 = 97
	//   mv = 4200 -> SOC = 100 - 4 = 96
	uint16_t soc = 100u - (uint16_t)(battery_mv / 1000u);
	return (uint8_t)soc;
}

// 单节 3.7V 锂电池电压→电量查表 (用户提供的 14串除14所得).
// cell_mv = 单节电池电压(mV). 返回电量百分比 0~100.
_attribute_ram_code_ uint8_t get_battery_soc_14s(uint32_t cell_mv)
{
	// 单节电压(mV) 和 对应 SOC(%), 按电压降序排列
	static const uint16_t tbl_v[] = {
		4200,4150,4140,4120,4100,4080,4050,4030,
		3970,3930,3900,3870,3840,3810,3790,3770,
		3760,3760,3740,3730,3720,3710,3710,3690,
		3660,3650,3640,3630,3610,3590,3580,3550,
		3500,3420,3300,3000,2700
	};
	static const uint8_t tbl_pct[] = {
		100, 99, 97, 95, 92, 90, 87, 85,
		 80, 75, 70, 65, 60, 55, 50, 45,
		 42, 40, 35, 30, 25, 20, 15, 12,
		 10,  8,  5,  3,  1,  0,  0,  0,
		  0,  0,  0,  0,  0
	};
	static const int n = sizeof(tbl_v) / sizeof(tbl_v[0]);

	if (cell_mv >= tbl_v[0]) return 100;
	for (int i = 0; i < n; i++) {
		if (cell_mv >= tbl_v[i])
			return tbl_pct[i];
	}
	return 0;
}

_attribute_ram_code_ void adc_temp_init(void)
{
	adc_set_chn_enable_and_max_state_cnt(ADC_MISC_CHN, 2);
	adc_set_state_length(240, 0, 10);  	//set R_max_mc,R_max_c,R_max_s

	//set Vbat divider select,
	adc_set_vref_vbat_divider(ADC_VBAT_DIVIDER_OFF);
	//ADC_VBAT_Scale = VBAT_Scale_tab[ADC_VBAT_DIVIDER_OFF];

	adc_set_input_mode(ADC_MISC_CHN, DIFFERENTIAL_MODE);
	adc_set_ain_channel_differential_mode(ADC_MISC_CHN, TEMSENSORP, TEMSENSORN);
	adc_set_ref_voltage(ADC_MISC_CHN, ADC_VREF_1P2V);//set channel Vref
	//ADC_Vref = (unsigned char)ADC_VREF_1P2V;
	adc_set_resolution(ADC_MISC_CHN, RES14);//set resolution
	//Number of ADC clock cycles in sampling phase
	adc_set_tsample_cycle(ADC_MISC_CHN, SAMPLING_CYCLES_6);

	//set Analog input pre-scaling and
	adc_set_ain_pre_scaler(ADC_PRESCALER_1);
	//ADC_Pre_Scale = 1<<(unsigned char)ADC_PRESCALER_1F8;
	//set NORMAL mode
	adc_set_mode(ADC_NORMAL_MODE);

}

_attribute_ram_code_ uint16_t get_temperature_c(void)
{
	analog_write(0x07, analog_read(0x07) & (~BIT(4)));
    adc_init();
	adc_temp_init();
    adc_power_on_sar_adc(1);
	uint16_t temp_reading = adc_sample_and_get_result();
	analog_write(0x07, analog_read(0x07) | BIT(4));
    return temp_reading;
	/*
	uint16_t temp_reading;
	analog_write(0x07, analog_read(0x07) & (~BIT(4)));
	temp_reading = get_adc_reading(TEMSENSORP, TEMSENSORN);
	analog_write(0x07, analog_read(0x07) | BIT(4));

	
    unsigned short  adc_temp_value = 0;
    adc_sample_and_get_result();
    adc_temp_value = 579-((temp_reading * 840)>>13);
    return adc_temp_value;*/
}