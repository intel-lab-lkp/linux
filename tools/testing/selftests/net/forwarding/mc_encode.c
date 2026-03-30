// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <stdlib.h>
#include <linux/bitops.h>

/* IGMPV3 floating-point exponential field threshold */
#define IGMPV3_EXP_MIN_THRESHOLD        128
/* Max representable (mant = 0xF, exp = 7) -> 31744 */
#define IGMPV3_EXP_MAX_THRESHOLD        31744

static inline uint8_t encode_field(unsigned int value)
{
    uint8_t mc_exp, mc_man;

    /* RFC3376: QQIC/MRC < 128 is literal */
    if (value < IGMPV3_EXP_MIN_THRESHOLD)
        return (uint8_t)value;

    /* Saturate at max representable (mant = 0xF, exp = 7) -> 31744 */
    if (value >= IGMPV3_EXP_MAX_THRESHOLD)
        return 0xFF;

    mc_exp  = (uint8_t)(fls(value) - 8);
    mc_man = (uint8_t)((value >> (mc_exp + 3)) & 0x0F);

    return 0x80 | (mc_exp << 4) | mc_man;
}

int main(int argc, char *argv[])
{
    unsigned int qqi = 0;
    if (argc >= 2)
        qqi = atoi(argv[1]);

    uint8_t qqic = encode_field(qqi);

    printf("%u\n", qqic);

    return 0;
}
