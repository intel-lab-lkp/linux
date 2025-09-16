// SPDX-License-Identifier: GPL-2.0-only
#include <asm/qos.h>

/* cached value of sqoscfg csr for each cpu */
DEFINE_PER_CPU(u32, cpu_srmcfg);
