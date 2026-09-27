/*
 * Copyright (c) 2026 Spotflow s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "boot_info.h"
#include "diag_metrics.h"
#include "shake_detect.h"
#include "faults.h"
#include "geo_sim.h"
#include "link_monitor.h"
#include "power_model.h"
#include "sensor.h"
#include "tracker.h"

LOG_MODULE_REGISTER(app_tracker, CONFIG_APP_TRACKER_LOG_LEVEL);

/*
 * The duty cycle sleeps in slices rather than one long sleep, and every slice takes an
 * accelerometer sample. That is what makes drop detection possible at all: a fall lasts
 * a few hundred milliseconds and the impact a few, while the reporting cycle is ten
 * seconds. Sampling and reporting are deliberately different rates.
 *
 * It also means a button press is acted on within a slice rather than at the end of a
 * cycle - on stage, the difference between "press and it happens" and "press and wait".
 */
#define SLICE_MS CONFIG_APP_SHAKE_SAMPLE_INTERVAL_MS

/* Report a device that has been failing this many reads in a row as good as gone. */
#define STREAK_ALARM 5U

/*
 * ...and every this many failures, try to bring it back. At a ten-second cycle that is
 * roughly every two minutes: often enough that a sensor which returns - a warm reset
 * that wedged the I2C bus, a module reseated at the booth - is picked up while someone
 * is still watching, rare enough that a genuinely dead part is not re-probed constantly.
 */
#define STREAK_REPROBE 12U

#define LED_HEALTHY_NODE DT_ALIAS(led0)
#define LED_DEGRADED_NODE DT_ALIAS(led1)

static const struct gpio_dt_spec led_healthy =
	GPIO_DT_SPEC_GET_OR(LED_HEALTHY_NODE, gpios, { 0 });
static const struct gpio_dt_spec led_degraded =
	GPIO_DT_SPEC_GET_OR(LED_DEGRADED_NODE, gpios, { 0 });

#ifdef CONFIG_APP_WATCHDOG
static const struct device *const wdt = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));
static int wdt_channel = -1;
#endif

static void sample_for_shakes(void);

static uint32_t sensor_error_streak;
static uint32_t cycle_count;

static void leds_init(void)
{
	if (gpio_is_ready_dt(&led_healthy)) {
		(void)gpio_pin_configure_dt(&led_healthy, GPIO_OUTPUT_INACTIVE);
	}

	if (gpio_is_ready_dt(&led_degraded)) {
		(void)gpio_pin_configure_dt(&led_degraded, GPIO_OUTPUT_INACTIVE);
	}
}

/*
 * The booth needs to know at a glance what the device thinks of itself: green while the
 * sensor bus is behaving, red once it is not.
 */
static void leds_update(void)
{
	bool degraded = sensor_is_degraded();

	if (gpio_is_ready_dt(&led_healthy)) {
		(void)gpio_pin_set_dt(&led_healthy, degraded ? 0 : 1);
	}

	if (gpio_is_ready_dt(&led_degraded)) {
		(void)gpio_pin_set_dt(&led_degraded, degraded ? 1 : 0);
	}
}

#ifdef CONFIG_APP_WATCHDOG
static void watchdog_init(void)
{
	struct wdt_timeout_cfg cfg = {
		.window.min = 0U,
		.window.max = CONFIG_APP_WATCHDOG_TIMEOUT_MS,
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};

	if (wdt == NULL || !device_is_ready(wdt)) {
		LOG_WRN("no watchdog available");
		return;
	}

	wdt_channel = wdt_install_timeout(wdt, &cfg);
	if (wdt_channel < 0) {
		LOG_WRN("watchdog timeout rejected: %d", wdt_channel);
		return;
	}

	/*
	 * Pausing while halted by the debugger is a convenience, not a requirement: it
	 * keeps single-stepping from resetting the device. Not every driver supports it,
	 * so fall back rather than losing the watchdog over it.
	 */
	if (wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG) != 0 && wdt_setup(wdt, 0) != 0) {
		LOG_WRN("watchdog setup failed");
		wdt_channel = -1;
	}
}

static void watchdog_feed(void)
{
	if (wdt_channel >= 0 && !faults_watchdog_starved()) {
		(void)wdt_feed(wdt, wdt_channel);
	}
}
#else
static void watchdog_init(void)
{
}

static void watchdog_feed(void)
{
}
#endif /* CONFIG_APP_WATCHDOG */

void tracker_wait_ms(uint32_t ms)
{
	uint32_t remaining = ms;

	while (remaining > 0U) {
		uint32_t slice = MIN(remaining, SLICE_MS);

		k_sleep(K_MSEC(slice));
		sample_for_shakes();
		faults_service();
		watchdog_feed();

		remaining -= slice;
	}
}

/* Idle time, accounted as such. */
static void idle_for(uint32_t ms)
{
	tracker_wait_ms(ms);
	power_model_note_idle(ms);
}

static void handle_shake(const struct shake_event *event)
{
	/*
	 * Say it plainly, and say it before the record is handled - the record parser is
	 * where the bug is, so this line and the metrics are what reach the cloud even
	 * when the next instruction faults.
	 */
	LOG_WRN("shake: %u swings over %u ms, peak %u.%03u g", event->swings,
		event->duration_ms, event->peak_mg / 1000U, event->peak_mg % 1000U);
	diag_report_shake(event->peak_mg);

	/*
	 * Let the announcement get out before the record is processed - the processing is
	 * what faults.
	 */
	for (uint32_t waited = 0; waited < CONFIG_APP_SHAKE_REPORT_GRACE_MS; waited += 100U) {
		k_sleep(K_MSEC(100));
		watchdog_feed();
	}

	shake_detect_record(event);
}

