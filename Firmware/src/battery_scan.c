// Battery-level pin auto-detection sweep.
//
// See battery_scan.h for the background.  In short: BLE cmd 0xB2 drives a
// cursor through every ADC-capable source the firmware does not already
// use for something else, samples it once and reports the result over
// ble_log().  The host picks the pin that reads ~battery voltage and the
// production battery path can later be moved off the shared MOSI pin.
//
// Output line format (single ASCII line per push):
//   BC:idx=<n> src=<NAME> mv=<dddd> note=<tag>
// tags: ref (VBAT benchmark), ok (2.2-3.6V, battery-like), low (0.1-2.2V),
//       grounded (<0.1V), skip (guarded), end (one sweep finished).
// A complete sweep is wrapped with a leading "BC:START" and a trailing
// "BC:END wrap" so the host can frame logs.

#include <stdint.h>
#include <stdio.h>
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"

#include "battery_scan.h"
#include "battery.h"
#include "ble.h"
#include "epd.h"
#include "ext_flash.h"

// ---------------------------------------------------------------------------
// Candidate table.  Order chosen so the reference comes first, then the
// truly-free GPIO candidates, then the shared-line probes that need guards.
// ---------------------------------------------------------------------------
typedef enum {
    SRC_VBAT,   // on-chip VBAT direct channel (reference, no GPIO)
    SRC_PB0,
    SRC_PB2,
    SRC_PB3,
    SRC_PB5,
    SRC_PC5,
    SRC_PB6,    // FLASH_MISO  -- guarded by ext_flash_is_safe()
    SRC_PB7,    // EPD/FLASH MOSI  -- guarded by ext_flash_is_safe()
    SRC_COUNT,
} src_id_t;

static const char *const src_name[SRC_COUNT] = {
    "VBAT", "PB0", "PB2", "PB3", "PB5", "PC5", "PB6", "PB7",
};

// ADC_GPIO_tab order (adc.c): PB0,PB1,PB2,PB3,PB4,PB5,PB6,PB7,PC4,PC5
// -> pbX -> idx X, PC4 -> idx 8, PC5 -> idx 9.
static const GPIO_PinTypeDef src_pin[SRC_COUNT] = {
    0,         // SRC_VBAT: unused (on-chip channel)
    GPIO_PB0,
    GPIO_PB2,
    GPIO_PB3,
    GPIO_PB5,
    GPIO_PC5,
    GPIO_PB6,
    GPIO_PB7,
};

static uint8_t s_cursor = 0;   // next source to sample on a 0xB2 push

// Forward declarations: B9 VDD monitor and B8 ADC scan share PB0, so they
// need to disable each other.  Defined further down in this file.
uint8_t vdd_scan_is_enabled(void);
void vdd_scan_set_enabled(uint8_t en);

void battery_scan_reset(void)
{
    s_cursor = 0;
}

// Classify a millivolt reading into a short note tag the host can match on.
static const char *classify_mv(uint16_t mv, uint8_t is_ref)
{
    if (is_ref)
        return "ref";
    if (mv < 100)
        return "grounded";
    if (mv < 2200)
        return "low";
    if (mv <= 3600)
        return "ok";
    return "low";   // >3.6V is implausible for CR2032 -> treat as "low"/noise
}

