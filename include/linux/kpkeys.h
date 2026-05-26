/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_KPKEYS_H
#define _LINUX_KPKEYS_H

#include <linux/bug.h>
#include <linux/cleanup.h>
#include <linux/jump_label.h>

#define KPKEYS_CTX_DEFAULT	0
#define KPKEYS_CTX_PGTABLES	1

#define KPKEYS_CTX_MIN		KPKEYS_CTX_DEFAULT
#define KPKEYS_CTX_MAX		KPKEYS_CTX_PGTABLES

/*
 * ... is used to discard extra arguments - this allows users of this macro
 * to have set_arg default to void.
 */
#define __KPKEYS_GUARD(name, set_context, restore_pkey_reg, set_arg, ...) \
	__DEFINE_CLASS_IS_CONDITIONAL(name, false);			\
	DEFINE_CLASS(name, u64,						\
		     restore_pkey_reg, set_context, set_arg);		\
	static inline void *class_##name##_lock_ptr(u64 *_T)		\
	{ return _T; }

/**
 * KPKEYS_GUARD_NOOP() - define a guard type that does nothing
 * @name: the name of the guard type
 * @cond_arg: an argument specification (optional)
 *
 * Define a guard type that does nothing, useful to match a real guard type
 * that is defined under an #ifdef. @cond_arg may optionally be passed to match
 * a guard defined using KPKEYS_GUARD_COND().
 */
#define KPKEYS_GUARD_NOOP(name, ...)					\
	__KPKEYS_GUARD(name, 0, (void)_T, ##__VA_ARGS__, void)

#ifdef CONFIG_ARCH_HAS_KPKEYS

#include <asm/kpkeys.h>

/**
 * KPKEYS_GUARD_COND() - define a guard type that conditionally switches to
 *                       a given kpkeys context
 * @name: the name of the guard type
 * @ctx: the kpkeys context to switch to
 * @cond: an expression that is evaluated as condition
 * @cond_arg: an argument specification for the condition (optional)
 *
 * Define a guard type that switches to @ctx if @cond evaluates to true,
 * and does nothing otherwise. @cond_arg may be specified to give access to a
 * caller-defined argument to @cond.
 */
#define KPKEYS_GUARD_COND(name, ctx, cond, ...)				\
	__KPKEYS_GUARD(name,						\
		       (cond) ? kpkeys_set_context(ctx)			\
			      : KPKEYS_PKEY_REG_INVAL,			\
		       kpkeys_restore_pkey_reg(_T),			\
		       ##__VA_ARGS__, void)

/**
 * KPKEYS_GUARD() - define a guard type that switches to a given kpkeys context
 *                  if kpkeys are enabled
 * @name: the name of the guard type
 * @ctx: the kpkeys context to switch to
 *
 * Define a guard type that switches to @ctx if the system supports kpkeys.
 */
#define KPKEYS_GUARD(name, ctx)						\
	KPKEYS_GUARD_COND(name, ctx, kpkeys_enabled())

/**
 * kpkeys_set_context() - switch kpkeys context
 * @ctx: the context to switch to
 *
 * Switches to specified kpkeys context. @ctx must be a compile-time
 * constant. The arch-specific pkey register will be updated accordingly, and
 * the original value returned.
 *
 * Return: the original pkey register value if the register was written to, or
 *         KPKEYS_PKEY_REG_INVAL otherwise (no write to the register was
 *         required).
 */
static __always_inline u64 kpkeys_set_context(int ctx)
{
	BUILD_BUG_ON_MSG(!__builtin_constant_p(ctx),
			 "kpkeys_set_context() only takes constant values");
	BUILD_BUG_ON_MSG(ctx < KPKEYS_CTX_MIN || ctx > KPKEYS_CTX_MAX,
			 "Invalid value passed to kpkeys_set_context()");

	return arch_kpkeys_set_context(ctx);
}

/**
 * kpkeys_restore_pkey_reg() - restores a pkey register value
 * @pkey_reg: the pkey register value to restore
 *
 * This function is meant to be passed the value returned by
 * kpkeys_set_context(), in order to restore the pkey register to its original
 * value (thus restoring the original kpkeys context).
 */
static __always_inline void kpkeys_restore_pkey_reg(u64 pkey_reg)
{
	if (pkey_reg != KPKEYS_PKEY_REG_INVAL)
		arch_kpkeys_restore_pkey_reg(pkey_reg);
}

static inline bool kpkeys_enabled(void)
{
	return arch_supports_kpkeys();
}

#else /* CONFIG_ARCH_HAS_KPKEYS */

#include <asm-generic/kpkeys.h>

static inline bool kpkeys_enabled(void)
{
	return false;
}

#endif /* CONFIG_ARCH_HAS_KPKEYS */

#ifdef CONFIG_KPKEYS_HARDENED_PGTABLES

DECLARE_STATIC_KEY_FALSE(kpkeys_hardened_pgtables_key);

static inline bool kpkeys_hardened_pgtables_enabled(void)
{
	return static_branch_unlikely(&kpkeys_hardened_pgtables_key);
}

static inline bool kpkeys_hardened_pgtables_early_enabled(void)
{
	return arch_supports_kpkeys_early();
}

/*
 * Should be called from mem_init(): as soon as the buddy allocator becomes
 * available and before any call to pagetable_alloc().
 */
void kpkeys_hardened_pgtables_init(void);

#else /* CONFIG_KPKEYS_HARDENED_PGTABLES */

static inline bool kpkeys_hardened_pgtables_enabled(void)
{
	return false;
}

static inline bool kpkeys_hardened_pgtables_early_enabled(void)
{
	return false;
}

static inline void kpkeys_hardened_pgtables_init(void) {}

#endif /* CONFIG_KPKEYS_HARDENED_PGTABLES */

#endif /* _LINUX_KPKEYS_H */
