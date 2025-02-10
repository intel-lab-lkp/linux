/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/notifier.h>

struct notifier_err_inject_action {
	unsigned long val;
	int error;
	const char *name;
};

#define NOTIFIER_ERR_INJECT_ACTION(action)	\
	.name = #action, .val = (action),

struct notifier_err_inject {
	struct notifier_block nb;
	struct notifier_err_inject_action actions[];
	/* The last slot must be terminated with zero sentinel */
};

extern struct debugfs_node *notifier_err_inject_dir;

extern struct debugfs_node *
notifier_err_inject_init(const char *name, struct debugfs_node *parent,
			 struct notifier_err_inject *err_inject,
			 int priority);
