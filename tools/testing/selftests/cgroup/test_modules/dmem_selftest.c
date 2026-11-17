// SPDX-License-Identifier: GPL-2.0
/*
 * Kselftest helper for the dmem cgroup controller.
 *
 * Registers a synthetic dmem region so tests can trigger allocations
 * from the calling task's cgroup via module parameters:
 *   /sys/module/dmem_selftest/parameters/alloc
 *   /sys/module/dmem_selftest/parameters/free
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cgroup_dmem.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/panic.h>
#include <linux/sysfs.h>

#define DM_SELFTEST_REGION_NAME	"dmem_selftest"
#define DM_SELFTEST_REGION_SIZE	(256ULL * 1024 * 1024)

static struct dmem_cgroup_region *selftest_region;
static struct dmem_cgroup_pool_state *alloc_pool;
static u64 alloc_size;
static DEFINE_MUTEX(alloc_lock);

static int param_set_alloc(const char *val, const struct kernel_param *kp)
{
	struct dmem_cgroup_pool_state *pool = NULL, *limit = NULL;
	u64 size;
	int ret;

	if (!selftest_region)
		return -ENODEV;

	ret = kstrtou64(val, 0, &size);
	if (ret)
		return ret;
	if (!size || size > DM_SELFTEST_REGION_SIZE)
		return -EINVAL;

	mutex_lock(&alloc_lock);
	if (alloc_pool) {
		mutex_unlock(&alloc_lock);
		return -EBUSY;
	}

	ret = dmem_cgroup_try_charge(selftest_region, size, &pool, &limit);
	if (ret == -EAGAIN && limit)
		dmem_cgroup_pool_state_put(limit);
	if (ret) {
		mutex_unlock(&alloc_lock);
		return ret;
	}

	alloc_pool = pool;
	alloc_size = size;
	mutex_unlock(&alloc_lock);
	return 0;
}

static int param_get_alloc(char *buffer, const struct kernel_param *kp)
{
	u64 size;

	mutex_lock(&alloc_lock);
	size = alloc_size;
	mutex_unlock(&alloc_lock);
	return sysfs_emit(buffer, "%llu\n", size);
}

static const struct kernel_param_ops alloc_ops = {
	.set = param_set_alloc,
	.get = param_get_alloc,
};

module_param_cb(alloc, &alloc_ops, NULL, 0644);
MODULE_PARM_DESC(alloc, "Allocate (charge) SIZE bytes against the calling task's dmem cgroup");

static int param_set_free(const char *val, const struct kernel_param *kp)
{
	mutex_lock(&alloc_lock);
	if (!alloc_pool) {
		mutex_unlock(&alloc_lock);
		return -EINVAL;
	}

	dmem_cgroup_uncharge(alloc_pool, alloc_size);
	alloc_pool = NULL;
	alloc_size = 0;
	mutex_unlock(&alloc_lock);
	return 0;
}

static const struct kernel_param_ops free_ops = {
	.flags = KERNEL_PARAM_OPS_FL_NOARG,
	.set = param_set_free,
};

module_param_cb(free, &free_ops, NULL, 0200);
MODULE_PARM_DESC(free, "Free the outstanding dmem selftest allocation");

static int __init dmem_selftest_init(void)
{
	static const struct dmem_cgroup_init init = {
		.size = DM_SELFTEST_REGION_SIZE,
	};

	selftest_region = dmem_cgroup_register_region(&init, DM_SELFTEST_REGION_NAME);
	if (IS_ERR(selftest_region))
		return PTR_ERR(selftest_region);
	if (!selftest_region)
		return -EINVAL;

	add_taint(TAINT_TEST, LOCKDEP_STILL_OK);
	pr_info("region '%s' registered; parameters alloc/free\n",
		DM_SELFTEST_REGION_NAME);
	return 0;
}

static void __exit dmem_selftest_exit(void)
{
	mutex_lock(&alloc_lock);
	if (alloc_pool) {
		dmem_cgroup_uncharge(alloc_pool, alloc_size);
		alloc_pool = NULL;
		alloc_size = 0;
	}
	mutex_unlock(&alloc_lock);

	if (selftest_region) {
		dmem_cgroup_unregister_region(selftest_region);
		selftest_region = NULL;
	}
	pr_info("unloaded.\n");
}

module_init(dmem_selftest_init);
module_exit(dmem_selftest_exit);

MODULE_AUTHOR("Albert Esteve <aesteve@redhat.com>");
MODULE_DESCRIPTION("Kselftest helper for cgroup dmem controller");
MODULE_LICENSE("GPL");
MODULE_INFO(test, "Y");
