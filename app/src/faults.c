/*
 * Copyright (c) 2026 Spotflow s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "faults.h"
#include "shake_detect.h"
#include "sensor.h"

LOG_MODULE_REGISTER(app_faults, LOG_LEVEL_INF);

#define SW0_NODE DT_ALIAS(sw0)
#define SW1_NODE DT_ALIAS(sw1)

/* Mechanical buttons bounce for a few milliseconds; a quarter second is plenty. */
#define DEBOUNCE_MS 250U

static const struct gpio_dt_spec btn_degrade = GPIO_DT_SPEC_GET_OR(SW0_NODE, gpios, { 0 });
static const struct gpio_dt_spec btn_fault = GPIO_DT_SPEC_GET_OR(SW1_NODE, gpios, { 0 });

static struct gpio_callback btn_degrade_cb;
static struct gpio_callback btn_fault_cb;

static atomic_t degrade_pending;
static atomic_t fault_pending;
static atomic_t watchdog_starved;

static uint32_t last_degrade_ms;
static uint32_t last_fault_ms;

static bool debounce(uint32_t *last_ms)
{
	uint32_t now = k_uptime_get_32();

	if ((now - *last_ms) < DEBOUNCE_MS) {
		return false;
	}

	*last_ms = now;

	return true;
}

static void on_degrade_pressed(const struct device *dev, struct gpio_callback *cb,
			       uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	if (debounce(&last_degrade_ms)) {
		atomic_set(&degrade_pending, 1);
	}
}

static void on_fault_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	if (debounce(&last_fault_ms)) {
		atomic_set(&fault_pending, 1);
	}
}

static int setup_button(const struct gpio_dt_spec *spec, struct gpio_callback *cb,
			gpio_callback_handler_t handler)
{
	int rc;

	if (!gpio_is_ready_dt(spec)) {
		LOG_ERR("button gpio not ready");
		return -ENODEV;
	}

	rc = gpio_pin_configure_dt(spec, GPIO_INPUT);
	if (rc != 0) {
		return rc;
	}

	rc = gpio_pin_interrupt_configure_dt(spec, GPIO_INT_EDGE_TO_ACTIVE);
	if (rc != 0) {
		return rc;
	}

	gpio_init_callback(cb, handler, BIT(spec->pin));

	return gpio_add_callback(spec->port, cb);
}

#ifdef CONFIG_APP_FAULT_KIND_STACK_EXHAUSTION
/*
 * Walk the stack down until the sentinel is about to be overwritten, then panic
 * deliberately. Without an MPU the alternative is silently corrupting whatever lives
 * below the stack, which makes for a coredump nobody can interpret.
 */
static volatile uint32_t recursion_depth;

static void consume_stack(uint32_t depth)
{
	volatile uint8_t frame[64];

	frame[0] = (uint8_t)depth;
	recursion_depth = depth;

	if ((uintptr_t)&frame[0] <
	    (uintptr_t)k_current_get()->stack_info.start + sizeof(frame) * 2U) {
		LOG_ERR("stack exhausted at depth %u", depth);
		k_panic();
	}

	consume_stack(depth + 1U);

	/* Keep the frame alive across the call so the compiler cannot make this a loop. */
	frame[1] = frame[0];
}
#endif /* CONFIG_APP_FAULT_KIND_STACK_EXHAUSTION */

static void trigger_fault(void)
{
#if defined(CONFIG_APP_FAULT_KIND_SHAKE)
	/*
	 * Complete a synthetic shake on the next sample. Everything after that is the same
	 * code a real shake runs - detection, reporting, and the record parser that faults.
	 */
	LOG_WRN("simulating a shake");
	shake_detect_force();
#elif defined(CONFIG_APP_FAULT_KIND_ASSERT)
	LOG_ERR("state machine invariant violated");
	k_oops();
#elif defined(CONFIG_APP_FAULT_KIND_STACK_EXHAUSTION)
	LOG_WRN("recursing until the stack runs out");
	consume_stack(0U);
#elif defined(CONFIG_APP_FAULT_KIND_WATCHDOG)
	LOG_WRN("watchdog will no longer be fed");
	atomic_set(&watchdog_starved, 1);
#endif
}

int faults_init(void)
{
	int rc = setup_button(&btn_degrade, &btn_degrade_cb, on_degrade_pressed);

	if (rc != 0) {
		LOG_ERR("button 1 unavailable: %d", rc);
		return rc;
	}

	rc = setup_button(&btn_fault, &btn_fault_cb, on_fault_pressed);
	if (rc != 0) {
		LOG_ERR("button 2 unavailable: %d", rc);
		return rc;
	}

	return 0;
}

void faults_service(void)
{
	if (atomic_cas(&degrade_pending, 1, 0)) {
		sensor_set_degraded(!sensor_is_degraded());
	}

	if (IS_ENABLED(CONFIG_APP_FAULT_INJECTION) && atomic_cas(&fault_pending, 1, 0)) {
		trigger_fault();
	}
}

bool faults_watchdog_starved(void)
{
	return atomic_get(&watchdog_starved) != 0;
}
