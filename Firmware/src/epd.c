#include <stdint.h>
#include "etime.h"
#include "tl_common.h"
#include "main.h"
#include "epd.h"
#include "epd_spi.h"
#include "epd_bw_213.h"
#include "drivers.h"
#include "stack/ble/ble.h"

#include "battery.h"
#include "battery_scan.h"
#include "flash.h"
#include "ble.h"
#include "ebook.h"
#include "epd_refresh.h"
#include "flash.h"
#include "buttons.h"   // is_charging() / charge_status for clock display

#include "OneBitDisplay.h"
#include "TIFF_G4.h"
extern const uint8_t ucMirror[];
#include "font_60.h"
#include "font16.h"
#include "font16zh.h"
#include "font30.h"

RAM uint8_t epd_model = 0; // 0 = Undetected, 1 = BW213, 2 = BWR213, 3 = BWR154, 4 = BW213ICE, 5 BWR296
const char *epd_model_string[] = {"NC", "BW213", "BWR213", "BWR154", "213ICE", "BWR296"};
RAM uint8_t epd_update_state = 0;
RAM uint8_t epd_partial_ready = 0;  // 0 = need full refresh, 1 = base map established, can use partial

RAM uint8_t epd_scene = 2;
RAM uint8_t epd_wait_update = 0;

RAM uint8_t minute_refresh = 100;

const char *BLE_conn_string[] = {"BLE 0", "BLE 1"};
RAM uint8_t epd_temperature_is_read = 0;
RAM uint8_t epd_temperature = 0;

RAM uint8_t epd_buffer[epd_buffer_size];
RAM uint8_t epd_temp[epd_buffer_size]; // for OneBitDisplay to draw into
OBDISP obd;                        // virtual display structure
TIFFIMAGE tiff;

// Force set an EPD model, report status via BLE, save to flash and reboot
void set_EPD_model(uint8_t model_nr)
{
    char buff[64];

    // Report current EPD state
    sprintf(buff, "EPD current: %s(%d)", epd_model_string[epd_model], epd_model);
    ble_log(buff);

    // Report the new model being set
    if (model_nr <= 5) {
        sprintf(buff, "EPD change to: %s(%d)", epd_model_string[model_nr], model_nr);
    } else {
        sprintf(buff, "EPD change to: UNKNOWN(%d)", model_nr);
    }
    ble_log(buff);

    // Set new model
    epd_model = model_nr;
    settings.epd_model = model_nr;

    // Save settings to flash
    save_settings_to_flash();
    ble_log("Settings saved");

    // Report reinitializing
    ble_log("Rebooting...");

    // Wait for BLE notifications to be sent
    sleep_ms(200);

    // Reboot the device
    irq_disable();
    REG_ADDR8(0x6f) = 0x20;
    while (1);
}

// With this we can force a display if it wasnt detected correctly
void set_EPD_scene(uint8_t scene)
{
    epd_scene = scene;
    set_EPD_wait_flush();
}

void set_EPD_wait_flush() {
    epd_wait_update = 1;
    epd_refresh_scene_enter(EPD_RF_SCENE_CLOCK);
    if (!settings.epd_partial_enabled)
        epd_partial_ready = 0;
}



// Here we detect what E-Paper display is connected
_attribute_ram_code_ void EPD_detect_model(void)
{
    // If a model was forced and saved in flash, use it
    if (settings.epd_model != 0) {
        epd_model = settings.epd_model;
        char buff[32];
        sprintf(buff, "EPD from flash: %s", epd_model_string[epd_model]);
        ble_log(buff);
        return;
    }

    EPD_init();
    // system power
    EPD_POWER_ON();

    WaitMs(10);
    // Reset the EPD driver IC
    gpio_write(EPD_RESET, 0);
    WaitMs(10);
    gpio_write(EPD_RESET, 1);
    WaitMs(10);

    // Force BW213
    epd_model = 1;
    ble_log("EPD model: BW213");
    EPD_POWER_OFF();
}

