/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CMPXCHG_32_H
#define _ASM_X86_CMPXCHG_32_H

/*
 * Note: if you use __cmpxchg64(), or their variants,
 *       you need to test for the feature in boot_cpu_data.
 */

union __u64_halves {
	u64 full;
	struct {
		u32 low, high;
	};
};

#define __arch_cmpxchg64(_ptr, _old, _new, _lock)			\
({									\
	union __u64_halves o = { .full = (_old), },			\
			   n = { .full = (_new), };			\
									\
	asm_inline volatile(_lock "cmpxchg8b %[ptr]"			\
		     : [ptr] "+m" (*(_ptr)),				\
		       "+a" (o.low), "+d" (o.high)			\
		     : "b" (n.low), "c" (n.high)			\
		     : "memory");					\
									\
	o.full;								\
})


static __always_inline u64 __cmpxchg64(volatile u64 *ptr, u64 old, u64 new)
{
	return __arch_cmpxchg64(ptr, old, new, LOCK_PREFIX);
}

static __always_inline u64 __cmpxchg64_local(volatile u64 *ptr, u64 old, u64 new)
{
	return __arch_cmpxchg64(ptr, old, new,);
}

#define __arch_try_cmpxchg64(_ptr, _oldp, _new, _lock)			\
({									\
	union __u64_halves o = { .full = *(_oldp), },			\
			   n = { .full = (_new), };			\
	bool ret;							\
									\
	asm_inline volatile(_lock "cmpxchg8b %[ptr]"			\
		     : "=@ccz" (ret),					\
		       [ptr] "+m" (*(_ptr)),				\
		       "+a" (o.low), "+d" (o.high)			\
		     : "b" (n.low), "c" (n.high)			\
		     : "memory");					\
									\
	if (unlikely(!ret))						\
		*(_oldp) = o.full;					\
									\
	likely(ret);							\
})

static __always_inline bool __try_cmpxchg64(volatile u64 *ptr, u64 *oldp, u64 new)
{
	return __arch_try_cmpxchg64(ptr, oldp, new, LOCK_PREFIX);
}

static __always_inline bool __try_cmpxchg64_local(volatile u64 *ptr, u64 *oldp, u64 new)
{
	return __arch_try_cmpxchg64(ptr, oldp, new,);
}

#define arch_cmpxchg64 __cmpxchg64

#define arch_cmpxchg64_local __cmpxchg64_local

#define arch_try_cmpxchg64 __try_cmpxchg64

#define arch_try_cmpxchg64_local __try_cmpxchg64_local

#define system_has_cmpxchg64()		true

#endif /* _ASM_X86_CMPXCHG_32_H */
