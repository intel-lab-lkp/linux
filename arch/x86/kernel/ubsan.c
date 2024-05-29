// SPDX-License-Identifier: GPL-2.0
/*
 * Clang Undefined Behavior Sanitizer trap mode support.
 */
#include <linux/bug.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <linux/ubsan.h>
#include <asm/ptrace.h>
#include <asm/ubsan.h>

/*
 * Checks for the information embedded in the UD1 trap instruction
 * for the UB Sanitizer in order to pass along debugging output.
 */
enum bug_trap_type handle_ubsan_failure(struct pt_regs *regs, int insn)
{
	u32 type = 0;

	if (insn == INSN_REX) {
		type = (*(u16 *)(regs->ip + LEN_REX + LEN_UD1));
		if ((type & 0xFF) == 0x40)
			type = (type >> 8) & 0xFF;
	} else {
		type = (*(u16 *)(regs->ip + LEN_UD1));
		if ((type & 0xFF) == 0x40)
			type = (type >> 8) & 0xFF;
	}
	pr_crit("%s at %pS\n", report_ubsan_failure(regs, type), (void *)regs->ip);

	return BUG_TRAP_TYPE_NONE;
}
