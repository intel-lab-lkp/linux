/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/compiler.h>
#if defined(__i386__) || defined(__x86_64__)
#include "../../arch/x86/include/asm/barrier.h"
#elif defined(__arm__)
#include "../../arch/arm/include/asm/barrier.h"
#elif defined(__aarch64__)
#include "../../arch/arm64/include/asm/barrier.h"
#elif defined(__powerpc__)
#include "../../arch/powerpc/include/asm/barrier.h"
#elif defined(__riscv)
#include "../../arch/riscv/include/asm/barrier.h"
#elif defined(__s390__)
#include "../../arch/s390/include/asm/barrier.h"
#elif defined(__sh__)
#include "../../arch/sh/include/asm/barrier.h"
#elif defined(__sparc__)
#include "../../arch/sparc/include/asm/barrier.h"
#elif defined(__tile__)
#include "../../arch/tile/include/asm/barrier.h"
#elif defined(__alpha__)
#include "../../arch/alpha/include/asm/barrier.h"
#elif defined(__mips__)
#include "../../arch/mips/include/asm/barrier.h"
#elif defined(__ia64__)
#include "../../arch/ia64/include/asm/barrier.h"
#elif defined(__xtensa__)
#include "../../arch/xtensa/include/asm/barrier.h"
#else
#include <asm-generic/barrier.h>
#endif

/*
 * Generic fallback smp_*() definitions for archs that haven't
 * been updated yet.
 */

#ifndef smp_rmb
# define smp_rmb()	rmb()
#endif

#ifndef smp_wmb
# define smp_wmb()	wmb()
#endif

#ifndef smp_mb
# define smp_mb()	mb()
#endif

#ifndef smp_store_release
# define smp_store_release(p, v)		\
do {						\
	smp_mb();				\
	WRITE_ONCE(*p, v);			\
} while (0)
#endif

#ifndef smp_load_acquire
# define smp_load_acquire(p)			\
({						\
	typeof(*p) ___p1 = READ_ONCE(*p);	\
	smp_mb();				\
	___p1;					\
})
#endif

#ifndef cpu_relax
#define cpu_relax() ({})
#endif

/**
 * smp_acquire__after_ctrl_dep() - Provide ACQUIRE ordering after a control dependency
 *
 * A control dependency provides a LOAD->STORE order, the additional RMB
 * provides LOAD->LOAD order, together they provide LOAD->{LOAD,STORE} order,
 * aka. (load)-ACQUIRE.
 *
 * Architectures that do not do load speculation can have this be barrier().
 */
#ifndef smp_acquire__after_ctrl_dep
#define smp_acquire__after_ctrl_dep()		smp_rmb()
#endif

/**
 * smp_cond_load_relaxed() - (Spin) wait for cond with no ordering guarantees
 * @ptr: pointer to the variable to wait on
 * @cond: boolean expression to wait for
 *
 * Equivalent to using READ_ONCE() on the condition variable.
 *
 * Due to C lacking lambda expressions we load the value of *ptr into a
 * pre-named variable @VAL to be used in @cond.
 */
#ifndef smp_cond_load_relaxed
#define smp_cond_load_relaxed(ptr, cond_expr) ({		\
	typeof(ptr) __PTR = (ptr);				\
	__unqual_scalar_typeof(*ptr) VAL;			\
	for (;;) {						\
		VAL = READ_ONCE(*__PTR);			\
		if (cond_expr)					\
			break;					\
		cpu_relax();					\
	}							\
	(typeof(*ptr))VAL;					\
})
#endif

/**
 * smp_cond_load_acquire() - (Spin) wait for cond with ACQUIRE ordering
 * @ptr: pointer to the variable to wait on
 * @cond: boolean expression to wait for
 *
 * Equivalent to using smp_load_acquire() on the condition variable but employs
 * the control dependency of the wait to reduce the barrier on many platforms.
 */
#ifndef smp_cond_load_acquire
#define smp_cond_load_acquire(ptr, cond_expr) ({		\
	__unqual_scalar_typeof(*ptr) _val;			\
	_val = smp_cond_load_relaxed(ptr, cond_expr);		\
	smp_acquire__after_ctrl_dep();				\
	(typeof(*ptr))_val;					\
})
#endif
