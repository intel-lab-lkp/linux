// SPDX-License-Identifier: GPL-2.0
/*
 * Zhaoxin specific MCE features
 * Author: Lyle Li
 */
#include <asm/msr.h>
#include "internal.h"

static void mce_zhaoxin_apply_mce_broadcast(struct cpuinfo_x86 *c)
{
	struct mca_config *cfg = &mca_cfg;

	/* Older CPUs do not do MCE broadcast */
	if (c->x86 < 6)
		return;
	/*
	 * All newer Zhaoxin and Centaur CPUs support MCE broadcasting. Enable
	 * synchronization with a one second timeout.
	 */
	if (c->x86 > 6)
		goto mce_broadcast;

	if (c->x86_vendor == X86_VENDOR_CENTAUR) {
		if (c->x86_model == 0xf && c->x86_stepping >= 0xe)
			goto mce_broadcast;
	} else if (c->x86_vendor == X86_VENDOR_ZHAOXIN) {
		if (c->x86_model == 0x19 || c->x86_model == 0x1f)
			goto mce_broadcast;
	}

	return;

mce_broadcast:
	if (cfg->monarch_timeout <= 0)
		cfg->monarch_timeout = USEC_PER_SEC;
}

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

	mce_zhaoxin_apply_mce_broadcast(c);
	intel_init_cmci();
	intel_init_lmce();
}

void mce_zhaoxin_feature_clear(struct cpuinfo_x86 *c)
{
	intel_clear_lmce();
}
