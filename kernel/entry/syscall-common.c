// SPDX-License-Identifier: GPL-2.0

#include <linux/entry-common.h>

#define CREATE_TRACE_POINTS
#include <trace/events/syscalls.h>

void __trace_sys_enter(struct pt_regs *regs, long syscall)
{
	trace_sys_enter(regs, syscall);
}

void __trace_sys_exit(struct pt_regs *regs, long ret)
{
	trace_sys_exit(regs, ret);
}
