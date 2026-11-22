/*
 * Host unit tests for the PMBus library. A small SMBus host model drives the target
 * engine byte by byte, the way an I2C target driver would, and checks every PEC.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include <pmbus/device.h>
#include <pmbus/format.h>
#include <pmbus/pmbus.h>

#define ADDR 0x58

static int failures;
static int checks;

#define CHECK(cond)                                                                        \
	do {                                                                               \
		checks++;                                                                  \
		if (!(cond)) {                                                             \
			failures++;                                                        \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
		}                                                                          \
	} while (0)

#define CHECK_EQ(a, b)                                                                     \
	do {                                                                               \
		long long a_ = (long long)(a), b_ = (long long)(b);                        \
		checks++;                                                                  \
		if (a_ != b_) {                                                            \
			failures++;                                                        \
			printf("  FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__,      \
			       __LINE__, #a, a_, b_);                                      \
		}                                                                          \
	} while (0)

/* --- The platform side --- */

static bool alert_line;
static int store_calls;
static bool have_stored;
static struct pmbus_device_config stored_cfg;

static void set_alert(void *ctx, bool asserted)
{
	(void)ctx;
	alert_line = asserted;
}

static void store(void *ctx, const struct pmbus_device_config *cfg)
{
	(void)ctx;
	store_calls++;
	stored_cfg = *cfg;
	have_stored = true;
}

static bool restore(void *ctx, struct pmbus_device_config *cfg)
{
	(void)ctx;
	if (!have_stored) {
		return false;
	}
	*cfg = stored_cfg;
	return true;
}

static const struct pmbus_device_ops ops = {
	.set_alert = set_alert,
	.store = store,
	.restore = restore,
};

static struct pmbus_device dev;

static const struct pmbus_sensors room = {
	.temp1_valid = true,
	.temp1_mc = 25000,
	.temp2_valid = true,
	.temp2_mc = 31500,
};

static void fresh_device(void)
{
	alert_line = false;
	store_calls = 0;
	have_stored = false;
	pmbus_device_init(&dev, ADDR, &ops, NULL, true, "SN123");
	pmbus_device_update(&dev, &room);
}

/* --- The SMBus host model --- */

static struct pmbus_target *t(void)
{
	return &dev.target;
}

/* Writes bytes (command first) in one transaction. Returns how many were ACKed. */
static int host_write(const uint8_t *bytes, size_t n, bool pec)
{
	uint8_t crc = pmbus_crc8_byte(0, ADDR << 1);
	int acked = 0;

	pmbus_target_write_start(t());
	for (size_t i = 0; i < n; i++) {
		acked += pmbus_target_write_byte(t(), bytes[i]);
		crc = pmbus_crc8_byte(crc, bytes[i]);
	}
	if (pec) {
		acked += pmbus_target_write_byte(t(), crc);
	}
	pmbus_target_stop(t());
	return acked;
}

/*
 * Writes the command (and any process-call data), then a repeated start and n reads.
 * If pec, reads one more byte and checks it. Returns 0, or -1 on a PEC mismatch.
 */
static int host_read(const uint8_t *wbytes, size_t wn, uint8_t *out, size_t n, bool pec)
{
	uint8_t crc = pmbus_crc8_byte(0, ADDR << 1);
	int ret = 0;

	pmbus_target_write_start(t());
	for (size_t i = 0; i < wn; i++) {
		pmbus_target_write_byte(t(), wbytes[i]);
	}
	crc = pmbus_crc8(crc, wbytes, wn);
	crc = pmbus_crc8_byte(crc, (ADDR << 1) | 1);
	for (size_t i = 0; i < n; i++) {
		out[i] = (i == 0) ? pmbus_target_read_start(t()) : pmbus_target_read_byte(t());
		crc = pmbus_crc8_byte(crc, out[i]);
	}
	if (pec && pmbus_target_read_byte(t()) != crc) {
		ret = -1;
	}
	/* The Zephyr LPI2C driver prefetches one byte past the end. */
	(void)pmbus_target_read_byte(t());
	pmbus_target_stop(t());
	return ret;
}

