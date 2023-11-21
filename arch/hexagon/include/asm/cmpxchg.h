/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xchg/cmpxchg operations for the Hexagon architecture
 *
 * Copyright (c) 2010-2011, The Linux Foundation. All rights reserved.
 */

#ifndef _ASM_CMPXCHG_H
#define _ASM_CMPXCHG_H

#include <linux/build_bug.h>

/*
 * __arch_xchg - atomically exchange a register and a memory location
 * @x: value to swap
 * @ptr: pointer to memory
 * @size:  size of the value
 *
 * Only 4 bytes supported currently.
 *
 * Note:  there was an errata for V2 about .new's and memw_locked.
 *
 */
static inline unsigned long
__arch_xchg(unsigned long x, volatile void *ptr, int size)
{
	unsigned long retval;

	/*  Can't seem to use printk or panic here, so just stop  */
	if (size != 4) do { asm volatile("brkpt;\n"); } while (1);

	__asm__ __volatile__ (
	"1:	%0 = memw_locked(%1);\n"    /*  load into retval */
	"	memw_locked(%1,P0) = %2;\n" /*  store into memory */
	"	if (!P0) jump 1b;\n"
	: "=&r" (retval)
	: "r" (ptr), "r" (x)
	: "memory", "p0"
	);
	return retval;
}

/*
 * Atomically swap the contents of a register with memory.  Should be atomic
 * between multiple CPU's and within interrupts on the same CPU.
 */
#define arch_xchg(ptr, v) ((__typeof__(*(ptr)))__arch_xchg((unsigned long)(v), (ptr), \
							   sizeof(*(ptr))))

/*
 *  see rt-mutex-design.txt; cmpxchg supposedly checks if *ptr == A and swaps.
 *  looks just like atomic_cmpxchg on our arch currently with a bunch of
 *  variable casting.
 */

#define __cmpxchg_32(ptr, old, new)				\
({								\
	__typeof__(ptr) __ptr = (ptr);				\
	__typeof__(*(ptr)) __old = (old);			\
	__typeof__(*(ptr)) __new = (new);			\
	__typeof__(*(ptr)) __oldval = 0;			\
								\
	BUILD_BUG_ON(sizeof(*(ptr)) != 4);			\
								\
	asm volatile(						\
		"1:	%0 = memw_locked(%1);\n"		\
		"	{ P0 = cmp.eq(%0,%2);\n"		\
		"	  if (!P0.new) jump:nt 2f; }\n"		\
		"	memw_locked(%1,p0) = %3;\n"		\
		"	if (!P0) jump 1b;\n"			\
		"2:\n"						\
		: "=&r" (__oldval)				\
		: "r" (__ptr), "r" (__old), "r" (__new)		\
		: "memory", "p0"				\
	);							\
	__oldval;						\
})

#define __cmpxchg(ptr, old, val, size)				\
({								\
	__typeof__(*(ptr)) oldval;				\
								\
	switch (size) {						\
	case 4:							\
		oldval = __cmpxchg_32(ptr, old, val);		\
		break;						\
	default:						\
		BUILD_BUG();					\
		oldval = val;					\
		break;						\
	}							\
								\
	oldval;							\
})

#define arch_cmpxchg(ptr, o, n)	__cmpxchg((ptr), (o), (n), sizeof(*(ptr)))

/*
 * always make arch_cmpxchg[64]_local available, native cmpxchg
 * will be used if available, then generic_cmpxchg[64]_local
 */
#include <asm-generic/cmpxchg-local.h>

#define arch_cmpxchg_local(ptr, old, val)			\
({								\
	__typeof__(*(ptr)) __retval;				\
	int __size = sizeof(*(ptr));				\
								\
	switch (__size) {					\
	case 4:							\
		__retval = __cmpxchg_32(ptr, old, val);		\
		break;						\
	default:						\
		__retval = __generic_cmpxchg_local(ptr, old,	\
						val, __size);	\
		break;						\
	}							\
								\
	__retval;						\
})

#define arch_cmpxchg64_local(ptr, o, n) __generic_cmpxchg64_local((ptr), (o), (n))

#endif /* _ASM_CMPXCHG_H */