// ---------------------------------------------------------------------------
// VBAT direct channel read.  Measures the chip's own supply (i.e. the
// battery that powers VDD) WITHOUT using any GPIO, so it works even when no
// free pin is wired to the battery.
//
// Correct configuration (the previous version used ADC_VREF_VBAT_N as the
// reference, which made it a dimensionless ratio and read 0):
//   - positive input = on-chip VBAT (enum VBAT = 0x0f)
//   - VBAT divider   = 1/3  (brings ~3V down into the 1.2V window)
//   - reference      = 1.2V internal bandgap (FIXED, not VBAT_N)
//   - pre-scale      = 1/8  (matches adc_sample_and_get_result's mv math)
// adc_sample_and_get_result() returns mV at its scaled input = VBAT/3, so
// the real cell voltage is 3x that.
// ---------------------------------------------------------------------------
static uint16_t read_vbat_direct(void)
{
    adc_init();
    adc_set_chn_enable_and_max_state_cnt(ADC_MISC_CHN, 2);
    adc_set_state_length(240, 0, 10);

    // 1/3 VBAT divider so VBAT (up to ~3.6V) fits inside the 1.2V window.
    adc_set_vref_vbat_divider(ADC_VBAT_DIVIDER_1F3);

    // Single-ended: positive = internal VBAT, negative = GND.
    adc_set_input_mode(ADC_MISC_CHN, SINGLE_ENDED_MODE);
    adc_set_ain_channel_single_ended_input_mode(ADC_MISC_CHN, VBAT);

    // FIXED 1.2V reference (NOT VBAT_N) -> absolute voltage measurement.
    adc_set_ref_voltage(ADC_MISC_CHN, ADC_VREF_1P2V);
    adc_set_resolution(ADC_MISC_CHN, RES14);
    adc_set_tsample_cycle(ADC_MISC_CHN, SAMPLING_CYCLES_6);
    adc_set_ain_pre_scaler(ADC_PRESCALER_1F8);           // 1/8 input scale
    adc_set_mode(ADC_NORMAL_MODE);

    adc_power_on_sar_adc(1);
    // Result is mV at the (1/3-divided, 1/8-scaled) input; the driver undoes
    // the 1/8 pre-scale, so what's left is VBAT/3 in mV. Multiply by 3.
    uint16_t vbat_over_3_mv = (uint16_t)adc_sample_and_get_result();
    adc_power_on_sar_adc(0);
    return (uint16_t)(vbat_over_3_mv * 3u);
}

// ---------------------------------------------------------------------------
// Generic single-ended GPIO read.  Reuses adc_base_init() (sets pin to
// high-Z input, single-ended vs GND, RES14, 1/8 pre-scale, 1.2V vref) and
// adc_sample_and_get_result() (returns mV).  Mirrors the existing
// get_battery_mv() path but parameterised by pin.
// ---------------------------------------------------------------------------
static uint16_t read_gpio_mv(GPIO_PinTypeDef pin)
{
    adc_init();
    adc_base_init(pin);
    adc_power_on_sar_adc(1);
    uint16_t mv = (uint16_t)adc_sample_and_get_result();
    adc_power_on_sar_adc(0);
    return mv;
}

void battery_scan_next(void)
{
    // Unconditional heartbeat: proves the 0xB2 route is wired up before we
    // touch any ADC/SPI state.  If you don't see this on the host, the bin
    // being flashed is not the rebuilt one (or 0xB2 isn't reaching here).
    ble_log("BC:ping");

    // Never race with the EPD/flash SPI bus.  If something is mid-refresh we
    // just complain and let the user retry once the panel settles.
    if (epd_update_state) {
        ble_log("BC:BUSY EPD");
        return;
    }

    if (s_cursor == 0)
        ble_log("BC:START");

    uint8_t idx = s_cursor;
    s_cursor = (s_cursor + 1u) % (uint8_t)SRC_COUNT;

    // Wrap marker: when idx wraps from last -> 0 of next sweep, announce it.
    // (Simpler approach below: emit end-of-sweep when current idx is the
    // last slot.)
    char line[48];

    // PB6/PB7 share the SPI bus with flash/EPD; only touch them when the
    // external-flash safety check passes.
    uint8_t needs_flash_guard =
        (idx == SRC_PB6) || (idx == SRC_PB7);
    if (needs_flash_guard && !ext_flash_is_safe()) {
        sprintf(line,
                "BC:idx=%u src=%s mv=---- note=skip flash",
                idx, src_name[idx]);
        ble_log(line);
        if (idx == (uint8_t)(SRC_COUNT - 1))
            ble_log("BC:END wrap");
        return;
    }

    uint16_t mv;
    if (idx == SRC_VBAT) {
        mv = read_vbat_direct();
        sprintf(line, "BC:idx=%u src=%s mv=%u note=%s",
                idx, src_name[idx], mv, classify_mv(mv, 1));
    } else {
        mv = read_gpio_mv(src_pin[idx]);
        sprintf(line, "BC:idx=%u src=%s mv=%u note=%s",
                idx, src_name[idx], mv, classify_mv(mv, 0));
    }
    ble_log(line);

    if (idx == (uint8_t)(SRC_COUNT - 1))
        ble_log("BC:END wrap");
}

