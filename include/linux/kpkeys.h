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

#define __KPKEYS_GUARD(name, set_level, restore_pkey_reg, set_arg, ...)	\
	__DEFINE_CLASS_IS_CONDITIONAL(name, false);			\
	DEFINE_CLASS(name, u64,						\
		     restore_pkey_reg, set_level, set_arg);		\
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
 *                       a given kpkeys level
 * @name: the name of the guard type
 * @level: the kpkeys level to switch to
 * @cond: an expression that is evaluated as condition
 * @cond_arg: an argument specification for the condition (optional)
 *
 * Define a guard type that switches to @level if @cond evaluates to true, and
 * does nothing otherwise. @cond_arg may be specified to give access to a
 * caller-defined argument to @cond.
 */
#define KPKEYS_GUARD_COND(name, level, cond, ...)			\
	__KPKEYS_GUARD(name,						\
		       cond ? kpkeys_set_level(level)			\
			    : KPKEYS_PKEY_REG_INVAL,			\
		       kpkeys_restore_pkey_reg(_T),			\
		       ##__VA_ARGS__, void)

/**
 * KPKEYS_GUARD() - define a guard type that switches to a given kpkeys level
 *                  if kpkeys are enabled
 * @name: the name of the guard type
 * @level: the kpkeys level to switch to
 *
 * Define a guard type that switches to @level if the system supports kpkeys.
 */
#define KPKEYS_GUARD(name, level)					\
	KPKEYS_GUARD_COND(name, level, arch_kpkeys_enabled())

/**
 * kpkeys_set_level() - switch kpkeys level
 * @level: the level to switch to
 *
 * Switches the kpkeys level to the specified value. @level must be a
 * compile-time constant. The arch-specific pkey register will be updated
 * accordingly, and the original value returned.
 *
 * Return: the original pkey register value if the register was written to, or
 *         KPKEYS_PKEY_REG_INVAL otherwise (no write to the register was
 *         required).
 */
static __always_inline u64 kpkeys_set_level(int level)
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
static __always_inline void kpkeys_restore_pkey_reg(u64 pkey_reg)
{
	if (pkey_reg != KPKEYS_PKEY_REG_INVAL)
		arch_kpkeys_restore_pkey_reg(pkey_reg);
}

#else /* CONFIG_ARCH_HAS_KPKEYS */

#include <asm-generic/kpkeys.h>

static inline bool arch_kpkeys_enabled(void)
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

struct page *kpkeys_pgtable_alloc(gfp_t gfp);
void kpkeys_pgtable_free(struct page *page);

int kpkeys_prepare_direct_map_split(void);
bool kpkeys_ready_for_direct_map_split(void);

/*
 * Should be called from mem_init(): as soon as the buddy allocator becomes
 * available and before any call to pagetable_alloc().
 */
void kpkeys_hardened_pgtables_init(void);

/*
 * Should be called by architecture code as soon as it is safe to modify the
 * pkey of arbitrary linear map ranges.
 */
void kpkeys_hardened_pgtables_init_late(void);

phys_addr_t kpkeys_physmem_pgtable_alloc(void);

#ifndef arch_kpkeys_protect_static_pgtables
static inline void arch_kpkeys_protect_static_pgtables(void) {}
#endif

#else /* CONFIG_KPKEYS_HARDENED_PGTABLES */

static inline bool kpkeys_hardened_pgtables_enabled(void)
{
	return false;
}

static inline struct page *kpkeys_pgtable_alloc(gfp_t gfp)
{
	return NULL;
}

static inline int kpkeys_prepare_direct_map_split(void)
{
	return 0;
}

static inline bool kpkeys_ready_for_direct_map_split(void)
{
	return true;
}

static inline void kpkeys_pgtable_free(struct page *page) {}

static inline void kpkeys_hardened_pgtables_init(void) {}

static inline void kpkeys_hardened_pgtables_init_late(void) {}

static inline phys_addr_t kpkeys_physmem_pgtable_alloc(void)
{
	return 0;
}

#endif /* CONFIG_KPKEYS_HARDENED_PGTABLES */

#endif /* _LINUX_KPKEYS_H */