_attribute_ram_code_ uint8_t EPD_read_temp(void)
{
    if (epd_temperature_is_read)
        return epd_temperature;

    if (!epd_model)
        EPD_detect_model();

    EPD_init();
    // system power
    EPD_POWER_ON();
    WaitMs(5);
    // Reset the EPD driver IC
    gpio_write(EPD_RESET, 0);
    WaitMs(10);
    gpio_write(EPD_RESET, 1);
    WaitMs(10);

    // Only BW213 (model=1) is actually used on this device.
    // Other models removed to save flash space.
    if (epd_model >= 1)
        epd_temperature = EPD_BW_213_read_temp();

    EPD_POWER_OFF();

    epd_temperature_is_read = 1;

    return epd_temperature;
}

_attribute_ram_code_ void EPD_Display(unsigned char *image, unsigned char *red_image, int size, uint8_t full_or_partial)
{
    char _dbuff[64];

    if (!epd_model)
        EPD_detect_model();

    sprintf(_dbuff, "EPD_Display: model=%d size=%d fp=%d", epd_model, size, full_or_partial);
    ble_log(_dbuff);
    sleep_ms(15);

    EPD_init();
    EPD_POWER_ON();
    WaitMs(5);
    /* Do NOT pulse RESET here.  Each refresh mode in EPD_BW_213_Display
     * runs its own init (HW/Fast/WakeOnly).  A blanket GPIO reset before
     * partial refresh wiped the 0x26 base map and shifted the image ~1px. */

    ble_log("EPD Display: calling model driver...");
    sleep_ms(15);

    // Only BW213 (model=1) is actually used on this device.
    // Other models removed to save flash space.
    if (epd_model >= 1)
        epd_temperature = EPD_BW_213_Display(image, size, full_or_partial);

    sprintf(_dbuff, "EPD Display: done temp=%d", epd_temperature);
    ble_log(_dbuff);
    sleep_ms(15);

    epd_temperature_is_read = 1;
    epd_update_state = 1;
}

_attribute_ram_code_ void epd_set_sleep(void)
{
    if (!epd_model)
        EPD_detect_model();

    if (epd_model >= 1)
        EPD_BW_213_set_sleep();

    EPD_POWER_OFF();
    epd_update_state = 0;
}

_attribute_ram_code_ uint8_t epd_state_handler(void)
{
    switch (epd_update_state)
    {
    case 0:
        // Nothing todo
        break;
    case 1: // check if refresh is done and sleep epd if so
        if (epd_model == 1)
        {
            if (!EPD_IS_BUSY())
                epd_set_sleep();
        }
        else
        {
            if (EPD_IS_BUSY())
                epd_set_sleep();
        }
        break;
    }
    return epd_update_state;
}

_attribute_ram_code_ void FixBuffer(uint8_t *pSrc, uint8_t *pDst, uint16_t width, uint16_t height)
{
    int x, y;
    uint8_t *s, *d;
    for (y = 0; y < (height / 8); y++)
    { // byte rows
        d = &pDst[y];
        s = &pSrc[y * width];
        for (x = 0; x < width; x++)
        {
            int src_x = epd_byte_flip ? (width - 1 - x) : x;
            d[x * (height / 8)] = ~ucMirror[s[src_x]]; // invert + bit-mirror, optional byte reverse
        }                                                      // for x
    }                                                          // for y
}

_attribute_ram_code_ void TIFFDraw(TIFFDRAW *pDraw)
{
    uint8_t uc = 0, ucSrcMask, ucDstMask, *s, *d;
    int x, y;

    s = pDraw->pPixels;
    y = pDraw->y;                          // current line
    d = &epd_buffer[(249 * 16) + (y / 8)]; // rotated 90 deg clockwise
    ucDstMask = 0x80 >> (y & 7);           // destination mask
    ucSrcMask = 0;                         // src mask
    for (x = 0; x < pDraw->iWidth; x++)
    {
        // Slower to draw this way, but it allows us to use a single buffer
        // instead of drawing and then converting the pixels to be the EPD format
        if (ucSrcMask == 0)
        { // load next source byte
            ucSrcMask = 0x80;
            uc = *s++;
        }
        if (!(uc & ucSrcMask))
        { // black pixel
            d[-(x * 16)] &= ~ucDstMask;
        }
        ucSrcMask >>= 1;
    }
}

