// SPDX-License-Identifier: GPL-2.0
/*
 * Based on arch/x86/kernel/tracepoint.c
 *
 */

#include <linux/jump_label.h>
#include <linux/atomic.h>

#include <asm/trace/exceptions.h>

DEFINE_STATIC_KEY_FALSE(trace_memabort_key);

int trace_memabort_reg(void)
{
	static_branch_inc(&trace_memabort_key);
	return 0;
}

void trace_memabort_unreg(void)
{
	static_branch_dec(&trace_memabort_key);
}
