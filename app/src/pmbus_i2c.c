/*
 * Glue between Zephyr's I2C target API (LPI2C0) and the PMBus target engine.
 *
 * The Zephyr LPI2C driver reports every byte from its interrupt handler, which is what
 * the engine expects. This file adds what the driver lacks for SMBus:
 *  - a timeout: a transaction that makes no progress for CONFIG_PMBUS_SMBUS_TIMEOUT_MS
 *    resets the target interface, releasing SDA if a host vanished mid-read;
 *  - the Alert Response Address: while SMBALERT# is asserted, the LPI2C also answers
 *    0x0C (SCFGR1[SAEN]), and the engine replies with this device's address.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <soc.h>

#include <pmbus/pmbus.h>

#include "app.h"

LOG_MODULE_REGISTER(pmbus_i2c, LOG_LEVEL_INF);

#define I2C_NODE DT_NODELABEL(lpi2c0)

static const struct device *const i2c_dev = DEVICE_DT_GET(I2C_NODE);
static LPI2C_Type *const lpi2c = (LPI2C_Type *)DT_REG_ADDR(I2C_NODE);

#define USER_NODE DT_PATH(zephyr_user)
static const struct gpio_dt_spec alert_gpio = GPIO_DT_SPEC_GET_OR(USER_NODE, smbalert_gpios, {0});

static struct i2c_target_config target_cfg;
static uint32_t n_timeouts;
static atomic_t alert_wanted;

static void timeout_expired(struct k_timer *timer);
static void reset_work_fn(struct k_work *work);
static void ara_work_fn(struct k_work *work);

static K_TIMER_DEFINE(timeout_timer, timeout_expired, NULL);
static K_WORK_DEFINE(reset_work, reset_work_fn);
static K_WORK_DELAYABLE_DEFINE(ara_work, ara_work_fn);

static inline void progress(void)
{
	k_timer_start(&timeout_timer, K_MSEC(CONFIG_PMBUS_SMBUS_TIMEOUT_MS), K_NO_WAIT);
}

static int on_write_requested(struct i2c_target_config *cfg)
{
	ARG_UNUSED(cfg);
	progress();
	pmbus_target_write_start(&pmbus_dev.target);
	return 0;
}

static int on_write_received(struct i2c_target_config *cfg, uint8_t val)
{
	ARG_UNUSED(cfg);
	progress();
	return pmbus_target_write_byte(&pmbus_dev.target, val) ? 0 : -EIO;
}

/*
 * Says whether this read is to the Alert Response Address rather than our own. The driver
 * has already read SASR; RADDR keeps the matched address (8-bit form) unless ANV says it
 * is stale, in which case fall back on context: a read with no command written first,
 * while alerting, is taken as the alert response.
 */
static bool is_alert_response_read(void)
{
	uint32_t sasr = lpi2c->SASR;

	if (!(sasr & LPI2C_SASR_ANV_MASK)) {
		return ((sasr & LPI2C_SASR_RADDR_MASK) >> 1) == SMBUS_ALERT_RESPONSE_ADDR;
	}
	return atomic_get(&alert_wanted) && pmbus_dev.target.rx_len == 0;
}

static int on_read_requested(struct i2c_target_config *cfg, uint8_t *val)
{
	ARG_UNUSED(cfg);
	progress();
	if (is_alert_response_read()) {
		*val = pmbus_target_ara_start(&pmbus_dev.target);
	} else {
		*val = pmbus_target_read_start(&pmbus_dev.target);
	}
	return 0;
}

static int on_read_processed(struct i2c_target_config *cfg, uint8_t *val)
{
	ARG_UNUSED(cfg);
	progress();
	*val = pmbus_target_read_byte(&pmbus_dev.target);
	return 0;
}

static int on_stop(struct i2c_target_config *cfg)
{
	ARG_UNUSED(cfg);
	k_timer_stop(&timeout_timer);
	pmbus_target_stop(&pmbus_dev.target);
	return 0;
}

static const struct i2c_target_callbacks callbacks = {
	.write_requested = on_write_requested,
	.write_received = on_write_received,
	.read_requested = on_read_requested,
	.read_processed = on_read_processed,
	.stop = on_stop,
};

/* SCFGR1 may only change while the target is disabled. */
static void set_alert_response(bool enable)
{
	uint32_t scr = lpi2c->SCR;

	lpi2c->SCR = scr & ~LPI2C_SCR_SEN_MASK;
	if (enable) {
		lpi2c->SCFGR1 |= LPI2C_SCFGR1_SAEN_MASK;
	} else {
		lpi2c->SCFGR1 &= ~LPI2C_SCFGR1_SAEN_MASK;
	}
	lpi2c->SCR = scr;
}

static void ara_work_fn(struct k_work *work)
{
	/* Wait for a quiet bus, so the switch can't cut into a transaction. */
	if (lpi2c->SSR & (LPI2C_SSR_SBF_MASK | LPI2C_SSR_BBF_MASK)) {
		k_work_reschedule(k_work_delayable_from_work(work), K_MSEC(1));
		return;
	}
	set_alert_response(atomic_get(&alert_wanted) != 0);
}

void pmbus_i2c_set_alert(bool asserted)
{
	if (alert_gpio.port != NULL) {
		gpio_pin_set_dt(&alert_gpio, asserted);
	}
	atomic_set(&alert_wanted, asserted);
	k_work_reschedule(&ara_work, K_NO_WAIT);
}

static int target_register(void)
{
	int ret = i2c_target_register(i2c_dev, &target_cfg);

	if (ret == 0 && atomic_get(&alert_wanted)) {
		set_alert_response(true);
	}
	return ret;
}

static void reset_work_fn(struct k_work *work)
{
	unsigned int key;

	ARG_UNUSED(work);
	LOG_WRN("SMBus timeout, resetting the target interface");
	key = irq_lock();
	pmbus_target_abort(&pmbus_dev.target);
	irq_unlock(key);
	i2c_target_unregister(i2c_dev, &target_cfg);
	if (target_register() != 0) {
		LOG_ERR("re-registering the I2C target failed");
	}
}

static void timeout_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	n_timeouts++;
	k_work_submit(&reset_work);
}

void pmbus_i2c_stats(uint32_t *timeouts)
{
	*timeouts = n_timeouts;
}

int pmbus_i2c_start(void)
{
	int ret;

	if (!device_is_ready(i2c_dev)) {
		LOG_ERR("LPI2C0 not ready");
		return -ENODEV;
	}
	if (alert_gpio.port != NULL) {
		ret = gpio_pin_configure_dt(&alert_gpio, GPIO_OUTPUT_INACTIVE);
		if (ret != 0) {
			LOG_ERR("SMBALERT# pin: %d", ret);
			return ret;
		}
	}

	target_cfg.address = CONFIG_PMBUS_ADDRESS;
	target_cfg.callbacks = &callbacks;
	ret = target_register();
	if (ret != 0) {
		LOG_ERR("i2c_target_register: %d", ret);
		return ret;
	}
	LOG_INF("PMBus target at 0x%02x", CONFIG_PMBUS_ADDRESS);
	return 0;
}
