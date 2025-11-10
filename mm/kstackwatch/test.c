// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/kstackwatch.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/prandom.h>
#include <linux/printk.h>
#include <linux/random.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>

static struct dentry *test_file;

#define BUFFER_SIZE 32

static void test_watch_fire(void)
{
	u64 buffer[BUFFER_SIZE] = { 0 };

	pr_info("entry of %s\n", __func__);
	ksw_watch_show();
	pr_info("buf: 0x%px\n", buffer);

	ksw_watch_fire();

	barrier_data(buffer);
	pr_info("exit of %s\n", __func__);
}

static void test_canary_overflow(void)
{
	u64 buffer[BUFFER_SIZE];

	pr_info("entry of %s\n", __func__);
	ksw_watch_show();
	pr_info("buf: 0x%px\n", buffer);

	/* intentionally overflow */
	for (int i = BUFFER_SIZE; i < BUFFER_SIZE + 10; i++)
		buffer[i] = 0xdeadbeefdeadbeef;
	barrier_data(buffer);

	pr_info("exit of %s\n", __func__);
}

static ssize_t test_dbgfs_write(struct file *file, const char __user *buffer,
				size_t count, loff_t *pos)
{
	char cmd[256];
	int test_num;

	if (count >= sizeof(cmd))
		return -EINVAL;

	if (copy_from_user(cmd, buffer, count))
		return -EFAULT;

	cmd[count] = '\0';
	strim(cmd);

	pr_info("received command: %s\n", cmd);

	if (sscanf(cmd, "test%d", &test_num) == 1) {
		switch (test_num) {
		case 0:
			test_watch_fire();
			break;
		case 1:
			test_canary_overflow();
			break;
		default:
			pr_err("Unknown test number %d\n", test_num);
			return -EINVAL;
		}
	} else {
		pr_err("invalid command format. Use 'testN'.\n");
		return -EINVAL;
	}

	return count;
}

static ssize_t test_dbgfs_read(struct file *file, char __user *buffer,
			       size_t count, loff_t *ppos)
{
	static const char usage[] =
		"KStackWatch Simplified Test Module\n"
		"============ usage ===============\n"
		"Usage:\n"
		"echo test{i} > /sys/kernel/debug/kstackwatch/test\n"
		" test0 - test watch fire\n"
		" test1 - test canary overflow\n";

	return simple_read_from_buffer(buffer, count, ppos, usage,
				       strlen(usage));
}

static const struct file_operations test_dbgfs_fops = {
	.owner = THIS_MODULE,
	.read = test_dbgfs_read,
	.write = test_dbgfs_write,
	.llseek = noop_llseek,
};

static int __init kstackwatch_test_init(void)
{
	struct dentry *ksw_dir = ksw_get_dbgdir();

	if (!ksw_dir) {
		pr_err("kstackwatch must be loaded first\n");
		return -ENODEV;
	}

	test_file = debugfs_create_file("test", 0600, ksw_dir, NULL,
					&test_dbgfs_fops);
	if (!test_file) {
		pr_err("Failed to create debugfs test file\n");
		return -ENOMEM;
	}

	pr_info("module loaded\n");
	return 0;
}

static void __exit kstackwatch_test_exit(void)
{
	debugfs_remove(test_file);
	pr_info("module unloaded\n");
}

module_init(kstackwatch_test_init);
module_exit(kstackwatch_test_exit);

MODULE_AUTHOR("Jinchao Wang");
MODULE_DESCRIPTION("KStackWatch Test Module");
MODULE_LICENSE("GPL");
