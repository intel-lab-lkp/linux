// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#include "lsdc_drv.h"
#include "lsdc_probe.h"

/*
 * Processor ID (implementation) values for bits 15:8 of the PRID register.
 */
#define LOONGSON_CPU_IMP_MASK           0xff00
#define LOONGSON_CPU_IMP_SHIFT          8

#define LOONGARCH_CPU_IMP_LS2K1000      0xa0
#define LOONGARCH_CPU_IMP_LS2K2000      0xb0
#define LOONGARCH_CPU_IMP_LS3A5000      0xc0

#define LOONGSON_CPU_MIPS_IMP_LS2K      0x61 /* Loongson 2K Mips series SoC */

/*
 * Particular Revision values for bits 7:0 of the PRID register.
 */
#define LOONGSON_CPU_REV_MASK           0x00ff

#define LOONGARCH_CPUCFG_PRID_REG       0x0

/*
 * We can achieve fine-grained control with the information about the host.
 */

unsigned int loongson_cpu_get_prid(u8 *imp, u8 *rev)
{
	unsigned int prid = 0;

#if defined(__loongarch__)
	__asm__ volatile("cpucfg %0, %1\n\t"
			: "=&r"(prid)
			: "r"(LOONGARCH_CPUCFG_PRID_REG)
			);
#endif

#if defined(__mips__)
	__asm__ volatile("mfc0\t%0, $15\n\t"
			: "=r" (prid)
			);
#endif

	if (imp)
		*imp = (prid & LOONGSON_CPU_IMP_MASK) >> LOONGSON_CPU_IMP_SHIFT;

	if (rev)
		*rev = prid & LOONGSON_CPU_REV_MASK;

	return prid;
}

enum loongson_chip_id loongson_chip_id_fixup(enum loongson_chip_id chip_id)
{
	u8 impl;

	if (loongson_cpu_get_prid(&impl, NULL)) {
		/*
		 * LS2K1000 has the LoongArch edition(with two LA264 CPU core)
		 * and the Mips edition(with two mips64r2 CPU core), Only the
		 * instruction set of the CPU are changed, the peripheral
		 * devices are basically same.
		 */
		if (chip_id == CHIP_LS7A1000) {
#if defined(__loongarch__)
			if (impl == LOONGARCH_CPU_IMP_LS2K1000)
				return CHIP_LS2K1000;
#endif

#if defined(__mips__)
			if (impl == LOONGSON_CPU_MIPS_IMP_LS2K)
				return CHIP_LS2K1000;
#endif
		}
	}

	return chip_id;
}