// BLE 0xB5: read the battery on PB5 and push one line back to the host.
// Format: "BAT:mv=<dddd> lvl=<n>"  (mv = millivolts, lvl = 0..100 percent).
// PB5 sampling takes ~1ms and does not touch the shared SPI bus, so no
// EPD/flash guard is needed here.
void battery_read_and_notify(void)
{
    uint16_t mv = get_battery_mv();
    uint8_t  lvl = get_battery_level(mv);
    char line[32];
    sprintf(line, "BAT:mv=%u lvl=%u", mv, lvl);
    ble_log(line);
}

// ---------------------------------------------------------------------------
// One-shot full sweep (BLE 0xB6).  Probes ONLY the 5 ADC-capable pins that
// are not already used by another function on this board: PB0, PB2, PB3,
// PB5, PC5.  Pins wired to active functions (PB4=FRONT button,
// PB6=FLASH MISO, PB7=EPD/FLASH MOSI, PC4=LEFT button) are NEVER touched --
// reconfiguring them would corrupt the SPI bus / button reads.  (PB1 used
// to be listed here as EPD power, but that gate was dead code and has been
// removed; PB1 is sampled in the 0xB8 scan instead.)
// Each free pin is sampled N times, the samples are sorted and the median
// is reported to reject single-shot noise.
//
// Notes:
//  - Guarded by ext_flash_is_safe() + epd_update_state so we never touch a
//    shared SPI line mid-transfer.
//  - ADC is powered down after each pin so the 30s periodic sampler in
//    app.c re-initialises it cleanly on its next tick.
// ---------------------------------------------------------------------------

// Only the 5 ADC-capable pins that are NOT used by anything else on this
// board.  The other ADC-capable pins (PB4=FRONT button, PB6=FLASH MISO,
// PB7=EPD/FLASH MOSI, PC4=LEFT button) are already wired to active
// functions and must never be reconfigured by the ADC, so they are
// deliberately excluded from this sweep.  (PB1 is free now -- sampled by
// the 0xB8 scan -- but kept out of this historical 5-pin sweep.)
static const GPIO_PinTypeDef all_adc_pins[5] = {
    GPIO_PB0, GPIO_PB2, GPIO_PB3, GPIO_PB5, GPIO_PC5,
};
static const char *const all_adc_names[5] = {
    "PB0", "PB2", "PB3", "PB5", "PC5",
};

#define SCAN_SAMPLES 5

// Sample one pin SCAN_SAMPLES times, drop the highest and lowest sample
// (each is the most-likely SPI/EPD transient), then average the middle
// three.  Empirically this is more robust than median on this board
// because the bursts can shift 3 of 5 samples (e.g. 1802, 2802, [3802],
// 4802, 5802) -- a median returns 3802 here, but raw 3802 also happens to
// be the right answer, so median isn't really defending us; truncating
// the outer pair plus averaging the middle three is the same result but
// is more obviously correct when 4/5 are clustered close with one flyer.
//
// Returns mV.  Driving the ADC for a single read costs ~1 ms; the loop
// across 5 samples is ~5 ms per pin, still well inside a 1 s tick budget.
static uint16_t sample_pin_median(GPIO_PinTypeDef pin)
{
    uint16_t s[SCAN_SAMPLES];
    int i, j;
    for (i = 0; i < SCAN_SAMPLES; i++) {
        adc_init();
        adc_base_init(pin);
        adc_power_on_sar_adc(1);
        s[i] = (uint16_t)adc_sample_and_get_result();
        adc_power_on_sar_adc(0);
    }
    // Simple insertion sort (N is tiny).
    for (i = 1; i < SCAN_SAMPLES; i++) {
        uint16_t t = s[i];
        for (j = i; j > 0 && s[j - 1] > t; j--)
            s[j] = s[j - 1];
        s[j] = t;
    }
    // Truncated mean: drop min and max, average the rest.
    uint32_t sum = (uint32_t)s[1] + s[2] + s[3];
    return (uint16_t)(sum / 3);
}

