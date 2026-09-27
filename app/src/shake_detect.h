/*
 * Copyright (c) 2026 Spotflow s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Shake detection - rough handling, not a drop.
 *
 * A parcel being shaken reads as repeated hard excursions either side of rest: the
 * magnitude swings far above 1 g, falls back, and does it again, several times a second.
 * Counting those swings is both easier to trigger than a fall and safer to demonstrate,
 * and it is the signal that actually matters for an asset in transit - most cargo is
 * mishandled rather than dropped.
 *
 * This replaced a free-fall detector. Free fall works and is a lovely signature, but on a
 * bare board on a bench it is genuinely hard to produce without risking the hardware, and
 * a demo you are afraid to run is not a demo.
 *
 * The duty cycle samples at APP_SHAKE_SAMPLE_INTERVAL_MS while otherwise idle, which at
 * 50 Hz is ample for a 2-5 Hz shake - no burst sampling needed, unlike an impact.
 */

#ifndef APP_SHAKE_DETECT_H
#define APP_SHAKE_DETECT_H

#include <stdbool.h>
#include <stdint.h>

struct shake_event {
	uint16_t swings;      /* how many hard excursions were counted */
	uint16_t peak_mg;     /* the strongest of them, as deviation from rest */
	uint16_t duration_ms; /* how long the shaking went on */
};

void shake_detect_init(void);

/*
 * Feed one magnitude sample, in milli-g of total acceleration - about 1000 at rest.
 *
 * Returns true once per shake, with the event filled in - and that happens when the
 * shaking *stops* (SHAKE_QUIET_MS with no swing), not when it is first recognised.
 * Reporting on recognition would mean every shake reported exactly SHAKE_MIN_SWINGS,
 * which is a threshold, not a measurement.
 */
bool shake_detect_feed(uint16_t magnitude_mg, uint32_t now_ms, struct shake_event *out);

/*
 * Hand the event to the recording path.
 *
 * Separate from detection because this is where the demo's bug lives: the record is
 * byte-packed and its counter is read with a misaligned word load, which this CPU cannot
 * do. See the comment on the implementation.
 */
void shake_detect_record(const struct shake_event *event);

/* Complete a synthetic shake on the next sample, for rehearsing without waving anything. */
void shake_detect_force(void);

/* True while a forced shake is waiting to be completed. */
bool shake_detect_forced(void);

#endif /* APP_SHAKE_DETECT_H */