_attribute_ram_code_ void epd_display_tiff(uint8_t *pData, int iSize)
{
    // test G4 decoder
    epd_clear();
    TIFF_openRAW(&tiff, 250, 122, BITDIR_MSB_FIRST, pData, iSize, TIFFDraw);
    TIFF_setDrawParameters(&tiff, 65536, TIFF_PIXEL_1BPP, 0, 0, 250, 122, NULL);
    TIFF_decode(&tiff);
    TIFF_close(&tiff);
    EPD_Display(epd_buffer, NULL, epd_buffer_size, 1);
}

extern uint8_t mac_public[6];
_attribute_ram_code_ void epd_display(struct date_time _time, uint16_t battery_mv, int16_t temperature, uint8_t full_or_partial)
{
    uint8_t battery_level;

    if (epd_update_state)
        return;

    if (!epd_model)
    {
        EPD_detect_model();
    }
    uint16_t resolution_w = 250;
    uint16_t resolution_h = 128; // 122 real pixel, but needed to have a full byte
    if (epd_model == 1)
    {
        resolution_w = 250;
        resolution_h = 128; // 122 real pixel, but needed to have a full byte
    }
    else if (epd_model == 2)
    {
        resolution_w = 250;
        resolution_h = 128; // 122 real pixel, but needed to have a full byte
    }
    else if (epd_model == 3)
    {
        resolution_w = 200;
        resolution_h = 200;
    }
    else if (epd_model == 4)
    {
        resolution_w = 212;
        resolution_h = 104;
    }
    else if (epd_model == 5)
    {
        resolution_w = 296;
        resolution_h = 128;
    }

    epd_clear();

    obdCreateVirtualDisplay(&obd, resolution_w, resolution_h, epd_temp);
    obdFill(&obd, 0, 0); // fill with white

    char buff[100];
    battery_level = get_battery_level(battery_mv);
    // Top status line (baseline EB_BAR_TEXT_Y -> glyph top ~10, visible region).
    sprintf(buff, "MPP_%02X%02X%02X %s", mac_public[2], mac_public[1], mac_public[0], epd_model_string[epd_model]);
    obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 1, EB_BAR_TEXT_Y, (char *)buff, 1);
    sprintf(buff, "%s", BLE_conn_string[ble_get_connected()]);
    obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 200, EB_BAR_TEXT_Y, (char *)buff, 1);
    obdRectangle(&obd, 0, EB_DIVIDER_Y1, resolution_w - 1, EB_DIVIDER_Y2, 1, 1);
    // Big clock - DSEG14 is 5 glyphs * 34 = 170px wide, center in 250 -> x=40.
    sprintf(buff, "%02d:%02d", _time.tm_hour, _time.tm_min);
    obdWriteStringCustom(&obd, (GFXfont *)&DSEG14_Classic_Mini_Regular_40,
                         EB_CLOCK_TIME_X, EB_CLOCK_TIME_Y, (char *)buff, 1);
    obdRectangle(&obd, 0, EB_CLOCK_DIV_MID_Y1, resolution_w - 1, EB_CLOCK_DIV_MID_Y2, 1, 1);
    // Bottom info line (temperature removed: panel has no real sensor).
    if (is_charging())
        sprintf(buff, "%dmV  BAT %d%% +", battery_mv, battery_level);
    else
        sprintf(buff, "%dmV  BAT %d%%", battery_mv, battery_level);
    obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 8, 110, (char *)buff, 1);
    FixBuffer(epd_temp, epd_buffer, resolution_w, resolution_h);
    EPD_Display(epd_buffer, NULL, resolution_w * resolution_h / 8, full_or_partial);
}

_attribute_ram_code_ void epd_display_char(uint8_t data)
{
    int i;
    for (i = 0; i < epd_buffer_size; i++)
    {
        epd_buffer[i] = data;
    }
    EPD_Display(epd_buffer, NULL, epd_buffer_size, 1);
}

_attribute_ram_code_ void epd_clear(void)
{
    memset(epd_buffer, 0x00, epd_buffer_size);
    memset(epd_temp, 0x00, epd_buffer_size);
}
void update_time_scene(struct date_time _time, uint16_t battery_mv, int16_t temperature, void (*scene)(struct date_time, uint16_t, int16_t,  uint8_t)) {
    // default scene: show default time, battery, ble address, temperature
    if (epd_update_state)
        return;

    if (!epd_model)
    {
        EPD_detect_model();
    }

    if (epd_wait_update) {
        uint8_t mode = epd_refresh_pick(EPD_RF_SCENE_CLOCK, 1);
        scene(_time, battery_mv, temperature, mode);
        epd_wait_update = 0;
        epd_partial_ready = 1;
    }

    else if (_time.tm_min != minute_refresh)
    {
        minute_refresh = _time.tm_min;
        {
            uint8_t mode = epd_refresh_pick(EPD_RF_SCENE_CLOCK, 1);
            scene(_time, battery_mv, temperature, mode);
            epd_partial_ready = 1;
        }
    }
}

