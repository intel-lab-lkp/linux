// SPDX-License-Identifier: GPL-2.0
/*
 * Kselftest helper for the dmem cgroup controller.
 *
 * Registers a dmem region and debugfs files so tests can trigger charges
 * from the calling task's cgroup.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cgroup_dmem.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "../../tools/testing/selftests/kselftest_module.h"

#define DM_SELFTEST_REGION_NAME	"dmem_selftest"
#define DM_SELFTEST_REGION_SIZE	(256ULL * 1024 * 1024)

KSTM_MODULE_GLOBALS();

static struct dmem_cgroup_region *selftest_region;
static struct dentry *dbg_dir;

static struct dmem_cgroup_pool_state *charged_pool;
static u64 charged_size;
static DEFINE_MUTEX(charge_lock);

static ssize_t dmem_selftest_charge_write(struct file *file, const char __user *user_buf,
					  size_t count, loff_t *ppos)
{
	struct dmem_cgroup_pool_state *pool = NULL, *limit = NULL;
	u64 size;
	char buf[32];
	int ret;

	if (!selftest_region)
		return -ENODEV;

	if (count == 0 || count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;
	buf[count] = '\0';

	ret = kstrtou64(strim(buf), 0, &size);
	if (ret)
		return ret;
	if (!size)
		return -EINVAL;

	mutex_lock(&charge_lock);
	if (charged_pool) {
		mutex_unlock(&charge_lock);
		return -EBUSY;
	}

	ret = dmem_cgroup_try_charge(selftest_region, size, &pool, &limit);
	if (ret == -EAGAIN && limit)
		dmem_cgroup_pool_state_put(limit);
	if (ret) {
		mutex_unlock(&charge_lock);
		return ret;
	}

	charged_pool = pool;
	charged_size = size;
	mutex_unlock(&charge_lock);

	return count;
}

static ssize_t dmem_selftest_uncharge_write(struct file *file, const char __user *user_buf,
					    size_t count, loff_t *ppos)
{
	if (!count)
		return -EINVAL;

	mutex_lock(&charge_lock);
	if (!charged_pool) {
		mutex_unlock(&charge_lock);
		return -EINVAL;
	}

	dmem_cgroup_uncharge(charged_pool, charged_size);
	charged_pool = NULL;
	charged_size = 0;
	mutex_unlock(&charge_lock);

	return count;
}

static const struct file_operations dmem_selftest_charge_fops = {
	.write = dmem_selftest_charge_write,
	.llseek = noop_llseek,
};

static const struct file_operations dmem_selftest_uncharge_fops = {
	.write = dmem_selftest_uncharge_write,
	.llseek = noop_llseek,
};

static int __init dmem_selftest_register(void)
{
	selftest_region = dmem_cgroup_register_region(
		DM_SELFTEST_REGION_SIZE, DM_SELFTEST_REGION_NAME);
	if (IS_ERR(selftest_region))
		return PTR_ERR(selftest_region);
	if (!selftest_region)
		return -EINVAL;

	dbg_dir = debugfs_create_dir("dmem_selftest", NULL);
	if (!dbg_dir) {
		dmem_cgroup_unregister_region(selftest_region);
		selftest_region = NULL;
		return -ENOMEM;
	}

	debugfs_create_file("charge", 0200, dbg_dir, NULL, &dmem_selftest_charge_fops);
	debugfs_create_file("uncharge", 0200, dbg_dir, NULL, &dmem_selftest_uncharge_fops);

	pr_info("region '%s' registered; debugfs at dmem_selftest/{charge,uncharge}\n",
		DM_SELFTEST_REGION_NAME);
	return 0;
}

static void dmem_selftest_remove(void)
{
	debugfs_remove_recursive(dbg_dir);
	dbg_dir = NULL;

	if (selftest_region) {
		dmem_cgroup_unregister_region(selftest_region);
		selftest_region = NULL;
	}
}

static void __init selftest(void)
{
	KSTM_CHECK_ZERO(!selftest_region);
	KSTM_CHECK_ZERO(!dbg_dir);
}

static int __init dmem_selftest_init(void)
{
	int report_rc;
	int err;

	err = dmem_selftest_register();
	if (err)
		return err;

	pr_info("loaded.\n");
	add_taint(TAINT_TEST, LOCKDEP_STILL_OK);
	selftest();
	report_rc = kstm_report(total_tests, failed_tests, skipped_tests);
	if (report_rc) {
		dmem_selftest_remove();
		return report_rc;
	}

	return 0;
}

static void __exit dmem_selftest_exit(void)
{
	pr_info("unloaded.\n");

	mutex_lock(&charge_lock);
	if (charged_pool) {
		dmem_cgroup_uncharge(charged_pool, charged_size);
		charged_pool = NULL;
	}
	mutex_unlock(&charge_lock);

	dmem_selftest_remove();
}

module_init(dmem_selftest_init);
module_exit(dmem_selftest_exit);

MODULE_AUTHOR("Albert Esteve <aesteve@redhat.com>");
MODULE_DESCRIPTION("Kselftest helper for cgroup dmem controller");
MODULE_LICENSE("GPL");
