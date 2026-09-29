/*
 * Copyright (c) 2026 Spotflow s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "geo_sim.h"
#include "power_model.h"
#include "tracker.h"

LOG_MODULE_REGISTER(app_geo, LOG_LEVEL_INF);

/* A warm receiver fixes quickly; the first fix after boot is the slow one. */
#define TTFF_WARM_MIN_MS 800U
#define TTFF_WARM_VAR_MS 1200U
#define TTFF_COLD_MIN_MS 4000U
#define TTFF_COLD_VAR_MS 3000U

/* How often a fix simply does not come. Higher once the hardware is misbehaving. */
#define FAIL_PCT 8U

/* A failed attempt still costs the energy of listening. */
#define FAIL_LISTEN_MS 5000U

static bool have_warm_receiver;

void geo_sim_init(void)
{
	have_warm_receiver = false;
}

void geo_sim_attempt(struct geo_fix *out)
{
	uint32_t ttff;

	if (have_warm_receiver) {
		ttff = TTFF_WARM_MIN_MS + (sys_rand32_get() % TTFF_WARM_VAR_MS);
	} else {
		ttff = TTFF_COLD_MIN_MS + (sys_rand32_get() % TTFF_COLD_VAR_MS);
	}

	if ((sys_rand32_get() % 100U) < FAIL_PCT) {
		tracker_wait_ms(FAIL_LISTEN_MS);
		power_model_note_fix(FAIL_LISTEN_MS);

		/* A failed attempt leaves the receiver cold again. */
		have_warm_receiver = false;

		out->ok = false;
		out->ttff_ms = 0U;
		out->sats = (uint8_t)(sys_rand32_get() % 3U);

		LOG_WRN("no fix after %u ms, %u sats", FAIL_LISTEN_MS, out->sats);
		return;
	}

	tracker_wait_ms(ttff);
	power_model_note_fix(ttff);

	have_warm_receiver = true;

	out->ok = true;
	out->ttff_ms = ttff;
	out->sats = (uint8_t)(5U + (sys_rand32_get() % 7U));

	LOG_DBG("fix in %u ms, %u sats", ttff, out->sats);
}
