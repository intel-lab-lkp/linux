// SPDX-License-Identifier: GPL-2.0
/*
 * ARM callchain support
 *
 * Copyright (C) 2009 picoChip Designs, Ltd., Jamie Iles
 * Copyright (C) 2010 ARM Ltd., Will Deacon <will.deacon@arm.com>
 *
 * This code is based on the ARM OProfile backtrace code.
 */
#include <linux/perf_event.h>
#include <linux/uaccess.h>

#include <asm/stacktrace.h>

/*
 * Gets called by walk_stackframe() for every stackframe. This will be called
 * whist unwinding the stackframe and is like a subroutine return so we use
 * the PC.
 */
static bool
callchain_trace(void *data, unsigned long pc)
{
	struct perf_callchain_entry_ctx *entry = data;
	return perf_callchain_store(entry, pc) == 0;
}

void
perf_callchain_user(struct perf_callchain_entry_ctx *entry, struct pt_regs *regs)
{
	arch_stack_walk_user(callchain_trace, entry, regs);
}

void
perf_callchain_kernel(struct perf_callchain_entry_ctx *entry, struct pt_regs *regs)
{
	struct stackframe fr;

	arm_get_current_stackframe(regs, &fr);
	walk_stackframe(&fr, callchain_trace, entry);
}

unsigned long perf_instruction_pointer(struct pt_regs *regs)
{
	return instruction_pointer(regs);
}

unsigned long perf_misc_flags(struct pt_regs *regs)
{
	int misc = 0;

	if (user_mode(regs))
		misc |= PERF_RECORD_MISC_USER;
	else
		misc |= PERF_RECORD_MISC_KERNEL;

	return misc;
}
