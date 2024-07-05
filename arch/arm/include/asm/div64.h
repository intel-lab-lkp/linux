/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_ARM_DIV64
#define __ASM_ARM_DIV64

#include <linux/types.h>
#include <asm/compiler.h>

/*
 * The semantics of __div64_32() are:
 *
 * uint32_t __div64_32(uint64_t *n, uint32_t base)
 * {
 * 	uint32_t remainder = *n % base;
 * 	*n = *n / base;
 * 	return remainder;
 * }
 *
 * In other words, a 64-bit dividend with a 32-bit divisor producing
 * a 64-bit result and a 32-bit remainder.  To accomplish this optimally
 * we override the generic version in lib/div64.c to call our __do_div64
 * assembly implementation with completely non standard calling convention
 * for arguments and results (beware).
 */
static inline uint32_t __div64_32(uint64_t *n, uint32_t base)
{
	register unsigned int __base      asm("r4") = base;
	register unsigned long long __n   asm("r0") = *n;
	register unsigned long long __res asm("r2");
	unsigned int __rem;
	asm(	__asmeq("%0", "r0")
		__asmeq("%1", "r2")
		__asmeq("%2", "r4")
		"bl	__do_div64"
		: "+r" (__n), "=r" (__res)
		: "r" (__base)
		: "ip", "lr", "cc");
	__rem = __n >> 32;
	*n = __res;
	return __rem;
}
#define __div64_32 __div64_32

#if !defined(CONFIG_AEABI)

/*
 * In OABI configurations, some uses of the do_div function
 * cause gcc to run out of registers. To work around that,
 * we can force the use of the out-of-line version for
 * configurations that build a OABI kernel.
 */
#define do_div(n, base) __div64_32(&(n), base)

#else

#include <asm-generic/div64.h>

#endif

#endif
