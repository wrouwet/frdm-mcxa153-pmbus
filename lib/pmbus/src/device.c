/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <pmbus/device.h>
#include <pmbus/format.h>
#include <pmbus/pmbus.h>

#define VOUT_EXP (-9)
#define V(mv)    pmbus_ulinear16_encode((mv), VOUT_EXP)
#define L(milli) pmbus_linear11_encode(milli)

/* Standby power drawn from the input while the output is off. */
#define STANDBY_PIN_MW 500

static const struct pmbus_device_config default_config = {
	.version = PMBUS_DEVICE_CONFIG_VERSION,
	.operation = PMBUS_OPERATION_ON,
	.on_off_config = PMBUS_ON_OFF_PU | PMBUS_ON_OFF_CMD,
	.write_protect = 0,
	.mfr = {
		[PMBUS_MFR_ID_STR] = {7, "EXAMPLE"},
		[PMBUS_MFR_MODEL_STR] = {13, "MCXA153-PMB-1"},
		[PMBUS_MFR_REVISION_STR] = {2, "A0"},
		[PMBUS_MFR_LOCATION_STR] = {3, "LAB"},
		[PMBUS_MFR_DATE_STR] = {6, "261122"},
		[PMBUS_MFR_SERIAL_STR] = {8, "00000000"},
	},
};

static void set_default_words(struct pmbus_device_config *c)
{
	c->vout_command = V(12000);
	c->vout_margin_high = V(12600);
	c->vout_margin_low = V(11400);
	c->limits[PMBUS_LIM_VOUT_OV_FAULT] = V(13800);
	c->limits[PMBUS_LIM_VOUT_OV_WARN] = V(13200);
	c->limits[PMBUS_LIM_VOUT_UV_WARN] = V(10800);
	c->limits[PMBUS_LIM_VOUT_UV_FAULT] = V(10200);
	c->limits[PMBUS_LIM_IOUT_OC_FAULT] = L(10000);
	c->limits[PMBUS_LIM_IOUT_OC_WARN] = L(8000);
	c->limits[PMBUS_LIM_OT_FAULT] = L(100000);
	c->limits[PMBUS_LIM_OT_WARN] = L(85000);
	c->limits[PMBUS_LIM_UT_WARN] = L(-20000);
	c->limits[PMBUS_LIM_UT_FAULT] = L(-40000);
	c->limits[PMBUS_LIM_VIN_OV_FAULT] = L(60000);
	c->limits[PMBUS_LIM_VIN_OV_WARN] = L(58000);
	c->limits[PMBUS_LIM_VIN_UV_WARN] = L(44000);
	c->limits[PMBUS_LIM_VIN_UV_FAULT] = L(40000);
}

static int32_t vout_limit(const struct pmbus_device *dev, enum pmbus_limit l)
{
	return pmbus_ulinear16_decode(dev->cfg.limits[l], VOUT_EXP);
}

static int32_t lin_limit(const struct pmbus_device *dev, enum pmbus_limit l)
{
	return pmbus_linear11_decode(dev->cfg.limits[l]);
}

/* --- Status and alerts --- */

static bool any_status(const struct pmbus_device *dev)
{
	return dev->status_vout || dev->status_iout || dev->status_input || dev->status_temp ||
	       dev->status_cml || dev->status_mfr;
}

static void latch(struct pmbus_device *dev, uint8_t *reg, uint8_t bits)
{
	if (bits & ~*reg) {
		*reg |= bits;
		if (dev->has_alert_pin) {
			dev->alert_pending = true;
			dev->ops->set_alert(dev->ops_ctx, true);
		}
	}
}

static void release_alert_if_clear(struct pmbus_device *dev)
{
	if (dev->alert_pending && !any_status(dev)) {
		dev->alert_pending = false;
		dev->ops->set_alert(dev->ops_ctx, false);
	}
}

void pmbus_device_clear_faults(struct pmbus_device *dev)
{
	dev->status_vout = 0;
	dev->status_iout = 0;
	dev->status_input = 0;
	dev->status_temp = 0;
	dev->status_cml = 0;
	dev->status_mfr = 0;
	release_alert_if_clear(dev);
}

