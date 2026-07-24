// SPDX-License-Identifier: GPL-2.0

#include <linux/error-injection.h>
#include <linux/kprobes.h>

void override_function_with_return(struct pt_regs *regs)
{
	instruction_pointer_set(regs, regs->regs[1]);
}
NOKPROBE_SYMBOL(override_function_with_return);

unsigned long fei_return_address(struct pt_regs *regs)
{
	return regs->regs[1];
}
NOKPROBE_SYMBOL(fei_return_address);
