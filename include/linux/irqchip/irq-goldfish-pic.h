/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Goldfish Programmable Interrupt Controller support
 */

#ifndef __LINUX_IRQCHIP_IRQ_GOLDFISH_PIC_H
#define __LINUX_IRQCHIP_IRQ_GOLDFISH_PIC_H

#include <linux/fwnode.h>
#include <linux/types.h>

int goldfish_pic_init(void __iomem *base, unsigned int parent_irq,
		      unsigned int irq_base, struct fwnode_handle *fwnode);

#endif /* __LINUX_IRQCHIP_IRQ_GOLDFISH_PIC_H */