void battery_scan_all(void)
{
    // Never race the EPD/flash SPI bus.
    if (epd_update_state || !ext_flash_is_safe()) {
        ble_log("BS:BUSY EPD/flash, retry");
        return;
    }

    ble_log("BS:START 5 free ADC pins (PB0/PB2/PB3/PB5/PC5), 5 samples each");

    int i;
    for (i = 0; i < 5; i++) {
        // Re-check the bus guard each iteration; if the EPD started a
        // refresh while we were sampling, bail out cleanly.
        if (epd_update_state) {
            ble_log("BS:ABORT EPD busy");
            return;
        }
        uint16_t mv = sample_pin_median(all_adc_pins[i]);
        char line[32];
        sprintf(line, "BS:%s mv=%u n=%s",
                all_adc_names[i], mv, classify_mv(mv, 0));
        ble_log(line);
    }

    ble_log("BS:END");
}

// ---------------------------------------------------------------------------
// Digital-level monitor (BLE 0xB7).  Inits EVERY free GPIO on the chip as a
// high-Z input and polls it every ~1s, notifying a line whenever any level
// changes.  Used to find the charge-detect pin: plug/unplug USB and watch
// which line flips.
//
// Pins already wired to active functions are NOT touched (reconfiguring them
// would break EPD/flash/buttons/LED): PA0 PA1 PA7, PB4 PB6 PB7, PC0 PC1
// PC4, PD2 PD3 PD4 PD7.  PE0..7 don't exist on this chip.  (PB1 used to be
// in this list as UART TX / EPD gate, but both are removed; it is kept out
// of this digital scan for now and is sampled as an ADC by the 0xB8 scan.)
// ---------------------------------------------------------------------------

// All free pins that are safe to init as inputs.
static const GPIO_PinTypeDef dg_pins[] = {
    GPIO_PA2, GPIO_PA3, GPIO_PA4, GPIO_PA5, GPIO_PA6,
    GPIO_PB0, GPIO_PB2, GPIO_PB3, GPIO_PB5,
    GPIO_PC2, GPIO_PC3, GPIO_PC5,
    GPIO_PD0, GPIO_PD1, GPIO_PD5, GPIO_PD6,
};
static const char *const dg_names[] = {
    "PA2", "PA3", "PA4", "PA5", "PA6",
    "PB0", "PB2", "PB3", "PB5",
    "PC2", "PC3", "PC5",
    "PD0", "PD1", "PD5", "PD6",
};
#define DG_COUNT (sizeof(dg_pins)/sizeof(dg_pins[0]))

static uint8_t dg_enabled = 0;
static uint8_t dg_initialized = 0;
static uint8_t dg_last[DG_COUNT];

void digital_scan_set_enabled(uint8_t en)
{
    dg_enabled = en ? 1 : 0;
    if (!dg_enabled) {
        ble_log("DG:OFF");
        return;
    }

    // Init every free pin as a high-Z digital input WITH a 1M pull-up.  With
    // a pull-up: a floating pin reads 1, a pin hard-tied to GND reads 0, and
    // a pin driven externally (e.g. a charge-detect line) follows its
    // driver.  This makes "0" meaningful (genuinely pulled low) instead of
    // the ambiguous all-zeros you get with no pull.
    unsigned int i;
    for (i = 0; i < DG_COUNT; i++) {
        gpio_set_func(dg_pins[i], AS_GPIO);
        gpio_set_output_en(dg_pins[i], 0);
        gpio_set_input_en(dg_pins[i], 1);
        gpio_setup_up_down_resistor(dg_pins[i], PM_PIN_PULLUP_1M);
    }
    // Seed last-state with a sentinel so the first tick emits a snapshot of
    // every pin's current level.
    for (i = 0; i < DG_COUNT; i++)
        dg_last[i] = 0xFF;
    dg_initialized = 1;
    ble_log("DG:ON all free pins");
}

uint8_t digital_scan_is_enabled(void)
{
    return dg_enabled;
}

