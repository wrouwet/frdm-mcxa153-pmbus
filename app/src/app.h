/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_H_
#define APP_H_

#include <stdbool.h>
#include <stdint.h>

#include <pmbus/device.h>

/* The one PMBus device. Thread code must hold irq_lock() while touching it. */
extern struct pmbus_device pmbus_dev;

/* Starts the I2C target on LPI2C0. */
int pmbus_i2c_start(void);
/* Drives SMBALERT#; safe in interrupt context. */
void pmbus_i2c_set_alert(bool asserted);
/* Counts of transactions, SMBus timeouts and interface resets. */
void pmbus_i2c_stats(uint32_t *timeouts);

/* Reads the stored config into RAM. Call before pmbus_device_load_stored(). */
int storage_init(void);
extern const struct pmbus_device_ops storage_device_ops;

/* Sensor overrides from the shell, for testing limits without a heat gun. */
struct temp_override {
	bool active;
	int32_t mc;
};
extern struct temp_override temp_override[2];

#endif /* APP_H_ */
