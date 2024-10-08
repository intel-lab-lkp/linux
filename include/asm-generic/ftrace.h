/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * linux/include/asm-generic/ftrace.h
 */
#ifndef __ASM_GENERIC_FTRACE_H__
#define __ASM_GENERIC_FTRACE_H__

/*
 * Not all architectures need their own ftrace.h, the most
 * common definitions are already in linux/ftrace.h.
 */

#ifndef CONFIG_HAVE_DYNAMIC_FTRACE_WITH_ARGS
struct __arch_ftrace_regs {
	struct pt_regs		regs;
};

#define arch_ftrace_get_regs(fregs)					\
	({ struct __arch_fregs_regs *__f = (struct __arch_ftrace_regs *)(fregs); \
		&__f->regs;						\
	})

struct ftrace_regs;
#define arch_ftrace_regs(fregs) ((struct __arch_ftrace_regs *)(fregs))

#endif /* __ASM_GENERIC_FTRACE_H__ */
