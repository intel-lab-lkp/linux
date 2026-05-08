// SPDX-License-Identifier: GPL-2.0
/*
 * Test target for the killswitch selftest.  ks_test_vuln() returns
 * -EBADMSG on a magic input, standing in for "the buggy path runs
 * and produces a bad outcome".  Engaging killswitch on this function
 * with retval 0 is the mitigation.
 *
 * Copyright (C) 2026 Sasha Levin <sashal@kernel.org>
 */

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#define KS_TEST_MAGIC	0xC0FFEEL

int ks_test_vuln(long magic);

/*
 * Returns -EBADMSG on the magic input -- stands in for "the buggy
 * path runs and produces a bad outcome".  Engaging a killswitch on
 * this function with retval 0 represents the mitigation: even on
 * the magic input, callers see success because the body never runs.
 *
 * noipa prevents inlining/IPA so the call actually reaches the
 * kprobe-instrumented entry point.
 */
noinline int ks_test_vuln(long magic)
{
	if (magic == KS_TEST_MAGIC)
		return -EBADMSG;
	return 0;
}
EXPORT_SYMBOL_GPL(ks_test_vuln);

static struct dentry *ks_test_dir;

static ssize_t ks_test_fire_write(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	char buf[32];
	long magic;
	int ret;

	if (count == 0 || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	ret = kstrtol(strim(buf), 0, &magic);
	if (ret)
		return ret;

	ret = ks_test_vuln(magic);
	return ret ? ret : count;
}

static const struct file_operations ks_test_fire_fops = {
	.write	= ks_test_fire_write,
	.open	= simple_open,
	.llseek	= noop_llseek,
};

static int __init test_killswitch_init(void)
{
	ks_test_dir = debugfs_create_dir("test_killswitch", NULL);
	debugfs_create_file("fire", 0200, ks_test_dir, NULL,
			    &ks_test_fire_fops);
	pr_info("test_killswitch: loaded (magic=0x%lx)\n", KS_TEST_MAGIC);
	return 0;
}
module_init(test_killswitch_init);

static void __exit test_killswitch_exit(void)
{
	debugfs_remove_recursive(ks_test_dir);
}
module_exit(test_killswitch_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Deliberately-vulnerable target for killswitch selftest");
