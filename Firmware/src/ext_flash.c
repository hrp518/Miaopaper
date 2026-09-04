#include <stdint.h>
#include "tl_common.h"
#include "drivers.h"
#include "ble.h"
#include "epd.h"
#include "ext_flash.h"

uint8_t ext_flash_is_safe(void)
{
    return (epd_update_state == 0) ? 1 : 0;
}

// 深度断电状态:powered_down=1 表示 Flash 处于 0xB9 深断电,任意 SPI 事务前
// 需先发 0xAB 唤醒。关键:必须放进 .retention_data —— 深度保留睡眠不清除,
// 否则断电后标志被清零,唤醒路径会误判"已醒"而跳过 0xAB,读取直接失败。
_attribute_data_retention_ static uint8_t flash_powered_down = 0;

void ext_flash_init(void)
{
    if (epd_update_state) return;

    gpio_set_func(FLASH_CS, AS_GPIO);
    gpio_set_output_en(FLASH_CS, 1);
    gpio_set_input_en(FLASH_CS, 0);
    gpio_write(FLASH_CS, 1);

    gpio_set_func(FLASH_CLK, AS_GPIO);
    gpio_set_output_en(FLASH_CLK, 1);
    gpio_write(FLASH_CLK, 1);

    gpio_set_func(FLASH_MOSI, AS_GPIO);
    gpio_set_output_en(FLASH_MOSI, 1);
    gpio_set_input_en(FLASH_MOSI, 0);

    gpio_set_func(FLASH_MISO, AS_GPIO);
    gpio_set_output_en(FLASH_MISO, 0);
    gpio_set_input_en(FLASH_MISO, 1);
}

// SPI Mode 0: data latched on rising edge of CLK
static void flash_spi_tx(uint8_t val)
{
    for (int i = 7; i >= 0; i--)
    {
        gpio_write(FLASH_CLK, 0);
        gpio_write(FLASH_MOSI, (val >> i) & 1);
        gpio_write(FLASH_CLK, 1);
    }
    gpio_write(FLASH_CLK, 0);
}

static uint8_t flash_spi_rx(void)
{
    uint8_t val = 0;
    for (int i = 7; i >= 0; i--)
    {
        gpio_write(FLASH_CLK, 0);
        gpio_write(FLASH_CLK, 1);
        val <<= 1;
        if (gpio_read(FLASH_MISO)) val |= 1;
    }
    gpio_write(FLASH_CLK, 0);
    return val;
}

// 发一条 0xAB 释放深度断电并等待唤醒完成。(NOR 唤醒延迟 ~ tens of us)
void ext_flash_wake_up(void)
{
    if (!flash_powered_down)
        return;
    gpio_write(FLASH_CS, 0);
    flash_spi_tx(0xAB);
    gpio_write(FLASH_CS, 1);
    flash_powered_down = 0;
    WaitUs(60);   // 深度断电唤醒时间 (tRES1),留足裕量再访问
}

// 进入深度断电(0xB9):休眠阶段把 Flash 从 standby(数十 uA)降到 ~1-2 uA。
// 幂等:已在深断电则直接返回。下次任意 Flash 访问会自动 0xAB 唤醒。
void ext_flash_deep_power_down(void)
{
    if (flash_powered_down)
        return;
    // 深度保留睡眠不保留 GPIO 配置,先 ext_flash_init 确保 SPI 引脚有效
    ext_flash_init();
    gpio_write(FLASH_CS, 0);
    flash_spi_tx(0xB9);
    gpio_write(FLASH_CS, 1);
    flash_powered_down = 1;
}

void ext_flash_read_jedec_id(uint8_t buf[3])
{
    ext_flash_wake_up();
    gpio_write(FLASH_CS, 0);
    flash_spi_tx(0x9F);
    buf[0] = flash_spi_rx();
    buf[1] = flash_spi_rx();
    buf[2] = flash_spi_rx();
    gpio_write(FLASH_CS, 1);
}

uint8_t ext_flash_read_status(void)
{
    uint8_t status;
    ext_flash_wake_up();
    gpio_write(FLASH_CS, 0);
    flash_spi_tx(0x05);
    status = flash_spi_rx();
    gpio_write(FLASH_CS, 1);
    return status;
}

void ext_flash_wait_ready(void)
{
    int timeout = 100000;
    while ((ext_flash_read_status() & 0x01) && --timeout)
        ;
}

void ext_flash_write_enable(void)
{
    ext_flash_wake_up();
    gpio_write(FLASH_CS, 0);
    flash_spi_tx(0x06);
    gpio_write(FLASH_CS, 1);
}

void ext_flash_read(uint32_t addr, uint16_t len, uint8_t *buf)
{
    ext_flash_wake_up();
    gpio_write(FLASH_CS, 0);
    flash_spi_tx(0x03);
    flash_spi_tx((addr >> 16) & 0xFF);
    flash_spi_tx((addr >> 8) & 0xFF);
    flash_spi_tx(addr & 0xFF);
    for (uint16_t i = 0; i < len; i++)
        buf[i] = flash_spi_rx();
    gpio_write(FLASH_CS, 1);
}

void ext_flash_page_program(uint32_t addr, uint16_t len, const uint8_t *data)
{
    // NOR flash Page Program (0x02) wraps within a 256-byte page: bytes that
    // cross a 256-byte boundary wrap back to the start of the same page and
    // corrupt the beginning instead of continuing.  Split the write so that
    // every SPI transaction stays inside one 256-byte page.  This fixes the
    // lock-image upload corruption (200-byte chunks straddling page edges).
    uint32_t offset = 0;
    while (offset < len)
    {
        uint32_t cur = addr + offset;
        uint16_t page_remain = 256 - (uint16_t)(cur & 0xFF);  // bytes left in this page
        uint16_t remain = len - offset;
        uint16_t chunk = (remain < page_remain) ? remain : page_remain;

        ext_flash_write_enable();
        gpio_write(FLASH_CS, 0);
        flash_spi_tx(0x02);
        flash_spi_tx((cur >> 16) & 0xFF);
        flash_spi_tx((cur >> 8) & 0xFF);
        flash_spi_tx(cur & 0xFF);
        for (uint16_t i = 0; i < chunk; i++)
            flash_spi_tx(data[offset + i]);
        gpio_write(FLASH_CS, 1);
        ext_flash_wait_ready();

        offset += chunk;
    }
}

void ext_flash_sector_erase(uint32_t addr)
{
    ext_flash_write_enable();
    gpio_write(FLASH_CS, 0);
    flash_spi_tx(0x20);
    flash_spi_tx((addr >> 16) & 0xFF);
    flash_spi_tx((addr >> 8) & 0xFF);
    flash_spi_tx(addr & 0xFF);
    gpio_write(FLASH_CS, 1);
    ext_flash_wait_ready();
}

void ext_flash_block_erase_64k(uint32_t addr)
{
    ext_flash_write_enable();
    gpio_write(FLASH_CS, 0);
    flash_spi_tx(0xD8);
    flash_spi_tx((addr >> 16) & 0xFF);
    flash_spi_tx((addr >> 8) & 0xFF);
    flash_spi_tx(addr & 0xFF);
    gpio_write(FLASH_CS, 1);
    ext_flash_wait_ready();
}