static bool power_good(const struct pmbus_device *dev)
{
	return dev->output_on && dev->vout_mv >= vout_limit(dev, PMBUS_LIM_VOUT_UV_FAULT);
}

uint8_t pmbus_device_status_byte(const struct pmbus_device *dev)
{
	uint8_t sb = 0;

	if (dev->busy) {
		sb |= PMBUS_SB_BUSY;
	}
	if (!dev->output_on) {
		sb |= PMBUS_SB_OFF;
	}
	if (dev->status_vout & PMBUS_SV_OV_FAULT) {
		sb |= PMBUS_SB_VOUT_OV_FAULT;
	}
	if (dev->status_iout & PMBUS_SI_OC_FAULT) {
		sb |= PMBUS_SB_IOUT_OC_FAULT;
	}
	if (dev->status_input & PMBUS_SIN_UV_FAULT) {
		sb |= PMBUS_SB_VIN_UV_FAULT;
	}
	if (dev->status_temp) {
		sb |= PMBUS_SB_TEMPERATURE;
	}
	if (dev->status_cml) {
		sb |= PMBUS_SB_CML;
	}
	if ((dev->status_vout & ~PMBUS_SV_OV_FAULT) || (dev->status_iout & ~PMBUS_SI_OC_FAULT) ||
	    (dev->status_input & ~PMBUS_SIN_UV_FAULT) || dev->status_mfr) {
		sb |= PMBUS_SB_NONE_OF_ABOVE;
	}
	return sb;
}

uint16_t pmbus_device_status_word(const struct pmbus_device *dev)
{
	uint16_t sw = pmbus_device_status_byte(dev);

	if (dev->status_vout) {
		sw |= PMBUS_SW_VOUT;
	}
	if (dev->status_iout) {
		sw |= PMBUS_SW_IOUT_POUT;
	}
	if (dev->status_input) {
		sw |= PMBUS_SW_INPUT;
	}
	if (dev->status_mfr) {
		sw |= PMBUS_SW_MFR;
	}
	if (!power_good(dev)) {
		sw |= PMBUS_SW_POWER_GOOD_N;
	}
	return sw;
}

/* --- The simulated converter --- */

static bool commanded_on(const struct pmbus_device *dev)
{
	uint8_t c = dev->cfg.on_off_config;

	if (!(c & PMBUS_ON_OFF_PU)) {
		return true;
	}
	if ((c & PMBUS_ON_OFF_CMD) && !(dev->cfg.operation & PMBUS_OPERATION_ON)) {
		return false;
	}
	if ((c & PMBUS_ON_OFF_CPR) && !dev->control_pin) {
		return false;
	}
	return (c & (PMBUS_ON_OFF_CMD | PMBUS_ON_OFF_CPR)) != 0;
}

static int32_t setpoint_mv(const struct pmbus_device *dev)
{
	uint16_t w;

	switch (dev->cfg.operation & PMBUS_OPERATION_MARGIN_MASK) {
	case PMBUS_OPERATION_MARGIN_LOW:
		w = dev->cfg.vout_margin_low;
		break;
	case PMBUS_OPERATION_MARGIN_HIGH:
		w = dev->cfg.vout_margin_high;
		break;
	default:
		w = dev->cfg.vout_command;
		break;
	}
	return pmbus_ulinear16_decode(w, VOUT_EXP);
}

static void compute_electricals(struct pmbus_device *dev)
{
	bool vin_ok = dev->sim.vin_mv >= lin_limit(dev, PMBUS_LIM_VIN_UV_FAULT);

	dev->output_on = commanded_on(dev) && !dev->fault_off && vin_ok;
	dev->vin_mv = dev->sim.vin_mv;
	if (dev->output_on) {
		dev->vout_mv = setpoint_mv(dev);
		dev->iout_ma = dev->sim.load_ma;
		dev->pout_mw = (int32_t)((int64_t)dev->vout_mv * dev->iout_ma / 1000);
		dev->pin_mw = (int32_t)((int64_t)dev->pout_mw * 1000 / dev->sim.eff_permille) +
			      STANDBY_PIN_MW;
	} else {
		dev->vout_mv = 0;
		dev->iout_ma = 0;
		dev->pout_mw = 0;
		dev->pin_mw = STANDBY_PIN_MW;
	}
	dev->iin_ma = (dev->vin_mv > 0) ? (int32_t)((int64_t)dev->pin_mw * 1000 / dev->vin_mv) : 0;
}

