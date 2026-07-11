#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include "tl_common.h"
#include "main.h"
#include "drivers.h"
#include "vendor/common/user_config.h"
#include "app_config.h"
#include "drivers/8258/gpio_8258.h"
#include "stack/ble/ble.h"
#include "vendor/common/blt_common.h"
#include "ota.h"

#include "ble.h"
#include "cmd_parser.h"
#include "flash.h"
#include "epd_ble_service.h"
#include "ebook.h"

RAM uint8_t ble_connected = 0;
RAM uint8_t ota_started = 0;
RAM static u32 ble_conn_tick;
RAM static u8 ble_mtu_done;
RAM static u8 ble_mtu_tries;
RAM static u8 ble_dle_done;
extern uint8_t my_tempVal[2];
extern uint8_t my_batVal[1];

/* Telink DLE/MTU reference: RX 288 (251+24), TX 264 (251+12). */
#define BLE_LL_RX_FIFO_SIZE  288
#define BLE_LL_RX_FIFO_NUM   8
#define BLE_LL_TX_FIFO_SIZE  264
#define BLE_LL_TX_FIFO_NUM   8
#define BLE_ATT_MTU_REQ      247
#define BLE_MTU_REQ_DELAY_US 1500000UL  /* 1.5 s after connect */
#define BLE_DLE_REQ_DELAY_US 2000000UL  /* 2.0 s after connect */

RAM uint8_t blt_rxfifo_b[BLE_LL_RX_FIFO_SIZE * BLE_LL_RX_FIFO_NUM] = {0};
RAM my_fifo_t blt_rxfifo = {
	BLE_LL_RX_FIFO_SIZE,
	BLE_LL_RX_FIFO_NUM,
	0,
	0,
	blt_rxfifo_b,
};

RAM uint8_t blt_txfifo_b[BLE_LL_TX_FIFO_SIZE * BLE_LL_TX_FIFO_NUM] = {0};
RAM my_fifo_t blt_txfifo = {
	BLE_LL_TX_FIFO_SIZE,
	BLE_LL_TX_FIFO_NUM,
	0,
	0,
	blt_txfifo_b,
};

RAM uint8_t ble_name[] = {11, 0x09, 'M', 'P', 'P', '_', '0', '0', '0', '0', '0', '0'};

RAM uint8_t advertising_data[] = {
	/*Description*/ 16, 0x16, 0x1a, 0x18,
	/*MAC*/ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	/*Temp*/ 0xaa, 0xaa,
	/*Humi*/ 0xbb,
	/*BatL*/ 0xcc,
	/*BatM*/ 0xdd, 0xdd,
	/*Counter*/ 0x00};

RAM uint8_t mac_public[6];

RAM uint8_t PUB_KEY[28] = {
    0x49,0x88,0x0,0x7a,0x27,0xac,0x38,0xb7,0x16,0x55,0x3c,0xc8,0x57,0x62,0x93,0xc3,0x95,0xef,0x3f,0x63,0x70,0xb2,0xa3,0x96,0x6d,0x4c,0x1a,0x7d //
};

RAM uint8_t air_tag_adv_data[31] = {
    0x1e, /* Length (30) */
    0xff, /* Manufacturer Specific Data (type 0xff) */
    0x4c, 0x00, /* Company ID (Apple) */
    0x12, 0x19, /* Offline Finding type and length */
    0x00, /* State */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, /* First two bits */
    0x00, /* Hint (0x00) */
};

RAM uint8_t AIR_TAG_OPEN = 0;

_attribute_ram_code_ void ble_log(const char *msg)
{
    if (!ble_connected || !msg)
        return;
    int len = 0;
    while (msg[len] && len < 64)
        len++;
    if (len > 0) {
        bls_att_pushNotifyData(RxTx_CMD_OUT_DP_H, (uint8_t *)msg, len);
        bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, (uint8_t *)msg, len);
    }
}

uint16_t ble_get_effective_mtu(void)
{
	return blc_att_getEffectiveMtuSize(0);
}

static void ble_reset_link_state(void)
{
	ble_conn_tick = 0;
	ble_mtu_done = 0;
	ble_mtu_tries = 0;
	ble_dle_done = 0;
}

static int ble_gap_host_event(u32 h, u8 *para, int n)
{
	u8 event = (u8)(h & 0xFF);

	if (event == GAP_EVT_ATT_EXCHANGE_MTU && para) {
		gap_gatt_mtuSizeExchangeEvt_t *ev = (gap_gatt_mtuSizeExchangeEvt_t *)para;
		char msg[48];
		sprintf(msg, "MTU eff=%u peer=%u", ev->effective_MTU, ev->peer_MTU);
		ble_log(msg);
		ble_mtu_done = 1;
	}
	return 0;
}

