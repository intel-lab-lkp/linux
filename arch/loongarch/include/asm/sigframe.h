/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Separated from arch/loongarch/kernel/signal.c.
 */

#include <asm/siginfo.h>
#include <asm/ucontext.h>

struct rt_sigframe {
	struct siginfo rs_info;
	struct ucontext rs_uctx;
};
