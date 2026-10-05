/*
 * Copyright (c) 2026 Spotflow s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Real BMI270 backend.
 *
 * The bring-up sequence here follows the one worked out for the Spotflow BLE BMI270
 * sample on this board (device-sdk branch feature/bmi-sample), including the reason it
 * looks the way it does:
 *
 *   The stock Zephyr BMI270 driver writes registers with i2c_burst_write_dt(). On the
 *   CC23xx I2C controller that gets split into separate transactions, and the BMI270
 *   needs the register address and its data in one continuous transaction. Split
 *   writes *return success* while the register never changes - so the part answers on
 *   the bus, reports the right chip ID, and then silently refuses to initialise, and
 *   every reading comes back zero.
 *
 * So every write here builds [register, data...] into one buffer and sends it with a
 * single i2c_write(), and the 8 KiB configuration blob goes out in 32-byte chunks.
 *
 * That bug is worth remembering when reading the rest of this file: a write that
 * succeeds and does nothing is exactly the class of failure the device is instrumented
 * to expose.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>

#include "bmi270_config_file.h"

#include "sensor.h"

LOG_MODULE_REGISTER(app_sensor, CONFIG_APP_SENSOR_LOG_LEVEL);

#define BMI270_NODE DT_NODELABEL(bmi270)

#define BMI270_CHIP_ID	  0x24
#define BMI270_CHUNK_SIZE 32
#define BMI270_INIT_RETRIES 15
#define BMI270_PROBE_RETRIES 3

#define BMI270_REG_CHIP_ID	   0x00
#define BMI270_REG_ACC_DATA	   0x0c
#define BMI270_REG_TEMPERATURE	   0x22
#define BMI270_REG_INTERNAL_STATUS 0x21
#define BMI270_REG_ACC_CONF	   0x40
#define BMI270_REG_ACC_RANGE	   0x41
#define BMI270_REG_GYR_CONF	   0x42
#define BMI270_REG_GYR_RANGE	   0x43
#define BMI270_REG_INIT_CTRL	   0x59
#define BMI270_REG_INIT_ADDR_0	   0x5b
#define BMI270_REG_INIT_DATA	   0x5e
#define BMI270_REG_PWR_CONF	   0x7c
#define BMI270_REG_PWR_CTRL	   0x7d
#define BMI270_REG_CMD		   0x7e

#define BMI270_CMD_SOFT_RESET	 0xb6
#define BMI270_INIT_STATUS_OK	 0x01
#define BMI270_PWR_CTRL_ACC_GYR	 0x06
#define BMI270_ACC_CONF_100_HZ	 0xa8
#define BMI270_ACC_RANGE_16G	 0x03
#define BMI270_GYR_CONF_100_HZ	 0xe8
#define BMI270_GYR_RANGE_500_DPS 0x02

/* The address nothing answers on. Used to produce real bus errors on demand. */

/* Full scale of the configured range, in milli-g. */
#define ACC_FULL_SCALE_MG 16000

/* Motion above this counts as the asset having moved. Rough handling is shake_detect.c. */
#define MOTION_WAKE_THRESHOLD_MG 250U

/*
 * Consecutive bit-identical accelerometer samples before the part is called stuck.
 *
 * Three is enough because the test is exact equality of the raw LSBs: a live BMI270 at
 * +-16 g has roughly 3 LSB of RMS noise, so three successive samples agreeing on all 48
 * bits does not happen to a part that is converting. A part that has stopped repeats
 * itself exactly, every time.
 */
#define STUCK_AFTER_IDENTICAL_READS 3

/*
 * Per-range accelerometer trim, read out of the part at init and kept in RAM for the
 * lifetime of the driver - the same thing the vendor driver does with its calibration
 * block. The index into it is bounded; the demo's bug lives in shake_detect.c.
 *
 * An earlier version made this table the fault: index it with the impact magnitude in
 * raw units and walk off the end of SRAM. The board disproved it. The read landed at
 * 0x20009748 - 1.8 KiB past the end of the 36 KiB SRAM - and returned quietly, so this
 * part evidently still decodes above its RAM. Guessing at a memory map is no way to
 * build a demo; the unaligned load is guaranteed by the architecture instead.
 */
