#include <stdint.h>
#include "tl_common.h"
#include "main.h"
#include "drivers.h"
#include "stack/ble/ble.h"
#include "vendor/common/blt_common.h"

#include "battery.h"
#include "ble.h"
#include "flash.h"

void user_init_normal(void);
void user_init_deepRetn(void);
void main_loop(void);
// 锁屏下单点击 F:GC 全刷显示 "Double Click to Unlock"(观察唤醒),并清掉待显示
// 的唤醒提示计时器,避免与深睡唤醒提示重复。
void app_lock_observe(void);
// BLE 数据活动回调:重置空闲计时/保持窗口,让上传/OTA 期间设备永不自动锁屏深睡。
void app_mark_ble_activity(void);