static void write_byte(uint8_t cmd, uint8_t v, bool pec)
{
	uint8_t b[] = {cmd, v};

	host_write(b, sizeof(b), pec);
}

static void write_word(uint8_t cmd, uint16_t v, bool pec)
{
	uint8_t b[] = {cmd, (uint8_t)v, (uint8_t)(v >> 8)};

	host_write(b, sizeof(b), pec);
}

static void send_byte(uint8_t cmd)
{
	host_write(&cmd, 1, false);
}

static int read_byte(uint8_t cmd, bool pec)
{
	uint8_t v;

	if (host_read(&cmd, 1, &v, 1, pec) < 0) {
		return -1;
	}
	return v;
}

static int read_word(uint8_t cmd, bool pec)
{
	uint8_t v[2];

	if (host_read(&cmd, 1, v, 2, pec) < 0) {
		return -1;
	}
	return v[0] | (v[1] << 8);
}

/* A block read as a real host does it: the count byte decides how much more to read. */
static int block_read(uint8_t cmd, uint8_t *buf, bool pec)
{
	uint8_t crc = pmbus_crc8_byte(0, ADDR << 1);
	uint8_t count;
	int ret;

	crc = pmbus_crc8_byte(crc, cmd);
	crc = pmbus_crc8_byte(crc, (ADDR << 1) | 1);
	pmbus_target_write_start(t());
	pmbus_target_write_byte(t(), cmd);
	count = pmbus_target_read_start(t());
	crc = pmbus_crc8_byte(crc, count);
	for (int i = 0; i < count; i++) {
		buf[i] = pmbus_target_read_byte(t());
		crc = pmbus_crc8_byte(crc, buf[i]);
	}
	ret = count;
	if (pec && pmbus_target_read_byte(t()) != crc) {
		ret = -1;
	}
	pmbus_target_stop(t());
	return ret;
}

/* --- Tests --- */

static void test_formats(void)
{
	static const uint8_t check[] = "123456789";

	CHECK_EQ(pmbus_crc8(0, check, 9), 0xF4); /* the CRC-8/SMBUS check value */

	CHECK_EQ(pmbus_linear11_encode(54000), 0xE360); /* 864 * 2^-4 */
	CHECK_EQ(pmbus_linear11_decode(0xE360), 54000);
	CHECK_EQ(pmbus_linear11_decode(pmbus_linear11_encode(-40000)), -40000);
	CHECK_EQ(pmbus_linear11_decode(pmbus_linear11_encode(0)), 0);
	CHECK_EQ(pmbus_linear11_decode(pmbus_linear11_encode(25125)), 25125);
	/* 1 mA needs a mantissa of 1.024 at N = -10: within half an LSB. */
	CHECK(pmbus_linear11_decode(pmbus_linear11_encode(1)) <= 1);
	/* Large values lose precision but stay within half an LSB (2^11 at N = 11). */
	CHECK_EQ(pmbus_linear11_decode(pmbus_linear11_encode(2000000000)), 2000896000);

	CHECK_EQ(pmbus_ulinear16_encode(12000, -9), 0x1800);
	CHECK_EQ(pmbus_ulinear16_decode(0x1800, -9), 12000);
	CHECK_EQ(pmbus_ulinear16_encode(-5, -9), 0);
	CHECK_EQ(pmbus_ulinear16_encode(200000, -9), 0xFFFF);
	CHECK_EQ(pmbus_vout_mode_exponent(PMBUS_DEVICE_VOUT_MODE), -9);
}

