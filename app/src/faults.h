/*
 * The two buttons, and what they do to the device.
 *
 * Button 1 turns the sensor bus unreliable and back again - the degradation that the
 * whole diagnostics story hangs off. Button 2 breaks the device on purpose, in the way
 * chosen at build time (see app/Kconfig).
 *
 * Presses are captured in the GPIO interrupt and acted on from the duty cycle thread,
 * so that a crash happens in thread context inside the real code path rather than
 * inside an ISR - the difference between a coredump that reads like a bug report and
 * one that reads like a puzzle.
 */

#ifndef APP_FAULTS_H
#define APP_FAULTS_H

#include <stdbool.h>

int faults_init(void);

/*
 * Act on anything the buttons asked for. Called frequently from the duty cycle so the
 * device responds to a press in a fraction of a second, not at the end of a cycle.
 */
void faults_service(void);

/* True once the watchdog fault has been triggered: stop feeding the dog. */
bool faults_watchdog_starved(void);

#endif /* APP_FAULTS_H */
