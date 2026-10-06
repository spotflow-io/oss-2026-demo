/*
 * Copyright (c) 2026 Spotflow s.r.o.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/atomic.h>

#include "wifi_link.h"

LOG_MODULE_REGISTER(app_wifi, LOG_LEVEL_INF);

#define WIFI_EVENT_MASK (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)
#define WIFI_RETRY_DELAY K_SECONDS(2)
#define WIFI_CONNECT_TIMEOUT K_SECONDS(30)

enum wifi_state_bit {
	WIFI_STATE_CONNECTING,
	WIFI_STATE_CONNECTED,
};

static struct net_if* wifi_iface;
static struct net_mgmt_event_callback wifi_event_callback;
static struct k_work_delayable reconnect_work;
static struct k_work_delayable connect_timeout_work;
static atomic_t wifi_state;

static void schedule_reconnect(void)
{
	(void)k_work_reschedule(&reconnect_work, WIFI_RETRY_DELAY);
}

static int connect(void)
{
	static const struct wifi_connect_req_params params = {
		.ssid = (const uint8_t*)CONFIG_APP_WIFI_SSID,
		.ssid_length = sizeof(CONFIG_APP_WIFI_SSID) - 1,
		.psk = (const uint8_t*)CONFIG_APP_WIFI_PASSWORD,
		.psk_length = sizeof(CONFIG_APP_WIFI_PASSWORD) - 1,
		.security = WIFI_SECURITY_TYPE_PSK,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
	};
	int rc;

	if (atomic_test_bit(&wifi_state, WIFI_STATE_CONNECTED) ||
	    atomic_test_and_set_bit(&wifi_state, WIFI_STATE_CONNECTING)) {
		return 0;
	}

	LOG_INF("connecting to Wi-Fi SSID %s", CONFIG_APP_WIFI_SSID);
	rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, wifi_iface, (void*)&params, sizeof(params));
	if (rc != 0) {
		atomic_clear_bit(&wifi_state, WIFI_STATE_CONNECTING);
		LOG_ERR("Wi-Fi connect request failed: %d", rc);
		schedule_reconnect();
	} else {
		(void)k_work_reschedule(&connect_timeout_work, WIFI_CONNECT_TIMEOUT);
	}

	return rc;
}

static void reconnect_handler(struct k_work* work)
{
	ARG_UNUSED(work);
	(void)connect();
}

static void connect_timeout_handler(struct k_work* work)
{
	ARG_UNUSED(work);

	if (!atomic_test_and_clear_bit(&wifi_state, WIFI_STATE_CONNECTING)) {
		return;
	}

	LOG_WRN("Wi-Fi association timed out");
	(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, wifi_iface, NULL, 0);
	schedule_reconnect();
}

static void wifi_event_handler(struct net_mgmt_event_callback* callback, uint64_t event,
			       struct net_if* iface)
{
	const struct wifi_status* status = callback->info;

	ARG_UNUSED(iface);

	switch (event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		(void)k_work_cancel_delayable(&connect_timeout_work);
		atomic_clear_bit(&wifi_state, WIFI_STATE_CONNECTING);
		if (status->status == 0) {
			atomic_set_bit(&wifi_state, WIFI_STATE_CONNECTED);
			(void)k_work_cancel_delayable(&reconnect_work);
			LOG_INF("Wi-Fi associated");
			net_dhcpv4_start(wifi_iface);
		} else {
			atomic_clear_bit(&wifi_state, WIFI_STATE_CONNECTED);
			LOG_WRN("Wi-Fi association failed: %d", status->status);
			schedule_reconnect();
		}
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		(void)k_work_cancel_delayable(&connect_timeout_work);
		atomic_clear_bit(&wifi_state, WIFI_STATE_CONNECTING);
		atomic_clear_bit(&wifi_state, WIFI_STATE_CONNECTED);
		LOG_WRN("Wi-Fi disconnected: %d", status->status);
		schedule_reconnect();
		break;
	default:
		break;
	}
}

int wifi_link_init(void)
{
	wifi_iface = net_if_get_wifi_sta();
	if (wifi_iface == NULL) {
		LOG_ERR("no Wi-Fi station interface");
		return -ENODEV;
	}

	k_work_init_delayable(&reconnect_work, reconnect_handler);
	k_work_init_delayable(&connect_timeout_work, connect_timeout_handler);
	net_mgmt_init_event_callback(&wifi_event_callback, wifi_event_handler, WIFI_EVENT_MASK);
	net_mgmt_add_event_callback(&wifi_event_callback);

	return connect();
}
