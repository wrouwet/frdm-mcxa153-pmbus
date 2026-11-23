/*
 * FRDM-MCXA153 as an SFP optical module (SFF-8472) for I2C host testing.
 *
 * LPI2C0 answers at 0x50 (A0h, serial ID) and 0x51 (A2h, diagnostics). Zephyr's
 * driver registers one target address; ADDR1 and SCFGR1[ADDRCFG] add the second,
 * and SASR[RADDR] tells the callbacks which one the host addressed.
 *
 * Like an SFP EEPROM, each address has a byte pointer: a write sets it (first byte)
 * and stores the rest, a read returns bytes from it, both wrap at 256. In A2h the
 * host may write the status/control bits it owns (byte 110), the page select (127)
 * and the user EEPROM (128-247).
 *
 * Diagnostics: the module temperature is the on-board P3T1755; supply voltage, laser
 * bias and optical powers are set values (shell: sfp set). Soft TX disable and a
 * forced LOS zero the TX and RX power. Alarm and warning flags follow from the
 * values and the thresholds at A2h 0-39.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sw_isr_table.h>
#include <zephyr/sys/byteorder.h>

#include <soc.h>

LOG_MODULE_REGISTER(sfp, LOG_LEVEL_INF);

#define ADDR_A0 0x50
#define ADDR_A2 0x51

#define I2C_NODE DT_NODELABEL(lpi2c0)
static const struct device *const i2c_dev = DEVICE_DT_GET(I2C_NODE);
static LPI2C_Type *const lpi2c = (LPI2C_Type *)DT_REG_ADDR(I2C_NODE);
static const struct device *const temp_dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(ambient_temp0));

/* The driver's LPI2C0 handler, which this app wraps (see lpi2c_isr) */
static struct _isr_table_entry driver_isr;
static volatile uint32_t ssr_at_address;

/*
 * Transaction timing from the DWT cycle counter, taken in lpi2c_isr: first address
 * match (end of the first address byte), repeated start (end of the second address
 * byte) and STOP. Only transactions with a repeated start (offset write, then read)
 * are counted.
 */
static struct {
	bool active, restarted;
	uint32_t t_addr, t_restart;
	uint32_t n, total_last, total_min, total_max, read_last;
	uint64_t total_sum;
	uint16_t nb;            /* read-phase TX interrupts of the current transaction */
	uint32_t tx[72];        /* their DWT stamps */
	uint32_t hold_max;      /* longest handler run on a TX interrupt (SCL held low) */
	uint16_t nb_last;
	uint32_t tx_last[72];
} tm;

/* A2h byte offsets */
#define A2_THRESHOLDS 0
#define A2_CC_DMI 95
#define A2_RT 96       /* temperature, Vcc, TX bias, TX power, RX power: 5 x be16 */
#define A2_STATUS 110
#define A2_ALARM 112   /* 2 bytes */
#define A2_WARNING 116 /* 2 bytes */
#define A2_PAGE 127
#define A2_USER_FIRST 128
#define A2_USER_LAST 247

/* A2h byte 110 */
#define ST_TX_DISABLE BIT(7)
#define ST_SOFT_TX_DISABLE BIT(6)
#define ST_SOFT_RS BIT(3)
#define ST_TX_FAULT BIT(2)
#define ST_RX_LOS BIT(1)
#define ST_HOST_BITS (ST_SOFT_TX_DISABLE | ST_SOFT_RS)

static uint8_t mem[2][256]; /* [0] = A0h, [1] = A2h */
static uint8_t ptr[2];
static int cur;             /* device of the current transaction */
static bool first_write;
static uint32_t n_reads[2], n_writes[2];

/* Diagnostic values, in SFF-8472 units */
enum { V_TEMP, V_VCC, V_BIAS, V_TX, V_RX, N_VALUES };
static const char *const v_name[N_VALUES] = {"temp", "vcc", "bias", "tx", "rx"};
static int32_t value[N_VALUES] = {
	25 * 256, /* 1/256 C */
	33000,    /* 0.1 mV: 3.3 V */
	3000,     /* 2 uA: 6 mA */
	5000,     /* 0.1 uW: 0.5 mW */
	4000,     /* 0.1 uW: 0.4 mW */
};
static bool temp_from_sensor = true;
static bool force_los, force_tx_fault;

