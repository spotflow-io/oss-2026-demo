/*
 * Copyright (c) 2026 Spotflow s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "shake_detect.h"

LOG_MODULE_REGISTER(app_shake, CONFIG_APP_SHAKE_LOG_LEVEL);

/*
 * Thresholds, in milli-g of deviation from a resting 1 g.
 *
 * At rest the deviation is a few tens of mg. Picking the device up or setting it down
 * produces two or three hundred. A deliberate shake produces well over a thousand,
 * repeatedly - so the bar is set high enough that handling cannot reach it, and the
 * swing has to relax below SWING_RESET before another one counts, which is what stops a
 * single sustained jolt registering as a dozen.
 */
#define SWING_PEAK_MG  900U
#define SWING_RESET_MG 400U

/*
 * A shake is several swings in quick succession. At 2-5 Hz the magnitude peaks twice per
 * cycle, so six swings is roughly a second of genuine shaking - easy to do on purpose,
 * hard to do by accident while picking the thing up.
 *
 * Six is the bar for calling it a shake at all. It is NOT the answer: once the bar is
 * cleared the detector keeps counting until the shaking stops, so the number it finally
 * reports is how many swings there actually were. Reporting on the sixth - which is what
 * this did at first - can only ever print "6 swings", which is not a measurement.
 */
#define SHAKE_MIN_SWINGS 6U
#define SHAKE_WINDOW_MS  2500U

/*
 * What ends a shake: a gap this long with no swing. One cycle of the slowest shake worth
 * counting is ~250 ms between peaks at 2 Hz, and the detector samples every
 * APP_SHAKE_SAMPLE_INTERVAL_MS, so 500 ms is comfortably past "still going" without
 * making the device wait around after you stop.
 */
#define SHAKE_QUIET_MS 500U

/* ...and a ceiling, so something rattling in a lorry still reports rather than counting
 * forever. */
#define SHAKE_MAX_MS 8000U

/* After reporting, ignore everything for a moment so one shake is one event. */
#define SHAKE_COOLDOWN_MS 3000U

static uint16_t swings;
static uint16_t peak_mg;
static uint32_t first_swing_ms;
static uint32_t last_swing_ms;
static uint32_t cooldown_until_ms;
static bool armed;
static bool forced;
static bool shaking; /* past the threshold, still counting */

/* Kept live so the compiler cannot discard the work that faults. */
static volatile uint32_t last_shake_counter;
static volatile char last_label_initial;

/*
 * The annotation a shake record carries - what the device was doing when it happened.
 * It is longer than the buffer it gets formatted into, which is the bug.
 */
static const char shake_annotation[] = "rough handling detected in transit";

/*
 * The annotation is reached through a volatile pointer so the compiler cannot see the
 * length, fold the copy, or warn about it at build time. In the decompilation this is an
 * ordinary `memcpy(dst, src, strlen(src))` - the shape the bug actually takes in the
 * field, where the source is a string whose length nobody bounded.
 */
static const char *volatile annotation = shake_annotation;

/*
 * The record the detector hands on: a header byte whose low bits give the offset of the
 * payload, then the measurements, then a 32-bit event counter.
 */
#define SHAKE_LABEL_TEXT_LEN 16
#define SHAKE_RECORD_LEN     16
#define SHAKE_RECORD_HEADER 0x5b /* low bits = payload offset: 3, not 4-byte aligned */

static uint8_t shake_record[SHAKE_RECORD_LEN];
static uint32_t shake_counter;

/*
 * The payload offset goes through a volatile so the compiler cannot constant-fold it.
 *
 * Without this, GCC follows the store of the header byte, proves the offset is 3, proves
 * the load is misaligned, and helpfully emits four LDRB instructions instead - which do
 * not fault, and the bug quietly stops being a bug. Checked in the disassembly: it must
 * be a single register-offset LDR.
 */
static volatile uint8_t record_offset;

void shake_detect_init(void)
{
	swings = 0U;
	peak_mg = 0U;
	armed = true;
	forced = false;
	shaking = false;
	last_swing_ms = 0U;
	cooldown_until_ms = 0U;
	shake_counter = 0U;
	memset(shake_record, 0, sizeof(shake_record));
}

static bool finish(uint32_t now_ms, struct shake_event *out)
{
	out->swings = swings;
	out->peak_mg = peak_mg;

	/*
	 * Measured from the first swing to the last one, not to now - the quiet gap that
	 * told us the shaking had stopped is not part of the shaking.
	 */
	out->duration_ms = (uint16_t)MIN(last_swing_ms - first_swing_ms, (uint32_t)UINT16_MAX);

	swings = 0U;
	peak_mg = 0U;
	armed = true;
	shaking = false;
	cooldown_until_ms = now_ms + SHAKE_COOLDOWN_MS;

	return true;
}

