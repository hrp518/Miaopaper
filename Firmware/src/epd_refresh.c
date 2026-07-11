#include "epd_refresh.h"
#include "epd.h"
#include "flash.h"
#include "app_config.h"

static uint8_t since_full[EPD_RF_SCENE_COUNT];
static uint8_t force_next[EPD_RF_SCENE_COUNT];
static uint8_t since_full_global;

static uint8_t gc_interval(void)
{
	uint8_t idx = settings.epd_gc_interval_idx;
	if (idx >= EPD_GC_INTERVAL_COUNT)
		idx = 0;
	if (g_epd_gc_interval[idx] > 255)
		return 255;
	return (uint8_t)g_epd_gc_interval[idx];
}

void epd_refresh_scene_enter(epd_rf_scene_t scene)
{
	if (scene >= EPD_RF_SCENE_COUNT) return;
	force_next[scene] = 1;
	if (settings.epd_partial_enabled) {
		/* Global partial ON: keep base map + GC counter across scene changes. */
		return;
	}
	since_full[scene] = 0;
	epd_partial_ready = 0;
}

uint8_t epd_refresh_pick(epd_rf_scene_t scene, uint8_t allow_partial)
{
	if (scene >= EPD_RF_SCENE_COUNT) return 1;

	if (!epd_partial_ready) {
		if (force_next[scene])
			force_next[scene] = 0;
		return 1;
	}

	if (force_next[scene]) {
		force_next[scene] = 0;
		if (!settings.epd_partial_enabled) {
			since_full[scene] = 0;
			return 1;
		}
		/* Global partial ON: scene changes count as normal partial refreshes. */
	}

	if (settings.epd_partial_enabled) {
		since_full_global++;
		if (since_full_global >= gc_interval()) {
			since_full_global = 0;
			return 1;
		}
		return 0;
	}

	if (!allow_partial)
		return 1;

	since_full[scene]++;
	if (since_full[scene] >= gc_interval()) {
		since_full[scene] = 0;
		return 1;
	}
	return 0;
}
