/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pmbus/format.h>

uint8_t pmbus_crc8_byte(uint8_t crc, uint8_t byte)
{
	crc ^= byte;
	for (int i = 0; i < 8; i++) {
		crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
	}
	return crc;
}

uint8_t pmbus_crc8(uint8_t crc, const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		crc = pmbus_crc8_byte(crc, buf[i]);
	}
	return crc;
}

/* Rounds num / den to the nearest integer, halves away from zero. den must be positive. */
static int64_t div_round(int64_t num, int64_t den)
{
	return (num >= 0) ? (num + den / 2) / den : -((-num + den / 2) / den);
}

/* milli * 2^-n / 1000, rounded. */
static int64_t scale_from_milli(int32_t milli, int n)
{
	if (n < 0) {
		return div_round((int64_t)milli * ((int64_t)1 << -n), 1000);
	}
	return div_round(milli, (int64_t)1000 << n);
}

/* y * 2^n * 1000, rounded. */
static int32_t scale_to_milli(int32_t y, int n)
{
	if (n < 0) {
		return (int32_t)div_round((int64_t)y * 1000, (int64_t)1 << -n);
	}
	return (int32_t)((int64_t)y * 1000 * ((int64_t)1 << n));
}

uint16_t pmbus_linear11_encode(int32_t milli)
{
	int64_t y = 0;
	int n;

	for (n = -16; n <= 15; n++) {
		y = scale_from_milli(milli, n);
		if (y >= -1024 && y <= 1023) {
			break;
		}
	}
	if (n > 15) {
		n = 15;
		y = (milli < 0) ? -1024 : 1023;
	}
	return (uint16_t)((((unsigned int)n & 0x1F) << 11) | ((unsigned int)y & 0x7FF));
}

int32_t pmbus_linear11_decode(uint16_t word)
{
	int n = (word >> 11) & 0x1F;
	int32_t y = word & 0x7FF;

	if (n & 0x10) {
		n -= 32;
	}
	if (y & 0x400) {
		y -= 2048;
	}
	return scale_to_milli(y, n);
}

uint16_t pmbus_ulinear16_encode(int32_t milli, int8_t exponent)
{
	int64_t y = scale_from_milli(milli, exponent);

	if (y < 0) {
		return 0;
	}
	if (y > 0xFFFF) {
		return 0xFFFF;
	}
	return (uint16_t)y;
}

int32_t pmbus_ulinear16_decode(uint16_t word, int8_t exponent)
{
	return scale_to_milli(word, exponent);
}

int8_t pmbus_vout_mode_exponent(uint8_t vout_mode)
{
	int8_t n = vout_mode & 0x1F;

	return (n & 0x10) ? (int8_t)(n - 32) : n;
}
