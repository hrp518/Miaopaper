#include <stdint.h>
#include <stdio.h>
#include "tl_common.h"
#include "main.h"
#include "drivers.h"
#include "stack/ble/ble.h"

#include "etime.h"        // current_unix_time
#include "ext_flash.h"
#include "super_sleep.h"

RAM extern uint32_t last_clock_increase;   // etime.c:1s 基准 tick(未在 etime.h 声明)
RAM extern uint32_t current_unix_time;     // etime.c:墙上时钟(unix 秒,北京偏移)

// 记录布局(12 字节,手动打包,避免结构体对齐问题):
// b0 magic=0x5B | b1 flags(bit0=曾在超级省电深睡) | b2 rsv=0xFF
// b3..b6 wall = current_unix_time (LE) | b7..b10 tick = clock_time() (LE)
// b11 crc = xor(b0..b10)

#define SS_MAGIC 0x5B

RAM static uint8_t  ss_append_valid = 0;
RAM static uint16_t ss_append_idx = 0;
RAM static uint8_t  ss_super_wake = 0;
RAM static uint8_t  ss_maintenance = 0;

static uint8_t rec_crc(const uint8_t *r)
{
    uint8_t c = 0;
    for (int i = 0; i < 11; i++) c ^= r[i];
    return c;
}

void ss_stash(uint8_t flags)
{
    uint8_t r[SS_REC_SIZE];

    if (!ext_flash_is_safe())   // EPD 刷新中放弃(下一次入睡会再写)
        return;
    ext_flash_init();

    if (!ss_append_valid) {
        // 扫描策略:扇区可能有出厂残留(非 0xFF 非 magic),不能见到垃圾就停。
        // 线性扫全扇区,统计 magic 记录数;垃圾当作空洞跳过。
        uint16_t off;
        uint8_t buf[256];
        ss_append_idx = 0;
        for (off = 0; off < 4096; off += 256) {
            ext_flash_read(SS_ADDR + off, 256, buf);
            for (int i = 0; i < 256; i += SS_REC_SIZE) {
                if (buf[i] == SS_MAGIC)
                    ss_append_idx = (off + i) / SS_REC_SIZE + 1;  // 最新 magic 之后
            }
        }
        if (ss_append_idx == 0) {
            // 整扇区无有效记录(出厂残留/首次使用):格式化后从 0 开始
            ext_flash_sector_erase(SS_ADDR);
            ss_append_idx = 0;
        }
        ss_append_valid = 1;
    }

    if (ss_append_idx >= SS_MAX_RECS) {
        // 扇区满:保留最新 KEEP 条
        uint16_t keep = SS_KEEP_ON_FULL;
        uint8_t buf[SS_KEEP_ON_FULL * SS_REC_SIZE];
        ext_flash_read(SS_ADDR + (SS_MAX_RECS - keep) * SS_REC_SIZE,
                       keep * SS_REC_SIZE, buf);
        ext_flash_sector_erase(SS_ADDR);
        ext_flash_page_program(SS_ADDR, keep * SS_REC_SIZE, buf);
        ss_append_idx = keep;
    }

    memset(r, 0xFF, sizeof(r));
    r[0] = SS_MAGIC;
    r[1] = flags;
    r[2] = 0xFF;
    uint32_t wall = current_unix_time;
    uint32_t tick = clock_time();
    r[3] = (uint8_t)wall;  r[4] = (uint8_t)(wall >> 8);
    r[5] = (uint8_t)(wall >> 16); r[6] = (uint8_t)(wall >> 24);
    r[7] = (uint8_t)tick;  r[8] = (uint8_t)(tick >> 8);
    r[9] = (uint8_t)(tick >> 16); r[10] = (uint8_t)(tick >> 24);
    r[11] = rec_crc(r);

    ext_flash_page_program(SS_ADDR + ss_append_idx * SS_REC_SIZE,
                           SS_REC_SIZE, r);
    ss_append_idx++;
}

uint8_t ss_boot_restore(void)
{
    // 冷启动:扫全扇区取"最后一条有效 magic 记录"(跳过出厂残留空洞)
    uint8_t best[SS_REC_SIZE];
    int found = 0;

    for (uint16_t off = 0; off < 4096; off += 256) {
        uint8_t buf[256];
        ext_flash_read(SS_ADDR + off, 256, buf);
        for (int i = 0; i < 256; i += SS_REC_SIZE) {
            if (buf[i] != SS_MAGIC) continue;           // 垃圾/空洞:跳过
            if (buf[i + 11] != rec_crc(buf + i)) continue; // 坏记录:跳过
            memcpy(best, buf + i, SS_REC_SIZE);         // 顺序扫,后者覆盖前者
            found = 1;
        }
    }

    ss_append_valid = 0;   // 迫使下次入睡重新扫描定位写指针

    if (!found || !(best[1] & SS_FLAG_WAS_SUPER))
        return 0;

    uint32_t wall = (uint32_t)best[3] | ((uint32_t)best[4] << 8) |
                    ((uint32_t)best[5] << 16) | ((uint32_t)best[6] << 24);
    if (wall > 1700000000u) {          // 合理性:2023 年之后
        current_unix_time = wall;      // 时钟恢复到入睡时刻(睡眠时长丢失)
        last_clock_increase = clock_time();
    }
    ss_super_wake = 1;
    return best[1];
}

uint8_t ss_is_super_wake(void)
{
    return ss_super_wake;
}

void ss_set_maintenance(uint8_t en)
{
    ss_maintenance = en ? 1 : 0;
}

uint8_t ss_is_maintenance(void)
{
    return ss_maintenance;
}
