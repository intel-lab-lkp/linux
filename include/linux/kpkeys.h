/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_KPKEYS_H
#define _LINUX_KPKEYS_H

#include <linux/bug.h>
#include <linux/cleanup.h>
#include <linux/kpkeys_types.h>

/**
 * KPKEYS_GUARD_NOOP() - define a guard type that does nothing
 * @name: the name of the guard type
 *
 * Define a guard type that does nothing, useful to match a real guard type
 * that is defined under an #ifdef.
 */
#define KPKEYS_GUARD_NOOP(name)						\
	__DEFINE_CLASS_IS_CONDITIONAL(name, false);			\
	DEFINE_CLASS(name, bool, (void)_T, false, void);		\
	static inline void *class_##name##_lock_ptr(bool *_T)		\
	{ return _T; }

#ifdef CONFIG_ARCH_HAS_KPKEYS

#include <asm/kpkeys.h>

/**
 * KPKEYS_GUARD_COND() - define a guard type that conditionally switches to
 *                       a given kpkeys context
 * @name: the name of the guard type
 * @ctx: the kpkeys context to switch to
 * @cond: an expression that is evaluated as condition
 *
 * Define a guard type that switches to @ctx if @cond evaluates to true,
 * and does nothing otherwise.
 */
#define KPKEYS_GUARD_COND(name, ctx, cond)				\
	__DEFINE_CLASS_IS_CONDITIONAL(name, false);			\
	DEFINE_CLASS(name, struct kpkeys_state,				\
		     kpkeys_leave_context(&_T),				\
		     (cond) ? kpkeys_enter_context(ctx) :		\
			      (struct kpkeys_state) {}, void);		\
	static inline							\
	void *class_##name##_lock_ptr(struct kpkeys_state *_T)		\
	{ return _T; }

/**
 * KPKEYS_GUARD() - define a guard type that switches to a given kpkeys context
 *                  if kpkeys are supported
 * @name: the name of the guard type
 * @ctx: the kpkeys context to switch to
 *
 * Define a guard type that switches to @ctx if the system supports kpkeys.
 */
#define KPKEYS_GUARD(name, ctx)						\
	KPKEYS_GUARD_COND(name, ctx, kpkeys_supported())

/**
 * kpkeys_enter_context() - enter a kpkeys context
 * @ctx: the context to switch to
 *
 * Enters the specified kpkeys context. @ctx must be a compile-time constant.
 *
 * Return: state to be passed to kpkeys_leave_context().
 */
static __always_inline
struct kpkeys_state kpkeys_enter_context(enum kpkeys_ctx ctx)
{
	BUILD_BUG_ON_MSG(!__builtin_constant_p(ctx),
			 "kpkeys_enter_context() only takes constant values");
	BUILD_BUG_ON_MSG(ctx < 0 || ctx >= KPKEYS_CTX_COUNT,
			 "Invalid value passed to kpkeys_enter_context()");

	return arch_kpkeys_enter_context(ctx);
}

/**
 * kpkeys_leave_context() - leave a kpkeys context
 * @state: state returned by kpkeys_enter_context()
 *
 * Restores the state saved when entering a kpkeys context. If no context was
 * entered, this function does nothing.
 */
static __always_inline
void kpkeys_leave_context(const struct kpkeys_state *state)
{
	if (state->entered_context)
		arch_kpkeys_leave_context(state);
}

static inline bool kpkeys_supported(void)
{
	return arch_supports_kpkeys();
}

#else /* CONFIG_ARCH_HAS_KPKEYS */

static inline bool kpkeys_supported(void)
{
	return false;
}

#endif /* CONFIG_ARCH_HAS_KPKEYS */

#endif /* _LINUX_KPKEYS_H */
