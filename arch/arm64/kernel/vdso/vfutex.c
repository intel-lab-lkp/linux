// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/stringify.h>
#include <vdso/futex.h>

#define LABEL(l)						\
	"	.globl " #l "\n"				\
	#l ":\n"

__u32 __vdso_futex_robust_list64_try_unlock(__u32 *lock, __u32 tid, __u64 *pop)
{
	/*
	 * arm64_futex_robust_unlock_get_pop() depends on this exact register
	 * allocation to work correctly.
	 */
	register __u64 pop_reg asm("x2") = (__u64) pop;
	register __u32 result_reg asm("w3") = 0;
	__u32 val;

	asm volatile (
		"	prfm	pstl1strm, %[lock]		\n"
		"retry:						\n"
		"	ldxr	%w[val], %[lock]		\n"
		"	cmp	%w[tid], %w[val]		\n"
		"	b.ne	__futex_list64_try_unlock_cs_end\n"
		"	stlxr	%w[result], wzr, %[lock]	\n"
		LABEL(__futex_list64_try_unlock_cs_start)
		"	cbnz	%w[result], retry		\n"
		LABEL(__futex_list64_try_unlock_cs_success)
		"	str	xzr, [%x[pop_reg]]		\n"
		LABEL(__futex_list64_try_unlock_cs_end)

		: [val] "=&r" (val),
		  [result] "=&r" (result_reg),
		  [lock] "+Q" (*lock)
		: [tid] "r" (tid),
		  [pop_reg] "r" (pop_reg)
		: "cc", "memory"
	);

	return val;
}
