/*
 * Shell commands on the debug UART for watching the device and steering the
 * simulation by hand.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <pmbus/pmbus.h>

#include "app.h"

/* Prints a milli-unit value as a decimal with three places. */
#define MILLI(v) ((v) < 0 ? "-" : ""), abs((v) / 1000), abs((v) % 1000)

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct pmbus_device d;
	uint32_t timeouts;
	unsigned int key;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	key = irq_lock();
	d = pmbus_dev;
	irq_unlock(key);
	pmbus_i2c_stats(&timeouts);

	shell_print(sh, "address 0x%02x, output %s%s", d.target.addr, d.output_on ? "on" : "off",
		    d.fault_off ? " (latched off by a fault)" : "");
	shell_print(sh, "VIN  %s%d.%03d V   IIN  %s%d.%03d A   PIN  %s%d.%03d W", MILLI(d.vin_mv),
		    MILLI(d.iin_ma), MILLI(d.pin_mw));
	shell_print(sh, "VOUT %s%d.%03d V   IOUT %s%d.%03d A   POUT %s%d.%03d W",
		    MILLI(d.vout_mv), MILLI(d.iout_ma), MILLI(d.pout_mw));
	shell_print(sh, "TEMP1 %s%s%d.%03d C   TEMP2 %s%s%d.%03d C",
		    d.sensors.temp1_valid ? "" : "(missing) ", MILLI(d.sensors.temp1_mc),
		    d.sensors.temp2_valid ? "" : "(missing) ", MILLI(d.sensors.temp2_mc));
	shell_print(sh, "STATUS_WORD 0x%04x  VOUT 0x%02x  IOUT 0x%02x  INPUT 0x%02x  "
		    "TEMPERATURE 0x%02x  CML 0x%02x",
		    pmbus_device_status_word(&d), d.status_vout, d.status_iout, d.status_input,
		    d.status_temp, d.status_cml);
	shell_print(sh, "SMBALERT# %s, transactions %u, PEC errors %u, aborts %u, timeouts %u",
		    d.alert_pending ? "asserted" : "released", d.target.n_transactions,
		    d.target.n_pec_errors, d.target.n_aborts, timeouts);
	return 0;
}

static int cmd_load(const struct shell *sh, size_t argc, char **argv)
{
	unsigned int key = irq_lock();

	ARG_UNUSED(argc);
	pmbus_dev.sim.load_ma = (int32_t)strtol(argv[1], NULL, 0);
	pmbus_device_update(&pmbus_dev, NULL);
	irq_unlock(key);
	shell_print(sh, "load %s mA", argv[1]);
	return 0;
}

static int cmd_vin(const struct shell *sh, size_t argc, char **argv)
{
	unsigned int key = irq_lock();

	ARG_UNUSED(argc);
	pmbus_dev.sim.vin_mv = (int32_t)strtol(argv[1], NULL, 0);
	pmbus_device_update(&pmbus_dev, NULL);
	irq_unlock(key);
	shell_print(sh, "VIN %s mV", argv[1]);
	return 0;
}

static int cmd_temp(const struct shell *sh, size_t argc, char **argv)
{
	long i = strtol(argv[1], NULL, 0);

	ARG_UNUSED(argc);
	if (i != 1 && i != 2) {
		shell_error(sh, "sensor is 1 or 2");
		return -EINVAL;
	}
	if (strcmp(argv[2], "off") == 0) {
		temp_override[i - 1].active = false;
		shell_print(sh, "temperature %ld: real sensor", i);
	} else {
		temp_override[i - 1].mc = (int32_t)strtol(argv[2], NULL, 0);
		temp_override[i - 1].active = true;
		shell_print(sh, "temperature %ld: %s m°C until 'pmbus temp %ld off'", i, argv[2],
			    i);
	}
	return 0;
}

static int cmd_control(const struct shell *sh, size_t argc, char **argv)
{
	unsigned int key = irq_lock();

	ARG_UNUSED(argc);
	pmbus_device_set_control_pin(&pmbus_dev, strtol(argv[1], NULL, 0) != 0);
	irq_unlock(key);
	shell_print(sh, "CONTROL %s", argv[1]);
	return 0;
}

static int cmd_clear(const struct shell *sh, size_t argc, char **argv)
{
	unsigned int key = irq_lock();

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	pmbus_device_clear_faults(&pmbus_dev);
	irq_unlock(key);
	shell_print(sh, "faults cleared");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(pmbus_cmds,
	SHELL_CMD(status, NULL, "Telemetry, status registers and counters", cmd_status),
	SHELL_CMD_ARG(load, NULL, "<mA>  Simulated load current", cmd_load, 2, 0),
	SHELL_CMD_ARG(vin, NULL, "<mV>  Simulated input voltage", cmd_vin, 2, 0),
	SHELL_CMD_ARG(temp, NULL, "<1|2> <m°C|off>  Override a temperature sensor",
		      cmd_temp, 3, 0),
	SHELL_CMD_ARG(control, NULL, "<0|1>  The CONTROL input (see ON_OFF_CONFIG)",
		      cmd_control, 2, 0),
	SHELL_CMD(clear, NULL, "CLEAR_FAULTS", cmd_clear),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(pmbus, &pmbus_cmds, "PMBus target", NULL);
