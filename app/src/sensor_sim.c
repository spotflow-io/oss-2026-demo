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
 * The one thing it cannot do is fail for real: with no bus and no part, there is nothing
 * to go wrong. Failure reporting is therefore only exercised by the BMI270 backend.
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

/*
 * Per-range accelerometer trim, read out of the part at init and kept in RAM for the
 * lifetime of the driver - the same thing the vendor driver does with its calibration
 * block. The index into it is bounded; the demo's bug lives in shake_detect.c.
 *
 */
#define CAL_BUCKETS 8
static int16_t cal_trim[CAL_BUCKETS];
static const int16_t cal_trim_factory[CAL_BUCKETS] = { 0, -1, 1, -2, 2, -3, 3, 0 };

static enum sensor_err last_err;

/* Plausible readings: mostly still, occasionally jostled, slowly drifting temperature. */
static void synth_reading(uint16_t *motion_mg, int16_t *temp_c_x10)
{
	static int16_t temp = 196; /* 19.6 C */

	if ((sys_rand32_get() % 100U) < 15U) {
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
	last_err = SENSOR_ERR_NONE;

	memcpy(cal_trim, cal_trim_factory, sizeof(cal_trim));

	LOG_INF("sensor ready (simulated)");

	return 0;
}

int sensor_poll_magnitude_mg(uint16_t *magnitude_mg)
{
	/* Resting 1 g with a little noise; a simulated shake is forced, not synthesised. */
	*magnitude_mg = (uint16_t)(980U + (sys_rand32_get() % 40U));

	return 0;
}

int sensor_read(struct sensor_sample *out)
{
	uint16_t motion_mg;
	int16_t temp_c_x10;

	last_err = SENSOR_ERR_NONE;

	synth_reading(&motion_mg, &temp_c_x10);

	out->motion_mg = motion_mg;
	out->temp_c_x10 = temp_c_x10;
	out->temp_valid = true;
	out->moved = motion_mg >= MOTION_WAKE_THRESHOLD_MG;

	LOG_DBG("motion=%u mg temp=" TEMP_C_X10_FMT " C", motion_mg, TEMP_C_X10_ARGS(temp_c_x10));

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

