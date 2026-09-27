/*
 * Copyright (c) 2026 Spotflow s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Battery model.
 *
 * The LaunchPad runs from the debug probe, so there is no real cell and no fuel gauge.
 * Rather than invent a number, this integrates the device's own behaviour: time spent
 * in each state and time with the radio on, against a fixed capacity. The result moves
 * for real reasons - a device that retries more, drains faster - which is what makes
 * the battery metric worth looking at next to the sensor error rate.
 *
 * A real backend replaces this with an ADC reading behind the same two calls.
 */

#ifndef APP_POWER_MODEL_H
#define APP_POWER_MODEL_H

#include <stdbool.h>
#include <stdint.h>

void power_model_init(void);

/* Charge accounting. Called by the duty cycle as it moves through its states. */
void power_model_note_idle(uint32_t ms);
void power_model_note_active(uint32_t ms);
void power_model_note_fix(uint32_t ms);

/* The radio is the expensive part; the link monitor drives this. */
void power_model_note_radio(bool on);

/* Measured supply voltage in mV, or 0 if the battery monitor reads implausibly. */
uint32_t power_model_battery_mv(void);

/* Share of the last reporting window with the radio on, 0-100. */
uint8_t power_model_radio_on_pct(void);

/* Starts a new reporting window; call after reporting. */
void power_model_window_reset(void);

#endif /* APP_POWER_MODEL_H */
