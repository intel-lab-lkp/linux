/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __TOOLS_LINUX_ATOMIC_H
#define __TOOLS_LINUX_ATOMIC_H

#include <stdatomic.h>
#include <linux/types.h>  // For atomic_t

/*
 * Reimplementation of the kernel's atomic.h using C11's stdatomic.h to avoid
 * build logic around compilers, inline assembler, etc.
 */

#define ATOMIC_OP(op, c_op)						\
static inline void generic_atomic_##op(int i, atomic_t *v)		\
{									\
	atomic_fetch_##op(v, i);				\
}

#define ATOMIC_OP_RETURN(op, c_op)					\
static inline int generic_atomic_##op##_return(int i, atomic_t *v)	\
{									\
	int c =	atomic_fetch_##op(v, i);			\
									\
	return c c_op i;						\
}

#define ATOMIC_FETCH_OP(op, c_op)					\
static inline int generic_atomic_fetch_##op(int i, atomic_t *v)		\
{									\
	return atomic_fetch_##op(v, i);			\
}

static inline int generic_atomic_read(const atomic_t *v)
{
	return atomic_load(v);
}

static inline void generic_atomic_set(atomic_t *v, int i)
{
	atomic_store(v, i);
}

static inline int generic_atomic_cmpxchg_relaxed(atomic_t *v, int old, int new)
{
	int expected = old;

	atomic_compare_exchange_weak_explicit(v, &expected, new,
					memory_order_relaxed, memory_order_relaxed);
	return expected;
}

static inline int generic_atomic_cmpxchg_release(atomic_t *v, int old, int new)
{
	int expected = old;

	/*
	 * Note, the stricter memory_order_seq_cst is used as
	 * memory_order_release fails with an invalid-memory-model error.
	 */
	atomic_compare_exchange_weak_explicit(v, &expected, new,
					memory_order_seq_cst, memory_order_seq_cst);
	return expected;
}

ATOMIC_OP_RETURN(add, +)
ATOMIC_OP_RETURN(sub, -)

ATOMIC_FETCH_OP(add, +)
ATOMIC_FETCH_OP(sub, -)
ATOMIC_FETCH_OP(and, &)
ATOMIC_FETCH_OP(or, |)
ATOMIC_FETCH_OP(xor, ^)

ATOMIC_OP(add, +)
ATOMIC_OP(sub, -)
ATOMIC_OP(and, &)
ATOMIC_OP(or, |)
ATOMIC_OP(xor, ^)

#undef ATOMIC_FETCH_OP
#undef ATOMIC_OP_RETURN
#undef ATOMIC_OP

#define arch_atomic_add_return			generic_atomic_add_return
#define arch_atomic_sub_return			generic_atomic_sub_return

#define arch_atomic_fetch_add			generic_atomic_fetch_add
#define arch_atomic_fetch_sub			generic_atomic_fetch_sub
#define arch_atomic_fetch_and			generic_atomic_fetch_and
#define arch_atomic_fetch_or			generic_atomic_fetch_or
#define arch_atomic_fetch_xor			generic_atomic_fetch_xor

#define arch_atomic_add				generic_atomic_add
#define arch_atomic_sub				generic_atomic_sub
#define arch_atomic_and				generic_atomic_and
#define arch_atomic_or				generic_atomic_or
#define arch_atomic_xor				generic_atomic_xor

#define arch_atomic_read(v)			generic_atomic_read(v)
#define arch_atomic_set(v, i)			generic_atomic_set(v, i)
#define atomic_set(v, i)			generic_atomic_set(v, i)
#define atomic_read(v)				generic_atomic_read(v)
#define atomic_cmpxchg_relaxed(v, o, n)		generic_atomic_cmpxchg_relaxed(v, o, n)
#define atomic_cmpxchg_release(v, o, n)		generic_atomic_cmpxchg_release(v, o, n)
#define atomic_inc(v)				generic_atomic_add(1, v)
#define atomic_dec(v)				generic_atomic_sub(1, v)

#endif /* __TOOLS_LINUX_ATOMIC_H */
