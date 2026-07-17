// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/stringify.h>
#include <vdso/futex.h>

#define LABEL(name, sz) __stringify(__futex_list##sz##_try_unlock_cs_##name)

#define GLOBLS(sz) ".globl " LABEL(start, sz) ", " LABEL(success, sz) ", " LABEL(end, sz) "\n"

__u32 __vdso_futex_robust_list64_try_unlock(__u32 *lock, __u32 tid, __u64 *pop)
{
	register __u64 *pop_reg asm("x2") = pop;
	register __u32 result_reg asm("w3") = 0;
	__u32 val;

	asm volatile (
		GLOBLS(64)
		"	prfm pstl1strm, %[lock]			\n"
		"retry:						\n"
		"	ldxr %w[val], %[lock]			\n"
		"	cmp %w[tid], %w[val]			\n"
		"	bne " LABEL(end, 64)"			\n"
		"	stlxr %w[result], wzr, %[lock]		\n"
		LABEL(start, 64)":				\n"
		"	cbnz %w[result], retry			\n"
		LABEL(success, 64)":				\n"
		"	str xzr, %[pop_reg]			\n"
		LABEL(end, 64)":				\n"

		: [val] "=&r" (val), [result] "=&r" (result_reg)
		: [tid] "r" (tid), [lock] "Q" (*lock), [pop_reg] "Q" (*pop_reg)
		: "cc", "memory"
	);

	return val;
}
