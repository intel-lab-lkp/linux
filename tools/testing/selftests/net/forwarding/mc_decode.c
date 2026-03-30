// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <stdlib.h>
#include <linux/bitops.h>

#define IGMPV3_FP_EXP(value)            (((value) >> 4) & 0x07)
#define IGMPV3_FP_MAN(value)            ((value) & 0x0f)

/* IGMPV3 floating-point exponential field threshold */
#define IGMPV3_EXP_MIN_THRESHOLD        128

static inline unsigned long decode_field(const u8 code)
{
    /* RFC3376, relevant sections:
     *  - 4.1.1. Maximum Response Code
     *  - 4.1.7. QQIC (Querier's Query Interval Code)
     */
    if (code < IGMPV3_EXP_MIN_THRESHOLD) {
        return (unsigned long)code;
    } else {
        unsigned long mc_man, mc_exp;
        mc_exp = IGMPV3_FP_EXP(code);
        mc_man = IGMPV3_FP_MAN(code);
        return ((mc_man | 0x10) << (mc_exp + 3));
    }
}

int main(int argc, char *argv[])
{
    uint8_t qqic = 0;
    if (argc >= 2)
        qqic = atoi(argv[1]);
    unsigned long qqi = decode_field(qqic);

    printf("%lu\n", qqi);

    return 0;
}
