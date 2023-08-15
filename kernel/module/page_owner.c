// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/module.h>
#include <linux/oom.h>
#include "internal.h"

static int po_oom_notify(struct notifier_block *self,
		unsigned long val, void *data)
{
	struct module *mod;
	int nr;
	int ret = notifier_from_errno(0);

	preempt_disable();
	pr_info("Modules state:\n");
	pr_info("module               nr_page_allocated\n");
	list_for_each_entry_rcu(mod, &modules, list) {
		nr = atomic_read(&mod->nr_pages_allocated);
		if (nr <= 0)
			continue;

		pr_info("%-20s %d\n", mod->name, nr);
	}
	preempt_enable();

	return ret;
}

static struct notifier_block po_oom_nb = {
	.notifier_call = po_oom_notify,
	.priority = 0
};

void po_register_oom_notifier(void)
{
	if (register_oom_notifier(&po_oom_nb))
		pr_warn("Failed to register pageowner oom notifier\n");
}
