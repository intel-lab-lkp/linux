// SPDX-License-Identifier: GPL-2.0

#include <linux/debugfs.h>
#include <linux/nmi.h>

static ssize_t backtrace_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct cpumask mask;
	int err;

	err = cpumask_parselist_user(buf, count, &mask);
	if (err < 0 || cpumask_last(&mask) >= nr_cpu_ids) {
		pr_err("backtrace: incorrect CPU range.\n");
		return -EINVAL;
	}

	if (!trigger_cpumask_backtrace(&mask)) {
		pr_err("backtrace: backtrace printing fails.\n");
		return -EINVAL;
	}

	return count;
}

static const struct file_operations backtrace_fops = {
	.owner  = THIS_MODULE,
	.write  = backtrace_write,
	.llseek = no_llseek,
};

static int __init backtrace_init(void)
{
	debugfs_create_file("backtrace", 0200, NULL, NULL,
						&backtrace_fops);

	return 0;
}
device_initcall(backtrace_init);