static void evaluate_limits(struct pmbus_device *dev)
{
	bool shutdown = false;

	if (dev->output_on) {
		if (dev->vout_mv > vout_limit(dev, PMBUS_LIM_VOUT_OV_FAULT)) {
			latch(dev, &dev->status_vout, PMBUS_SV_OV_FAULT);
			shutdown = true;
		} else if (dev->vout_mv > vout_limit(dev, PMBUS_LIM_VOUT_OV_WARN)) {
			latch(dev, &dev->status_vout, PMBUS_SV_OV_WARN);
		}
		if (dev->vout_mv < vout_limit(dev, PMBUS_LIM_VOUT_UV_FAULT)) {
			latch(dev, &dev->status_vout, PMBUS_SV_UV_FAULT);
			shutdown = true;
		} else if (dev->vout_mv < vout_limit(dev, PMBUS_LIM_VOUT_UV_WARN)) {
			latch(dev, &dev->status_vout, PMBUS_SV_UV_WARN);
		}
		if (dev->iout_ma > lin_limit(dev, PMBUS_LIM_IOUT_OC_FAULT)) {
			latch(dev, &dev->status_iout, PMBUS_SI_OC_FAULT);
			shutdown = true;
		} else if (dev->iout_ma > lin_limit(dev, PMBUS_LIM_IOUT_OC_WARN)) {
			latch(dev, &dev->status_iout, PMBUS_SI_OC_WARN);
		}
	}

	if (dev->vin_mv > lin_limit(dev, PMBUS_LIM_VIN_OV_FAULT)) {
		latch(dev, &dev->status_input, PMBUS_SIN_OV_FAULT);
		shutdown = true;
	} else if (dev->vin_mv > lin_limit(dev, PMBUS_LIM_VIN_OV_WARN)) {
		latch(dev, &dev->status_input, PMBUS_SIN_OV_WARN);
	}
	if (dev->vin_mv < lin_limit(dev, PMBUS_LIM_VIN_UV_FAULT)) {
		/* Under-voltage lockout: the output stays off until VIN recovers. */
		latch(dev, &dev->status_input, PMBUS_SIN_UV_FAULT | PMBUS_SIN_OFF_LOW_IN);
	} else if (dev->vin_mv < lin_limit(dev, PMBUS_LIM_VIN_UV_WARN)) {
		latch(dev, &dev->status_input, PMBUS_SIN_UV_WARN);
	}

	/* Temperature limits apply to the hottest (and coldest) valid sensor. */
	bool any_temp = false;
	int32_t hot = 0, cold = 0;

	if (dev->sensors.temp1_valid) {
		hot = cold = dev->sensors.temp1_mc;
		any_temp = true;
	}
	if (dev->sensors.temp2_valid) {
		if (!any_temp || dev->sensors.temp2_mc > hot) {
			hot = dev->sensors.temp2_mc;
		}
		if (!any_temp || dev->sensors.temp2_mc < cold) {
			cold = dev->sensors.temp2_mc;
		}
		any_temp = true;
	}
	if (any_temp) {
		if (hot > lin_limit(dev, PMBUS_LIM_OT_FAULT)) {
			latch(dev, &dev->status_temp, PMBUS_ST_OT_FAULT);
			shutdown = true;
		} else if (hot > lin_limit(dev, PMBUS_LIM_OT_WARN)) {
			latch(dev, &dev->status_temp, PMBUS_ST_OT_WARN);
		}
		if (cold < lin_limit(dev, PMBUS_LIM_UT_FAULT)) {
			latch(dev, &dev->status_temp, PMBUS_ST_UT_FAULT);
		} else if (cold < lin_limit(dev, PMBUS_LIM_UT_WARN)) {
			latch(dev, &dev->status_temp, PMBUS_ST_UT_WARN);
		}
	}

	if (shutdown && dev->output_on) {
		/* Latch off, as most converters do by default. OPERATION off, then on, restarts. */
		dev->fault_off = true;
		compute_electricals(dev);
	}
}

