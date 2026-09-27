/*
 * Copyright (c) 2026 Spotflow s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Asset tracker demo for the TI LP-EM-CC2340R5.
 *
 * A battery-powered BLE asset tag whose entire uplink to Spotflow is diagnostics: what
 * the device knows about its own health, sent to somewhere an engineer can read it
 * without a cable. Sensor readings and position fixes are simulated and stay on the
 * device; what leaves it is whether those subsystems are working.
 *
 * The radio belongs to the Spotflow SDK: it calls bt_enable(), advertises the
 * observability service and talks to whatever gateway is in range. This application
 * never calls into the Bluetooth API - it only watches the link (see link_monitor.c).
 *
 * See ti/PLAN.md for the whole design, and MEMORY.md for why this fits in 36 KiB.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "metrics/system/spotflow_metrics_system.h"

#include "boot_info.h"
#include "diag_metrics.h"
#include "faults.h"
#include "link_monitor.h"
#include "power_model.h"
#include "tracker.h"

LOG_MODULE_REGISTER(app_main, LOG_LEVEL_INF);

/*
 * Stack metrics are limited to two threads on this part, so they go to the two whose
 * headroom is least certain: this one, which runs the duty cycle, and the Bluetooth
 * stack's task, whose stack we cut below TI's default to make everything fit.
 */
static void track_stack_usage(void)
{
#if defined(CONFIG_SPOTFLOW_METRICS_SYSTEM_STACK) && \
	!defined(CONFIG_SPOTFLOW_METRICS_SYSTEM_STACK_ALL_THREADS)
	/*
	 * The metrics subsystem comes up on its own schedule; registering before it is
	 * ready returns -EINVAL. Retry briefly rather than losing the metric for the
	 * lifetime of the device.
	 */
	for (int attempt = 0; attempt < 20; attempt++) {
		int rc = spotflow_metrics_system_enable_thread_stack(NULL);

		if (rc == 0 || rc == -EEXIST) {
			return;
		}

		if (rc != -EINVAL) {
			LOG_WRN("stack metric unavailable: %d", rc);
			return;
		}

		k_sleep(K_MSEC(100));
	}

	LOG_WRN("stack metrics never became ready");
#endif
}

int main(void)
{
	boot_info_init();

	/*
	 * A failed init is reported, not fatal.
	 *
	 * This used to return, which killed the duty cycle while the Spotflow thread kept
	 * running: the device stayed connected, kept its place in the portal and answered
	 * for its logs, and did nothing at all. A unit that is dead but present is the
	 * worst thing this demo could ship, because it is the one failure the monitoring
	 * cannot show you.
	 *
	 * Everything except the sensor is already up by the time this can fail, and the
	 * tracker is built to run with a bad sensor - that is what degraded mode is. So run
	 * it: the reads fail, the streak climbs, the red LED comes on and the device says
	 * out loud what is wrong with it. Which is the entire point of the device.
	 */
	int rc = tracker_init();

	if (rc != 0) {
		LOG_ERR("tracker init failed: %d - running with the sensor marked bad", rc);
	}

	rc = faults_init();
	if (rc != 0) {
		/* The demo needs its buttons, but a device without them still reports. */
		LOG_ERR("fault injection unavailable: %d", rc);
	}

	link_monitor_init();
	diag_metrics_init();
	track_stack_usage();

	diag_report_boot(boot_info_count());

	/*
	 * The first line an engineer reads: which firmware, which boot, why it restarted.
	 *
	 * Emitted here rather than at the top of main() because the Bluetooth stack's own
	 * startup burst overflows the log buffer in the first 30 ms and this line was being
	 * dropped - the one line the demo opens on. By now the burst has passed.
	 */
	LOG_INF("asset tracker up: boot %u, reset %s%s", boot_info_count(),
		boot_info_reset_cause(), boot_info_was_cold() ? ", cold" : "");

	/*
	 * Say the supply voltage once at boot, whether or not it is plausible. Watching
	 * battery_v decline is how this device's battery life gets measured, and without
	 * this line a missing metric is indistinguishable from a flat reading - it reads
	 * as unavailable on a bench supply or debug probe, which is not a fault.
	 */
	uint32_t supply_mv = power_model_battery_mv();

	if (supply_mv != 0U) {
		LOG_INF("supply %u.%03u V", supply_mv / 1000U, supply_mv % 1000U);
	} else {
		LOG_INF("supply not measurable (probe or bench power?)");
	}

	tracker_run();

	return 0;
}