/* High alarm, low alarm, high warning, low warning, per value (A2h 0-39) */
static const int32_t threshold[N_VALUES][4] = {
	{75 * 256, -5 * 256, 70 * 256, 0},
	{36000, 30000, 35000, 31000},
	{6000, 1000, 5500, 1500},
	{10000, 1000, 8000, 1500},
	{10000, 200, 8000, 400},
};

static void put_str(uint8_t *m, int off, int len, const char *s)
{
	memset(m + off, ' ', len);
	memcpy(m + off, s, MIN((int)strlen(s), len));
}

static uint8_t checksum(const uint8_t *m, int from, int to)
{
	uint8_t sum = 0;

	for (int i = from; i <= to; i++) {
		sum += m[i];
	}
	return sum;
}

static void init_a0(void)
{
	uint8_t *m = mem[0];

	m[0] = 0x03;  /* SFP/SFP+ */
	m[1] = 0x04;  /* serial ID via two-wire interface */
	m[2] = 0x07;  /* LC */
	m[3] = 0x10;  /* 10GBASE-SR */
	m[11] = 0x06; /* 64B/66B */
	m[12] = 0x67; /* 10.3 Gb/s nominal */
	m[16] = 8;    /* OM2: 80 m */
	m[17] = 3;    /* OM1: 30 m */
	m[18] = 40;   /* OM4: 400 m */
	m[19] = 30;   /* OM3: 300 m */
	put_str(m, 20, 16, "EXAMPLE");
	put_str(m, 40, 16, "MCXA153-SFP-SR");
	put_str(m, 56, 4, "A0");
	sys_put_be16(850, &m[60]); /* nm */
	m[63] = checksum(m, 0, 62);
	m[65] = 0x1a; /* TX_DISABLE, TX_FAULT and LOS implemented */
	put_str(m, 68, 16, "MCXA153-0001");
	put_str(m, 84, 8, "261123");
	m[92] = 0x68; /* DDM, internally calibrated, average RX power */
	m[93] = 0xf0; /* alarm/warning flags, soft TX disable, soft TX fault, soft RX LOS */
	m[94] = 0x08; /* SFF-8472 rev 12.3 */
	m[95] = checksum(m, 64, 94);
}

static void init_a2(void)
{
	uint8_t *m = mem[1];

	for (int v = 0; v < N_VALUES; v++) {
		for (int i = 0; i < 4; i++) {
			sys_put_be16((uint16_t)threshold[v][i], &m[A2_THRESHOLDS + v * 8 + i * 2]);
		}
	}
	/* External calibration (unused with internal calibration): slopes 1, offsets 0 */
	sys_put_be32(0x3f800000, &m[72]); /* RX_PWR(1) = 1.0f */
	for (int off = 76; off <= 88; off += 4) {
		sys_put_be16(0x0100, &m[off]);
	}
	m[A2_CC_DMI] = checksum(m, 0, 94);
}

static bool out_high(int v, int32_t x, int which)
{
	return v == V_TEMP ? (int16_t)x > (int16_t)threshold[v][which]
			   : (uint16_t)x > (uint16_t)threshold[v][which];
}

static bool out_low(int v, int32_t x, int which)
{
	return v == V_TEMP ? (int16_t)x < (int16_t)threshold[v][which]
			   : (uint16_t)x < (uint16_t)threshold[v][which];
}

/* Recompute the live part of A2h: values, status, flags. */
static void update_a2(void)
{
	int32_t live[N_VALUES];
	uint16_t alarm = 0, warning = 0;
	unsigned int key;
	uint8_t status;

	memcpy(live, value, sizeof(live));
	key = irq_lock();
	status = mem[1][A2_STATUS] & ST_HOST_BITS;
	irq_unlock(key);

	if (status & ST_SOFT_TX_DISABLE) {
		status |= ST_TX_DISABLE;
		live[V_TX] = 0;
	}
	if (force_tx_fault) {
		status |= ST_TX_FAULT;
	}
	if (force_los) {
		status |= ST_RX_LOS;
		live[V_RX] = 0;
	}

	/* Flag bits, MSB first: temp high/low, vcc, bias, tx, rx (alarm and warning alike) */
	for (int v = 0; v < N_VALUES; v++) {
		int bit = 15 - 2 * v;

		alarm |= out_high(v, live[v], 0) << bit | out_low(v, live[v], 1) << (bit - 1);
		warning |= out_high(v, live[v], 2) << bit | out_low(v, live[v], 3) << (bit - 1);
	}

	key = irq_lock();
	for (int v = 0; v < N_VALUES; v++) {
		sys_put_be16((uint16_t)live[v], &mem[1][A2_RT + 2 * v]);
	}
	mem[1][A2_STATUS] = status;
	sys_put_be16(alarm, &mem[1][A2_ALARM]);
	sys_put_be16(warning, &mem[1][A2_WARNING]);
	irq_unlock(key);
}