void pmbus_device_update(struct pmbus_device *dev, const struct pmbus_sensors *sensors)
{
	if (sensors != NULL) {
		dev->sensors = *sensors;
	}
	compute_electricals(dev);
	evaluate_limits(dev);
}

void pmbus_device_set_control_pin(struct pmbus_device *dev, bool asserted)
{
	dev->control_pin = asserted;
	pmbus_device_update(dev, NULL);
}

void pmbus_device_store_done(struct pmbus_device *dev, bool ok)
{
	dev->busy = false;
	if (!ok) {
		latch(dev, &dev->status_cml, PMBUS_CML_MEMORY_FAULT);
	}
}

/* --- Command handlers --- */

static void put_word(uint8_t *buf, uint16_t w)
{
	buf[0] = (uint8_t)w;
	buf[1] = (uint8_t)(w >> 8);
}

static uint16_t get_word(const uint8_t *buf)
{
	return (uint16_t)(buf[0] | (buf[1] << 8));
}

static uint16_t *word_reg(struct pmbus_device *dev, uint8_t code)
{
	switch (code) {
	case PMBUS_VOUT_COMMAND:
		return &dev->cfg.vout_command;
	case PMBUS_VOUT_MARGIN_HIGH:
		return &dev->cfg.vout_margin_high;
	case PMBUS_VOUT_MARGIN_LOW:
		return &dev->cfg.vout_margin_low;
	case PMBUS_VOUT_OV_FAULT_LIMIT:
		return &dev->cfg.limits[PMBUS_LIM_VOUT_OV_FAULT];
	case PMBUS_VOUT_OV_WARN_LIMIT:
		return &dev->cfg.limits[PMBUS_LIM_VOUT_OV_WARN];
	case PMBUS_VOUT_UV_WARN_LIMIT:
		return &dev->cfg.limits[PMBUS_LIM_VOUT_UV_WARN];
	case PMBUS_VOUT_UV_FAULT_LIMIT:
		return &dev->cfg.limits[PMBUS_LIM_VOUT_UV_FAULT];
	case PMBUS_IOUT_OC_FAULT_LIMIT:
		return &dev->cfg.limits[PMBUS_LIM_IOUT_OC_FAULT];
	case PMBUS_IOUT_OC_WARN_LIMIT:
		return &dev->cfg.limits[PMBUS_LIM_IOUT_OC_WARN];
	case PMBUS_OT_FAULT_LIMIT:
		return &dev->cfg.limits[PMBUS_LIM_OT_FAULT];
	case PMBUS_OT_WARN_LIMIT:
		return &dev->cfg.limits[PMBUS_LIM_OT_WARN];
	case PMBUS_UT_WARN_LIMIT:
		return &dev->cfg.limits[PMBUS_LIM_UT_WARN];
	case PMBUS_UT_FAULT_LIMIT:
		return &dev->cfg.limits[PMBUS_LIM_UT_FAULT];
	case PMBUS_VIN_OV_FAULT_LIMIT:
		return &dev->cfg.limits[PMBUS_LIM_VIN_OV_FAULT];
	case PMBUS_VIN_OV_WARN_LIMIT:
		return &dev->cfg.limits[PMBUS_LIM_VIN_OV_WARN];
	case PMBUS_VIN_UV_WARN_LIMIT:
		return &dev->cfg.limits[PMBUS_LIM_VIN_UV_WARN];
	case PMBUS_VIN_UV_FAULT_LIMIT:
		return &dev->cfg.limits[PMBUS_LIM_VIN_UV_FAULT];
	default:
		return NULL;
	}
}

static uint8_t *status_reg(struct pmbus_device *dev, uint8_t code)
{
	switch (code) {
	case PMBUS_STATUS_VOUT:
		return &dev->status_vout;
	case PMBUS_STATUS_IOUT:
		return &dev->status_iout;
	case PMBUS_STATUS_INPUT:
		return &dev->status_input;
	case PMBUS_STATUS_TEMPERATURE:
		return &dev->status_temp;
	case PMBUS_STATUS_CML:
		return &dev->status_cml;
	case PMBUS_STATUS_MFR_SPECIFIC:
		return &dev->status_mfr;
	default:
		return NULL;
	}
}

