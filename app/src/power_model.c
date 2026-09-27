#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#if defined(CONFIG_SOC_CC2340R5)
/* Same include style the SoC's own ccfg.c uses; the HAL puts these on the path. */
#include <inc/hw_memmap.h>
#include <inc/hw_pmud.h>
#include <inc/hw_types.h>
#define HAVE_BATTERY_MONITOR 1
#endif

#include "power_model.h"

/*
 * Two AA cells: 3.0-3.2 V fresh, and flat by about 2.0 V (1.0 V per cell). Both the SoC
 * (down to 1.71 V) and the BMI270 run across that whole range, so the cells are the
 * limit, not the electronics.
 *
 * Anything outside this window means the reading is not a battery - bench supply, debug
 * probe, or a monitor that has not converted yet - and is reported as unavailable rather
 * than as a wrong number.
 */
#define BATTERY_PLAUSIBLE_MIN_MV 1500U
#define BATTERY_PLAUSIBLE_MAX_MV 3800U

/*
 * Charge drawn per second in each state, in microamp-seconds. Ratios matter more than
 * absolute values: the radio dominates, a fix costs real energy, idle is nearly free.
 * Scaled so a demo device visibly discharges over an afternoon rather than a decade.
 */
#define DRAW_IDLE_UAS_PER_S   4U
#define DRAW_ACTIVE_UAS_PER_S 900U
#define DRAW_FIX_UAS_PER_S    2600U
#define DRAW_RADIO_UAS_PER_S  5400U

/* Modelled capacity, chosen so the above drains a demo unit at a watchable rate. */
#define CAPACITY_UAS 90000000ULL

static uint64_t charge_used_uas;

static uint32_t radio_on_ms_window;
static uint32_t window_start_ms;
static uint32_t radio_on_since_ms;
static bool radio_on;

static void draw(uint32_t ms, uint32_t uas_per_s)
{
	charge_used_uas += ((uint64_t)ms * uas_per_s) / 1000ULL;
}

void power_model_init(void)
{
	charge_used_uas = 0;
	radio_on_ms_window = 0;
	radio_on = false;
	window_start_ms = k_uptime_get_32();
}

void power_model_note_idle(uint32_t ms)
{
	draw(ms, DRAW_IDLE_UAS_PER_S);
}

void power_model_note_active(uint32_t ms)
{
	draw(ms, DRAW_ACTIVE_UAS_PER_S);
}

void power_model_note_fix(uint32_t ms)
{
	draw(ms, DRAW_FIX_UAS_PER_S);
}

void power_model_note_radio(bool on)
{
	uint32_t now = k_uptime_get_32();

	if (on && !radio_on) {
		radio_on_since_ms = now;
		radio_on = true;
	} else if (!on && radio_on) {
		uint32_t elapsed = now - radio_on_since_ms;

		radio_on_ms_window += elapsed;
		draw(elapsed, DRAW_RADIO_UAS_PER_S);
		radio_on = false;
	}
}

uint32_t power_model_battery_mv(void)
{
#ifdef HAVE_BATTERY_MONITOR
	/*
	 * PMUD.BAT holds the supply voltage as a 3-bit integer and 8-bit fractional part
	 * of a volt, kept current by the always-on battery monitor. Same arithmetic as
	 * TI's BatteryMonitor_getVoltage(), without pulling in the notification driver.
	 */
	uint32_t raw = HWREG(PMUD_BASE + PMUD_O_BAT) & (PMUD_BAT_INT_M | PMUD_BAT_FRAC_M);
	uint32_t mv = ((raw * 1000U) + (1U << (PMUD_BAT_INT_S - 1U))) >> PMUD_BAT_INT_S;

	if (mv < BATTERY_PLAUSIBLE_MIN_MV || mv > BATTERY_PLAUSIBLE_MAX_MV) {
		return 0U;
	}

	return mv;
#else
	return 0U;
#endif
}

uint8_t power_model_radio_on_pct(void)
{
	uint32_t now = k_uptime_get_32();
	uint32_t window_ms = now - window_start_ms;
	uint32_t on_ms = radio_on_ms_window;

	/* Include the part of an ongoing radio session that falls inside this window. */
	if (radio_on) {
		uint32_t since = (radio_on_since_ms > window_start_ms) ? radio_on_since_ms
								      : window_start_ms;
		on_ms += now - since;
	}

	if (window_ms == 0U) {
		return 0U;
	}

	if (on_ms > window_ms) {
		on_ms = window_ms;
	}

	return (uint8_t)((on_ms * 100U) / window_ms);
}

void power_model_window_reset(void)
{
	uint32_t now = k_uptime_get_32();

	/*
	 * Charge is cumulative, but the radio-on ratio is per window. If the radio is on
	 * right now, the next window starts with it already on, so bank what has elapsed
	 * and restart the clock from here.
	 */
	if (radio_on) {
		uint32_t since = (radio_on_since_ms > window_start_ms) ? radio_on_since_ms
								      : window_start_ms;
		draw(now - since, DRAW_RADIO_UAS_PER_S);
		radio_on_since_ms = now;
	}

	radio_on_ms_window = 0;
	window_start_ms = now;
}
