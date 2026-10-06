/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __X86_KERNEL_FPU_INTERNAL_H
#define __X86_KERNEL_FPU_INTERNAL_H

#include <asm/cpufeature.h>
#include <asm/fpu/types.h>

extern struct fpstate init_fpstate;

/* CPU feature check wrappers */
static __always_inline __pure bool use_xsave(void)
{
	return cpu_feature_enabled(X86_FEATURE_XSAVE);
}

static __always_inline __pure bool use_fxsr(void)
{
	return cpu_feature_enabled(X86_FEATURE_FXSR);
}

/*
 * AMD CPUs without XSaveErPtr may leave FDP/FIP/FOP unchanged on restore
 * when no x87 exception is pending.  Using an x87 load to overwrite them
 * leaks the kernel instruction and operand addresses through FNSTENV.
 *
 * FNINIT clears the pointers without recording any new ones.  Only do this
 * when the x87 state is about to be replaced; a partial XRSTOR must leave
 * unrequested components alone.
 */
static inline void fpregs_restore_prepare(u64 mask)
{
	if (unlikely(static_cpu_has_bug(X86_BUG_FXSAVE_LEAK)) &&
	    (mask & XFEATURE_MASK_FP))
		asm volatile("fninit");
}

#ifdef CONFIG_X86_DEBUG_FPU
# define WARN_ON_FPU(x) WARN_ON_ONCE(x)
#else
# define WARN_ON_FPU(x) ({ BUILD_BUG_ON_INVALID(x); 0; })
#endif

/* Used in init.c */
extern void fpstate_init_user(struct fpstate *fpstate);
extern void fpstate_reset(struct fpu *fpu);

#endif
