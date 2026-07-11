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