void digital_scan_tick(void)
{
    if (!dg_enabled || !dg_initialized)
        return;
    if (!ble_get_connected())
        return;
    if (epd_update_state)
        return;

    // Print EVERY pin's level on every tick, split into two packets so each
    // stays well under the 20-byte notify MTU.  Half the pins per line:
    //   "DG1:PA2=0 PA3=1 ..."   "DG2:PC2=0 ... PD6=1"
    char line[64];
    int pos, max;
    unsigned int i, half;
    half = DG_COUNT / 2;   // first half on DG1, rest on DG2

    pos = 0; max = sizeof(line);
    { const char *p = "DG1:"; while (*p && pos < max-1) line[pos++]=*p++; }
    for (i = 0; i < DG_COUNT; i++) {
        if (i == half) break;
        if (i && pos < max-1) line[pos++]=' ';
        const char *n = dg_names[i];
        while (*n && pos < max-1) line[pos++]=*n++;
        if (pos < max-1) line[pos++]='=';
        if (pos < max-1) line[pos++]='0'+(gpio_read(dg_pins[i])?1:0);
    }
    line[pos]='\0';
    ble_log(line);

pos = 0;
	{ const char *p = "DG2:"; while (*p && pos < max-1) line[pos++]=*p++; }
	for (i = half; i < DG_COUNT; i++) {
		if (i > half && pos < max-1) line[pos++]=' ';
		const char *n = dg_names[i];
		while (*n && pos < max-1) line[pos++]=*n++;
		if (pos < max-1) line[pos++]='=';
		if (pos < max-1) line[pos++]='0'+(gpio_read(dg_pins[i])?1:0);
	}
	line[pos]='\0';
	ble_log(line);
}

// ---------------------------------------------------------------------------
// Continuous ADC scan (BLE 0xB8).
//
// Samples the 6 ADC-capable pins that have no other active role on this
// board and prints their voltage in MILLIVOLTS once per ~1s tick:
//   PB0  PB1  PB2  PB3  PB5  PC5
//
// Each pin is read through the SAME path as the production get_battery_mv()
// (adc_init + adc_base_init + power on + adc_sample_and_get_result + power
// off).  adc_sample_and_get_result() already averages the middle 4 of 8
// internal samples, so a single call is as stable as the production read --
// no extra re-init/median loop.  If PB5 reads sane here, the other pins do
// too.
//
// The other 4 ADC pins stay off-limits:
//   PB4  FRONT button (1M pull-up)     - sampling kills button detection
//   PC4  LEFT  button (1M pull-up)     - same
//   PB6  ext-Flash MISO                - sampling corrupts running flash reads
//   PB7  EPD/Flash MOSI (shared)       - sampling corrupts SPI byte stream
// ---------------------------------------------------------------------------

static const GPIO_PinTypeDef adc_scan_pins[6] = {
	GPIO_PB0, GPIO_PB1, GPIO_PB2, GPIO_PB3, GPIO_PB5, GPIO_PC5,
};
static const char *const adc_scan_names[6] = {
	"PB0", "PB1", "PB2", "PB3", "PB5", "PC5",
};
#define ADC_SCAN_COUNT 6

static uint8_t adc_scan_enabled = 0;

// One clean mV read of one pin.  Mirrors get_battery_mv() exactly so the
// behaviour is identical to the production battery path.
static uint16_t adc_read_pin_mv(GPIO_PinTypeDef pin)
{
	adc_init();
	adc_base_init(pin);
	adc_power_on_sar_adc(1);
	uint16_t mv = (uint16_t)adc_sample_and_get_result();
	adc_power_on_sar_adc(0);
	return mv;
}

void adc_scan_set_enabled(uint8_t en)
{
	adc_scan_enabled = en ? 1 : 0;
	if (adc_scan_enabled) {
		// B8 and B9 both use PB0 (B9 drives it high to measure VDD); they are
		// mutually exclusive.  Disable VDD monitor if it is running.
		if (vdd_scan_is_enabled()) {
			vdd_scan_set_enabled(0);
			ble_log("ADC:NOTE VDD monitor stopped (shares PB0)");
		}
		ble_log("ADC:ON PB0/PB1/PB2/PB3/PB5/PC5");
	} else {
		// Power ADC down so the 30s periodic sampler in app.c starts cleanly.
		adc_power_on_sar_adc(0);
		ble_log("ADC:OFF");
	}
}

