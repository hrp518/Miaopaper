#pragma once
#include <stdint.h>

/* EPD refresh planner:
 *   fp=1  full GC (0xF7)
 *   fp=0  partial (0xFF)
 * Scene enter -> full; else partial; GC every N partials per scene. */
typedef enum {
	EPD_RF_SCENE_CLOCK = 0,
	EPD_RF_SCENE_READ,
	EPD_RF_SCENE_MENU,
	EPD_RF_SCENE_COUNT
} epd_rf_scene_t;

void epd_refresh_scene_enter(epd_rf_scene_t scene);
uint8_t epd_refresh_pick(epd_rf_scene_t scene, uint8_t allow_partial);
