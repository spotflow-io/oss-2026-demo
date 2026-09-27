/*
 * Position fixes, simulated.
 *
 * There is no GNSS receiver on this board, and deliberately no position data in what
 * the device reports: coordinates are the asset owner's business, not the firmware
 * engineer's. What the firmware engineer needs is whether the positioning subsystem is
 * working - how long a fix takes, how often it fails, and whether that is getting worse.
 *
 * So this models the behaviour of a receiver (cold starts are slow, a failing supply or
 * a shielded pallet makes fixes fail) and reports only the health of it.
 */

#ifndef APP_GEO_SIM_H
#define APP_GEO_SIM_H

#include <stdbool.h>
#include <stdint.h>

struct geo_fix {
	bool ok;
	uint32_t ttff_ms; /* time to first fix; only meaningful when ok */
	uint8_t sats;     /* satellites used; a thin fix is worth seeing before it fails */
};

void geo_sim_init(void);

/*
 * Attempt a fix. Blocks for as long as the attempt would realistically take, because
 * the duty cycle's time and energy accounting should reflect it.
 */
void geo_sim_attempt(struct geo_fix *out);

#endif /* APP_GEO_SIM_H */