uint8_t adc_scan_is_enabled(void)
{
	return adc_scan_enabled;
}

void adc_scan_tick(void)
{
	if (!adc_scan_enabled)
		return;
	if (!ble_get_connected())
		return;
	if (epd_update_state || !ext_flash_is_safe())
		return;   // skip this tick; sample again next second

	uint16_t v[ADC_SCAN_COUNT];
	int i;
	for (i = 0; i < ADC_SCAN_COUNT; i++) {
		if (epd_update_state || !ext_flash_is_safe())
			return;   // bus became busy mid-scan; skip this tick
		v[i] = adc_read_pin_mv(adc_scan_pins[i]);
	}

	// Three short notify lines, 2 pins each, well under the 20-byte BLE MTU.
	char line[40];
	sprintf(line, "ADC1:%s=%u %s=%u",
	        adc_scan_names[0], v[0], adc_scan_names[1], v[1]);
	ble_log(line);
	sprintf(line, "ADC2:%s=%u %s=%u",
	        adc_scan_names[2], v[2], adc_scan_names[3], v[3]);
	ble_log(line);
	sprintf(line, "ADC3:%s=%u %s=%u",
	        adc_scan_names[4], v[4], adc_scan_names[5], v[5]);
	ble_log(line);
}

// ---------------------------------------------------------------------------
// Battery / supply voltage probe (BLE 0xB9).
//
// The TLSR8258 has NO reliable on-chip channel that exposes VDD directly to
// the ADC -- the VBAT enum (positive input 0x0f) does not read the rail
// correctly (verified across several configs).  Telink's recommendation (from
// their battery-voltage sample code) is to drive a free ADC-capable GPIO HIGH
// and measure it: a driven-high pin sits at VDD, so sampling it returns the
// real supply voltage.
//
// Each tick:
//   PB0, PB1: driven HIGH before sampling through the standard Telink
//     battery path (differential vs GND, 1/8 pre-scale, 1.2 V ref,
//     RES14 with BIT(13) as sign bit).  Reading ~VDD means the pin is
//     floating; reading lower means an external divider is fighting the
//     driver.
//   PB5: sampled through the QUIRK configuration that mirrors the unknown
//     firmware's register dump: +AIN = B5P, -AIN = VBAT, 0xFA = 0xFD
//     (1/8 + 1.2V bias), 0xF9 = 0x10 (VBAT divider OFF, reserved BIT4=1).
//     The ADC measures V_PB5 - V_VBAT, NOT an absolute voltage.  This
//     lets us cross-check behaviour against that dump.
//
// Output, once per ~1s tick while enabled, three lines (millivolts each):
//   "PB0=<dddd>"  "PB1=<dddd>"  "PB5=<dddd>"
// 10 reads are averaged per pin.  Toggle with BLE 0xB9.  Mutually exclusive
// with B8 (ADC scan) since both drive these pins.
// ---------------------------------------------------------------------------

// PB0 is the free ADC pin used to "export" VDD for measurement.
#define VDD_DETECT_PIN  GPIO_PB0

static uint8_t vdd_scan_enabled = 0;