static int read_byte(void *ctx, uint8_t code, const uint8_t *in, size_t in_len, uint8_t *buf,
		     size_t max)
{
	struct pmbus_device *dev = ctx;
	uint8_t *reg;

	(void)in;
	(void)in_len;
	(void)max;

	switch (code) {
	case PMBUS_PAGE:
		buf[0] = 0;
		break;
	case PMBUS_OPERATION:
		buf[0] = dev->cfg.operation;
		break;
	case PMBUS_ON_OFF_CONFIG:
		buf[0] = dev->cfg.on_off_config;
		break;
	case PMBUS_WRITE_PROTECT:
		buf[0] = dev->cfg.write_protect;
		break;
	case PMBUS_CAPABILITY:
		buf[0] = PMBUS_CAP_PEC | PMBUS_CAP_400KHZ |
			 (dev->has_alert_pin ? PMBUS_CAP_SMBALERT : 0);
		break;
	case PMBUS_VOUT_MODE:
		buf[0] = PMBUS_DEVICE_VOUT_MODE;
		break;
	case PMBUS_STATUS_BYTE:
		buf[0] = pmbus_device_status_byte(dev);
		break;
	case PMBUS_PMBUS_REVISION:
		buf[0] = PMBUS_REVISION_1_3;
		break;
	default:
		reg = status_reg(dev, code);
		if (reg == NULL) {
			return -1;
		}
		buf[0] = *reg;
		break;
	}
	return 1;
}

static int read_word(void *ctx, uint8_t code, const uint8_t *in, size_t in_len, uint8_t *buf,
		     size_t max)
{
	struct pmbus_device *dev = ctx;
	uint16_t *reg;
	uint16_t w;

	(void)in;
	(void)in_len;
	(void)max;

	switch (code) {
	case PMBUS_STATUS_WORD:
		w = pmbus_device_status_word(dev);
		break;
	case PMBUS_READ_VIN:
		w = L(dev->vin_mv);
		break;
	case PMBUS_READ_IIN:
		w = L(dev->iin_ma);
		break;
	case PMBUS_READ_VOUT:
		w = V(dev->vout_mv);
		break;
	case PMBUS_READ_IOUT:
		w = L(dev->iout_ma);
		break;
	case PMBUS_READ_TEMPERATURE_1:
		w = L(dev->sensors.temp1_mc);
		break;
	case PMBUS_READ_TEMPERATURE_2:
		w = L(dev->sensors.temp2_mc);
		break;
	case PMBUS_READ_POUT:
		w = L(dev->pout_mw);
		break;
	case PMBUS_READ_PIN:
		w = L(dev->pin_mw);
		break;
	case PMBUS_MFR_SPECIFIC_00:
		w = L(dev->sim.load_ma);
		break;
	case PMBUS_MFR_SPECIFIC_01:
		w = L(dev->sim.vin_mv);
		break;
	default:
		reg = word_reg(dev, code);
		if (reg == NULL) {
			return -1;
		}
		w = *reg;
		break;
	}
	put_word(buf, w);
	return 2;
}

static int read_block(void *ctx, uint8_t code, const uint8_t *in, size_t in_len, uint8_t *buf,
		      size_t max)
{
	struct pmbus_device *dev = ctx;
	const struct pmbus_mfr_string *s = &dev->cfg.mfr[code - PMBUS_MFR_ID];

	(void)in;
	(void)in_len;

	if (s->len > max) {
		return -1;
	}
	memcpy(buf, s->s, s->len);
	return s->len;
}

static const struct pmbus_cmd *lookup(void *ctx, uint8_t code);

static int read_query(void *ctx, uint8_t code, const uint8_t *in, size_t in_len, uint8_t *buf,
		      size_t max)
{
	const struct pmbus_cmd *cmd;

	(void)code;
	(void)max;

	if (in_len != 1) {
		return -1;
	}
	cmd = lookup(ctx, in[0]);
	buf[0] = 0;
	if (cmd != NULL) {
		buf[0] = PMBUS_QUERY_SUPPORTED | cmd->query;
		if (cmd->access & PMBUS_A_W) {
			buf[0] |= PMBUS_QUERY_WRITE;
		}
		if (cmd->access & PMBUS_A_R) {
			buf[0] |= PMBUS_QUERY_READ;
		}
	}
	return 1;
}

