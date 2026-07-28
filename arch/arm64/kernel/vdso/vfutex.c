// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/stringify.h>
#include <vdso/futex.h>

__u32 __vdso_futex_robust_list64_try_unlock(__u32 *lock, __u32 tid, __u64 *pop)
{
	register __u64 *pop_reg asm("x2") = pop;
	register __u32 result_reg asm("w3") = 0;
	__u32 val;

	asm volatile (
		".globl						  "
		"__futex_list64_try_unlock_cs_start,		  "
		"__futex_list64_try_unlock_cs_success,		  "
		"__futex_list64_try_unlock_cs_end		\n"

		"	prfm pstl1strm, %[lock]			\n"
		"retry:						\n"
		"	ldxr %w[val], %[lock]			\n"
		"	cmp %w[tid], %w[val]			\n"
		"	bne __futex_list64_try_unlock_cs_end	\n"
		"	stlxr %w[result], wzr, %[lock]		\n"
		"__futex_list64_try_unlock_cs_start:		\n"
		"	cbnz %w[result], retry			\n"
		"__futex_list64_try_unlock_cs_success:		\n"
		"	str xzr, %[pop_reg]			\n"
		"__futex_list64_try_unlock_cs_end:		\n"

		: [val] "=&r" (val), [result] "=&r" (result_reg), [pop_reg] "+Q" (*pop_reg)
		: [tid] "r" (tid), [lock] "Q" (*lock)
		: "cc", "memory"
	);

	return val;
}