_attribute_ram_code_ void ble_link_maintenance_tick(void)
{
	if (!ble_connected || !ble_conn_tick)
		return;

	if (!ble_mtu_done) {
		if (ble_mtu_tries == 0 && clock_time_exceed(ble_conn_tick, BLE_MTU_REQ_DELAY_US)) {
			blc_att_requestMtuSizeExchange(BLS_CONN_HANDLE, BLE_ATT_MTU_REQ);
			ble_mtu_tries = 1;
			ble_log("MTU req 1");
		} else if (ble_mtu_tries == 1 && clock_time_exceed(ble_conn_tick, BLE_MTU_REQ_DELAY_US + 1500000UL)) {
			blc_att_requestMtuSizeExchange(BLS_CONN_HANDLE, BLE_ATT_MTU_REQ);
			ble_mtu_tries = 2;
			ble_log("MTU req 2");
		}
	}

	if (!ble_dle_done && clock_time_exceed(ble_conn_tick, BLE_DLE_REQ_DELAY_US)) {
		blc_ll_exchangeDataLength(LL_LENGTH_REQ, 251);
		ble_dle_done = 1;
		ble_log("DLE req");
	}
}

_attribute_ram_code_ void app_switch_to_indirect_adv(uint8_t e, uint8_t *p, int n)
{
	bls_ll_setAdvParam(ADVERTISING_INTERVAL, ADVERTISING_INTERVAL + 50, ADV_TYPE_CONNECTABLE_UNDIRECTED, OWN_ADDRESS_PUBLIC, 0, NULL, BLT_ENABLE_ADV_ALL, ADV_FP_NONE);
	bls_ll_setAdvEnable(1);
}

void ble_set_advertising(uint8_t on)
{
	bls_ll_setAdvEnable(on ? 1 : 0);
}

_attribute_ram_code_ void ble_disconnect_callback(uint8_t e, uint8_t *p, int n)
{
	ble_connected = 0;
	ota_started = 0;
	ble_reset_link_state();
	ebook_ble_reset_upload();
	printf("BLE disconnected\r\n");
}

_attribute_ram_code_ void user_set_rf_power(uint8_t e, uint8_t *p, int n)
{
	rf_set_power_level_index(RF_POWER_P3p01dBm);
}

_attribute_ram_code_ void ble_connect_callback(uint8_t e, uint8_t *p, int n)
{
	ble_connected = 1;
	ota_started = 0;
	ble_reset_link_state();
	ble_conn_tick = clock_time() | 1;
	ble_set_connection_speed(6);
	ble_log("BLE connected");
}

_attribute_ram_code_ void ble_set_connection_speed(uint16_t speed)
{
	bls_l2cap_requestConnParamUpdate(speed, speed + 2, 0, 2000);
}

_attribute_ram_code_ int otaWritePre(void *p)
{
	rf_packet_att_data_t *req = (rf_packet_att_data_t *)p;
	if (req->dat[0] >=0x10 && req->dat[0] <=0x44)
		return epd_ble_handle_write(p);
	if (ota_started == 0)
	{
		ota_started = 1;
		ble_set_connection_speed(6);
	}
	return custom_otaWrite(p);
}

_attribute_ram_code_ int RxTxWrite(void *p)
{
	uint8_t dbg[3] = {'R', 'X', 0};
	rf_packet_att_data_t *req = (rf_packet_att_data_t*)p;
	dbg[2] = req->dat[0];
	bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, dbg, 3);
	cmd_parser(p);
	return 0;
}

_attribute_ram_code_ void blt_pm_proc(void)
{
	bls_pm_setSuspendMask(SUSPEND_ADV | DEEPSLEEP_RETENTION_ADV | SUSPEND_CONN | DEEPSLEEP_RETENTION_CONN);
}

