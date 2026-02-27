/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_KPKEYS_H
#define __ASM_KPKEYS_H

#include <asm/barrier.h>
#include <asm/cpufeature.h>
#include <asm/por.h>

#include <asm-generic/kpkeys.h>

static inline bool arch_kpkeys_enabled(void)
{
	return system_supports_poe();
}

#ifdef CONFIG_ARM64_POE

static inline u64 por_set_kpkeys_level(u64 por, int level)
{
	por = por_elx_set_pkey_perms(por, KPKEYS_PKEY_DEFAULT, POE_RWX);

	return por;
}

static __always_inline void __kpkeys_set_pkey_reg_nosync(u64 pkey_reg)
{
	write_sysreg_s(pkey_reg, SYS_POR_EL1);
}

static __always_inline int arch_kpkeys_set_level(int level)
{
	u64 prev_por = read_sysreg_s(SYS_POR_EL1);
	u64 new_por = por_set_kpkeys_level(prev_por, level);

	__kpkeys_set_pkey_reg_nosync(new_por);
	isb();

	return prev_por;
}

static __always_inline void arch_kpkeys_restore_pkey_reg(u64 pkey_reg)
{
	__kpkeys_set_pkey_reg_nosync(pkey_reg);
	isb();
}

#endif /* CONFIG_ARM64_POE */

#endif	/* __ASM_KPKEYS_H */
