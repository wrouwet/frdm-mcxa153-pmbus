/*
 * A single-page PMBus device: a simulated 54 V to 12 V converter whose temperatures
 * come from real sensors.
 *
 * The model implements the commands, keeps limits and set points as the raw words the
 * host wrote (so reading back gives exactly what was written), latches status bits,
 * shuts the output down on faults, and drives SMBALERT#. Everything that touches
 * hardware or storage goes through struct pmbus_device_ops.
 *
 * Concurrency: the I2C side (struct pmbus_target) calls into the device from interrupt
 * context. Callers in thread context must keep that interrupt out while they call
 * pmbus_device_* functions, e.g. with irq_lock() on Zephyr.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PMBUS_DEVICE_H_
#define PMBUS_DEVICE_H_

#include <stdbool.h>
#include <stdint.h>

#include <pmbus/target.h>

#define PMBUS_MFR_STR_MAX 20

/* The limit registers, in the order struct pmbus_device_config stores them. */
enum pmbus_limit {
	PMBUS_LIM_VOUT_OV_FAULT,
	PMBUS_LIM_VOUT_OV_WARN,
	PMBUS_LIM_VOUT_UV_WARN,
	PMBUS_LIM_VOUT_UV_FAULT,
	PMBUS_LIM_IOUT_OC_FAULT,
	PMBUS_LIM_IOUT_OC_WARN,
	PMBUS_LIM_OT_FAULT,
	PMBUS_LIM_OT_WARN,
	PMBUS_LIM_UT_WARN,
	PMBUS_LIM_UT_FAULT,
	PMBUS_LIM_VIN_OV_FAULT,
	PMBUS_LIM_VIN_OV_WARN,
	PMBUS_LIM_VIN_UV_WARN,
	PMBUS_LIM_VIN_UV_FAULT,
	PMBUS_LIM_COUNT,
};

enum pmbus_mfr_str {
	PMBUS_MFR_ID_STR,
	PMBUS_MFR_MODEL_STR,
	PMBUS_MFR_REVISION_STR,
	PMBUS_MFR_LOCATION_STR,
	PMBUS_MFR_DATE_STR,
	PMBUS_MFR_SERIAL_STR,
	PMBUS_MFR_STR_COUNT,
};

struct pmbus_mfr_string {
	uint8_t len;
	char s[PMBUS_MFR_STR_MAX];
};

/* What STORE_DEFAULT_ALL saves and RESTORE_DEFAULT_ALL loads. */
struct pmbus_device_config {
	uint16_t version;
	uint8_t operation;
	uint8_t on_off_config;
	uint8_t write_protect;
	uint16_t vout_command;
	uint16_t vout_margin_high;
	uint16_t vout_margin_low;
	uint16_t limits[PMBUS_LIM_COUNT];
	struct pmbus_mfr_string mfr[PMBUS_MFR_STR_COUNT];
};

#define PMBUS_DEVICE_CONFIG_VERSION 1

struct pmbus_device_ops {
	/* SMBALERT#: true pulls the line low. */
	void (*set_alert)(void *ctx, bool asserted);
	/*
	 * STORE_DEFAULT_ALL. Called from interrupt context: copy the config and write it to
	 * flash later, then call pmbus_device_store_done().
	 */
	void (*store)(void *ctx, const struct pmbus_device_config *cfg);
	/* RESTORE_DEFAULT_ALL. Returns false if nothing was stored. Must be quick. */
	bool (*restore)(void *ctx, struct pmbus_device_config *cfg);
};

/* Readings from real sensors, in milli-degrees Celsius. */
struct pmbus_sensors {
	bool temp1_valid;
	int32_t temp1_mc;
	bool temp2_valid;
	int32_t temp2_mc;
};

/* The simulated electrical side, in milli-units. */
struct pmbus_sim {
	int32_t vin_mv;    /* input voltage */
	int32_t load_ma;   /* output current when the output is on */
	int32_t eff_permille; /* conversion efficiency */
};

struct pmbus_device {
	struct pmbus_target target;
	const struct pmbus_device_ops *ops;
	void *ops_ctx;

	struct pmbus_device_config cfg;
	bool has_alert_pin;
	bool control_pin; /* the CONTROL input; true means asserted */

	struct pmbus_sensors sensors;
	struct pmbus_sim sim;

	/* Live telemetry, refreshed by pmbus_device_update(). */
	int32_t vin_mv, iin_ma, vout_mv, iout_ma, pout_mw, pin_mw;
	bool output_on;
	bool fault_off; /* latched off by a fault until OPERATION turns the unit off */
	bool busy;

	/* Latched status registers. */
	uint8_t status_vout, status_iout, status_input, status_temp, status_cml, status_mfr;
	bool alert_pending;
};

/*
 * Sets the device up with the built-in defaults: VOUT 12 V, VIN 54 V, a 5 A load and
 * dummy identification strings. serial may be NULL.
 */
void pmbus_device_init(struct pmbus_device *dev, uint8_t addr, const struct pmbus_device_ops *ops,
		       void *ops_ctx, bool has_alert_pin, const char *serial);

/* Loads the stored config, if there is one. Call once after pmbus_device_init(). */
void pmbus_device_load_stored(struct pmbus_device *dev);

/* Takes new sensor readings, recomputes telemetry and evaluates the limits. */
void pmbus_device_update(struct pmbus_device *dev, const struct pmbus_sensors *sensors);

/* Called once a store that pmbus_device_ops.store started has finished. */
void pmbus_device_store_done(struct pmbus_device *dev, bool ok);

void pmbus_device_set_control_pin(struct pmbus_device *dev, bool asserted);

uint8_t pmbus_device_status_byte(const struct pmbus_device *dev);
uint16_t pmbus_device_status_word(const struct pmbus_device *dev);
void pmbus_device_clear_faults(struct pmbus_device *dev);

/* VOUT_MODE is fixed: linear, exponent -9 (1.95 mV steps, up to 128 V). */
#define PMBUS_DEVICE_VOUT_MODE 0x17

#endif /* PMBUS_DEVICE_H_ */