static int write_cmd(void *ctx, uint8_t code, const uint8_t *data, size_t len)
{
	struct pmbus_device *dev = ctx;
	struct pmbus_device_config stored;
	uint16_t *wreg;
	uint8_t *sreg;

	switch (code) {
	case PMBUS_PAGE:
		return (data[0] == 0 || data[0] == 0xFF) ? 0 : -1;
	case PMBUS_OPERATION:
		if (!(data[0] & PMBUS_OPERATION_ON)) {
			dev->fault_off = false;
		}
		dev->cfg.operation = data[0];
		break;
	case PMBUS_ON_OFF_CONFIG:
		dev->cfg.on_off_config = data[0] & 0x1F;
		break;
	case PMBUS_CLEAR_FAULTS:
		pmbus_device_clear_faults(dev);
		return 0;
	case PMBUS_WRITE_PROTECT:
		if (data[0] != 0 && data[0] != PMBUS_WP_ALL && data[0] != PMBUS_WP_OP &&
		    data[0] != PMBUS_WP_VOUT_CMD) {
			return -1;
		}
		dev->cfg.write_protect = data[0];
		return 0;
	case PMBUS_STORE_DEFAULT_ALL:
		dev->busy = true;
		dev->ops->store(dev->ops_ctx, &dev->cfg);
		return 0;
	case PMBUS_RESTORE_DEFAULT_ALL:
		if (!dev->ops->restore(dev->ops_ctx, &stored) ||
		    stored.version != PMBUS_DEVICE_CONFIG_VERSION) {
			stored = default_config;
			set_default_words(&stored);
			stored.mfr[PMBUS_MFR_SERIAL_STR] = dev->cfg.mfr[PMBUS_MFR_SERIAL_STR];
		}
		dev->cfg = stored;
		dev->fault_off = false;
		break;
	case PMBUS_MFR_ID:
	case PMBUS_MFR_MODEL:
	case PMBUS_MFR_REVISION:
	case PMBUS_MFR_LOCATION:
	case PMBUS_MFR_DATE:
	case PMBUS_MFR_SERIAL:
		if (len > PMBUS_MFR_STR_MAX) {
			return -1;
		}
		dev->cfg.mfr[code - PMBUS_MFR_ID].len = (uint8_t)len;
		memcpy(dev->cfg.mfr[code - PMBUS_MFR_ID].s, data, len);
		return 0;
	case PMBUS_MFR_SPECIFIC_00:
		dev->sim.load_ma = pmbus_linear11_decode(get_word(data));
		break;
	case PMBUS_MFR_SPECIFIC_01:
		dev->sim.vin_mv = pmbus_linear11_decode(get_word(data));
		break;
	default:
		sreg = status_reg(dev, code);
		if (sreg != NULL) {
			/* Writing 1 to a status bit clears it. */
			*sreg &= (uint8_t)~data[0];
			release_alert_if_clear(dev);
			return 0;
		}
		wreg = word_reg(dev, code);
		if (wreg == NULL) {
			return -1;
		}
		*wreg = get_word(data);
		break;
	}
	pmbus_device_update(dev, NULL);
	return 0;
}

#define RW  (PMBUS_A_R | PMBUS_A_W)
#define RO  PMBUS_A_R
#define WO  PMBUS_A_W
#define LIN PMBUS_QUERY_FMT_LINEAR
#define U8  PMBUS_QUERY_FMT_U8
#define NN  PMBUS_QUERY_FMT_NONNUM

#define CMD_SEND(c)        {c, PMBUS_T_SEND_BYTE, WO, NN, NULL, write_cmd}
#define CMD_BYTE(c, a, q)  {c, PMBUS_T_BYTE, a, q, read_byte, write_cmd}
#define CMD_WORD(c, a, q)  {c, PMBUS_T_WORD, a, q, read_word, write_cmd}
#define CMD_BLOCK(c, a)    {c, PMBUS_T_BLOCK, a, NN, read_block, write_cmd}

