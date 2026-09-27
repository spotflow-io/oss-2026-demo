/*
 * Copyright (c) 2026 Spotflow s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Simulated sensor backend.
 *
 * Stands in for the BMI270 when no sensor is wired up, and is the booth fallback if the
 * wiring lets go. It mirrors the real backend's behaviour rather than inventing its own:
 * the same readings, the same failure modes, the same impact path with the same bug in
 * it, so the demo tells an identical story either way.
 *
 * The one thing it cannot do is fail for real. Where the BMI270 backend produces a
 * genuine bus NAK by addressing a device that is not there, and genuinely stops
 * converting by powering the part down, this one returns the error codes those
 * situations produce.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>

#include "sensor.h"

LOG_MODULE_REGISTER(app_sensor, CONFIG_APP_SENSOR_LOG_LEVEL);

/* Motion above this counts as the asset having moved. Rough handling is shake_detect.c. */
#define MOTION_WAKE_THRESHOLD_MG 250U

/* Degraded-mode failure rates, in percent per read. */
#define DEGRADED_NAK_PCT     35
#define DEGRADED_TIMEOUT_PCT 10
#define DEGRADED_BADDATA_PCT 15

/* After this many consecutive degraded reads the part stops converting. */
#define DEGRADED_STUCK_AFTER 4

/*
 * Per-range accelerometer trim, read out of the part at init and kept in RAM for the
 * lifetime of the driver - the same thing the vendor driver does with its calibration
 * block. The index into it is bounded; the demo's bug lives in shake_detect.c.
 *
 * An earlier version made this table the fault: index it with the impact magnitude in
 * raw units and walk off the end of SRAM. The board disproved it. The read landed at
 * 0x20009748 - 1.8 KiB past the end of the 36 KiB SRAM - and returned quietly, so this
 * part evidently still decodes above its RAM. Guessing at a memory map is no way to
 * build a demo; the unaligned load is guaranteed by the architecture instead.
 */
#define CAL_BUCKETS 8
static int16_t cal_trim[CAL_BUCKETS];
static const int16_t cal_trim_factory[CAL_BUCKETS] = { 0, -1, 1, -2, 2, -3, 3, 0 };

static bool degraded;
static enum sensor_err last_err;
static uint32_t degraded_reads;

static uint32_t roll_percent(void)
{
	return sys_rand32_get() % 100U;
}


/* Plausible readings: mostly still, occasionally jostled, slowly drifting temperature. */
static void synth_reading(uint16_t *motion_mg, int16_t *temp_c_x10)
{
	static int16_t temp = 196; /* 19.6 C */

	if (roll_percent() < 15U) {
		*motion_mg = 300U + (uint16_t)(sys_rand32_get() % 900U);
	} else {
		*motion_mg = (uint16_t)(sys_rand32_get() % 120U);
	}

	temp += (int16_t)(sys_rand32_get() % 3U) - 1;
	temp = CLAMP(temp, 150, 260);
	*temp_c_x10 = temp;
}

int sensor_init(void)
{
	degraded = false;
	degraded_reads = 0;
	last_err = SENSOR_ERR_NONE;

	memcpy(cal_trim, cal_trim_factory, sizeof(cal_trim));

	LOG_INF("sensor ready (simulated)");

	return 0;
}

int sensor_poll_magnitude_mg(uint16_t *magnitude_mg)
{
	if (degraded) {
		return -EAGAIN;
	}

	/* Resting 1 g with a little noise; a simulated drop is forced, not synthesised. */
	*magnitude_mg = (uint16_t)(980U + (sys_rand32_get() % 40U));

	return 0;
}

int sensor_read(struct sensor_sample *out)
{
	static uint16_t stuck_motion;
	static int16_t stuck_temp;

	uint16_t motion_mg;
	int16_t temp_c_x10;

	last_err = SENSOR_ERR_NONE;

	if (degraded) {
		uint32_t roll = roll_percent();

		degraded_reads++;

		if (roll < DEGRADED_NAK_PCT) {
			last_err = SENSOR_ERR_NAK;
			LOG_DBG("no ack from device at 0x68");
			return -EIO;
		}

		if (roll < DEGRADED_NAK_PCT + DEGRADED_TIMEOUT_PCT) {
			last_err = SENSOR_ERR_TIMEOUT;
			LOG_DBG("transfer did not complete");
			return -ETIMEDOUT;
		}

		if (roll < DEGRADED_NAK_PCT + DEGRADED_TIMEOUT_PCT + DEGRADED_BADDATA_PCT) {
			last_err = SENSOR_ERR_BADDATA;
			LOG_DBG("no valid reading available");
			return -ENODATA;
		}
	} else {
		degraded_reads = 0;
		stuck_motion = 0U;
	}

	synth_reading(&motion_mg, &temp_c_x10);

	/*
	 * A part that has been failing for a while stops converting and repeats its last
	 * answer. Reads "succeed", which is what makes this failure mode nasty in the
	 * field and worth a metric of its own.
	 */
	if (degraded && degraded_reads > DEGRADED_STUCK_AFTER) {
		if (stuck_motion == 0U) {
			stuck_motion = motion_mg;
			stuck_temp = temp_c_x10;
		}
		motion_mg = stuck_motion;
		temp_c_x10 = stuck_temp;
		last_err = SENSOR_ERR_STUCK;
	}


	out->motion_mg = motion_mg;
	out->temp_c_x10 = temp_c_x10;
	out->moved = motion_mg >= MOTION_WAKE_THRESHOLD_MG;


	LOG_DBG("motion=%u mg temp=%d", motion_mg, temp_c_x10);

	return 0;
}

enum sensor_err sensor_last_err(void)
{
	return last_err;
}

const char *sensor_err_str(enum sensor_err err)
{
	switch (err) {
	case SENSOR_ERR_NONE:
		return "none";
	case SENSOR_ERR_NAK:
		return "nak";
	case SENSOR_ERR_TIMEOUT:
		return "timeout";
	case SENSOR_ERR_BADDATA:
		return "baddata";
	case SENSOR_ERR_STUCK:
		return "stuck";
	default:
		return "unknown";
	}
}

const char *sensor_backend_name(void)
{
	return "sim";
}

void sensor_set_degraded(bool value)
{
	degraded = value;
	degraded_reads = 0;

	if (value) {
		LOG_WRN("sensor bus is unreliable");
	} else {
		LOG_INF("sensor bus recovered");
	}
}

bool sensor_is_degraded(void)
{
	return degraded;
}

