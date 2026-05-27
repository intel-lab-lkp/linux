// SPDX-License-Identifier: GPL-2.0

#include <linux/console.h>
#include <linux/debugfs.h>
#include <linux/irq_work.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rtnetlink.h>
#include <linux/string.h>

static void crash(const char *context)
{
	panic("User triggered crash in context %s", context);
}

#define CRASH_TESTCASE_LIST \
	X(irq) \
	X(bh) \
	X(user) \
	X(user_nobh) \
	X(user_noirq) \
	X(user_nopreempt) \
	X(user_rculock) \
	X(user_conlock) \
	X(user_rtnllock)

static void crash_irq_work(struct irq_work *work)
{
	crash("irq");
}

static struct irq_work irq_crash_work;

static void crash_irq(void)
{
	if (!irq_work_queue(&irq_crash_work))
		return;

	irq_work_sync(&irq_crash_work);
}

static void crash_bh_work(struct work_struct *work)
{
	crash("bh");
}

static struct work_struct bh_crash_work;

static void crash_bh(void)
{
	if (!queue_work(system_bh_wq, &bh_crash_work))
		return;

	flush_work(&bh_crash_work);
}

static void crash_user(void)
{
	crash("user");
}

static void crash_user_nobh(void)
{
	local_bh_disable();
	crash("user with bh disabled");
}

static void crash_user_noirq(void)
{
	local_irq_disable();
	crash("user with irqs disabled");
}

static void crash_user_nopreempt(void)
{
	preempt_disable();
	crash("user with preemption disabled");
}

static void crash_user_rculock(void)
{
	rcu_read_lock();
	crash("user in RCU critical section");
}

static void crash_user_conlock(void)
{
	console_lock();
	crash("user with console_lock held");
}

static void crash_user_rtnllock(void)
{
	rtnl_lock();
	crash("user with rtnl_lock held");
}

struct testcase {
	void (*fn)(void);
	const char *str;
};

#define X(name) (struct testcase){.fn = crash_##name, .str = #name "\n" },
static const struct testcase testcases[] = {
	CRASH_TESTCASE_LIST
};
#undef X

static ssize_t test_crash_write(struct file *file, const char __user *buf,
				size_t count, loff_t *pos)
{
	char tmp[16] = {0};
	int i;

	if (copy_from_user(tmp, buf, min(count, sizeof(tmp) - 1)) != 0)
		return -EFAULT;

	for (i = 0; i < ARRAY_SIZE(testcases); i++) {
		const struct testcase *ctx = testcases + i;

		if (!strcmp(ctx->str, tmp))
			ctx->fn();
	}

	return -EINVAL;
}

static ssize_t test_crash_read(struct file *file, char __user *buf,
			       size_t count, loff_t *pos)
{
	#define X(name) #name "\n"
	static const char help_string[] = CRASH_TESTCASE_LIST;
	#undef X
	static const int help_string_len = sizeof(help_string) - 1;
	int off, len, done;

	off = clamp(*pos, 0, help_string_len);
	if (off == help_string_len)
		return 0;

	len = min(count, help_string_len - off);
	done = len - copy_to_user(buf, help_string + off, len);
	*pos = off + done;

	return done ?: -EFAULT;
}

static const struct file_operations test_crash_fops = {
	.write = test_crash_write,
	.read = test_crash_read,
};

static struct dentry *test_crash_dentry;

static int __init setup_test_crash(void)
{
	INIT_WORK(&bh_crash_work, crash_bh_work);
	init_irq_work(&irq_crash_work, crash_irq_work);
	test_crash_dentry = debugfs_create_file("test_crash", 0600, NULL, NULL,
						&test_crash_fops);
	if (IS_ERR(test_crash_dentry))
		return PTR_ERR(test_crash_dentry);

	return 0;
}
late_initcall(setup_test_crash);

static void __exit cleanup_test_crash(void)
{
	debugfs_remove(test_crash_dentry);
}
module_exit(cleanup_test_crash);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Calvin Owens <calvin@wbinvd.org>");
MODULE_DESCRIPTION("Test module to trigger crashes");
