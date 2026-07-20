// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/stringify.h>
#include <vdso/futex.h>

__u32 __vdso_futex_robust_list32_try_unlock(__u32 *lock, __u32 tid, __u32 *pop)
{
	register __u32 *pop_reg asm("r2") = pop, result_reg asm("r3") = 0;
	__u32 val, zero = 0;

	asm volatile (
		".globl						  "
		"__futex_list32_try_unlock_cs_start,		  "
		"__futex_list32_try_unlock_cs_success,		  "
		"__futex_list32_try_unlock_cs_end		\n"

		"retry:						\n"
		"	ldrex %[val], %[lock]			\n"
		"	cmp %[tid], %[val]			\n"
		"	bne __futex_list32_try_unlock_cs_end	\n"
		"	strex %[result], %[zero], %[lock]	\n"
		"__futex_list32_try_unlock_cs_start:		\n"
		"	cmp %[result], #0			\n"
		"	bne retry				\n"
		"__futex_list32_try_unlock_cs_success:		\n"
		"	str %[zero], %[pop_reg]			\n"
		"__futex_list32_try_unlock_cs_end:		\n"

		: [val] "=&r" (val), [result] "=r" (result_reg)
		: [tid] "r" (tid), [lock] "Q" (*lock), [pop_reg] "Q" (*pop_reg), [zero] "r" (zero)
		: "cc", "memory"
	);

	return val;
}
