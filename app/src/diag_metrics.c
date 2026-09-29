/*
 * Copyright (c) 2026 Spotflow s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "metrics/spotflow_metrics_backend.h"

#include "diag_metrics.h"

LOG_MODULE_REGISTER(app_metrics, LOG_LEVEL_INF);

/*
 * Aggregation choices.
 *
 * Sampled quantities aggregate over a minute so the uplink stays quiet between demo
 * beats; discrete happenings are events, which the SDK counts. boot_count is reported
 * once at startup and must not be averaged away, so it goes out unaggregated.
 */
#define AGG_SAMPLED SPOTFLOW_AGG_INTERVAL_1MIN
#define AGG_EVENT   SPOTFLOW_AGG_INTERVAL_1MIN
#define AGG_ONCE    SPOTFLOW_AGG_INTERVAL_NONE

/* Bounded label sets: one timeseries each, and the heap is small. */
#define MAX_DISCONNECT_REASONS 3
#define MAX_SENSOR_ERR_KINDS   4

static struct spotflow_metric_int *m_boot_count;
static struct spotflow_metric_float *m_battery_v;
static struct spotflow_metric_int *m_radio_on_pct;
static struct spotflow_metric_int *m_link_disconnects;
static struct spotflow_metric_int *m_sensor_errors;
static struct spotflow_metric_int *m_sensor_streak;
static struct spotflow_metric_int *m_shakes;
static struct spotflow_metric_float *m_shake_peak_g;

static int register_plain(const char *name, enum spotflow_agg_interval agg,
			  struct spotflow_metric_int **out)
{
	int rc = spotflow_register_metric_int(name, agg, out);

	if (rc < 0) {
		/* Worth an error: a missing metric is a hole in the dashboard. */
		LOG_ERR("metric %s not registered: %d", name, rc);
	}

	return rc;
}

/*
 * Volts and g, not millivolts and milli-g.
 *
 * The device measures in integers because that is what the hardware gives it and there
 * is no FPU on this part, but a dashboard is read by a person: 3.273 V and 1.715 g are
 * the units an engineer thinks in. The conversion happens here, at the reporting
 * boundary, so the sensing code stays in its natural integer units and only the numbers
 * that leave the device carry the human ones.
 */
static int register_plain_float(const char *name, enum spotflow_agg_interval agg,
				struct spotflow_metric_float **out)
{
	int rc = spotflow_register_metric_float(name, agg, out);

	if (rc < 0) {
		LOG_ERR("metric %s not registered: %d", name, rc);
	}

	return rc;
}

static int register_labelled(const char *name, uint8_t max_timeseries,
			     struct spotflow_metric_int **out)
{
	int rc = spotflow_register_metric_int_with_labels(name, AGG_EVENT, max_timeseries, 1,
							 out);

	if (rc < 0) {
		LOG_ERR("metric %s not registered: %d", name, rc);
	}

	return rc;
}

int diag_metrics_init(void)
{
	register_plain("boot_count", AGG_ONCE, &m_boot_count);
	register_plain_float("battery_v", AGG_SAMPLED, &m_battery_v);
	register_plain("radio_on_pct", AGG_SAMPLED, &m_radio_on_pct);
	register_plain("sensor_error_streak", AGG_SAMPLED, &m_sensor_streak);
	/*
	 * Unaggregated, unlike everything else here. A shake is rare and urgent, and the
	 * device faults moments after detecting one: anything held in a one-minute
	 * aggregation window dies with it. Measured on hardware - the first working crash
	 * reached the cloud with no event metrics at all.
	 */
	register_plain("shakes_detected", AGG_ONCE, &m_shakes);
	register_plain_float("shake_peak_g", AGG_ONCE, &m_shake_peak_g);

	register_labelled("link_disconnects", MAX_DISCONNECT_REASONS, &m_link_disconnects);
	register_labelled("sensor_errors", MAX_SENSOR_ERR_KINDS, &m_sensor_errors);

	return 0;
}

static void report(struct spotflow_metric_int *metric, int64_t value, const char *name)
{
	if (metric == NULL) {
		return;
	}

	int rc = spotflow_report_metric_int(metric, value);

	if (rc < 0) {
		LOG_WRN("metric %s dropped: %d", name, rc);
	}
}

static void report_float(struct spotflow_metric_float *metric, float value, const char *name)
{
	if (metric == NULL) {
		return;
	}

	int rc = spotflow_report_metric_float(metric, value);

	if (rc < 0) {
		LOG_WRN("metric %s dropped: %d", name, rc);
	}
}

void diag_report_boot(uint32_t boot_count)
{
	report(m_boot_count, boot_count, "boot_count");
}

void diag_report_battery(uint32_t mv)
{
	report_float(m_battery_v, (float)mv / 1000.0f, "battery_v");
}

void diag_report_radio_on_pct(uint8_t pct)
{
	report(m_radio_on_pct, pct, "radio_on_pct");
}

void diag_report_sensor_streak(uint32_t streak)
{
	report(m_sensor_streak, streak, "sensor_error_streak");
}

void diag_report_link_disconnect(uint8_t hci_reason, uint8_t count)
{
	char reason[7];

	if (m_link_disconnects == NULL) {
		return;
	}

	/*
	 * The raw HCI reason code, not a prettified name: it is what the Bluetooth spec
	 * and every other tool calls it, and a label is not the place to lose that.
	 */
	(void)snprintf(reason, sizeof(reason), "0x%02x", hci_reason);

	struct spotflow_label labels[] = { { .key = "reason", .value = reason } };

	for (uint8_t i = 0; i < count; i++) {
		int rc = spotflow_report_event_with_labels(m_link_disconnects, labels, 1);

		if (rc < 0) {
			LOG_WRN("metric link_disconnects dropped: %d", rc);
			return;
		}
	}
}

void diag_report_sensor_error(enum sensor_err kind)
{
	if (m_sensor_errors == NULL || kind == SENSOR_ERR_NONE) {
		return;
	}

	struct spotflow_label labels[] = { { .key = "kind", .value = sensor_err_str(kind) } };

	int rc = spotflow_report_event_with_labels(m_sensor_errors, labels, 1);

	if (rc < 0) {
		LOG_WRN("metric sensor_errors dropped: %d", rc);
	}
}

void diag_report_shake(uint16_t peak_mg)
{
	report_float(m_shake_peak_g, (float)peak_mg / 1000.0f, "shake_peak_g");

	if (m_shakes != NULL && spotflow_report_event(m_shakes) < 0) {
		LOG_WRN("metric shakes_detected dropped");
	}
}
