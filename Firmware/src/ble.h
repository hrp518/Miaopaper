#pragma once

#include <stdbool.h>
#include <stdint.h>

void init_ble(void);
void set_adv_data(int16_t temp, uint8_t battery_level, uint16_t battery_mv);
void set_air_tag_adv_data(void); // 发送 airtag 广播
bool ble_get_connected(void);
bool ble_get_ota_started(void);
void ble_send_temp(int16_t temp);
void ble_send_battery(uint8_t value);
void blt_pm_proc(void);

int RxTxWrite(void *p);
int otaWritePre(void *p);
void ble_set_connection_speed(uint16_t speed);
void ble_log(const char *msg);
void ble_set_advertising(uint8_t on);
void ble_adv_slow_for_lock(void);   // 锁屏:广播保留,间隔拉长到 2s(深睡省电)
void ble_adv_restore_fast(void);    // 解锁:还原 1s 广播间隔
void ble_link_maintenance_tick(void);
uint16_t ble_get_effective_mtu(void);
