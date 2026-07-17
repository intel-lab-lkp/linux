// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/stringify.h>
#include <vdso/futex.h>

#define LABEL(name, sz) __stringify(__futex_list##sz##_try_unlock_cs_##name)

#define GLOBLS(sz) ".globl " LABEL(start, sz) ", " LABEL(success, sz) ", " LABEL(end, sz) "\n"

__u32 __vdso_futex_robust_list32_try_unlock(__u32 *lock, __u32 tid, __u32 *pop)
{
	register __u32 *pop_reg asm("r2") = pop, result_reg asm("r3") = 0;
	__u32 val, zero = 0;

	asm volatile (
		GLOBLS(32)
		"retry:						\n"
		"	ldrex %[val], %[lock]			\n"
		"	cmp %[tid], %[val]			\n"
		"	bne " LABEL(end, 32)"			\n"
		"	strex %[result], %[zero], %[lock]	\n"
		LABEL(start, 32)":				\n"
		"	cmp %[result], #0			\n"
		"	bne retry				\n"
		LABEL(success, 32)":				\n"
		"	str %[zero], %[pop_reg]			\n"
		LABEL(end, 32)":				\n"
		: [val] "=&r" (val), [result] "=r" (result_reg)
		: [tid] "r" (tid), [lock] "Q" (*lock), [pop_reg] "Q" (*pop_reg), [zero] "r" (zero)
		: "cc", "memory"
	);

	return val;
}