bool shake_detect_feed(uint16_t magnitude_mg, uint32_t now_ms, struct shake_event *out)
{
	uint16_t deviation;

	if (forced) {
		forced = false;
		swings = 9U;
		peak_mg = 2800U;
		first_swing_ms = now_ms - 1300U;
		last_swing_ms = now_ms;
		LOG_WRN("shake detected (forced): %u swings, peak %u.%03u g", swings,
			peak_mg / 1000U, peak_mg % 1000U);
		return finish(now_ms, out);
	}

	if (now_ms < cooldown_until_ms) {
		return false;
	}

	deviation = (magnitude_mg > 1000U) ? (magnitude_mg - 1000U) : (1000U - magnitude_mg);

	/* A swing has to relax before the next one counts. */
	if (deviation <= SWING_RESET_MG) {
		armed = true;
	}

	/*
	 * Track the peak on every sample, not just on the sample that happens to trip the
	 * swing threshold.
	 *
	 * Updating it inside the detection branch below - which is what this did at first -
	 * records the value at the moment the swing was *noticed*, i.e. the first sample
	 * above SWING_PEAK_MG on the way up, and never the top of the swing. It reads low
	 * by whatever the sampling grid happens to miss: a simulated 1500 mg shake reported
	 * 1266. "Peak" has to mean peak.
	 */
	if (swings > 0U) {
		peak_mg = MAX(peak_mg, deviation);
	}

	if (armed && deviation >= SWING_PEAK_MG) {
		armed = false;

		if (swings == 0U) {
			first_swing_ms = now_ms;
		}

		swings++;
		last_swing_ms = now_ms;

		LOG_DBG("swing %u at %u mg", swings, deviation);

		if (swings >= SHAKE_MIN_SWINGS) {
			/* Bar cleared - from here it is a shake, and we are measuring it. */
			shaking = true;
		}
	}

	/*
	 * Report once the shaking stops, or once it has gone on long enough that waiting
	 * for it to stop is its own problem. Only now are swings, peak and duration the
	 * whole shake rather than the first second of it.
	 */
	if (shaking) {
		if ((now_ms - last_swing_ms) >= SHAKE_QUIET_MS ||
		    (now_ms - first_swing_ms) >= SHAKE_MAX_MS) {
			LOG_WRN("shake detected: %u swings, peak %u.%03u g", swings,
				peak_mg / 1000U, peak_mg % 1000U);
			return finish(now_ms, out);
		}

		return false;
	}

	/* Swings spread over too long a time are handling, not shaking. */
	if (swings > 0U && (now_ms - first_swing_ms) > SHAKE_WINDOW_MS) {
		LOG_DBG("only %u swings in %u ms, not a shake", swings, SHAKE_WINDOW_MS);
		swings = 0U;
		peak_mg = 0U;
	}

	return false;
}

void shake_detect_force(void)
{
	forced = true;
}

bool shake_detect_forced(void)
{
	return forced;
}

/*
 * Record the shake.
 *
 * The bug the demo is built around is here, in the detection logic rather than bolted on
 * beside it: the event counter is read back with a 32-bit load at the offset carried in
 * the record header, and that offset is 3.
 *
 * Cortex-M4 fixes unaligned accesses up in hardware, so this survives a prototype and
 * every desk test. ARMv6-M has no unaligned support at all - the load faults, and with no
 * UsageFault handler it escalates straight to HardFault. A driver moved from an M4 to an
 * M0+ is exactly how this reaches the field.
 */
/*
 * A label being prepared for the shake record: the text, and the callback that emits it.
 *
 * The field order matters and is guaranteed by the language - text first, emit directly
 * after it - which is what makes the overrun below land on the function pointer every
 * time rather than depending on how the compiler happened to lay out a stack frame.
 */
struct shake_label {
	char text[SHAKE_LABEL_TEXT_LEN];
	void (*emit)(const char *text);
};

/*
 * Deliberately not static.
 *
 * A file-local handler leaves no symbol behind, so the struct initialiser decompiles as a
 * store of a bare constant - and a bare constant in a function pointer is the first thing
 * any reader, human or machine, accuses. Exporting the symbol makes the initialiser read
 * as `label.emit = shake_label_emit`, which is plainly fine, and leaves the copy below as
 * the only suspicious thing in the function.
 */
void shake_label_emit(const char *text);

void shake_label_emit(const char *text)
{
	last_label_initial = text[0];
}

/*
 * Format the annotation into the record's label.
 *
 * The bug: the copy is bounded by the annotation's length rather than by the size of the
 * buffer it is going into. The annotation is 34 bytes, the buffer is 16, and the 18 bytes
 * that do not fit run straight over the emit pointer that follows it in the struct.
 * Calling it then branches into nonsense and the device hard faults.
 *
 * This is the ordinary shape of a firmware buffer overflow: a length that describes the
 * source instead of the destination, and no bounds check between them. There is no MPU
 * on this part to catch the write - the corruption is only discovered when the clobbered
 * pointer is used.
 */
static void format_shake_label(void)
{
	struct shake_label label = { .emit = shake_label_emit };
	const char *text = (const char *)annotation;

	/*
	 * The length describes the source, not the destination. sizeof(label.text) is 16;
	 * strlen(text) is 34. Nothing between the two is checked.
	 */
	memcpy(label.text, text, strlen(text));

	label.emit(label.text);
}

void shake_detect_record(const struct shake_event *event)
{
	uint8_t offset;

	shake_counter++;

	shake_record[0] = SHAKE_RECORD_HEADER;
	shake_record[1] = (uint8_t)(event->peak_mg & 0xFFU);
	shake_record[2] = (uint8_t)(event->peak_mg >> 8);
	sys_put_le32(shake_counter, &shake_record[SHAKE_RECORD_HEADER & 0x07U]);
	sys_put_le16(event->swings, &shake_record[8]);
	sys_put_le16(event->duration_ms, &shake_record[10]);

	if (!IS_ENABLED(CONFIG_APP_SHAKE_RECORD_BUG)) {
		memcpy((void *)&last_shake_counter,
		       &shake_record[SHAKE_RECORD_HEADER & 0x07U], sizeof(uint32_t));
		return;
	}

	if (IS_ENABLED(CONFIG_APP_SHAKE_BUG_OVERFLOW)) {
		format_shake_label();
		return;
	}

	record_offset = shake_record[0] & 0x07U;
	offset = record_offset;
	last_shake_counter = *(const uint32_t *)&shake_record[offset];
}
