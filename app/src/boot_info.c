#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/toolchain.h>

#include "boot_info.h"

#define BOOT_MAGIC 0x5A7C0DE1U

#ifdef CONFIG_APP_BOOT_COUNTER_NOINIT
static __noinit uint32_t boot_magic;
static __noinit uint32_t boot_count;
#else
static uint32_t boot_magic;
static uint32_t boot_count;
#endif

static bool cold_boot;
static const char *cause_str = "other";

static void classify_reset(void)
{
	uint32_t cause = 0;

	if (hwinfo_get_reset_cause(&cause) != 0) {
		cause_str = "unknown";
		return;
	}

	(void)hwinfo_clear_reset_cause();

	/*
	 * Ordered by what an engineer wants to hear first: a watchdog or a fault reset
	 * is a story, a power-on is not.
	 */
	if (cause & RESET_WATCHDOG) {
		cause_str = "watchdog";
	} else if (cause & RESET_BROWNOUT) {
		cause_str = "brownout";
	} else if (cause & RESET_POR) {
		cause_str = "power";
	} else if (cause & RESET_PIN) {
		cause_str = "pin";
	} else if (cause & RESET_SOFTWARE) {
		cause_str = "software";
	} else if (cause & RESET_CPU_LOCKUP) {
		cause_str = "fault";
	} else if (cause & RESET_DEBUG) {
		cause_str = "debug";
	} else if (cause == 0U) {
		cause_str = "none";
	}
}

void boot_info_init(void)
{
	if (boot_magic != BOOT_MAGIC) {
		boot_magic = BOOT_MAGIC;
		boot_count = 1U;
		cold_boot = true;
	} else {
		boot_count++;
		cold_boot = false;
	}

	classify_reset();
}

uint32_t boot_info_count(void)
{
	return boot_count;
}

bool boot_info_was_cold(void)
{
	return cold_boot;
}

const char *boot_info_reset_cause(void)
{
	return cause_str;
}