static const struct pmbus_cmd commands[] = {
	CMD_BYTE(PMBUS_PAGE, RW, NN),
	CMD_BYTE(PMBUS_OPERATION, RW, NN),
	CMD_BYTE(PMBUS_ON_OFF_CONFIG, RW, NN),
	CMD_SEND(PMBUS_CLEAR_FAULTS),
	CMD_BYTE(PMBUS_WRITE_PROTECT, RW, NN),
	CMD_SEND(PMBUS_STORE_DEFAULT_ALL),
	CMD_SEND(PMBUS_RESTORE_DEFAULT_ALL),
	CMD_BYTE(PMBUS_CAPABILITY, RO, NN),
	{PMBUS_QUERY, PMBUS_T_BLOCK_PROC, RO, NN, read_query, NULL},
	CMD_BYTE(PMBUS_VOUT_MODE, RO, NN),
	CMD_WORD(PMBUS_VOUT_COMMAND, RW, LIN),
	CMD_WORD(PMBUS_VOUT_MARGIN_HIGH, RW, LIN),
	CMD_WORD(PMBUS_VOUT_MARGIN_LOW, RW, LIN),
	CMD_WORD(PMBUS_VOUT_OV_FAULT_LIMIT, RW, LIN),
	CMD_WORD(PMBUS_VOUT_OV_WARN_LIMIT, RW, LIN),
	CMD_WORD(PMBUS_VOUT_UV_WARN_LIMIT, RW, LIN),
	CMD_WORD(PMBUS_VOUT_UV_FAULT_LIMIT, RW, LIN),
	CMD_WORD(PMBUS_IOUT_OC_FAULT_LIMIT, RW, LIN),
	CMD_WORD(PMBUS_IOUT_OC_WARN_LIMIT, RW, LIN),
	CMD_WORD(PMBUS_OT_FAULT_LIMIT, RW, LIN),
	CMD_WORD(PMBUS_OT_WARN_LIMIT, RW, LIN),
	CMD_WORD(PMBUS_UT_WARN_LIMIT, RW, LIN),
	CMD_WORD(PMBUS_UT_FAULT_LIMIT, RW, LIN),
	CMD_WORD(PMBUS_VIN_OV_FAULT_LIMIT, RW, LIN),
	CMD_WORD(PMBUS_VIN_OV_WARN_LIMIT, RW, LIN),
	CMD_WORD(PMBUS_VIN_UV_WARN_LIMIT, RW, LIN),
	CMD_WORD(PMBUS_VIN_UV_FAULT_LIMIT, RW, LIN),
	CMD_BYTE(PMBUS_STATUS_BYTE, RO, NN),
	CMD_WORD(PMBUS_STATUS_WORD, RO, NN),
	CMD_BYTE(PMBUS_STATUS_VOUT, RW, NN),
	CMD_BYTE(PMBUS_STATUS_IOUT, RW, NN),
	CMD_BYTE(PMBUS_STATUS_INPUT, RW, NN),
	CMD_BYTE(PMBUS_STATUS_TEMPERATURE, RW, NN),
	CMD_BYTE(PMBUS_STATUS_CML, RW, NN),
	CMD_BYTE(PMBUS_STATUS_MFR_SPECIFIC, RW, NN),
	CMD_WORD(PMBUS_READ_VIN, RO, LIN),
	CMD_WORD(PMBUS_READ_IIN, RO, LIN),
	CMD_WORD(PMBUS_READ_VOUT, RO, LIN),
	CMD_WORD(PMBUS_READ_IOUT, RO, LIN),
	CMD_WORD(PMBUS_READ_TEMPERATURE_1, RO, LIN),
	CMD_WORD(PMBUS_READ_TEMPERATURE_2, RO, LIN),
	CMD_WORD(PMBUS_READ_POUT, RO, LIN),
	CMD_WORD(PMBUS_READ_PIN, RO, LIN),
	CMD_BYTE(PMBUS_PMBUS_REVISION, RO, NN),
	CMD_BLOCK(PMBUS_MFR_ID, RW),
	CMD_BLOCK(PMBUS_MFR_MODEL, RW),
	CMD_BLOCK(PMBUS_MFR_REVISION, RW),
	CMD_BLOCK(PMBUS_MFR_LOCATION, RW),
	CMD_BLOCK(PMBUS_MFR_DATE, RW),
	CMD_BLOCK(PMBUS_MFR_SERIAL, RW),
	/* Manufacturer-specific knobs for the simulation: load current and input voltage. */
	CMD_WORD(PMBUS_MFR_SPECIFIC_00, RW, LIN),
	CMD_WORD(PMBUS_MFR_SPECIFIC_01, RW, LIN),
};