static bool host_writable(int dev, uint8_t off)
{
	return dev == 1 && (off == A2_STATUS || off == A2_PAGE ||
			    (off >= A2_USER_FIRST && off <= A2_USER_LAST));
}

/*
 * The driver reads SASR on every address match, which clears RADDR and the address
 * match flags before the callbacks run. This wrapper runs first and keeps SSR (reading
 * it has no side effects) from the interrupt that reported the address.
 */
static void lpi2c_isr(const void *arg)
{
	uint32_t ssr = lpi2c->SSR;

	uint32_t now = DWT->CYCCNT;

	ARG_UNUSED(arg);
	if ((ssr & LPI2C_SSR_AVF_MASK) != 0U) {
		ssr_at_address = ssr;
		if (!tm.active) {
			tm.active = true;
			tm.restarted = false;
			tm.t_addr = now;
		} else {
			tm.restarted = true;
			tm.t_restart = now;
			tm.nb = 0;
		}
	}
	if ((ssr & LPI2C_SSR_TDF_MASK) != 0U && tm.restarted && tm.nb < ARRAY_SIZE(tm.tx)) {
		tm.tx[tm.nb++] = now;
	}
	if ((ssr & LPI2C_SSR_SDF_MASK) != 0U && tm.active) {
		tm.active = false;
		if (tm.restarted) {
			uint32_t total = now - tm.t_addr;

			tm.total_last = total;
			tm.read_last = now - tm.t_restart;
			tm.total_min = tm.n == 0U ? total : MIN(tm.total_min, total);
			tm.total_max = MAX(tm.total_max, total);
			tm.total_sum += total;
			tm.n++;
			tm.nb_last = tm.nb;
			memcpy(tm.tx_last, tm.tx, tm.nb * sizeof(tm.tx[0]));
		}
	}
	driver_isr.isr(driver_isr.arg);
	if ((ssr & LPI2C_SSR_TDF_MASK) != 0U && tm.restarted) {
		tm.hold_max = MAX(tm.hold_max, DWT->CYCCNT - now);
	}
}

/* The device the host addressed, from the address match flags */
static int addressed(void)
{
	uint32_t ssr = ssr_at_address;

	if ((ssr & LPI2C_SSR_AM1F_MASK) != 0U) {
		return 1;
	}
	if ((ssr & LPI2C_SSR_AM0F_MASK) != 0U) {
		return 0;
	}
	return cur;
}

static int on_write_requested(struct i2c_target_config *cfg)
{
	ARG_UNUSED(cfg);
	cur = addressed();
	first_write = true;
	n_writes[cur]++;
	return 0;
}

static int on_write_received(struct i2c_target_config *cfg, uint8_t val)
{
	ARG_UNUSED(cfg);
	if (first_write) {
		ptr[cur] = val;
		first_write = false;
		return 0;
	}
	if (host_writable(cur, ptr[cur])) {
		if (ptr[cur] == A2_STATUS) {
			val = (mem[1][A2_STATUS] & ~ST_HOST_BITS) | (val & ST_HOST_BITS);
		}
		mem[cur][ptr[cur]] = val;
	}
	ptr[cur]++;
	return 0;
}

static int on_read_requested(struct i2c_target_config *cfg, uint8_t *val)
{
	ARG_UNUSED(cfg);
	cur = addressed();
	n_reads[cur]++;
	*val = mem[cur][ptr[cur]++];
	return 0;
}

static int on_read_processed(struct i2c_target_config *cfg, uint8_t *val)
{
	ARG_UNUSED(cfg);
	*val = mem[cur][ptr[cur]++];
	return 0;
}

static int on_stop(struct i2c_target_config *cfg)
{
	ARG_UNUSED(cfg);
	return 0;
}

