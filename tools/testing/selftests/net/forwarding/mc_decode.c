// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <stdlib.h>
#include <linux/bitops.h>

/* 8-bit floating-point exponential field decode */
#define FP_8BIT_EXP(value)		(((value) >> 4) & 0x07)
#define FP_8BIT_MAN(value)		((value) & 0x0f)

/* 16-bit floating-point exponential field decode */
#define FP_16BIT_EXP(value)		(((value) >> 12) & 0x0007)
#define FP_16BIT_MAN(value)		((value) & 0x0fff)

/* 8-bit floating-point exponential field linear threshold */
#define FP_8BIT_MIN_THRESHOLD		128
/* 8-bit non linear max representable (mant = 0xF, exp = 7) -> 31744 */
#define FP_8BIT_MAX_THRESHOLD		31744

/* 16-bit floating-point exponential field linear threshold */
#define FP_16BIT_MIN_THRESHOLD		32768UL
/* 16-bit non linear max representable (mant = 0xFFF, exp = 7) -> 8387584 */
#define FP_16BIT_MAX_THRESHOLD		8387584

/* This decodes 8-bit floating-point exponential values */
static inline uint32_t decode_8bit_field(const u8 code)
{
	if (code < FP_8BIT_MIN_THRESHOLD) {
		return code;
	} else {
		uint32_t mc_man, mc_exp;

		mc_exp = FP_8BIT_EXP(code);
		mc_man = FP_8BIT_MAN(code);
		return (mc_man | 0x10) << (mc_exp + 3);
	}
}

/* This decodes 16-bit floating-point exponential values */
static inline uint32_t decode_16bit_field(const uint16_t code)
{
	if (code < FP_16BIT_MIN_THRESHOLD) {
		return code;
	} else {
		uint32_t mc_man, mc_exp;

		mc_exp = FP_16BIT_EXP(code);
		mc_man = FP_16BIT_MAN(code);

		return (mc_man | 0x1000) << (mc_exp + 3);
	}
}

int main(int argc, char *argv[])
{
	uint32_t bits = 8, code = 0, decode = 0;

	if (argc != 3)
		return 1;

	if (bits != 8 && bits != 16)
		return 1;

	bits = atoi(argv[1]);
	code = atoi(argv[2]);

	if (bits == 8)
		decode = decode_8bit_field(code);
	else
		decode = decode_16bit_field(code);
	printf("%u\n", decode);

	return 0;
}