/*
 * One accelerometer sample into the shake detector.
 *
 * A shake is 2-5 Hz and lasts a second or more, so the ordinary 20 ms idle cadence sees
 * every swing - no burst sampling, unlike an impact, which is over in milliseconds.
 */
static void sample_for_shakes(void)
{
	struct shake_event event;
	uint16_t magnitude_mg;

	if (sensor_poll_magnitude_mg(&magnitude_mg) != 0) {
		/*
		 * No reading, so normally there is nothing to feed the detector - but a
		 * forced shake still has to land.
		 *
		 * Fault injection is most useful exactly when the accelerometer is
		 * absent or broken: on a board with nothing wired to I2C, button 2 is
		 * the only way to reach the crash at all, and returning here used to
		 * leave the forced flag set forever with the button appearing dead.
		 *
		 * The magnitude passed below is ignored - shake_detect_feed() handles
		 * the forced flag before it looks at it - but pass rest rather than an
		 * uninitialised or stale value.
		 */
		if (!shake_detect_forced()) {
			return;
		}

		magnitude_mg = 1000U;
	}

	if (shake_detect_feed(magnitude_mg, k_uptime_get_32(), &event)) {
		handle_shake(&event);
	}
}

static void do_sample(void)
{
	struct sensor_sample sample;
	uint32_t t0 = k_uptime_get_32();
	int rc = sensor_read(&sample);

	power_model_note_active(k_uptime_get_32() - t0);

	if (rc != 0) {
		enum sensor_err kind = sensor_last_err();

		sensor_error_streak++;
		diag_report_sensor_error(kind);

		if (sensor_error_streak == STREAK_ALARM) {
			/* One line, at the moment it stops being noise. */
			LOG_ERR("sensor unusable: %u failed reads in a row",
				sensor_error_streak);
		} else {
			LOG_WRN("sensor read failed (%s), streak %u", sensor_err_str(kind),
				sensor_error_streak);
		}

		/*
		 * Degraded mode is a failure this device was told to have, so leave it
		 * alone - re-initialising would quietly undo the demo. Everything else is
		 * a failure worth trying to recover from.
		 */
		if (!sensor_is_degraded() && (sensor_error_streak % STREAK_REPROBE) == 0U) {
			LOG_WRN("re-initialising sensor after %u failed reads",
				sensor_error_streak);

			if (sensor_init() == 0) {
				LOG_INF("sensor recovered after %u failed reads",
					sensor_error_streak);
				sensor_error_streak = 0;
			}
		}

		return;
	}

	if (sensor_last_err() == SENSOR_ERR_STUCK) {
		/*
		 * The read succeeded, which is exactly why this is worth a warning: a part
		 * repeating itself looks healthy from the outside.
		 */
		sensor_error_streak++;
		diag_report_sensor_error(SENSOR_ERR_STUCK);
		LOG_WRN("sensor value has not changed in %u reads", sensor_error_streak);
		return;
	}

	sensor_error_streak = 0;
	LOG_DBG("sample: motion=%u mg temp=%d.%u C", sample.motion_mg, sample.temp_c_x10 / 10,
		(unsigned int)(sample.temp_c_x10 % 10));

	if (sample.moved) {
		LOG_INF("asset moved: %u mg", sample.motion_mg);
	}
}

static void do_fix(void)
{
	struct geo_fix fix;

	geo_sim_attempt(&fix);
	diag_report_fix(fix.ok, fix.ttff_ms);
}

static void do_report(void)
{
	struct link_disconnect_event disconnects[LINK_DISCONNECT_REASONS];
	uint32_t ttc_ms;

	uint32_t battery_mv = power_model_battery_mv();

	if (battery_mv != 0U) {
		diag_report_battery(battery_mv);
	}
	diag_report_radio_on_pct(power_model_radio_on_pct());
	diag_report_gateway_absent(link_absent_s());
	diag_report_sensor_streak(sensor_error_streak);

	if (link_take_connect_event(&ttc_ms)) {
		diag_report_link_connected(ttc_ms);
	}

	uint8_t n = link_take_disconnects(disconnects);

	for (uint8_t i = 0; i < n; i++) {
		diag_report_link_disconnect(disconnects[i].reason, disconnects[i].count);
	}

	power_model_window_reset();
}

int tracker_init(void)
{
	leds_init();
	watchdog_init();
	shake_detect_init();
	geo_sim_init();
	power_model_init();

	return sensor_init();
}

void tracker_run(void)
{
	LOG_INF("duty cycle every %u ms, fix every %u cycles",
		CONFIG_APP_DUTY_CYCLE_INTERVAL_MS, CONFIG_APP_FIX_INTERVAL_CYCLES);

	while (true) {
		uint32_t cycle_start = k_uptime_get_32();

		cycle_count++;
		leds_update();

		do_sample();

		if ((cycle_count % CONFIG_APP_FIX_INTERVAL_CYCLES) == 0U) {
			do_fix();
		}

		do_report();

		/*
		 * Sleep out the rest of the period. A cycle that overran - a slow fix, a
		 * long retry - simply gets no idle time rather than drifting the schedule.
		 */
		uint32_t elapsed = k_uptime_get_32() - cycle_start;

		if (elapsed < CONFIG_APP_DUTY_CYCLE_INTERVAL_MS) {
			idle_for(CONFIG_APP_DUTY_CYCLE_INTERVAL_MS - elapsed);
		} else {
			LOG_WRN("cycle overran by %u ms",
				elapsed - CONFIG_APP_DUTY_CYCLE_INTERVAL_MS);
			faults_service();
			watchdog_feed();
		}
	}
}