static void test_basic_reads(void)
{
	uint8_t buf[PMBUS_BLOCK_MAX];
	int n;

	fresh_device();
	CHECK_EQ(read_byte(PMBUS_PMBUS_REVISION, true), 0x33);
	CHECK_EQ(read_byte(PMBUS_CAPABILITY, true), 0xB0);
	CHECK_EQ(read_byte(PMBUS_VOUT_MODE, true), 0x17);
	CHECK_EQ(read_word(PMBUS_READ_VOUT, true), 0x1800);
	CHECK_EQ(pmbus_linear11_decode(read_word(PMBUS_READ_VIN, true)), 54000);
	CHECK_EQ(pmbus_linear11_decode(read_word(PMBUS_READ_IOUT, true)), 5000);
	CHECK_EQ(pmbus_linear11_decode(read_word(PMBUS_READ_POUT, true)), 60000);
	CHECK_EQ(pmbus_linear11_decode(read_word(PMBUS_READ_TEMPERATURE_1, true)), 25000);
	CHECK_EQ(pmbus_linear11_decode(read_word(PMBUS_READ_TEMPERATURE_2, true)), 31500);
	/* The same reads without PEC give the same data. */
	CHECK_EQ(read_word(PMBUS_READ_VOUT, false), 0x1800);

	n = block_read(PMBUS_MFR_MODEL, buf, true);
	CHECK_EQ(n, 13);
	CHECK(n == 13 && memcmp(buf, "MCXA153-PMB-1", 13) == 0);
	n = block_read(PMBUS_MFR_SERIAL, buf, true);
	CHECK(n == 5 && memcmp(buf, "SN123", 5) == 0);

	CHECK_EQ(read_byte(PMBUS_STATUS_BYTE, true), 0);
	CHECK_EQ(read_word(PMBUS_STATUS_WORD, true), 0);
	CHECK(!alert_line);
}

static void test_unsupported_command(void)
{
	fresh_device();
	/* READ_VCAP is not supported: the reply is all ones, with no valid PEC. */
	CHECK_EQ(read_word(0x8A, false), 0xFFFF);
	CHECK_EQ(read_byte(PMBUS_STATUS_BYTE, true), PMBUS_SB_CML);
	CHECK_EQ(read_byte(PMBUS_STATUS_CML, true), PMBUS_CML_INVALID_CMD);
	CHECK(alert_line);
	send_byte(PMBUS_CLEAR_FAULTS);
	CHECK_EQ(read_byte(PMBUS_STATUS_BYTE, true), 0);
	CHECK(!alert_line);

	/* Writing a read-only command is an invalid command too. */
	write_word(PMBUS_READ_VOUT, 0x1234, false);
	CHECK_EQ(read_byte(PMBUS_STATUS_CML, false), PMBUS_CML_INVALID_CMD);
	/* Status bits clear by writing 1 to them. */
	write_byte(PMBUS_STATUS_CML, PMBUS_CML_INVALID_CMD, false);
	CHECK_EQ(read_byte(PMBUS_STATUS_CML, false), 0);
}

static void test_writes_and_pec(void)
{
	uint8_t bad[] = {PMBUS_VOUT_COMMAND, 0x00, 0x19, 0x00};

	fresh_device();
	write_word(PMBUS_VOUT_COMMAND, 0x1900, true); /* 12.5 V, good PEC */
	CHECK_EQ(read_word(PMBUS_VOUT_COMMAND, true), 0x1900);
	CHECK_EQ(read_word(PMBUS_READ_VOUT, true), 0x1900);

	host_write(bad, sizeof(bad), false); /* 4th byte is a wrong PEC */
	CHECK_EQ(read_word(PMBUS_VOUT_COMMAND, true), 0x1900);
	CHECK_EQ(read_byte(PMBUS_STATUS_CML, true), PMBUS_CML_PEC_FAILED);
	CHECK_EQ(dev.target.n_pec_errors, 1);
	send_byte(PMBUS_CLEAR_FAULTS);

	/* Too few and too many data bytes. */
	host_write(bad, 2, false);
	CHECK_EQ(read_byte(PMBUS_STATUS_CML, true), PMBUS_CML_INVALID_DATA);
	send_byte(PMBUS_CLEAR_FAULTS);
	uint8_t longer[] = {PMBUS_OPERATION, 0x80, 0x00, 0x00};

	host_write(longer, sizeof(longer), false);
	CHECK_EQ(read_byte(PMBUS_STATUS_CML, true), PMBUS_CML_INVALID_DATA);
	send_byte(PMBUS_CLEAR_FAULTS);

	/* PAGE: only 0 and 0xFF exist. */
	write_byte(PMBUS_PAGE, 1, true);
	CHECK_EQ(read_byte(PMBUS_STATUS_CML, true), PMBUS_CML_INVALID_DATA);
	send_byte(PMBUS_CLEAR_FAULTS);
	write_byte(PMBUS_PAGE, 0xFF, true);
	CHECK_EQ(read_byte(PMBUS_STATUS_CML, true), 0);
}

