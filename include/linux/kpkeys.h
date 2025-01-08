/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_KPKEYS_H
#define _LINUX_KPKEYS_H

#include <linux/bug.h>
#include <linux/cleanup.h>
#include <linux/jump_label.h>

#define KPKEYS_LVL_DEFAULT	0
#define KPKEYS_LVL_PGTABLES	1

#define KPKEYS_LVL_MIN		KPKEYS_LVL_DEFAULT
#define KPKEYS_LVL_MAX		KPKEYS_LVL_PGTABLES

#define KPKEYS_GUARD(_name, set_level, restore_pkey_reg)		\
	__DEFINE_CLASS_IS_CONDITIONAL(_name, false);			\
	DEFINE_CLASS(_name, u64,					\
		     restore_pkey_reg, set_level, void);		\
	static inline void *class_##_name##_lock_ptr(u64 *_T)		\
	{ return _T; }

#ifdef CONFIG_ARCH_HAS_KPKEYS

#include <asm/kpkeys.h>

/**
 * kpkeys_set_level() - switch kpkeys level
 * @level: the level to switch to
 *
 * Switches the kpkeys level to the specified value. @level must be a
 * compile-time constant. The arch-specific pkey register will be updated
 * accordingly, and the original value returned.
 *
 * Return: the original pkey register value.
 */
static inline u64 kpkeys_set_level(int level)
{
	BUILD_BUG_ON_MSG(!__builtin_constant_p(level),
			 "kpkeys_set_level() only takes constant levels");
	BUILD_BUG_ON_MSG(level < KPKEYS_LVL_MIN || level > KPKEYS_LVL_MAX,
			 "Invalid level passed to kpkeys_set_level()");

	return arch_kpkeys_set_level(level);
}

/**
 * kpkeys_restore_pkey_reg() - restores a pkey register value
 * @pkey_reg: the pkey register value to restore
 *
 * This function is meant to be passed the value returned by kpkeys_set_level(),
 * in order to restore the pkey register to its original value (thus restoring
 * the original kpkeys level).
 */
static inline void kpkeys_restore_pkey_reg(u64 pkey_reg)
{
	arch_kpkeys_restore_pkey_reg(pkey_reg);
}

#else /* CONFIG_ARCH_HAS_KPKEYS */

static inline bool arch_kpkeys_enabled(void)
{
	return false;
}

#endif /* CONFIG_ARCH_HAS_KPKEYS */

#ifdef CONFIG_KPKEYS_HARDENED_PGTABLES

DECLARE_STATIC_KEY_FALSE(kpkeys_hardened_pgtables_enabled);

/*
 * Use guard(kpkeys_hardened_pgtables)() to temporarily grant write access
 * to page tables.
 */
KPKEYS_GUARD(kpkeys_hardened_pgtables,
	     static_branch_unlikely(&kpkeys_hardened_pgtables_enabled) ?
		     kpkeys_set_level(KPKEYS_LVL_PGTABLES) :
		     KPKEYS_PKEY_REG_INVAL,
	     _T != KPKEYS_PKEY_REG_INVAL ?
		     kpkeys_restore_pkey_reg(_T) :
		     (void)0)

int kpkeys_protect_pgtable_memory(struct folio *folio);
int kpkeys_unprotect_pgtable_memory(struct folio *folio);

/*
 * Enables kpkeys_hardened_pgtables and switches existing kernel page tables to
 * a privileged pkey (KPKEYS_PKEY_PGTABLES).
 *
 * Should be called as early as possible by architecture code, after (k)pkeys
 * are initialised and before any user task is spawned.
 */
void kpkeys_hardened_pgtables_enable(void);

#else /* CONFIG_KPKEYS_HARDENED_PGTABLES */

KPKEYS_GUARD(kpkeys_hardened_pgtables, 0, (void)_T)

static inline int kpkeys_protect_pgtable_memory(struct folio *folio)
{
	return 0;
}
static inline int kpkeys_unprotect_pgtable_memory(struct folio *folio)
{
	return 0;
}
static inline void kpkeys_hardened_pgtables_enable(void) {}

#endif /* CONFIG_KPKEYS_HARDENED_PGTABLES */

#endif /* _LINUX_KPKEYS_H */