#define CAL_BUCKETS 8
static int16_t cal_trim[CAL_BUCKETS];
static const int16_t cal_trim_factory[CAL_BUCKETS] = { 0, -1, 1, -2, 2, -3, 3, 0 };

static const struct i2c_dt_spec bmi270 = I2C_DT_SPEC_GET(BMI270_NODE);

static enum sensor_err last_err;
/*
 * The last raw accelerometer sample, for stuck detection - the six bytes as they came
 * off the bus, not anything derived from them. See the comment in sensor_read().
 */
static uint8_t last_raw[6];
static bool have_last_raw;
static uint32_t unchanged_reads;


static int bmi270_write(uint8_t reg, const uint8_t *data, size_t len)
{
	uint8_t buffer[BMI270_CHUNK_SIZE + 1];

	if (len > BMI270_CHUNK_SIZE) {
		return -EINVAL;
	}

	/* One transaction: address byte and payload in the same buffer. See the header. */
	buffer[0] = reg;
	if (len > 0U) {
		memcpy(&buffer[1], data, len);
	}

	return i2c_write(bmi270.bus, buffer, len + 1U, bmi270.addr);
}

static int bmi270_write_byte(uint8_t reg, uint8_t value)
{
	int rc = bmi270_write(reg, &value, 1U);

	if (rc == 0) {
		k_usleep(1000);
	}

	return rc;
}

static int bmi270_read(uint8_t reg, uint8_t *data, size_t len)
{
	return i2c_write_read(bmi270.bus, bmi270.addr, &reg, 1U, data, len);
}

static int load_config_blob(void)
{
	for (size_t offset = 0; offset < sizeof(bmi270_config_file_max_fifo);
	     offset += BMI270_CHUNK_SIZE) {
		uint8_t address[] = { (offset / 2) & 0x0f, (offset / 2) >> 4 };
		size_t remaining = sizeof(bmi270_config_file_max_fifo) - offset;
		size_t len = MIN(remaining, BMI270_CHUNK_SIZE);
		int rc = bmi270_write(BMI270_REG_INIT_ADDR_0, address, sizeof(address));

		if (rc != 0) {
			return rc;
		}
		k_usleep(1000);

		rc = bmi270_write(BMI270_REG_INIT_DATA,
				  &bmi270_config_file_max_fifo[offset], len);
		if (rc != 0) {
			return rc;
		}
		k_usleep(1000);
	}

	return 0;
}

