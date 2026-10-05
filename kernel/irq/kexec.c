// SPDX-License-Identifier: GPL-2.0

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqnr.h>

#include "internals.h"

void machine_kexec_mask_interrupts(void)
{
	struct irq_desc *desc;
	unsigned int i;

	for_each_irq_desc(i, desc) {
		struct irq_chip *chip;
		int check_eoi = 1;

		chip = irq_desc_get_chip(desc);
		if (!chip || !irqd_is_started(&desc->irq_data))
			continue;

		/*
		 * irq_set_irqchip_state() takes the bus lock, and a chip that
		 * has one may sleep or talk to its bus when the lock is dropped.
		 */
		if (IS_ENABLED(CONFIG_GENERIC_IRQ_KEXEC_CLEAR_VM_FORWARD) && !chip->irq_bus_lock) {
			/*
			 * First try to remove the active state from an interrupt which is forwarded
			 * to a VM. If the interrupt is not forwarded, try to EOI the interrupt.
			 */
			check_eoi = irq_set_irqchip_state(i, IRQCHIP_STATE_ACTIVE, false);
		}

		if (check_eoi && chip->irq_eoi && irqd_irq_inprogress(&desc->irq_data))
			chip->irq_eoi(&desc->irq_data);

		irq_shutdown(desc);
	}
}