// Sample one differential positive input (vs GND) through the standard Telink
// battery path: RES14 differential (BIT13 = sign), 1/8 pre-scale, 1.2 V ref,
// 10 reads averaged.  If drive_pin != 0, that GPIO is driven HIGH first (so
// the pin voltage equals VDD -- the Telink method for measuring the supply),
// and released to high-Z afterwards.  Returns millivolts.
static uint16_t sample_diff_mv(ADC_InputPchTypeDef p_in, GPIO_PinTypeDef drive_pin)
{
	adc_power_on_sar_adc(0);
	adc_set_sample_clk(5);
	adc_set_left_right_gain_bias(GAIN_STAGE_BIAS_PER100, GAIN_STAGE_BIAS_PER100);
	adc_set_chn_enable_and_max_state_cnt(ADC_MISC_CHN, 2);
	adc_set_state_length(240, 0, 10);

	if (drive_pin) {
		// Telink advice: drive a GPIO with ADC function HIGH; its voltage
		// equals VBAT, then measure it.
		gpio_set_func(drive_pin, AS_GPIO);
		gpio_set_input_en(drive_pin, 0);
		gpio_set_output_en(drive_pin, 1);
		gpio_write(drive_pin, 1);
	}

	// Differential mode, P = p_in, N = GND.  RES14 with the differential
	// sign bit enabled (BIT(13) = sign).
	analog_write(anareg_adc_res_m, RES14 | FLD_ADC_EN_DIFF_CHN_M);
	adc_set_ain_chn_misc(p_in, GND);
	adc_set_ref_voltage(ADC_MISC_CHN, ADC_VREF_1P2V);
	adc_set_tsample_cycle_chn_misc(SAMPLING_CYCLES_6);
	adc_set_ain_pre_scaler(ADC_PRESCALER_1F8);   // 1/8 -> input fits 1.2 V window
	adc_power_on_sar_adc(1);

	// Average 10 reads; each adc_sample_and_get_result() internally takes 8
	// codes, drops the sign-bit ones, and averages the middle 4.
	uint32_t sum = 0;
	int n = 0;
	for (int k = 0; k < 10; k++) {
		sum += (uint16_t)adc_sample_and_get_result();
		n++;
	}
	// Leave ADC powered on (matches the unknown firmware behaviour where
	// 0xFC stays at 0xC5 across dumps).
	if (drive_pin)
		gpio_set_output_en(drive_pin, 0);   // release to high-Z
	return (uint16_t)(sum / n);
}

void vdd_scan_set_enabled(uint8_t en)
{
	vdd_scan_enabled = en ? 1 : 0;
	if (vdd_scan_enabled) {
		// B9 drives PB0/PB1/PB5 high; B8 also uses those pins, so they cannot
		// run together.  Stop the ADC scan if it is running.
		if (adc_scan_is_enabled()) {
			adc_scan_set_enabled(0);
			ble_log("VDD:NOTE ADC scan stopped (shares PB0/PB1/PB5)");
		}
		ble_log("VDD:ON PB0/PB1/PB5 all driven HIGH, 10x avg each");
	} else {
		adc_power_on_sar_adc(0);
		gpio_set_output_en(GPIO_PB0, 0);
		gpio_set_output_en(GPIO_PB1, 0);
		gpio_set_output_en(GPIO_PB5, 0);
		ble_log("VDD:OFF");
	}
}

uint8_t vdd_scan_is_enabled(void)
{
	return vdd_scan_enabled;
}

// PB5 read configured to match the unknown-firmware register dump verbatim.
// This is a quirk configuration: +AIN = B5P, -AIN = VBAT (i.e. differential
// of PB5 vs the chip's own VDD).  The result is V_PB5 - V_VBAT, not an
// absolute voltage.  We use it as-is to mirror what some external firmware
// does, so behaviour can be cross-checked against that dump.
static uint16_t sample_pb5_quirk_mv(void)
{
	adc_power_on_sar_adc(0);
	adc_set_sample_clk(5);
	adc_set_left_right_gain_bias(GAIN_STAGE_BIAS_PER100, GAIN_STAGE_BIAS_PER100);
	adc_set_chn_enable_and_max_state_cnt(ADC_MISC_CHN, 2);
	adc_set_state_length(240, 0, 10);
    gpio_setup_up_down_resistor(GPIO_PB5, 0);
	// Leave PB5 as-is (no drive, normal ADC input).  Skip gpio_setup_up_down
	// so the floating state from the user/board is preserved.

	// Differential mode, RES14 with the differential sign bit enabled
	// (BIT(13) = sign), matching the unknown-firmware dump.
	analog_write(anareg_adc_res_m, RES14 | FLD_ADC_EN_DIFF_CHN_M);

	// Quirk: +AIN = B5P (0x6), -AIN = VBAT (0xF).  This is NOT the SDK
	// standard path (which uses -AIN = GND).  It produces V_PB5 - V_VBAT.
	analog_write(areg_adc_ain_chn_misc, (B5P << 4) | VBAT);

	// VBAT divider kept OFF (BIT_RNG(2,3) = 0) but BIT(4) = 1 to match the
	// unknown dump byte 0x10.  This is a reserved bit, harmless but copied
	// verbatim so the configuration is identical.
	analog_write(areg_adc_vref_vbat_div, 0x10);

	adc_set_ref_voltage(ADC_MISC_CHN, ADC_VREF_1P2V);   // sets 0xFA bias too
	adc_set_tsample_cycle_chn_misc(SAMPLING_CYCLES_6);
	adc_set_ain_pre_scaler(ADC_PRESCALER_1F8);         // 1/8 -> 0xFA = 0xFD
	adc_power_on_sar_adc(1);

	// Average 10 reads.  Leave ADC powered on after sampling (matches the
	// unknown firmware behaviour where 0xFC stays at 0xC5 across dumps).
	uint32_t sum = 0;
	for (int k = 0; k < 10; k++)
		sum += (uint16_t)adc_sample_and_get_result();
	return (uint16_t)(sum / 10u);
}