static int start_sensor(void)
{
	uint8_t value;
	int rc;

	/*
	 * 100 Hz is twice the 50 Hz the duty cycle polls at, and a shake is only 2-5 Hz,
	 * so every swing is seen several times over. This was 1600 Hz when the demo
	 * detected drops, because a landing is over in a millisecond or two; shaking needs
	 * nothing like that, and the lower rate costs less current.
	 */
	rc = bmi270_write_byte(BMI270_REG_ACC_CONF, BMI270_ACC_CONF_100_HZ);
	if (rc != 0) {
		return rc;
	}

	/*
	 * 16 g, not the 2 g a tracker would otherwise want. A parcel landing on a hard
	 * floor peaks well past 2 g, and at that range every impact clips to the same
	 * number - the severity, which is the interesting part, would be unmeasurable.
	 * The cost is resolution: 0.49 mg per count instead of 0.06, still far finer than
	 * the 350 mg free-fall threshold needs.
	 */
	rc = bmi270_write_byte(BMI270_REG_ACC_RANGE, BMI270_ACC_RANGE_16G);
	if (rc != 0) {
		return rc;
	}

	/*
	 * The gyro is enabled even though the tracker only uses acceleration, because on
	 * this part the temperature sensor is only updated while the gyro is running.
	 * Accelerometer-only (0x04) left TEMPERATURE reading 0x8000 - "no valid data" - on
	 * every sample after the first, which is what the board showed us. Turning off
	 * advanced power save alone did not fix it; this does.
	 *
	 * It costs gyro current, which this demo does not care about: the device is
	 * mains-powered and its battery figure is modelled from time-in-state.
	 */
	rc = bmi270_write_byte(BMI270_REG_GYR_CONF, BMI270_GYR_CONF_100_HZ);
	if (rc != 0) {
		return rc;
	}

	rc = bmi270_write_byte(BMI270_REG_GYR_RANGE, BMI270_GYR_RANGE_500_DPS);
	if (rc != 0) {
		return rc;
	}

	rc = bmi270_write_byte(BMI270_REG_PWR_CTRL, BMI270_PWR_CTRL_ACC_GYR);
	if (rc != 0) {
		return rc;
	}

	/*
	 * Advanced power save off. With it on (0x01) the part suspends its temperature
	 * sensor between conversions and every read after the first returns 0x8000,
	 * which we surface as -ENODATA. This device is mains-powered on the bench and
	 * its battery figure is modelled, so a working temperature is worth more.
	 */
	rc = bmi270_write_byte(BMI270_REG_PWR_CONF, 0x00);
	if (rc != 0) {
		return rc;
	}

	k_sleep(K_MSEC(45));

	/*
	 * Read the power state back rather than trusting the write. On this controller a
	 * write that did nothing still reports success - see the file header.
	 */
	rc = bmi270_read(BMI270_REG_PWR_CTRL, &value, 1U);
	if (rc != 0) {
		return rc;
	}

	if ((value & BMI270_PWR_CTRL_ACC_GYR) != BMI270_PWR_CTRL_ACC_GYR) {
		LOG_ERR("sensor did not start, pwr_ctrl 0x%02x", value);
		return -EIO;
	}


	return 0;
}

int sensor_init(void)
{
	uint8_t value;
	int rc;

	unchanged_reads = 0;
	have_last_raw = false;
	last_err = SENSOR_ERR_NONE;

	memcpy(cal_trim, cal_trim_factory, sizeof(cal_trim));

	if (!device_is_ready(bmi270.bus)) {
		LOG_ERR("i2c bus not ready");
		return -ENODEV;
	}

	/*
	 * Configure the controller once and only once.
	 *
	 * This function is called again to recover a sensor that stopped answering, and
	 * the CC23xx controller refuses to be reconfigured once it is up - it returns
	 * -EBUSY and logs its own error at ERR level. Tolerating the -EBUSY works, but the
	 * driver's red line still lands in the log every retry, which on a device whose
	 * job is to be read by a human is a real cost. The settings never change, so just
	 * do not ask twice.
	 */
	static bool i2c_configured;

	if (!i2c_configured) {
		rc = i2c_configure(bmi270.bus,
				   I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));
		if (rc != 0) {
			LOG_ERR("i2c configure failed: %d", rc);
			return rc;
		}

		i2c_configured = true;
	}

	/*
	 * Probe, and do not take the first NAK for an answer.
	 *
	 * A warm reset - which is what the Spotflow SDK does after writing a coredump -
	 * restarts the CPU without restarting the sensor. If the reset lands in the middle
	 * of an I2C read, the BMI270 is left mid-transaction holding SDA down, and every
	 * transfer afterwards NAKs: the part is fine, the bus is wedged. i2c_recover_bus()
	 * clocks the stuck slave out of it. Drivers that do not implement it return
	 * -ENOSYS, which costs nothing.
	 *
	 * This is the difference between a device that comes back from its own crash and
	 * one that needs a human with a USB cable - which, for a tracker in a container,
	 * is the difference between a bug report and a dead unit.
	 */
	for (int attempt = 0; attempt <= BMI270_PROBE_RETRIES; attempt++) {
		rc = bmi270_read(BMI270_REG_CHIP_ID, &value, 1U);
		if (rc == 0) {
			break;
		}

		if (attempt == BMI270_PROBE_RETRIES) {
			LOG_ERR("no answer from sensor at 0x%02x after %d tries: %d",
				bmi270.addr, attempt + 1, rc);
			return rc;
		}

		/*
		 * i2c_recover_bus() is NULL on the CC23xx controller - the driver does not
		 * implement it - so this returns -ENOSYS and clocks nothing out. Say so
		 * rather than claiming a recovery that did not happen: if the bus is
		 * genuinely wedged, only a power cycle clears it. The retry below still
		 * helps when the sensor itself is slow or was briefly absent.
		 */
		int rec = i2c_recover_bus(bmi270.bus);

		LOG_WRN("sensor silent at 0x%02x (%d), bus recovery %s", bmi270.addr, rc,
			rec == 0 ? "done" : "unsupported - power cycle needed if wedged");
		k_sleep(K_MSEC(5));
	}

	if (value != BMI270_CHIP_ID) {
		LOG_ERR("unexpected chip id 0x%02x", value);
		return -ENODEV;
	}

	rc = bmi270_write_byte(BMI270_REG_CMD, BMI270_CMD_SOFT_RESET);
	if (rc != 0) {
		return rc;
	}
	k_sleep(K_MSEC(2));

	rc = bmi270_write_byte(BMI270_REG_PWR_CONF, 0x00);
	if (rc != 0) {
		return rc;
	}

	rc = bmi270_write_byte(BMI270_REG_INIT_CTRL, 0x00);
	if (rc != 0) {
		return rc;
	}

	rc = load_config_blob();
	if (rc != 0) {
		LOG_ERR("config load failed: %d", rc);
		return rc;
	}

	rc = bmi270_write_byte(BMI270_REG_INIT_CTRL, 0x01);
	if (rc != 0) {
		return rc;
	}

	for (int attempt = 0; attempt <= BMI270_INIT_RETRIES; attempt++) {
		rc = bmi270_read(BMI270_REG_INTERNAL_STATUS, &value, 1U);
		if (rc != 0) {
			return rc;
		}

		if ((value & 0x0f) == BMI270_INIT_STATUS_OK) {
			break;
		}

		if (attempt == BMI270_INIT_RETRIES) {
			/* The silent-write failure mode lands here. */
			LOG_ERR("sensor never initialised, status 0x%02x", value);
			return -EIO;
		}

		k_sleep(K_MSEC(10));
	}

	rc = start_sensor();
	if (rc != 0) {
		return rc;
	}

	LOG_INF("sensor ready (bmi270 at 0x%02x)", bmi270.addr);

	return 0;
}

