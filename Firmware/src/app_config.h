#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

#define CLOCK_SYS_CLOCK_HZ  	24000000

#define ADVERTISING_INTERVAL 1600

// Firmware version + build timestamp: generated automatically on every build
// by make/gen_build_info.py into build_info.h.  Including it here means every
// source file that includes app_config.h (via main.h) sees the same version.
#include "build_info.h"
// Author contact info shown on the About screen.
#define AUTHOR_NAME    "hrp"
#define AUTHOR_EMAIL   "hrp8888@outlook.com"
#define AUTHOR_BILIBILI "hrp8888"
#define AUTHOR_GITHUB  "hrp518/MiaoPaper"

// Selectable idle-sleep timeouts (seconds). Indexed by settings.sleep_timeout_idx.
// Kept here (not in flash.h) so both app.c and the settings menu see it.
// NOTE: use 'unsigned short' (a builtin type) instead of uint16_t, because
// stdint.h may not be included yet at this point in the include order.
#define SLEEP_TIMEOUT_COUNT 4
extern const unsigned short g_sleep_timeout_s[SLEEP_TIMEOUT_COUNT];

#define EPD_GC_INTERVAL_COUNT 4
extern const unsigned short g_epd_gc_interval[EPD_GC_INTERVAL_COUNT];

// 锁屏轮询唤醒周期(ms)。锁屏时广播已停止,MCU 靠 32k 应用唤醒定时器按此
// 周期醒来轮询按钮(见 app.c)。唤醒间隔 = 按钮响应的最坏等待时间,也是
// 锁屏电流的主要决定因素:1000ms 与旧版 1s 广播唤醒体验一致。
#define LOCK_POLL_MS  1000

#define RAM _attribute_data_retention_ // short version, this is needed to keep the values in ram after sleep

#include "application/print/u_printf.h"
enum{
	CLOCK_SYS_CLOCK_1S = CLOCK_SYS_CLOCK_HZ,
	CLOCK_SYS_CLOCK_1MS = (CLOCK_SYS_CLOCK_1S / 1000),
	CLOCK_SYS_CLOCK_1US = (CLOCK_SYS_CLOCK_1S / 1000000),
};

///////////////////////////////////// ATT  HANDLER define ///////////////////////////////////////
typedef enum
{
	ATT_H_START = 0,

	//// Gap ////
	/**********************************************************************************************/
	GenericAccess_PS_H, 					//UUID: 2800, 	VALUE: uuid 1800
	GenericAccess_DeviceName_CD_H,			//UUID: 2803, 	VALUE:  			Prop: Read | Notify
	GenericAccess_DeviceName_DP_H,			//UUID: 2A00,   VALUE: device name
	GenericAccess_Appearance_CD_H,			//UUID: 2803, 	VALUE:  			Prop: Read
	GenericAccess_Appearance_DP_H,			//UUID: 2A01,	VALUE: appearance
	CONN_PARAM_CD_H,						//UUID: 2803, 	VALUE:  			Prop: Read
	CONN_PARAM_DP_H,						//UUID: 2A04,   VALUE: connParameter

	//// gatt ////
	/**********************************************************************************************/
	GenericAttribute_PS_H,					//UUID: 2800, 	VALUE: uuid 1801
	GenericAttribute_ServiceChanged_CD_H,	//UUID: 2803, 	VALUE:  			Prop: Indicate
	GenericAttribute_ServiceChanged_DP_H,   //UUID:	2A05,	VALUE: service change
	GenericAttribute_ServiceChanged_CCB_H,	//UUID: 2902,	VALUE: serviceChangeCCC

	//// battery service ////
	/**********************************************************************************************/
	BATT_PS_H, 								//UUID: 2800, 	VALUE: uuid 180f
	BATT_LEVEL_INPUT_CD_H,					//UUID: 2803, 	VALUE:  			Prop: Read | Notify
	BATT_LEVEL_INPUT_DP_H,					//UUID: 2A19 	VALUE: batVal
	BATT_LEVEL_INPUT_CCB_H,					//UUID: 2902, 	VALUE: batValCCC

	//// Temp service ////
	/**********************************************************************************************/
	TEMP_PS_H, 								//UUID: 2800, 	VALUE: uuid 181A
	TEMP_LEVEL_INPUT_CD_H,					//UUID: 2803, 	VALUE:  			Prop: Read | Notify
	TEMP_LEVEL_INPUT_DP_H,					//UUID: 2A19 	VALUE: tempVal
	TEMP_LEVEL_INPUT_CCB_H,					//UUID: 2902, 	VALUE: tempValCCC

	//// Ota ////
	/**********************************************************************************************/
	OTA_PS_H, 								//UUID: 2800, 	VALUE: telink ota service uuid
	OTA_CMD_OUT_CD_H,						//UUID: 2803, 	VALUE:  			Prop: read | write_without_rsp
	OTA_CMD_OUT_DP_H,						//UUID: telink ota uuid,  VALUE: otaData
	OTA_CMD_OUT_DESC_H,						//UUID: 2901, 	VALUE: otaName

	//// RxTx ////
	/**********************************************************************************************/
	RxTx_PS_H, 								//UUID: , 	VALUE: RxTx service uuid
	RxTx_CMD_OUT_CD_H,						//UUID: , 	VALUE:  			Prop: read | write_without_rsp
	RxTx_CMD_OUT_DP_H,						//UUID: RxTx uuid,  VALUE: RxTxData
	RxTx_CMD_OUT_DESC_H,						//UUID: 2901, 	VALUE: RxTxName

	//// EPD_BLE ////
	/**********************************************************************************************/
	EPD_BLE_PS_H, 								//UUID: , 	VALUE: EPD_BLE service uuid
	EPD_BLE_CMD_OUT_CD_H,						//UUID: , 	VALUE:  			Prop: write_without_rsp
	EPD_BLE_CMD_OUT_DP_H,						//UUID: EPD_BLE uuid,  VALUE: EPD_BLEData
	EPD_BLE_CMD_OUT_DESC_H,						//UUID: , 	VALUE: EPD_BLEName

	ATT_END_H,

}ATT_HANDLE;

#include "vendor/common/default_config.h"

#if defined(__cplusplus)
}
#endif