void vdd_scan_tick(void)
{
	if (!vdd_scan_enabled)
		return;
	if (!ble_get_connected())
		return;
	if (epd_update_state)
		return;

	// Debug: confirm tick is entered.
	ble_log("VDD:TICK");

	// PB0 driven HIGH, then PB1 driven HIGH, each sampled through the standard
	// differential path.
	uint16_t pb0 = sample_diff_mv(B0P, GPIO_PB0);
	uint16_t pb1 = sample_diff_mv(B1P, GPIO_PB1);

	// Cross-coupling test: drive PB1 HIGH with maximum strength, then read PB5
	// to check if PB1 and PB5 are electrically connected on the board.
	gpio_set_func(GPIO_PB1, AS_GPIO);
	gpio_set_output_en(GPIO_PB1, 1);
	gpio_set_data_strength(GPIO_PB1, 1);   // max drive strength
	gpio_write(GPIO_PB1, 1);                // PB1 = HIGH
	sleep_us(10);                           // let settle
	uint16_t pb5_driven = sample_pb5_quirk_mv();
	gpio_set_output_en(GPIO_PB1, 0);        // release PB1

	char line[28];
	sprintf(line, "PB0=%u", pb0);
	ble_log(line);
	sprintf(line, "PB1=%u", pb1);
	ble_log(line);
		sprintf(line, "PB5_D=%u", pb5_driven);
		ble_log(line);
}

// ------------------------------------------
// 全局电池状态
// ------------------------------------------
uint16_t measured_batt_mv  = 0;   // 14串电池总电压 (mV)
uint8_t  measured_batt_soc = 0;   // 电量 0~100

// 每秒调用, 每 60 秒测量一次: 驱动 PB1 高, 测 PB5 差分,
// 单电池电压 = PB5_差分_mV × 2, 直接查表得 SOC.
// 开机后第一次 tick 立刻触发（cnt 初始=60）。
void battery_check_tick(void)
{
	static uint16_t cnt = 60;   // start at 60 so first call triggers immediately
	if (cnt < 60) { cnt++; return; }
	cnt = 1;                    // next measurement in 60 seconds

	// 驱动 PB1 = HIGH
	gpio_set_func(GPIO_PB1, AS_GPIO);
	gpio_set_output_en(GPIO_PB1, 1);
	gpio_set_data_strength(GPIO_PB1, 1);
	gpio_write(GPIO_PB1, 1);
	sleep_us(100);  // 等待稳定

	// 测 PB5 差分 (B5P - VBAT, 1/8预分频, RES14 差分)
	uint16_t pb5_mv = sample_pb5_quirk_mv();

	// 单电池电压 = PB5 × 2 (用户经验公式), 直接查 SOC 表（不用除14）
	uint32_t cell_mv = (uint32_t)pb5_mv * 2u;

	measured_batt_mv  = (uint16_t)cell_mv;
	measured_batt_soc = get_battery_soc_14s(cell_mv);

	// 拉低 PB1 减少发热
	gpio_write(GPIO_PB1, 0);
	gpio_set_output_en(GPIO_PB1, 0);
}