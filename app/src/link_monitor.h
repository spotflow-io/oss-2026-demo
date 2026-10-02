/*
 * Copyright (c) 2026 Spotflow s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Link diagnostics.
 *
 * Spotflow uses a BLE gateway or direct MQTT/TLS. The application watches the selected
 * transport's connection state so time-to-connect, disconnects and radio duty remain
 * useful diagnostics in both images.
 *
 * Callbacks only record. Reporting happens on the duty cycle thread, which drains what
 * accumulated here.
 */

#ifndef APP_LINK_MONITOR_H
#define APP_LINK_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

/* One disconnect, waiting to be reported with its transport reason as a label. */
struct link_disconnect_event {
	uint8_t reason;
	uint8_t count;
};

void link_monitor_init(void);

bool link_is_up(void);

/*
 * Milliseconds between the device becoming reachable (boot, or the last disconnect)
 * and the selected transport connecting. Zero until the first connection.
 */
uint32_t link_last_time_to_connect_ms(void);

/* True once, per connection: there is a fresh time-to-connect worth reporting. */
bool link_take_connect_event(uint32_t *time_to_connect_ms);

/*
 * Drain accumulated disconnects. Returns how many entries were written to out, which
 * holds at most LINK_DISCONNECT_REASONS distinct reasons.
 */
#define LINK_DISCONNECT_REASONS 4
uint8_t link_take_disconnects(struct link_disconnect_event *out);

/* Seconds since the link was last up; 0 while connected. */
uint32_t link_absent_s(void);

#endif /* APP_LINK_MONITOR_H */
