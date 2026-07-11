#include "charge_state.h"
#include "buttons.h"
#include "ble.h"
#include "epd.h"   // set_EPD_wait_flush() -- force a refresh on next main_loop

// Sentinel 0xFF means "no reading yet" so the first tick always emits.
static uint8_t last_charge_state = 0xFF;

void charge_state_tick(void)
{
	// PC1 (I2C SCL) shares its pin with the I2C peripheral.  btn_reconfig_gpio()
	// disables the I2C master but doesn't touch PC1's GPIO function, so a
	// silent peripheral state may leave the pad in an unknown mode that masks
	// the external charge-status signal.  Re-assert the high-Z GPIO mode on
	// every tick -- cheap, idempotent, and guarantees that is_charging()
	// always sees the real pad level (not whatever the I2C controller
	// happened to release).
	charge_status_init();

	uint8_t cur = is_charging();   // 0 = not charging (PC1 HIGH), 1 = charging (PC1 LOW)
	if (cur != last_charge_state) {
		last_charge_state = cur;
		ble_log(cur ? "CHG:1" : "CHG:0");

		// Charge-status just flipped.  The clock scene normally refreshes
		// once per minute, so without this nudge the "+" indicator on the
		// EPD would be invisible for up to ~59 s after plugging in or
		// unplugging.  Mark the EPD for an immediate full refresh on the
		// next main_loop pass.
		set_EPD_wait_flush();
	}
}