static const struct i2c_target_callbacks callbacks = {
	.write_requested = on_write_requested,
	.write_received = on_write_received,
	.read_requested = on_read_requested,
	.read_processed = on_read_processed,
	.stop = on_stop,
};

static struct i2c_target_config target_cfg = {
	.address = ADDR_A0,
	.callbacks = &callbacks,
};

/* Add A2h as address match 1; SCFGR1 may only change while the target is disabled. */
static void add_a2_address(void)
{
	uint32_t scr = lpi2c->SCR;

	lpi2c->SCR = scr & ~LPI2C_SCR_SEN_MASK;
	lpi2c->SAMR = LPI2C_SAMR_ADDR0(ADDR_A0) | LPI2C_SAMR_ADDR1(ADDR_A2);
	lpi2c->SCFGR1 = (lpi2c->SCFGR1 & ~LPI2C_SCFGR1_ADDRCFG_MASK) | LPI2C_SCFGR1_ADDRCFG(2);
	lpi2c->SCR = scr;
}

static void read_temperature(void)
{
	struct sensor_value t;

	if (!temp_from_sensor || temp_dev == NULL || !device_is_ready(temp_dev)) {
		return;
	}
	if (sensor_sample_fetch(temp_dev) == 0 &&
	    sensor_channel_get(temp_dev, SENSOR_CHAN_AMBIENT_TEMP, &t) == 0) {
		value[V_TEMP] = (int32_t)(sensor_value_to_double(&t) * 256.0);
	}
}

int main(void)
{
	int ret;

	init_a0();
	init_a2();
	read_temperature();
	update_a2();

	if (!device_is_ready(i2c_dev)) {
		LOG_ERR("LPI2C0 not ready");
		return 0;
	}
	ret = i2c_target_register(i2c_dev, &target_cfg);
	if (ret != 0) {
		LOG_ERR("i2c_target_register: %d", ret);
		return 0;
	}
	add_a2_address();
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	driver_isr = _sw_isr_table[DT_IRQN(I2C_NODE) - CONFIG_GEN_IRQ_START_VECTOR];
	irq_connect_dynamic(DT_IRQN(I2C_NODE), DT_IRQ(I2C_NODE, priority), lpi2c_isr, NULL, 0);
	LOG_INF("SFP module: A0h at 0x%02x, A2h at 0x%02x", ADDR_A0, ADDR_A2);

	while (1) {
		read_temperature();
		update_a2();
		k_sleep(K_MSEC(500));
	}
	return 0;
}

/* Shell */

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t a2[256];
	unsigned int key;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	key = irq_lock();
	memcpy(a2, mem[1], sizeof(a2));
	irq_unlock(key);

	shell_print(sh, "SFP module: A0h 0x%02x, A2h 0x%02x; %s %s rev %.4s",
		    ADDR_A0, ADDR_A2, "EXAMPLE", "MCXA153-SFP-SR", &mem[0][56]);
	shell_print(sh, "temp %d.%02d C%s  vcc %u.%04u V  bias %u.%03u mA",
		    (int16_t)sys_get_be16(&a2[96]) / 256,
		    abs((int16_t)sys_get_be16(&a2[96]) % 256) * 100 / 256,
		    temp_from_sensor ? " (sensor)" : "",
		    sys_get_be16(&a2[98]) / 10000, sys_get_be16(&a2[98]) % 10000,
		    sys_get_be16(&a2[100]) * 2 / 1000, sys_get_be16(&a2[100]) * 2 % 1000);
	shell_print(sh, "tx %u.%04u mW  rx %u.%04u mW",
		    sys_get_be16(&a2[102]) / 10000, sys_get_be16(&a2[102]) % 10000,
		    sys_get_be16(&a2[104]) / 10000, sys_get_be16(&a2[104]) % 10000);
	shell_print(sh, "status 0x%02x  alarm 0x%02x%02x  warning 0x%02x%02x",
		    a2[A2_STATUS], a2[A2_ALARM], a2[A2_ALARM + 1], a2[A2_WARNING],
		    a2[A2_WARNING + 1]);
	shell_print(sh, "transactions: A0h %u reads %u writes, A2h %u reads %u writes",
		    n_reads[0], n_writes[0], n_reads[1], n_writes[1]);
	shell_print(sh, "last address SSR 0x%08x", ssr_at_address);
	return 0;
}

