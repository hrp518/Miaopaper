#pragma once
#include <stdint.h>

// Pin definitions (shared with EPD bit-bang SPI)
#define FLASH_CS    GPIO_PD2   // exclusive
#define FLASH_CLK   GPIO_PD7   // shared with EPD_CLK
#define FLASH_MOSI  GPIO_PB7   // shared with EPD_MOSI
#define FLASH_MISO  GPIO_PB6   // exclusive

void ext_flash_init(void);
void ext_flash_read_jedec_id(uint8_t buf[3]);
uint8_t ext_flash_read_status(void);
void ext_flash_wait_ready(void);
void ext_flash_write_enable(void);
void ext_flash_read(uint32_t addr, uint16_t len, uint8_t *buf);
void ext_flash_page_program(uint32_t addr, uint16_t len, const uint8_t *data);
void ext_flash_sector_erase(uint32_t addr);
void ext_flash_block_erase_64k(uint32_t addr);
uint8_t ext_flash_is_safe(void);
// 把外置 NOR Flash 置入深度断电(0xB9):休眠阶段从 standby(数十 uA)降到
// ~1-2 uA。下次任意 Flash 访问(读到 0xAB)自动唤醒。幂等,可反复调用。
void ext_flash_deep_power_down(void);
// 冷启动调用一次:同步 RAM 标志与 Flash 物理深断电状态(无条件补发 0xAB)。
void ext_flash_boot_resync(void);
// 从深度断电唤醒(0xAB 释放)。通常无需显式调用 —— 每次 Flash 访问都会自动
// 先唤醒;仅在确实需要同步等待唤醒完成时使用。
void ext_flash_wake_up(void);
