/*
 * Copyright (c) 2026 Spotflow s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The duty cycle.
 *
 * An asset tag wakes, reads its sensors, occasionally spends real energy on a position
 * fix, reports, and goes back to sleep. This runs the whole thing on the main thread -
 * the application gets exactly one thread, because on a 36 KiB part a second stack
 * costs more than it is worth. See MEMORY.md.
 */

#ifndef APP_TRACKER_H
#define APP_TRACKER_H

#include <stdint.h>

int tracker_init(void);

/* Runs the duty cycle forever. Never returns. */
void tracker_run(void);

/*
 * Sleep, without going deaf or letting the watchdog bite.
 *
 * Anything in the duty cycle that waits for more than a fraction of a second must wait
 * here rather than in k_sleep(): a position fix can take seven seconds, and the
 * watchdog window is eight. This slices the wait, feeds the dog and services the
 * buttons, so a long wait neither resets the device nor makes it ignore a press.
 *
 * Energy accounting stays with the caller, which knows what the time was spent on.
 */
void tracker_wait_ms(uint32_t ms);

#endif /* APP_TRACKER_H */
