/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_KPKEYS_H
#define __ASM_KPKEYS_H

#include <linux/kpkeys_types.h>

#include <asm/barrier.h>
#include <asm/cpufeature.h>
#include <asm/por.h>

/*
 * Equivalent to por_set_kpkeys_context(0, KPKEYS_CTX_DEFAULT), but can also be
 * used in assembly.
 */
#define POR_EL1_INIT	(POR_ELx_PERM_PREP(0, POE_RWX) | \
			 POR_ELx_PERM_PREP(KPKEYS_PKEY_PGTABLES, POE_R))

#ifndef __ASSEMBLY__

static inline bool arch_supports_kpkeys(void)
{
	return system_supports_poe();
}

static inline bool arch_supports_kpkeys_early(void)
{
	/* POE is a boot feature */
	return boot_capabilities_finalized() ?
		system_supports_poe() : cpu_has_poe();
}

#ifdef CONFIG_ARM64_POE

static inline u64 por_set_kpkeys_context(u64 por, enum kpkeys_ctx ctx)
{
	por = por_elx_set_pkey_perms(por, 0, POE_RWX);
	por = por_elx_set_pkey_perms(por, KPKEYS_PKEY_PGTABLES,
				     ctx == KPKEYS_CTX_PGTABLES ? POE_RW : POE_R);

	return por;
}

static __always_inline void __kpkeys_set_pkey_reg_nosync(u64 pkey_reg)
{
	write_sysreg_s(pkey_reg, SYS_POR_EL1);
}

static __always_inline
struct kpkeys_state arch_kpkeys_enter_context(enum kpkeys_ctx ctx)
{
	const u64 prev_por = read_sysreg_s(SYS_POR_EL1);
	const u64 new_por = por_set_kpkeys_context(prev_por, ctx);

	if (prev_por == new_por)
		return (struct kpkeys_state) { .entered_context = false };

	__kpkeys_set_pkey_reg_nosync(new_por);
	isb();

	return (struct kpkeys_state) {
		.entered_context = true,
		.arch_state.por = prev_por,
	};
}

static __always_inline
void arch_kpkeys_leave_context(const struct kpkeys_state *state)
{
	__kpkeys_set_pkey_reg_nosync(state->arch_state.por);
	isb();
}

#endif /* CONFIG_ARM64_POE */

#ifdef CONFIG_KPKEYS_HARDENED_PGTABLES

#define arch_kpkeys_protect_static_pgtables arch_kpkeys_protect_static_pgtables
void arch_kpkeys_protect_static_pgtables(void);

#endif /* CONFIG_KPKEYS_HARDENED_PGTABLES */

#endif	/* __ASSEMBLY__ */

#endif	/* __ASM_KPKEYS_H */
