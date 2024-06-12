/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2017-2018 Bartosz Golaszewski <brgl@bgdev.pl>
 * Copyright (C) 2020 Bartosz Golaszewski <bgolaszewski@baylibre.com>
 */

#ifndef _LINUX_IRQ_SIM_H
#define _LINUX_IRQ_SIM_H

#include <linux/device.h>
#include <linux/fwnode.h>
#include <linux/irqdomain.h>
#include <linux/notifier.h>

/*
 * Provides a framework for allocating simulated interrupts which can be
 * requested like normal irqs and enqueued from process context.
 */

struct irq_domain *irq_domain_create_sim(struct fwnode_handle *fwnode,
					 unsigned int num_irqs);
struct irq_domain *devm_irq_domain_create_sim(struct device *dev,
					      struct fwnode_handle *fwnode,
					      unsigned int num_irqs);
void irq_domain_remove_sim(struct irq_domain *domain);

enum {
	IRQ_SIM_DOMAIN_IRQ_REQUESTED = 1,
	IRQ_SIM_DOMAIN_IRQ_RELEASED,
};

int irq_sim_domain_register_notifier(struct irq_domain *domain,
				     struct notifier_block *nb);
int irq_sim_domain_unregister_notifier(struct irq_domain *domain,
				       struct notifier_block *nb);

#endif /* _LINUX_IRQ_SIM_H */