/* sfp set <temp|vcc|bias|tx|rx> <value|auto>: C, mV, uA, uW */
static int cmd_set(const struct shell *sh, size_t argc, char **argv)
{
	long x;

	ARG_UNUSED(argc);
	for (int v = 0; v < N_VALUES; v++) {
		if (strcmp(argv[1], v_name[v]) != 0) {
			continue;
		}
		if (v == V_TEMP && strcmp(argv[2], "auto") == 0) {
			temp_from_sensor = true;
			return 0;
		}
		x = strtol(argv[2], NULL, 0);
		switch (v) {
		case V_TEMP:
			temp_from_sensor = false;
			value[v] = x * 256;
			break;
		case V_VCC:
			value[v] = x * 10;
			break;
		case V_BIAS:
			value[v] = x / 2;
			break;
		default:
			value[v] = x * 10;
			break;
		}
		update_a2();
		return 0;
	}
	shell_error(sh, "unknown value %s (temp, vcc, bias, tx, rx)", argv[1]);
	return -EINVAL;
}

static int set_flag(const struct shell *sh, char **argv, bool *flag)
{
	if (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0) {
		shell_error(sh, "on or off");
		return -EINVAL;
	}
	*flag = strcmp(argv[1], "on") == 0;
	update_a2();
	return 0;
}

static int cmd_los(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	return set_flag(sh, argv, &force_los);
}

static int cmd_txfault(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	return set_flag(sh, argv, &force_tx_fault);
}

static int cmd_dump(const struct shell *sh, size_t argc, char **argv)
{
	int dev = strcmp(argv[1], "a2") == 0;

	ARG_UNUSED(argc);
	shell_hexdump(sh, mem[dev], sizeof(mem[dev]));
	return 0;
}

static uint32_t cyc_to_ns(uint32_t cycles)
{
	return (uint32_t)((uint64_t)cycles * 1000000000ULL / SystemCoreClock);
}

static int cmd_timing(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "reset") == 0) {
		unsigned int key = irq_lock();

		memset(&tm, 0, sizeof(tm));
		irq_unlock(key);
		return 0;
	}
	shell_print(sh, "core clock %u Hz; %u transactions with a repeated start", SystemCoreClock,
		    tm.n);
	if (tm.n == 0U) {
		return 0;
	}
	shell_print(sh, "first address byte to STOP: last %u ns, min %u ns, max %u ns, avg %u ns",
		    cyc_to_ns(tm.total_last), cyc_to_ns(tm.total_min), cyc_to_ns(tm.total_max),
		    cyc_to_ns((uint32_t)(tm.total_sum / tm.n)));
	shell_print(sh, "read address byte to STOP (last): %u ns", cyc_to_ns(tm.read_last));
	if (tm.nb_last > 2U) {
		uint32_t mn = UINT32_MAX, mx = 0;

		for (int i = 1; i < tm.nb_last; i++) {
			uint32_t d = tm.tx_last[i] - tm.tx_last[i - 1];

			mn = MIN(mn, d);
			mx = MAX(mx, d);
		}
		shell_print(sh, "byte to byte (last read, %u TX irqs): min %u ns, max %u ns, avg %u ns",
			    tm.nb_last, cyc_to_ns(mn), cyc_to_ns(mx),
			    cyc_to_ns((tm.tx_last[tm.nb_last - 1] - tm.tx_last[0]) /
				      (tm.nb_last - 1)));
	}
	shell_print(sh, "longest TX handler (SCL held by the target): %u ns", cyc_to_ns(tm.hold_max));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sfp_cmds,
	SHELL_CMD(status, NULL, "Show the module state", cmd_status),
	SHELL_CMD_ARG(set, NULL, "set <temp|vcc|bias|tx|rx> <C|mV|uA|uW|auto>", cmd_set, 3, 0),
	SHELL_CMD_ARG(los, NULL, "los <on|off>: force RX loss of signal", cmd_los, 2, 0),
	SHELL_CMD_ARG(txfault, NULL, "txfault <on|off>: force TX fault", cmd_txfault, 2, 0),
	SHELL_CMD_ARG(dump, NULL, "dump <a0|a2>", cmd_dump, 2, 0),
	SHELL_CMD_ARG(timing, NULL, "timing [reset]: transaction times", cmd_timing, 1, 1),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(sfp, &sfp_cmds, "SFP module emulation", NULL);
