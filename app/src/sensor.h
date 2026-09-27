/*
 * Sensor seam.
 *
 * The tracker reads motion through this interface. Two backends implement it:
 *
 *   bmi270  a real Bosch BMI270 on I2C0 (the default). Wiring and power notes are in
 *           README.md - the module needs a real 3.3 V rail, which the debug probe does
 *           not supply unless its TGT VDD jumper says so.
 *   sim     a simulated part, for working without hardware and as a booth fallback.
 *
 * What matters for this demo is on the error side of the interface, not the data side:
 * the readings never leave the device, but every failure mode does.
 *
 * With the real part, the failures are real too. Degraded mode does not fake an error
 * code - it points the bus at an address nothing answers on, so the controller returns
 * a genuine NAK, and then powers the sensor down, so reads keep succeeding while the
 * data stops changing. That second one is the failure worth showing: from the outside
 * the device looks healthy.
 */

#ifndef APP_SENSOR_H
#define APP_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

/* How a read failed. Reported as a metric label, so the strings stay short. */
enum sensor_err {
	SENSOR_ERR_NONE = 0,
	SENSOR_ERR_NAK,     /* nothing acknowledged on the bus */
	SENSOR_ERR_TIMEOUT, /* transfer did not complete */
	SENSOR_ERR_BADDATA, /* the part answered, but not with a usable reading */
	SENSOR_ERR_STUCK,   /* bit-identical raw samples - a part that stopped converting */
};

struct sensor_sample {
	int16_t temp_c_x10; /* tenths of a degree, so logs need no float formatting */
	uint16_t motion_mg; /* deviation from rest, in milli-g */
	bool moved;         /* motion crossed the wake threshold */
};

int sensor_init(void);

/*
 * Read one sample. Returns 0 on success or a negative errno; on failure the reason is
 * available from sensor_last_err() in a form the metrics can label.
 */
int sensor_read(struct sensor_sample *out);

/*
 * One cheap magnitude sample in milli-g - about 1000 at rest, near 0 in free fall.
 *
 * The drop detector calls this many times a second, which the 10-second sensor_read()
 * cadence cannot serve: an impact lasts milliseconds. Returns -EAGAIN while the part is
 * degraded or powered down.
 */
int sensor_poll_magnitude_mg(uint16_t *magnitude_mg);

enum sensor_err sensor_last_err(void);

/* Short, stable strings - these become metric label values. */
const char *sensor_err_str(enum sensor_err err);

/* Which backend was built in: "bmi270" or "sim". Goes into session metadata. */
const char *sensor_backend_name(void);

/*
 * Degradation control. This is what turns a healthy device into a failing one on stage.
 */
void sensor_set_degraded(bool degraded);
bool sensor_is_degraded(void);



#endif /* APP_SENSOR_H */
