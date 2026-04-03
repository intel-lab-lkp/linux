// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <stdlib.h>
#include <linux/bitops.h>

/* 8-bit floating-point exponential field linear threshold */
#define FP_8BIT_MIN_THRESHOLD		128
/* 8-bit non linear max representable (mant = 0xF, exp = 7) -> 31744 */
#define FP_8BIT_MAX_THRESHOLD		31744

/* 16-bit floating-point exponential field linear threshold */
#define FP_16BIT_MIN_THRESHOLD		32768UL
/* 16-bit non linear max representable (mant = 0xFFF, exp = 7) -> 8387584 */
#define FP_16BIT_MAX_THRESHOLD		8387584

/* This encodes value to 8-bit floating-point exponential format */
static inline uint8_t encode_8bit_field(unsigned int value)
{
	uint8_t mc_exp, mc_man;

	/* Value < 128 is literal */
	if (value < FP_8BIT_MIN_THRESHOLD)
		return value;

	/* Saturate at max representable (mant = 0xF, exp = 7) -> 31744 */
	if (value >= FP_8BIT_MAX_THRESHOLD)
		return 0xFF;

	mc_exp  = fls(value) - 8;
	mc_man = (value >> (mc_exp + 3)) & 0x0F;

	return 0x80 | (mc_exp << 4) | mc_man;
}

/* This encodes value to 16-bit floating-point exponential format */
static inline uint16_t encode_16bit_field(unsigned int value)
{
	uint16_t mc_man, mc_exp;

	/* Value < 32768 is literal */
	if (value < FP_16BIT_MIN_THRESHOLD)
		return value;

	/* Saturate at max representable (mant = 0xFFF, exp = 7) -> 8387584 */
	if (value >= FP_16BIT_MAX_THRESHOLD)
		return 0xFFFF;

	mc_exp = fls(value) - 16;
	mc_man = (value >> (mc_exp + 3)) & 0x0FFF;

	return 0x8000 | (mc_exp << 12) | mc_man;
}

int main(int argc, char *argv[])
{
	unsigned int bits = 8, value = 0;
	uint8_t encoded8 = 0;
	uint16_t encoded16 = 0;

	if (argc != 3)
		return 1;

	bits = atoi(argv[1]);
	value = atoi(argv[2]);

	if (bits != 8 && bits != 16)
		return 1;

	if (bits == 8) {
		encoded8 = encode_8bit_field(value);
		printf("%hhu\n", encoded8);
	} else {
		encoded16 = encode_16bit_field(value);
		printf("%hu\n", encoded16);
	}

	return 0;
}
