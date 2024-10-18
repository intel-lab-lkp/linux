// SPDX-License-Identifier: GPL-2.0

#include <linux/irqflags.h>

void rust_helper_local_interrupt_disable(void)
{
	local_interrupt_disable();
}

void rust_helper_local_interrupt_enable(void)
{
	local_interrupt_enable();
}

bool rust_helper_irqs_disabled(void)
{
	return irqs_disabled();
}
