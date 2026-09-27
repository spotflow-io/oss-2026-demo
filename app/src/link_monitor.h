/*
 * Copyright (c) 2026 Spotflow s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Link diagnostics.
 *
 * The application never touches the Bluetooth API - the Spotflow SDK owns the radio,
 * advertises the observability service and talks to the gateway. But the application
 * can still watch, and for a device that only reaches the world when a gateway happens
 * to be in range, the health of that link is a first-class diagnostic: how long it
 * takes to come up, why it drops, and how long the device has been talking to nobody.
 *
 * Callbacks run in the Bluetooth RX context, so they only record. Reporting happens on
 * the duty cycle thread, which drains what accumulated here.
 */

#ifndef APP_LINK_MONITOR_H
#define APP_LINK_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

/* One disconnect, waiting to be reported with its HCI reason as a label. */
struct link_disconnect_event {
	uint8_t reason;
	uint8_t count;
};

void link_monitor_init(void);

bool link_is_up(void);

/*
 * Milliseconds between the device becoming reachable (boot, or the last disconnect)
 * and the gateway connecting. Zero until the first connection.
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
