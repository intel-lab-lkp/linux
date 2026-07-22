// SPDX-License-Identifier: GPL-2.0-only
#include <linux/stringify.h>
#include <vdso/futex.h>
#include <asm/hwcap.h>
#include <asm/cpufeature-macros.h>
#include <asm/vdso/futex.h>

#define LABEL(pop_size, which) __stringify(__vdso_futex_list##pop_size##_try_unlock_cs_##which)

#define futex_robust_try_unlock_cas(pop_size, store_pop, lock, tid, pop)	\
({										\
	/*									\
	 * arch_futex_robust_unlock_get_pop() assumes the variables are in	\
	 * those registers. So make sure.					\
	 *									\
	 * tid and pop are in a1 and a2 at function entry according to the	\
	 * calling convention, so it likely still works if we remove _tid	\
	 * and _pop. But technically compiler is allowed to move tid and pop	\
	 * to different registers, and _tid and _pop do not generate any	\
	 * extra instructions so it does not hurt to keep them.			\
	 */									\
	register __u32  ret asm ("a0") = tid;					\
	register __u32 _tid asm ("a1") = tid;					\
	register void *_pop asm ("a2") = pop;					\
										\
	asm volatile (								\
		"	.option push\n"						\
		"	.option arch, +zacas\n"					\
		"	amocas.w.rl	%[ret], zero, (%[lock])\n"		\
		"	.option pop\n"						\
		LABEL(pop_size, cas_start)":"					\
		"	bne %[ret], %[tid], "LABEL(pop_size, cas_end)"\n"	\
		"	"store_pop" zero, (%[pop])\n"				\
		LABEL(pop_size, cas_end)":"					\
		: [ret]  "+&r" (ret)						\
		: [tid]  "r"   (_tid),						\
		  [lock] "r"   (lock),						\
		  [pop]  "r"   (_pop)						\
		: "memory"							\
	);									\
										\
	ret;									\
})

#define futex_robust_try_unlock_lrsc(pop_size, store_pop, lock, tid, pop)	\
({										\
	register void *_pop asm ("a2") = pop;					\
	__u32 ret;								\
										\
	asm volatile (								\
		"1:	lr.w %[ret], (%[lock])\n"				\
		"	bne %[ret], %[tid], "LABEL(pop_size, lrsc_end)"\n"	\
		"	sc.w.rl	t0, x0, (%[lock])\n"				\
		LABEL(pop_size, lrsc_start)":"					\
		"	bnez t0, 1b\n"						\
		"	"store_pop" zero, (%[pop])\n"				\
		LABEL(pop_size, lrsc_end)":"					\
		: [ret]  "=&r" (ret)						\
		: [tid]  "r"   (tid),						\
		  [lock] "r"   (lock),						\
		  [pop]  "r"   (_pop)						\
		: "t0", "memory"						\
	);									\
										\
	ret;									\
})


#if __riscv_xlen == 64
__u32 __vdso_futex_robust_list64_try_unlock(__u32 *lock, __u32 tid, __u64 *pop)
{
	if (cpu_supports_zacas())
		return futex_robust_try_unlock_cas(64, "sd", lock, tid, pop);
	else
		return futex_robust_try_unlock_lrsc(64, "sd", lock, tid, pop);
}
#endif

#if __riscv_xlen == 32
__u32 __vdso_futex_robust_list32_try_unlock(__u32 *lock, __u32 tid, __u32 *pop)
{
	if (cpu_supports_zacas())
		return futex_robust_try_unlock_cas(32, "sw", lock, tid, pop);
	else
		return futex_robust_try_unlock_lrsc(32, "sw", lock, tid, pop);
}
#endif