static void test_block_write(void)
{
	uint8_t msg[2 + 6] = {PMBUS_MFR_SERIAL, 6, 'A', 'B', 'C', '1', '2', '3'};
	uint8_t buf[PMBUS_BLOCK_MAX];
	uint8_t big[2 + 25] = {PMBUS_MFR_SERIAL, 25};

	fresh_device();
	host_write(msg, sizeof(msg), true);
	CHECK(block_read(PMBUS_MFR_SERIAL, buf, true) == 6 && memcmp(buf, "ABC123", 6) == 0);

	/* Longer than the device stores. */
	host_write(big, sizeof(big), true);
	CHECK_EQ(read_byte(PMBUS_STATUS_CML, true), PMBUS_CML_INVALID_DATA);
	CHECK(block_read(PMBUS_MFR_SERIAL, buf, true) == 6);
}

static void test_query(void)
{
	uint8_t w[3] = {PMBUS_QUERY, 1, PMBUS_READ_VOUT};
	uint8_t r[2];

	fresh_device();
	CHECK_EQ(host_read(w, 3, r, 2, true), 0);
	CHECK_EQ(r[0], 1);
	CHECK_EQ(r[1], PMBUS_QUERY_SUPPORTED | PMBUS_QUERY_READ | PMBUS_QUERY_FMT_LINEAR);

	w[2] = PMBUS_VOUT_COMMAND;
	host_read(w, 3, r, 2, true);
	CHECK_EQ(r[1], PMBUS_QUERY_SUPPORTED | PMBUS_QUERY_READ | PMBUS_QUERY_WRITE);

	w[2] = PMBUS_CLEAR_FAULTS;
	host_read(w, 3, r, 2, true);
	CHECK_EQ(r[1], PMBUS_QUERY_SUPPORTED | PMBUS_QUERY_WRITE | PMBUS_QUERY_FMT_NONNUM);

	w[2] = 0x8A;
	host_read(w, 3, r, 2, true);
	CHECK_EQ(r[1], 0);
}

static void test_write_protect(void)
{
	fresh_device();
	write_byte(PMBUS_WRITE_PROTECT, PMBUS_WP_ALL, true);
	write_word(PMBUS_VOUT_COMMAND, 0x1900, true);
	CHECK_EQ(read_word(PMBUS_VOUT_COMMAND, true), 0x1800);
	CHECK_EQ(read_byte(PMBUS_STATUS_CML, true), PMBUS_CML_INVALID_CMD);
	send_byte(PMBUS_CLEAR_FAULTS); /* still allowed */
	CHECK_EQ(read_byte(PMBUS_STATUS_CML, true), 0);

	write_byte(PMBUS_WRITE_PROTECT, PMBUS_WP_VOUT_CMD, true);
	write_word(PMBUS_VOUT_COMMAND, 0x1900, true);
	CHECK_EQ(read_word(PMBUS_VOUT_COMMAND, true), 0x1900);
	write_word(PMBUS_VOUT_OV_FAULT_LIMIT, 0x2000, true);
	CHECK(read_word(PMBUS_VOUT_OV_FAULT_LIMIT, true) != 0x2000);

	write_byte(PMBUS_WRITE_PROTECT, 0x11, true); /* not a defined level */
	CHECK_EQ(read_byte(PMBUS_WRITE_PROTECT, true), PMBUS_WP_VOUT_CMD);
}

