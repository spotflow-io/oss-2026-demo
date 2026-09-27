/*
 * The diagnostics catalog.
 *
 * Eight application metrics, registered in one place so the budget is visible.
 *
 * It was ten until the board said otherwise. On hardware the shared 4 KiB heap could not
 * carry the catalog: registration returned -ENOMEM and the CBOR encoder could not get a
 * buffer, so metrics that had registered could not be sent either. Raising the heap
 * twice was not enough - there is no RAM left to raise it with - so three metrics were
 * cut and the rest tightened. What survived is what the demo actually tells a story with.
 *
 * Each one answers a question a firmware engineer asks about a device they cannot
 * reach. Nothing here describes the cargo; all of it describes the device.
 *
 *   boot_count               is it restarting, and how often?
 *   battery_v                how long has it got left?
 *   radio_on_pct             is something keeping the radio awake?
 *   link_disconnects         why does the link keep dropping? (label: HCI reason)
 *   sensor_errors            what is the bus doing? (label: failure kind)
 *   sensor_error_streak      is it flaky, or is it gone?
 *   shakes_detected           has it been handled roughly, and how often?
 *   shake_peak_g              how hard was the worst of it?
 *
 * Cut, and why they were the ones to go: link_time_to_connect_ms and gateway_absent_s
 * are both inferable from the transport connection state the SDK already reports, and
 * fix_failures is visible as a gap in ttff_ms. None of them carries a beat of the demo.
 * All three are still measured in the code - only the reporting is gone - so putting one
 * back is a registration away if the heap ever allows it.
 *
 * Two of these are floats, and they are the two a person reads off a dashboard rather
 * than a log: battery_v and shake_peak_g. The device measures millivolts and milli-g
 * because that is what the hardware gives it and there is no FPU on this part, but 3.273
 * and 1.715 are the numbers an engineer thinks in, and a metric name that carries its
 * unit has to tell the truth about it. The divide happens in diag_report_*, so the
 * sensing code below this line never sees a float.
 *
 * Label cardinality is RAM here: every distinct label value allocates a timeseries of
 * about 164 bytes from a 4 KiB heap shared with everything else. Both labelled metrics
 * declare small, bounded value sets for that reason.
 *
 * Deferred until hardware shows what the heap can take: watchdog margin, duty cycle
 * overrun, and link RSSI (which Zephyr exposes no portable API for, and which would
 * need a vendor HCI command the TI controller may or may not answer).
 */

#ifndef APP_DIAG_METRICS_H
#define APP_DIAG_METRICS_H

#include <stdbool.h>
#include <stdint.h>

#include "sensor.h"

int diag_metrics_init(void);

void diag_report_boot(uint32_t boot_count);
void diag_report_battery(uint32_t mv);   /* reported as battery_v, in volts */
void diag_report_radio_on_pct(uint8_t pct);
void diag_report_link_connected(uint32_t time_to_connect_ms);
void diag_report_link_disconnect(uint8_t hci_reason, uint8_t count);
void diag_report_gateway_absent(uint32_t seconds);
void diag_report_sensor_error(enum sensor_err kind);
void diag_report_sensor_streak(uint32_t streak);
void diag_report_fix(bool ok, uint32_t ttff_ms);
void diag_report_shake(uint16_t peak_mg); /* reported as shake_peak_g, in g */

#endif /* APP_DIAG_METRICS_H */
