/*
 * Copyright (c) 2026 Spotflow s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Session metadata: the labels that let someone filter a fleet.
 *
 * The SDK already sends a run ID and the build ID, which is what matches a coredump to
 * an ELF. What it cannot know is how this unit is deployed, and that is what turns
 * "one device is misbehaving" into "every unit on this firmware with this sensor
 * backend is misbehaving".
 *
 * Keys are capped at 15 characters and values at 31 by the SDK, and the whole set has
 * to encode into CONFIG_SPOTFLOW_SESSION_METADATA_BUFFER_SIZE. Everything here is
 * static storage, as the callback contract requires.
 */

#include <stdint.h>

#include <zephyr/kernel.h>

#include <spotflow/session_metadata.h>

#include "sensor.h"

/* Bumped by hand per demo build; the build ID is what uniquely identifies an image. */
#define APP_FW_VERSION "0.1.0"

/* Which hardware this image expects. */
#define APP_HW_REV "lp-em-cc2340r5"

static const struct spotflow_session_label labels[] = {
	{
		.key = "fw_version",
		.type = SPOTFLOW_SESSION_LABEL_STRING,
		.value.string = APP_FW_VERSION,
	},
	{
		.key = "hw_rev",
		.type = SPOTFLOW_SESSION_LABEL_STRING,
		.value.string = APP_HW_REV,
	},
	{
		/* Simulated or real sensors: the first question to ask about a reading. */
		.key = "sensors",
		.type = SPOTFLOW_SESSION_LABEL_STRING,
		.value.string = IS_ENABLED(CONFIG_APP_SENSOR_BMI270) ? "bmi270" : "sim",
	},
	{
		/* What this unit is pretending to be, for fleet-level filtering. */
		.key = "profile",
		.type = SPOTFLOW_SESSION_LABEL_STRING,
		.value.string = "asset-tracker",
	},
};

struct spotflow_session_metadata_labels spotflow_override_session_metadata_labels(void)
{
	struct spotflow_session_metadata_labels out = {
		.items = labels,
		.count = ARRAY_SIZE(labels),
	};

	return out;
}