/* Integer square root, so magnitude needs no floating point. */
static uint32_t isqrt(uint64_t value)
{
	uint64_t result = 0;
	uint64_t bit = 1ULL << 42;

	while (bit > value) {
		bit >>= 2;
	}

	while (bit != 0U) {
		if (value >= result + bit) {
			value -= result + bit;
			result = (result >> 1) + bit;
		} else {
			result >>= 1;
		}
		bit >>= 2;
	}

	return (uint32_t)result;
}


static int read_temperature(int16_t *temp_c_x10)
{
	uint8_t data[2];
	int rc = bmi270_read(BMI270_REG_TEMPERATURE, data, sizeof(data));

	if (rc != 0) {
		return rc;
	}

	int16_t raw = (int16_t)sys_get_le16(data);

	/* 0x8000 is the part's "no valid temperature" answer. */
	if ((uint16_t)raw == 0x8000U) {
		return -ENODATA;
	}

	/* 23 C + raw/512, in tenths of a degree. */
	*temp_c_x10 = (int16_t)(230 + ((int32_t)raw * 10) / 512);

	return 0;
}

/* Total acceleration in milli-g: about 1000 at rest, near 0 in free fall. */
static int read_magnitude_mg(uint16_t *magnitude_mg, uint8_t *raw_out)
{
	uint8_t data[6];
	int64_t sum_squares = 0;
	int rc = bmi270_read(BMI270_REG_ACC_DATA, data, sizeof(data));

	if (rc != 0) {
		return rc;
	}

	if (raw_out != NULL) {
		memcpy(raw_out, data, sizeof(data));
	}

	for (size_t axis = 0; axis < 3U; axis++) {
		int16_t raw = (int16_t)sys_get_le16(&data[axis * 2]);
		int32_t mg = (int32_t)(((int64_t)raw * ACC_FULL_SCALE_MG) / INT16_MAX);

		sum_squares += (int64_t)mg * mg;
	}

	*magnitude_mg = (uint16_t)MIN(isqrt((uint64_t)sum_squares), (uint32_t)UINT16_MAX);

	return 0;
}

