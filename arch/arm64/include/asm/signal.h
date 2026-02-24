/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARM64_ASM_SIGNAL_H
#define __ARM64_ASM_SIGNAL_H

#include <asm/memory.h>
#include <asm/processor.h>
#include <uapi/asm/signal.h>
#include <uapi/asm/siginfo.h>

static inline void __user *arch_untagged_si_addr(void __user *addr,
						 unsigned long sig,
						 unsigned long si_code)
{
	unsigned long masked;

	/*
	 * For historical reasons, all bits of the fault address are exposed as
	 * address bits for watchpoint exceptions. New architectures should
	 * handle the tag bits consistently.
	 */
	if (sig == SIGTRAP && si_code == TRAP_BRKPT)
		return addr;

	/*
	 * Strip tag bits only for valid user addresses. For addresses
	 * in the VA hole, preserve the original value so userspace can
	 * see the actual faulting address for debugging.
	 */
	masked = (unsigned long)addr & ((1UL << 56) - 1);
	if (masked >= TASK_SIZE)
		return addr;

	return (void __user *)masked;
}
#define arch_untagged_si_addr arch_untagged_si_addr

#endif
