/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_KPKEYS_H
#define _LINUX_KPKEYS_H

#include <linux/bug.h>
#include <linux/cleanup.h>

#define KPKEYS_LVL_DEFAULT	0

#define KPKEYS_LVL_MIN		KPKEYS_LVL_DEFAULT
#define KPKEYS_LVL_MAX		KPKEYS_LVL_DEFAULT

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

#endif /* _LINUX_KPKEYS_H */