static void test_temperature_alert_and_ara(void)
{
	struct pmbus_sensors hot = room;
	uint8_t ara;

	fresh_device();
	write_word(PMBUS_OT_WARN_LIMIT, pmbus_linear11_encode(30000), true);
	/* temp2 is 31.5 C: the write itself re-evaluates the limits. */
	CHECK(alert_line);
	CHECK_EQ(read_byte(PMBUS_STATUS_TEMPERATURE, true), PMBUS_ST_OT_WARN);
	CHECK_EQ(read_byte(PMBUS_STATUS_BYTE, true), PMBUS_SB_TEMPERATURE);

	/* A Receive Byte to the device's own address is not an alert response. */
	ara = pmbus_target_read_start(t());
	pmbus_target_stop(t());
	CHECK_EQ(ara, 0xFF);
	CHECK(alert_line);

	/* The host reads the Alert Response Address. */
	ara = pmbus_target_ara_start(t());
	pmbus_target_stop(t());
	CHECK_EQ(ara, ADDR << 1);
	CHECK(!alert_line);

	/* The bit stays latched until cleared, and a second ARA read finds nothing. */
	ara = pmbus_target_ara_start(t());
	pmbus_target_stop(t());
	CHECK_EQ(ara, 0xFF);
	CHECK_EQ(read_byte(PMBUS_STATUS_TEMPERATURE, true), PMBUS_ST_OT_WARN);

	/* An over-temperature fault shuts the output down. */
	hot.temp1_mc = 101000;
	pmbus_device_update(&dev, &hot);
	CHECK(dev.fault_off);
	CHECK(pmbus_device_status_byte(&dev) & PMBUS_SB_OFF);
	CHECK_EQ(read_word(PMBUS_READ_VOUT, true), 0);
}

static void test_ov_fault_latch_and_restart(void)
{
	fresh_device();
	write_word(PMBUS_VOUT_COMMAND, pmbus_ulinear16_encode(14000, -9), true);
	pmbus_device_update(&dev, &room);
	CHECK_EQ(read_byte(PMBUS_STATUS_VOUT, true), PMBUS_SV_OV_FAULT);
	CHECK_EQ(read_byte(PMBUS_STATUS_BYTE, true), PMBUS_SB_OFF | PMBUS_SB_VOUT_OV_FAULT);
	CHECK(read_word(PMBUS_STATUS_WORD, true) & PMBUS_SW_POWER_GOOD_N);
	CHECK_EQ(read_word(PMBUS_READ_VOUT, true), 0);

	/* Fixing the set point and clearing faults does not restart a latched-off unit... */
	write_word(PMBUS_VOUT_COMMAND, 0x1800, true);
	send_byte(PMBUS_CLEAR_FAULTS);
	pmbus_device_update(&dev, &room);
	CHECK(pmbus_device_status_byte(&dev) & PMBUS_SB_OFF);
	/* ...cycling OPERATION does. */
	write_byte(PMBUS_OPERATION, 0x00, true);
	write_byte(PMBUS_OPERATION, PMBUS_OPERATION_ON, true);
	pmbus_device_update(&dev, &room);
	CHECK_EQ(read_byte(PMBUS_STATUS_BYTE, true), 0);
	CHECK_EQ(read_word(PMBUS_READ_VOUT, true), 0x1800);
}