void epd_update(struct date_time _time, uint16_t battery_mv, int16_t temperature) {
    switch(epd_scene) {
        case 1:
            update_time_scene(_time, battery_mv, temperature, epd_display);
            break;
        case 2:
            update_time_scene(_time, battery_mv, temperature, epd_display_time_with_date);
            break;
        default:
            break;
    }
}

void epd_display_time_with_date(struct date_time _time, uint16_t battery_mv, int16_t temperature, uint8_t full_or_partial) {
    uint16_t battery_level;
    uint16_t resolution_w = 250;
    uint16_t resolution_h = 128;
    if (epd_model == 1) {
        resolution_w = 250;
        resolution_h = 128;
    } else if (epd_model == 2) {
        resolution_w = 250;
        resolution_h = 128;
    } else if (epd_model == 5) {
        resolution_w = 296;
        resolution_h = 128;
    }

    epd_clear();

    obdCreateVirtualDisplay(&obd, resolution_w, resolution_h, epd_temp);
    obdFill(&obd, 0, 0); // fill with white

    char buff[100];

    // ====== Layout reflowed for 2.13" (250 wide x ~122 usable rows) ======
    // The original scene-2 design was authored for a 296-wide (2.66") panel:
    // its right info column (x=216..295), battery icon and every full-width
    // divider (x2=295) were silently dropped on the 250-wide panel, leaving
    // the top/bottom frames missing.  Everything is now kept inside 0..249.
    //
    // IMPORTANT: only rows y=6..~121 are physically visible (top 6 rows are
    // hidden). obdWriteStringCustom treats y as the BASELINE; glyphs extend
    // ~12px upward for Dialog_plain_16, so the top text line must use
    // baseline >= 18 to keep glyph tops at/above y=6.

    // Top status line (baseline EB_BAR_TEXT_Y -> glyph top ~10, inside the visible region).
    sprintf(buff, "MPP_%02X%02X%02X", mac_public[2], mac_public[1], mac_public[0]);
    obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 1, EB_BAR_TEXT_Y, (char *)buff, 1);
	    if (is_charging())
	        sprintf(buff, "%d%% +", measured_batt_soc);
	    else
	        sprintf(buff, "%d%%", measured_batt_soc);
	    obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 148, EB_BAR_TEXT_Y, (char *)buff, 1);

    obdRectangle(&obd, 0, EB_DIVIDER_Y1, resolution_w - 1, EB_DIVIDER_Y2, 1, 1);

    sprintf(buff, "%02d:%02d", _time.tm_hour, _time.tm_min);
    obdWriteStringCustom(&obd, (GFXfont *)&DSEG14_Classic_Mini_Regular_40,
                         EB_CLOCK_TIME_X, EB_CLOCK_TIME_Y, (char *)buff, 1);

    obdRectangle(&obd, 0, EB_CLOCK_DIV_MID_Y1, resolution_w - 1, EB_CLOCK_DIV_MID_Y2, 1, 1);

	    if (measured_batt_mv > 0)
	        sprintf(buff, "%umV  %s", measured_batt_mv, BLE_conn_string[ble_get_connected()]);
	    else
	        sprintf(buff, "--mV  %s", BLE_conn_string[ble_get_connected()]);
	    obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 4, EB_CLOCK_INFO_Y, (char *)buff, 1);

    obdRectangle(&obd, 0, EB_CLOCK_DIV_BOT_Y1, resolution_w - 1, EB_CLOCK_DIV_BOT_Y2, 1, 1);

    sprintf(buff, "%d-%02d-%02d", _time.tm_year, _time.tm_month, _time.tm_day);
    obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 4, EB_CLOCK_DATE_Y, (char *)buff, 1);

    if (_time.tm_week == 7) {
        sprintf(buff, "9:%c", _time.tm_week + 0x20 + 6);
    } else {
        sprintf(buff, "9:%c", _time.tm_week + 0x20);
    }
    obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16_zh, 120, EB_CLOCK_WEEK_Y, (char *)buff, 1);

    obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16_zh, 200, EB_CLOCK_WEEK_Y, (char *)"", 1);

    FixBuffer(epd_temp, epd_buffer, resolution_w, resolution_h);

    EPD_Display(epd_buffer, NULL, resolution_w * resolution_h / 8, full_or_partial);
}

