// SPDX-License-Identifier: GPL-2.0+
#include <linux/debugfs.h>
#include <linux/kstrtox.h>
#include <linux/of.h>

#include "of_private.h"

void of_debug_mark_queried(struct property *pp)
{
	pp->queried = true;
}

static int dtmq_update_node_sysfs(struct device_node *np)
{
	struct property *pp;
	int ret = 0;

	if (!IS_ENABLED(CONFIG_SYSFS) || !of_kset)
		goto out;

	for_each_property_of_node(np, pp) {
		if (pp->queried) {
			ret = sysfs_chmod_file(&np->kobj, &pp->attr.attr,
					       pp->attr.attr.mode | S_IWUSR);
			if (ret)
				break;
		}
	}

out:
	return ret;
}

static int dtmq_update_sysfs(void)
{
	struct device_node *np;
	int ret = 0;

	mutex_lock(&of_mutex);
	for_each_of_allnodes(np) {
		ret = dtmq_update_node_sysfs(np);
		if (ret)
			break;
	}
	mutex_unlock(&of_mutex);

	return ret;
}

static ssize_t dtmq_file_write(struct file *file, const char __user *user_buf,
			       size_t count, loff_t *ppos)
{
	bool do_it;
	int ret;

	ret = kstrtobool_from_user(user_buf, count, &do_it);
	if (ret)
		goto out;

	if (do_it) {
		ret = dtmq_update_sysfs();
		if (!ret)
			ret = count;
	} else {
		ret = -EINVAL;
	}

out:
	return ret;
}

static const struct file_operations dtmq_fops = {
	.write  = dtmq_file_write,
	.open	= simple_open,
	.owner  = THIS_MODULE,
};

static int __init of_debug_init(void)
{
	return PTR_ERR_OR_ZERO(debugfs_create_file("of_mark_queried", 0644, NULL, NULL,
			       &dtmq_fops));
}
late_initcall(of_debug_init);
