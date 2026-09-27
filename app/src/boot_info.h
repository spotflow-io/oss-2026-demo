/*
 * Copyright (c) 2026 Spotflow s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Why did this device restart, and how many times has it done so?
 *
 * The reset cause comes from hwinfo (the cc23x0 driver provides it) and is also
 * reported as a Spotflow system metric. The boot counter has nowhere persistent to
 * live - Settings and NVS do not fit in this build - so it sits in uninitialised RAM
 * behind a magic value. That survives a warm reset if the SoC preserves SRAM through
 * one, and degrades to "this looks like a cold boot" if it does not.
 */

#ifndef APP_BOOT_INFO_H
#define APP_BOOT_INFO_H

#include <stdbool.h>
#include <stdint.h>

void boot_info_init(void);

/* 1 on the first boot after power-on, incrementing across warm resets. */
uint32_t boot_info_count(void);

/* True when the counter was not carried across the reset. */
bool boot_info_was_cold(void);

/* Short, stable string: "power", "pin", "watchdog", "software", "fault", "other". */
const char *boot_info_reset_cause(void);

#endif /* APP_BOOT_INFO_H */