static const struct pmbus_cmd *lookup(void *ctx, uint8_t code)
{
	struct pmbus_device *dev = ctx;

	/* A temperature without a working sensor reads as unsupported. */
	if ((code == PMBUS_READ_TEMPERATURE_1 && !dev->sensors.temp1_valid) ||
	    (code == PMBUS_READ_TEMPERATURE_2 && !dev->sensors.temp2_valid)) {
		return NULL;
	}
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (commands[i].code == code) {
			return &commands[i];
		}
	}
	return NULL;
}

static bool write_allowed(void *ctx, uint8_t code)
{
	struct pmbus_device *dev = ctx;
	uint8_t wp = dev->cfg.write_protect;

	/*
	 * CLEAR_FAULTS and PAGE stay writable at every level, so a host such as Linux, which
	 * clears faults after probing, keeps working on a write-protected device.
	 */
	if (code == PMBUS_WRITE_PROTECT || code == PMBUS_CLEAR_FAULTS || code == PMBUS_PAGE) {
		return true;
	}
	if (wp & PMBUS_WP_ALL) {
		return false;
	}
	if (wp & PMBUS_WP_OP) {
		return code == PMBUS_OPERATION;
	}
	if (wp & PMBUS_WP_VOUT_CMD) {
		return code == PMBUS_OPERATION || code == PMBUS_ON_OFF_CONFIG ||
		       code == PMBUS_VOUT_COMMAND;
	}
	return true;
}

static void cml_error(void *ctx, uint8_t bits)
{
	struct pmbus_device *dev = ctx;

	latch(dev, &dev->status_cml, bits);
}

static bool alert_response(void *ctx)
{
	struct pmbus_device *dev = ctx;

	if (!dev->alert_pending) {
		return false;
	}
	dev->alert_pending = false;
	dev->ops->set_alert(dev->ops_ctx, false);
	return true;
}

static const struct pmbus_target_ops target_ops = {
	.lookup = lookup,
	.write_allowed = write_allowed,
	.cml_error = cml_error,
	.alert_response = alert_response,
};

void pmbus_device_init(struct pmbus_device *dev, uint8_t addr, const struct pmbus_device_ops *ops,
		       void *ops_ctx, bool has_alert_pin, const char *serial)
{
	memset(dev, 0, sizeof(*dev));
	pmbus_target_init(&dev->target, addr, &target_ops, dev);
	dev->ops = ops;
	dev->ops_ctx = ops_ctx;
	dev->has_alert_pin = has_alert_pin;
	dev->control_pin = true;
	dev->cfg = default_config;
	set_default_words(&dev->cfg);
	if (serial != NULL) {
		size_t n = strlen(serial);

		if (n > PMBUS_MFR_STR_MAX) {
			n = PMBUS_MFR_STR_MAX;
		}
		dev->cfg.mfr[PMBUS_MFR_SERIAL_STR].len = (uint8_t)n;
		memcpy(dev->cfg.mfr[PMBUS_MFR_SERIAL_STR].s, serial, n);
	}
	dev->sim.vin_mv = 54000;
	dev->sim.load_ma = 5000;
	dev->sim.eff_permille = 950;
	compute_electricals(dev);
}

void pmbus_device_load_stored(struct pmbus_device *dev)
{
	struct pmbus_device_config stored;

	if (dev->ops->restore(dev->ops_ctx, &stored) &&
	    stored.version == PMBUS_DEVICE_CONFIG_VERSION) {
		dev->cfg = stored;
	}
	compute_electricals(dev);
}
