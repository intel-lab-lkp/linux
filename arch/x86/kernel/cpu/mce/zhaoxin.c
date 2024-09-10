// SPDX-License-Identifier: GPL-2.0
/*
 * Zhaoxin specific MCE features
 * Author: Lyle Li
 */
#include <asm/msr.h>
#include "internal.h"

void mce_zhaoxin_feature_init(struct cpuinfo_x86 *c)
{
	struct mce_bank *mce_banks = this_cpu_ptr(mce_banks_array);

	/*
	 * These CPUs have MCA bank 8 which reports only one error type called
	 * SVAD (System View Address Decoder). The reporting of that error is
	 * controlled by IA32_MC8.CTL.0.
	 *
	 * If enabled, prefetching on these CPUs will cause SVAD MCE when
	 * virtual machines start and result in a system  panic. Always disable
	 * bank 8 SVAD error by default.
	 */
	if ((c->x86 == 7 && c->x86_model == 0x1b) ||
	    (c->x86_model == 0x19 || c->x86_model == 0x1f)) {
		if (this_cpu_read(mce_num_banks) > 8)
			mce_banks[8].ctl = 0;
	}

	intel_init_cmci();
	intel_init_lmce();
}

void mce_zhaoxin_feature_clear(struct cpuinfo_x86 *c)
{
	intel_clear_lmce();
}