static void test_margins_and_overcurrent(void)
{
	fresh_device();
	write_byte(PMBUS_OPERATION, PMBUS_OPERATION_ON | PMBUS_OPERATION_MARGIN_HIGH, true);
	CHECK_EQ(pmbus_ulinear16_decode(read_word(PMBUS_READ_VOUT, true), -9), 12600);
	write_byte(PMBUS_OPERATION, PMBUS_OPERATION_ON | PMBUS_OPERATION_MARGIN_LOW, true);
	CHECK_EQ(pmbus_ulinear16_decode(read_word(PMBUS_READ_VOUT, true), -9), 11400);
	write_byte(PMBUS_OPERATION, PMBUS_OPERATION_ON, true);

	/* The simulated load is a manufacturer-specific command. */
	write_word(PMBUS_MFR_SPECIFIC_00, pmbus_linear11_encode(9000), true);
	pmbus_device_update(&dev, &room);
	CHECK_EQ(read_byte(PMBUS_STATUS_IOUT, true), PMBUS_SI_OC_WARN);
	CHECK(dev.output_on);
	write_word(PMBUS_MFR_SPECIFIC_00, pmbus_linear11_encode(11000), true);
	pmbus_device_update(&dev, &room);
	CHECK(read_byte(PMBUS_STATUS_IOUT, true) & PMBUS_SI_OC_FAULT);
	CHECK(!dev.output_on);
}

static void test_input_undervoltage_lockout(void)
{
	fresh_device();
	write_word(PMBUS_MFR_SPECIFIC_01, pmbus_linear11_encode(38000), true);
	CHECK(!dev.output_on);
	CHECK(read_byte(PMBUS_STATUS_INPUT, true) & PMBUS_SIN_UV_FAULT);
	CHECK(read_byte(PMBUS_STATUS_BYTE, true) & PMBUS_SB_VIN_UV_FAULT);
	/* Unlike a latched fault, the output returns with the input. */
	write_word(PMBUS_MFR_SPECIFIC_01, pmbus_linear11_encode(54000), true);
	CHECK(dev.output_on);
}

static void test_store_restore(void)
{
	fresh_device();
	write_word(PMBUS_OT_WARN_LIMIT, pmbus_linear11_encode(70000), true);
	send_byte(PMBUS_STORE_DEFAULT_ALL);
	CHECK_EQ(store_calls, 1);
	CHECK(read_byte(PMBUS_STATUS_BYTE, true) & PMBUS_SB_BUSY);
	pmbus_device_store_done(&dev, true);
	CHECK(!(read_byte(PMBUS_STATUS_BYTE, true) & PMBUS_SB_BUSY));

	write_word(PMBUS_OT_WARN_LIMIT, pmbus_linear11_encode(60000), true);
	send_byte(PMBUS_RESTORE_DEFAULT_ALL);
	CHECK_EQ(pmbus_linear11_decode(read_word(PMBUS_OT_WARN_LIMIT, true)), 70000);

	/* A new device instance picks the stored values up at boot. */
	pmbus_device_init(&dev, ADDR, &ops, NULL, true, "SN123");
	pmbus_device_load_stored(&dev);
	CHECK_EQ(pmbus_linear11_decode(read_word(PMBUS_OT_WARN_LIMIT, true)), 70000);

	/* A failed flash write shows up as a memory fault. */
	send_byte(PMBUS_STORE_DEFAULT_ALL);
	pmbus_device_store_done(&dev, false);
	CHECK_EQ(read_byte(PMBUS_STATUS_CML, true), PMBUS_CML_MEMORY_FAULT);
}

static void test_abort_and_quick_command(void)
{
	fresh_device();
	/* A transaction cut off by an SMBus timeout leaves no trace. */
	pmbus_target_write_start(t());
	pmbus_target_write_byte(t(), PMBUS_VOUT_COMMAND);
	pmbus_target_write_byte(t(), 0x00);
	pmbus_target_abort(t());
	CHECK_EQ(dev.target.n_aborts, 1);
	CHECK_EQ(read_word(PMBUS_VOUT_COMMAND, true), 0x1800);
	CHECK_EQ(read_byte(PMBUS_STATUS_CML, true), 0);

	/* An address-only write (i2cdetect's quick command) does nothing. */
	host_write(NULL, 0, false);
	CHECK_EQ(read_byte(PMBUS_STATUS_CML, true), 0);
}