// EPD debug init - step by step with BLE logging
void epd_debug_init(void)
{
    char buff[64];

    ble_log("=== EPD Debug Start ===");
    sleep_ms(50);

    // Print current pin layout (compile-time constants)
    sprintf(buff, "Pin: RST=0x%02X DC=0x%02X BUSY=0x%02X",
            (unsigned)EPD_RESET, (unsigned)EPD_DC, (unsigned)EPD_BUSY);
    ble_log(buff);
    sleep_ms(50);
    sprintf(buff, "Pin: CS=0x%02X CLK=0x%02X MOSI=0x%02X",
            (unsigned)EPD_CS, (unsigned)EPD_CLK, (unsigned)EPD_MOSI);
    ble_log(buff);
    sleep_ms(50);

    // Step 1: Read GPIO states before init
    sprintf(buff, "Pre: RST=%d DC=%d CS=%d CLK=%d MOSI=%d",
            gpio_read(EPD_RESET), gpio_read(EPD_DC), gpio_read(EPD_CS),
            gpio_read(EPD_CLK), gpio_read(EPD_MOSI));
    ble_log(buff);
    sleep_ms(50);

    sprintf(buff, "Pre: BUSY=%d",
            gpio_read(EPD_BUSY));
    ble_log(buff);
    sleep_ms(50);

    // Step 2: Init GPIOs
    EPD_init();
    ble_log("GPIO init done");
    sleep_ms(50);

    // Step 3: Read GPIO states after init
    sprintf(buff, "Post: RST=%d DC=%d CS=%d CLK=%d MOSI=%d",
            gpio_read(EPD_RESET), gpio_read(EPD_DC), gpio_read(EPD_CS),
            gpio_read(EPD_CLK), gpio_read(EPD_MOSI));
    ble_log(buff);
    sleep_ms(50);

    sprintf(buff, "Post: BUSY=%d",
            gpio_read(EPD_BUSY));
    ble_log(buff);
    sleep_ms(50);

    // Step 4: Power on
    EPD_POWER_ON();
    sleep_ms(100);
    sprintf(buff, "PWR ON: BUSY=%d",
            gpio_read(EPD_BUSY));
    ble_log(buff);
    sleep_ms(50);

    // Step 5: Hardware reset
    ble_log("HW reset start");
    gpio_write(EPD_RESET, 0);
    WaitMs(20);
    gpio_write(EPD_RESET, 1);
    WaitMs(100);

    sprintf(buff, "HW reset: BUSY=%d", gpio_read(EPD_BUSY));
    ble_log(buff);
    sleep_ms(50);

    // Step 6: Send SW reset command (0x12)
    ble_log("Send SWRST 0x12");
    EPD_WriteCmd(0x12);
    WaitMs(200);
    sprintf(buff, "SWRST: BUSY=%d", gpio_read(EPD_BUSY));
    ble_log(buff);
    sleep_ms(50);

    // Step 7: Try read display status via SPI
    ble_log("SPI read test...");
    EPD_WriteCmd(0x0F); // Get Status (SSD1680)
    WaitMs(10);
    uint8_t spi_val = EPD_SPI_read();
    sprintf(buff, "SPI read: 0x%02X", spi_val);
    ble_log(buff);
    sleep_ms(50);

    // Step 8: Send Driver Output Control (0x01) and read back
    EPD_WriteCmd(0x2F); // Read RAM (SSD1680)
    WaitMs(10);
    uint8_t spi_val2 = EPD_SPI_read();
    sprintf(buff, "SPI read2: 0x%02X", spi_val2);
    ble_log(buff);
    sleep_ms(50);

    ble_log("=== EPD Debug End ===");

    EPD_POWER_OFF();
}
