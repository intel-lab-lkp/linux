// SPDX-License-Identifier: GPL-2.0

#include <linux/irqflags.h>

unsigned long rust_helper_local_irq_save(void)
{
	unsigned long flags;

	local_irq_save(flags);

	return flags;
}

void rust_helper_local_irq_restore(unsigned long flags)
{
	local_irq_restore(flags);
}

bool rust_helper_irqs_disabled(void)
{
	return irqs_disabled();
}