void init_ble(void)
{
	uint8_t mac_random_static[6];

    if(AIR_TAG_OPEN) {
        mac_public[5] = PUB_KEY[0] | 0b11000000;
        mac_public[4] = PUB_KEY[1];
        mac_public[3] = PUB_KEY[2];
        mac_public[2] = PUB_KEY[3];
        mac_public[1] = PUB_KEY[4];
        mac_public[0] = PUB_KEY[5];

        blc_setMacAddress(CFG_ADR_MAC, mac_public);
    } else {
        blc_initMacAddress(CFG_ADR_MAC, mac_public, mac_random_static);
    }

	const char *hex_ascii = {"0123456789ABCDEF"};
	ble_name[6] = hex_ascii[mac_public[2] >> 4];
	ble_name[7] = hex_ascii[mac_public[2] & 0x0f];
	ble_name[8] = hex_ascii[mac_public[1] >> 4];
	ble_name[9] = hex_ascii[mac_public[1] & 0x0f];
	ble_name[10] = hex_ascii[mac_public[0] >> 4];
	ble_name[11] = hex_ascii[mac_public[0] & 0x0f];

	advertising_data[4] = mac_public[5];
	advertising_data[5] = mac_public[4];
	advertising_data[6] = mac_public[3];
	advertising_data[7] = mac_public[2];
	advertising_data[8] = mac_public[1];
	advertising_data[9] = mac_public[0];

	blc_ll_initBasicMCU();
	blc_ll_initStandby_module(mac_public);
	blc_ll_initAdvertising_module(mac_public);
	blc_ll_initConnection_module();
	blc_ll_initSlaveRole_module();
	blc_ll_initPowerManagement_module();

	blc_gap_peripheral_init();
	extern void my_att_init();
	my_att_init();
	blc_l2cap_register_handler(blc_l2cap_packet_receive);
	blc_smp_setSecurityLevel(No_Security);

	bls_ll_setScanRspData((uint8_t *)ble_name, sizeof(ble_name));
	bls_ll_setAdvParam(ADVERTISING_INTERVAL, ADVERTISING_INTERVAL + 50, ADV_TYPE_CONNECTABLE_UNDIRECTED, OWN_ADDRESS_PUBLIC, 0, NULL, BLT_ENABLE_ADV_ALL, ADV_FP_NONE);
	bls_ll_setAdvEnable(1);
	user_set_rf_power(0, 0, 0);
	bls_app_registerEventCallback(BLT_EV_FLAG_SUSPEND_EXIT, &user_set_rf_power);
	bls_app_registerEventCallback(BLT_EV_FLAG_CONNECT, &ble_connect_callback);
	bls_app_registerEventCallback(BLT_EV_FLAG_TERMINATE, &ble_disconnect_callback);

	blc_ll_initPowerManagement_module();
	bls_pm_setSuspendMask(SUSPEND_ADV | DEEPSLEEP_RETENTION_ADV | SUSPEND_CONN | DEEPSLEEP_RETENTION_CONN);
	blc_pm_setDeepsleepRetentionThreshold(95, 95);
	blc_pm_setDeepsleepRetentionEarlyWakeupTiming(240);
	blc_pm_setDeepsleepRetentionType(DEEPSLEEP_MODE_RET_SRAM_LOW32K);

	blc_att_setRxMtuSize(BLE_ATT_MTU_REQ);
	blc_gap_registerHostEventHandler(ble_gap_host_event);
	blc_gap_setEventMask(GAP_EVT_MASK_ATT_EXCHANGE_MTU);
	bls_l2cap_setMinimalUpdateReqSendingTime_after_connCreate(1000);
}

_attribute_ram_code_ bool ble_get_connected(void)
{
	return ble_connected;
}

_attribute_ram_code_ bool ble_get_ota_started(void)
{
	return ota_started;
}

_attribute_ram_code_ void set_adv_data(int16_t temp, uint8_t battery_level, uint16_t battery_mv)
{
	advertising_data[10] = temp >> 8;
	advertising_data[11] = temp & 0xff;

	advertising_data[13] = battery_level;

	advertising_data[14] = battery_mv >> 8;
	advertising_data[15] = battery_mv & 0xff;

	advertising_data[16]++;

	bls_ll_setAdvData((uint8_t *)advertising_data, sizeof(advertising_data));
}

_attribute_ram_code_ void set_air_tag_adv_data(void)
{
    if (AIR_TAG_OPEN) {
        memcpy(&air_tag_adv_data[7], &PUB_KEY[6], 22);
        air_tag_adv_data[29] = PUB_KEY[0] >> 6;

        bls_ll_setAdvData((uint8_t *)air_tag_adv_data, sizeof(air_tag_adv_data));
    }
}

_attribute_ram_code_ void ble_send_temp(int16_t temp)
{
	my_tempVal[0] = temp & 0xFF;
	my_tempVal[1] = temp >> 8;
	bls_att_pushNotifyData(TEMP_LEVEL_INPUT_DP_H, my_tempVal, 2);
}

_attribute_ram_code_ void ble_send_battery(uint8_t value)
{
	my_batVal[0] = value;
	bls_att_pushNotifyData(BATT_LEVEL_INPUT_DP_H, (uint8_t *)my_batVal, 1);
}