int sensor_poll_magnitude_mg(uint16_t *magnitude_mg)
{
	/*
	 * Deliberately not the full sensor_read(): the shake detector calls this many
	 * times a second and only needs the six acceleration bytes, not temperature.
	 */
	return read_magnitude_mg(magnitude_mg, NULL);
}

static enum sensor_err classify(int rc)
{
	switch (rc) {
	case -EIO:
		return SENSOR_ERR_NAK;
	case -ETIMEDOUT:
		return SENSOR_ERR_TIMEOUT;
	case -ENODATA:
		return SENSOR_ERR_BADDATA;
	default:
		return SENSOR_ERR_NAK;
	}
}

int sensor_read(struct sensor_sample *out)
{
	uint16_t magnitude_mg;
	uint16_t motion_mg;
	uint8_t raw[6];
	int16_t temp_c_x10 = 0;
	int rc;

	last_err = SENSOR_ERR_NONE;

	rc = read_magnitude_mg(&magnitude_mg, raw);
	if (rc != 0) {
		last_err = classify(rc);
		LOG_DBG("motion read failed at 0x%02x: %d", bmi270.addr, rc);
		return rc;
	}

	/* Motion is how far from a resting 1 g the device is, in either direction. */
	motion_mg = (magnitude_mg > 1000U) ? (magnitude_mg - 1000U) : (1000U - magnitude_mg);

	rc = read_temperature(&temp_c_x10);
	if (rc != 0) {
		/* Not fatal: a sample without temperature is still a sample. */
		LOG_DBG("temperature unavailable: %d", rc);
		temp_c_x10 = 0;
	}


	out->motion_mg = motion_mg;
	out->temp_c_x10 = temp_c_x10;
	out->temp_valid = (rc == 0);
	out->moved = motion_mg >= MOTION_WAKE_THRESHOLD_MG;


	/*
	 * A powered-down part keeps answering with the same numbers. Catching that is
	 * the difference between "the device is fine" and "the device has been dead for
	 * a day and nobody noticed".
	 *
	 * Compare the RAW sample, not motion_mg. motion_mg is |magnitude - 1000| rounded
	 * to whole milli-g, so on a device that is simply sitting still it is a small
	 * integer - 0, 3, 4, 5 - and five consecutive reads landing on the same one is
	 * ordinary luck, not a fault. Measured in the office overnight: that fired every
	 * twenty to thirty minutes on a device nobody had touched, which for an asset
	 * tracker is the normal resting state and exactly the wrong thing to cry wolf
	 * about.
	 *
	 * The raw LSBs do not have that problem. A converting part dithers them; a
	 * stopped one repeats all six bytes exactly. That is the difference the check is
	 * actually trying to detect.
	 */
	if (have_last_raw && memcmp(raw, last_raw, sizeof(raw)) == 0) {
		unchanged_reads++;
		if (unchanged_reads >= STUCK_AFTER_IDENTICAL_READS) {
			last_err = SENSOR_ERR_STUCK;
		}
	} else {
		unchanged_reads = 0;
		memcpy(last_raw, raw, sizeof(raw));
		have_last_raw = true;
	}

	LOG_DBG("motion=%u mg temp=" TEMP_C_X10_FMT " C", motion_mg, TEMP_C_X10_ARGS(temp_c_x10));

	return 0;
}

enum sensor_err sensor_last_err(void)
{
	return last_err;
}

const char *sensor_err_str(enum sensor_err err)
{
	switch (err) {
	case SENSOR_ERR_NONE:
		return "none";
	case SENSOR_ERR_NAK:
		return "nak";
	case SENSOR_ERR_TIMEOUT:
		return "timeout";
	case SENSOR_ERR_BADDATA:
		return "baddata";
	case SENSOR_ERR_STUCK:
		return "stuck";
	default:
		return "unknown";
	}
}

const char *sensor_backend_name(void)
{
	return "bmi270";
}

