/*
 * Copyright (c) 2026 Spotflow s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "link_monitor.h"
#include "power_model.h"

LOG_MODULE_REGISTER(app_link, CONFIG_APP_LINK_LOG_LEVEL);

static atomic_t link_up;

/* When the device last became reachable: boot, or the moment of the last disconnect. */
static uint32_t reachable_since_ms;
static uint32_t down_since_ms;

static uint32_t last_ttc_ms;
static atomic_t ttc_pending;

static struct link_disconnect_event disconnects[LINK_DISCONNECT_REASONS];
static struct k_spinlock lock;

static void record_disconnect(uint8_t reason)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	for (int i = 0; i < LINK_DISCONNECT_REASONS; i++) {
		if (disconnects[i].count != 0U && disconnects[i].reason == reason) {
			if (disconnects[i].count < UINT8_MAX) {
				disconnects[i].count++;
			}
			goto out;
		}
	}

	for (int i = 0; i < LINK_DISCONNECT_REASONS; i++) {
		if (disconnects[i].count == 0U) {
			disconnects[i].reason = reason;
			disconnects[i].count = 1U;
			goto out;
		}
	}

	/*
	 * More than four distinct reasons between two reports. Fold the overflow into
	 * the first slot rather than dropping it: the count stays honest even if the
	 * attribution does not.
	 */
	if (disconnects[0].count < UINT8_MAX) {
		disconnects[0].count++;
	}

out:
	k_spin_unlock(&lock, key);
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	ARG_UNUSED(conn);

	if (err != 0U) {
		LOG_WRN("gateway connection failed, err %u", err);
		return;
	}

	last_ttc_ms = k_uptime_get_32() - reachable_since_ms;
	atomic_set(&ttc_pending, 1);
	atomic_set(&link_up, 1);
	power_model_note_radio(true);

	LOG_INF("gateway connected after %u ms", last_ttc_ms);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);

	atomic_set(&link_up, 0);
	down_since_ms = k_uptime_get_32();
	reachable_since_ms = down_since_ms;
	power_model_note_radio(false);
	record_disconnect(reason);

	LOG_WRN("gateway disconnected, reason 0x%02x", reason);
}

static struct bt_conn_cb conn_callbacks = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

void link_monitor_init(void)
{
	uint32_t now = k_uptime_get_32();

	reachable_since_ms = now;
	down_since_ms = now;
	memset(disconnects, 0, sizeof(disconnects));

	bt_conn_cb_register(&conn_callbacks);
}

bool link_is_up(void)
{
	return atomic_get(&link_up) != 0;
}

uint32_t link_last_time_to_connect_ms(void)
{
	return last_ttc_ms;
}

bool link_take_connect_event(uint32_t *time_to_connect_ms)
{
	if (atomic_cas(&ttc_pending, 1, 0)) {
		*time_to_connect_ms = last_ttc_ms;
		return true;
	}

	return false;
}

uint8_t link_take_disconnects(struct link_disconnect_event *out)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint8_t n = 0;

	for (int i = 0; i < LINK_DISCONNECT_REASONS; i++) {
		if (disconnects[i].count != 0U) {
			out[n++] = disconnects[i];
			disconnects[i].count = 0U;
		}
	}

	k_spin_unlock(&lock, key);

	return n;
}

uint32_t link_absent_s(void)
{
	if (link_is_up()) {
		return 0U;
	}

	return (k_uptime_get_32() - down_since_ms) / 1000U;
}
