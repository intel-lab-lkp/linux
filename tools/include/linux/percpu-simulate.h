/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Userspace implementation of per_cpu_ptr for adapted kernel code.
 *
 * Userspace code does not have and does not need a per-cpu concept, but
 * instead can declare variables as thread-local.  However, the kernel per-cpu
 * further provides 1) the guarantee of static lifetime when thread exits, and
 * 2) the ability to access other CPU's per-cpu data.  This file provides a
 * simple implementation of such functionality, but with slightly different
 * APIs and without linker script changes.
 *
 * 2025  Yuzhuo Jing <yuzhuo@google.com>
 */
#ifndef __PERCPU_SIMULATE_H__
#define __PERCPU_SIMULATE_H__

#include <assert.h>

#include <linux/compiler.h>
#include <linux/types.h>

/*
 * The maximum supported number of CPUs.  Per-cpu variables are defined as a
 * PERCPU_MAX length array, indexed by a thread-local cpu id.
 */
#define PERCPU_MAX 4096

#ifdef ASSERT_PERCPU
#define __check_cpu_id(cpu)						\
({									\
	u32 cpuid = (cpu);						\
	assert(cpuid < PERCPU_MAX);					\
	cpuid;								\
})
#else
#define __check_cpu_id(cpu)	(cpu)
#endif

/*
 * Use weak symbol: only define __thread_per_cpu_id variable if any perf tool
 * includes this header file.
 */
_Thread_local u32 __thread_per_cpu_id __weak;

static inline u32 get_this_cpu_id(void)
{
	return __thread_per_cpu_id;
}

/*
 * The user code must call this function inside of each thread that uses
 * per-cpu data structures.  The user code can choose an id of their choice,
 * but must ensure each thread uses a different id.
 *
 * Safety: asserts CPU id smaller than PERCPU_MAX if ASSERT_PERCPU is defined.
 */
static inline void set_this_cpu_id(u32 id)
{
	__thread_per_cpu_id = __check_cpu_id(id);
}

/*
 * Declare a per-cpu data structure.  This only declares the data type and
 * array length. Different per-cpu data are differentiated by a key (identifer).
 *
 * Different from the kernel version, this API must be called before the actual
 * definition (i.e. DEFINE_PER_CPU_ALIGNED).
 *
 * Note that this implementation does not support prepending static qualifier,
 * or appending assignment expressions.
 */
#define DECLARE_PER_CPU_ALIGNED(key, type, data) \
	extern struct __percpu_type_##key { \
		type data; \
	} __percpu_data_##key[PERCPU_MAX]

/*
 * Define the per-cpu data storage for a given key.  This uses a previously
 * defined data type in DECLARE_PER_CPU_ALIGNED.
 *
 * Different from the kernel version, this API only accepts a key name.
 */
#define DEFINE_PER_CPU_ALIGNED(key) \
	struct __percpu_type_##key __percpu_data_##key[PERCPU_MAX]

#define __raw_per_cpu_value(key, field, cpu) \
	(__percpu_data_##key[cpu].field)

/*
 * Get a pointer of per-cpu data for a given key.
 *
 * Different from the kernel version, users of this API don't need to pass the
 * address of the base variable (through `&varname').
 *
 * Safety: asserts CPU id smaller than PERCPU_MAX if ASSERT_PERCPU is defined.
 */
#define per_cpu_ptr(key, field, cpu) (&per_cpu_value(key, field, cpu))
#define this_cpu_ptr(key, field) (&this_cpu_value(key, field))

/*
 * Additional APIs for direct value access.  Effectively, `*per_cpu_ptr(...)'.
 *
 * Safety: asserts CPU id smaller than PERCPU_MAX if ASSERT_PERCPU is defined.
 */
#define per_cpu_value(key, field, cpu) \
	(__raw_per_cpu_value(key, field, __check_cpu_id(cpu)))
#define this_cpu_value(key, field) \
	(__raw_per_cpu_value(key, field, __thread_per_cpu_id))

/*
 * Helper functions of simple per-cpu operations.
 *
 * The kernel version differentiates __this_cpu_* from this_cpu_* for
 * preemption/interrupt-safe contexts, but the userspace version defines them
 * as the same.
 */

#define __this_cpu_add(key, field, val)	(this_cpu_value(key, field) += (val))
#define __this_cpu_sub(key, field, val)	(this_cpu_value(key, field) -= (val))
#define __this_cpu_inc(key, field)	(++this_cpu_value(key, field))
#define __this_cpu_dec(key, field)	(--this_cpu_value(key, field))

#define this_cpu_add	__this_cpu_add
#define this_cpu_sub	__this_cpu_sub
#define this_cpu_inc	__this_cpu_inc
#define this_cpu_dec	__this_cpu_dec

#endif /* __PERCPU_SIMULATE_H__ */