static void test_missing_sensor(void)
{
	struct pmbus_sensors s = room;

	fresh_device();
	s.temp1_valid = false;
	pmbus_device_update(&dev, &s);
	CHECK_EQ(read_word(PMBUS_READ_TEMPERATURE_1, false), 0xFFFF);
	CHECK_EQ(read_byte(PMBUS_STATUS_CML, true), PMBUS_CML_INVALID_CMD);
}

/*
 * The reads the Linux generic "pmbus" driver makes while probing (pmbus_core.c): for
 * each candidate register it reads the value, then STATUS_BYTE, and if the CML bit is
 * set it calls the register unsupported and clears faults.
 */
static bool linux_check_register(uint8_t cmd)
{
	bool supported = read_word(cmd, true) >= 0;
	int sb = read_byte(PMBUS_STATUS_BYTE, true);

	if (sb >= 0 && (sb & PMBUS_SB_CML)) {
		int cml = read_byte(PMBUS_STATUS_CML, true);

		if (cml & (PMBUS_CML_INVALID_CMD | PMBUS_CML_INVALID_DATA)) {
			supported = false;
		}
		send_byte(PMBUS_CLEAR_FAULTS);
	}
	return supported;
}

static void test_linux_probe(void)
{
	static const uint8_t yes[] = {
		PMBUS_READ_VIN, PMBUS_READ_IIN, PMBUS_READ_PIN, PMBUS_READ_VOUT, PMBUS_READ_IOUT,
		PMBUS_READ_POUT, PMBUS_READ_TEMPERATURE_1, PMBUS_READ_TEMPERATURE_2,
		PMBUS_STATUS_WORD,
	};
	static const uint8_t no[] = {0x8A /* VCAP */, 0x8F /* TEMPERATURE_3 */,
				     0x3A /* FAN_CONFIG_12 */, 0x90 /* FAN_SPEED_1 */};

	fresh_device();
	CHECK(read_byte(PMBUS_CAPABILITY, true) & PMBUS_CAP_PEC);
	CHECK_EQ(read_word(PMBUS_STATUS_WORD, false), 0);
	/* Page detection: page 1 must fail. */
	write_byte(PMBUS_PAGE, 1, true);
	CHECK(read_byte(PMBUS_STATUS_BYTE, true) & PMBUS_SB_CML);
	send_byte(PMBUS_CLEAR_FAULTS);
	write_byte(PMBUS_PAGE, 0, true);
	CHECK_EQ(read_byte(PMBUS_VOUT_MODE, true) >> 5, 0); /* linear */

	for (size_t i = 0; i < sizeof(yes); i++) {
		CHECK(linux_check_register(yes[i]));
	}
	for (size_t i = 0; i < sizeof(no); i++) {
		CHECK(!linux_check_register(no[i]));
	}
	CHECK_EQ(read_word(PMBUS_STATUS_WORD, true), 0);
	CHECK(!alert_line);
}

int main(void)
{
	struct {
		const char *name;
		void (*fn)(void);
	} tests[] = {
		{"formats", test_formats},
		{"basic_reads", test_basic_reads},
		{"unsupported_command", test_unsupported_command},
		{"writes_and_pec", test_writes_and_pec},
		{"block_write", test_block_write},
		{"query", test_query},
		{"write_protect", test_write_protect},
		{"temperature_alert_and_ara", test_temperature_alert_and_ara},
		{"ov_fault_latch_and_restart", test_ov_fault_latch_and_restart},
		{"margins_and_overcurrent", test_margins_and_overcurrent},
		{"input_undervoltage_lockout", test_input_undervoltage_lockout},
		{"store_restore", test_store_restore},
		{"abort_and_quick_command", test_abort_and_quick_command},
		{"missing_sensor", test_missing_sensor},
		{"linux_probe", test_linux_probe},
	};

	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int before = failures;

		tests[i].fn();
		printf("%-28s %s\n", tests[i].name, failures == before ? "ok" : "FAILED");
	}
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
