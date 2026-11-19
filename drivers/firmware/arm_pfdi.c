// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Arm Ltd.

#define pr_fmt(fmt) "arm-pfdi: " fmt

#include <linux/arm-smccc.h>
#include <linux/completion.h>
#include <linux/compat.h>
#include <linux/cpuhotplug.h>
#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pfdi.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

struct pfdi_per_cpu_info {
	atomic_t run_inprogress;
	/* Ensures a single userspace owner per CPU while the node is open. */
	struct semaphore open_sem;
	/* Serializes per-CPU run state and device availability. */
	struct mutex state_lock;
	struct pfdi_test_range ongoing_run_range;
	wait_queue_head_t run_wait;
};

struct pfdi_dev_data {
	unsigned int cpu_id;
	char name[16];
	char nodename[24];
};

struct pfdi_cpu_worker {
	struct kthread_work work;
	struct completion done;
	u32 feature_fid;
	/* Protects worker lifetime and in-flight work state. */
	struct mutex worker_lock;
	struct pfdi_force_error error_info;
	struct pfdi_test_run test_run;
	struct arm_smccc_res smc_ret;
	int result;
};

static const u32 pfdi_supported_fids[] = {
	PFDI_FN_VERSION,
	PFDI_FN_FEATURES,
	PFDI_FN_PE_TEST_ID,
	PFDI_FN_PE_TEST_PART_COUNT,
	PFDI_FN_PE_TEST_RUN,
	PFDI_FN_PE_TEST_RESULT,
	PFDI_FN_FW_CHECK,
	PFDI_FN_FORCE_ERROR,
};

static DEFINE_PER_CPU(struct kthread_worker *, pfdi_worker);
static DEFINE_PER_CPU(struct miscdevice, pfdi_misc_device);
static DEFINE_PER_CPU(struct pfdi_dev_data, pfdi_device_data);
static DEFINE_PER_CPU(struct pfdi_cpu_worker, pfdi_worker_ctx);
static DEFINE_PER_CPU(struct pfdi_per_cpu_info, pfdi_cpu_info);

static enum cpuhp_state pfdi_cpuhp_state;

static unsigned int pfdi_wait_timeout_ms = 60;
module_param_named(wait_timeout_ms, pfdi_wait_timeout_ms, uint, 0444);
MODULE_PARM_DESC(wait_timeout_ms,
		 "Maximum time in ms to wait for an in-progress PFDI run");
static int pfdi_to_linux_errno(int err)
{
	switch (err) {
	case SMCCC_RET_SUCCESS:
		return 0;
	case SMCCC_RET_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	case PFDI_RET_INVALID_PARAM:
		return -EINVAL;
	case PFDI_RET_NOT_RUN:
	case PFDI_RET_TEST_COUNT_ZERO:
		return -EAGAIN;
	case PFDI_RET_ERROR:
	case PFDI_RET_UNKNOWN:
		return -EIO;
	case PFDI_RET_FAULT_FOUND:
		return -EFAULT;
	default:
		return -EINVAL;
	}
}

static struct arm_smccc_res pfdi_smc_call(u32 smc_fid, u64 x1, u64 x2, u64 x3,
					  u64 x4)
{
	struct arm_smccc_res res;

	arm_smccc_smc(smc_fid, x1, x2, x3, x4, 0, 0, 0, &res);
	return res;
}

static int pfdi_smc_feature(u32 smc_fid)
{
	return (int)pfdi_smc_call(PFDI_FN_FEATURES, smc_fid, 0, 0, 0).a0;
}

static bool pfdi_fid_supported(u32 smc_fid)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pfdi_supported_fids); i++) {
		if (pfdi_supported_fids[i] == smc_fid)
			return true;
	}

	return false;
}

static int pfdi_validate_cpu_features(void)
{
	unsigned int i;
	int ret;
	struct arm_smccc_res smc_ret;

	smc_ret = pfdi_smc_call(PFDI_FN_VERSION, 0, 0, 0, 0);
	if ((int)smc_ret.a0 < SMCCC_RET_SUCCESS)
		return pfdi_to_linux_errno((int)smc_ret.a0);

	if (PFDI_VERSION_MAJOR(smc_ret.a0) != 1)
		return -EOPNOTSUPP;

	for (i = 0; i < ARRAY_SIZE(pfdi_supported_fids); i++) {
		ret = pfdi_smc_feature(pfdi_supported_fids[i]);
		if (ret < 0)
			return pfdi_to_linux_errno(ret);
	}

	return 0;
}

static int pfdi_thread_worker(unsigned int cpu,
			      void (*handler)(struct kthread_work *work),
			      unsigned long timeout)
{
	int ret = 0;
	struct kthread_worker *worker;
	struct pfdi_cpu_worker *worker_ctx = &per_cpu(pfdi_worker_ctx, cpu);

	mutex_lock(&worker_ctx->worker_lock);
	worker = per_cpu(pfdi_worker, cpu);
	if (!worker) {
		mutex_unlock(&worker_ctx->worker_lock);
		return -ENODEV;
	}

	reinit_completion(&worker_ctx->done);
	kthread_init_work(&worker_ctx->work, handler);
	kthread_queue_work(worker, &worker_ctx->work);

	if (!wait_for_completion_timeout(&worker_ctx->done, timeout)) {
		ret = -ETIME;
		kthread_cancel_work_sync(&worker_ctx->work);
	}

	mutex_unlock(&worker_ctx->worker_lock);
	return ret;
}

static void pfdi_pe_get_test_id_fn(struct kthread_work *work)
{
	struct pfdi_cpu_worker *worker_ctx;

	worker_ctx = container_of(work, struct pfdi_cpu_worker, work);
	worker_ctx->smc_ret = pfdi_smc_call(PFDI_FN_PE_TEST_ID, 0, 0, 0, 0);
	complete(&worker_ctx->done);
}

static void pfdi_pe_get_version_fn(struct kthread_work *work)
{
	struct pfdi_cpu_worker *worker_ctx;

	worker_ctx = container_of(work, struct pfdi_cpu_worker, work);
	worker_ctx->smc_ret = pfdi_smc_call(PFDI_FN_VERSION, 0, 0, 0, 0);
	complete(&worker_ctx->done);
}

static int pfdi_pe_get_version(unsigned int cpu, u32 *version)
{
	s64 ret;
	struct pfdi_cpu_worker *worker_ctx = &per_cpu(pfdi_worker_ctx, cpu);

	ret = pfdi_thread_worker(cpu, pfdi_pe_get_version_fn,
				 MAX_SCHEDULE_TIMEOUT);
	if (ret)
		return ret;

	ret = (int)worker_ctx->smc_ret.a0;
	if (ret >= 0)
		*version = (u32)ret;

	if (ret >= 0)
		return 0;

	return pfdi_to_linux_errno((int)ret);
}

static void pfdi_pe_get_features_fn(struct kthread_work *work)
{
	struct pfdi_cpu_worker *worker_ctx;

	worker_ctx = container_of(work, struct pfdi_cpu_worker, work);
	worker_ctx->result = pfdi_smc_call(PFDI_FN_FEATURES,
					   worker_ctx->feature_fid,
					   0, 0, 0).a0;
	complete(&worker_ctx->done);
}

static int pfdi_pe_get_features(unsigned int cpu, u32 fid)
{
	int ret;
	struct pfdi_cpu_worker *worker_ctx = &per_cpu(pfdi_worker_ctx, cpu);

	worker_ctx->feature_fid = fid;
	ret = pfdi_thread_worker(cpu, pfdi_pe_get_features_fn,
				 MAX_SCHEDULE_TIMEOUT);
	if (ret)
		return ret;

	return pfdi_to_linux_errno(worker_ctx->result);
}

static int pfdi_pe_get_test_id(unsigned int cpu, u64 *lib_info)
{
	int ret;
	struct pfdi_cpu_worker *worker_ctx = &per_cpu(pfdi_worker_ctx, cpu);

	ret = pfdi_thread_worker(cpu, pfdi_pe_get_test_id_fn,
				 MAX_SCHEDULE_TIMEOUT);
	if (ret)
		return ret;

	ret = (int)worker_ctx->smc_ret.a0;
	if (!ret)
		*lib_info = worker_ctx->smc_ret.a1;

	return pfdi_to_linux_errno(ret);
}

static void pfdi_pe_get_test_count_fn(struct kthread_work *work)
{
	struct pfdi_cpu_worker *worker_ctx;

	worker_ctx = container_of(work, struct pfdi_cpu_worker, work);
	worker_ctx->smc_ret = pfdi_smc_call(PFDI_FN_PE_TEST_PART_COUNT,
					    0, 0, 0, 0);
	complete(&worker_ctx->done);
}

static int pfdi_pe_get_test_count(unsigned int cpu, s64 *count)
{
	s64 ret;
	struct pfdi_cpu_worker *worker_ctx = &per_cpu(pfdi_worker_ctx, cpu);

	ret = pfdi_thread_worker(cpu, pfdi_pe_get_test_count_fn,
				 MAX_SCHEDULE_TIMEOUT);
	if (ret)
		return ret;

	ret = (s64)worker_ctx->smc_ret.a0;
	if (ret >= 0) {
		*count = ret;
		return 0;
	}

	return pfdi_to_linux_errno((int)ret);
}

static void pfdi_pe_fw_check_fn(struct kthread_work *work)
{
	struct pfdi_cpu_worker *worker_ctx;

	worker_ctx = container_of(work, struct pfdi_cpu_worker, work);
	worker_ctx->result = pfdi_smc_call(PFDI_FN_FW_CHECK,
					   0, 0, 0, 0).a0;
	complete(&worker_ctx->done);
}

static int pfdi_pe_fw_check(unsigned int cpu)
{
	int ret;

	ret = pfdi_thread_worker(cpu, pfdi_pe_fw_check_fn, MAX_SCHEDULE_TIMEOUT);
	if (ret)
		return ret;

	return pfdi_to_linux_errno(per_cpu(pfdi_worker_ctx, cpu).result);
}

static void pfdi_pe_get_result_fn(struct kthread_work *work)
{
	struct pfdi_cpu_worker *worker_ctx;

	worker_ctx = container_of(work, struct pfdi_cpu_worker, work);
	worker_ctx->smc_ret = pfdi_smc_call(PFDI_FN_PE_TEST_RESULT,
					    0, 0, 0, 0);
	complete(&worker_ctx->done);
}

static int pfdi_pe_get_result(unsigned int cpu, s64 *ft_id)
{
	int ret;
	struct pfdi_cpu_worker *worker_ctx = &per_cpu(pfdi_worker_ctx, cpu);

	ret = pfdi_thread_worker(cpu, pfdi_pe_get_result_fn,
				 MAX_SCHEDULE_TIMEOUT);
	if (ret)
		return ret;

	ret = (int)worker_ctx->smc_ret.a0;
	if (ret == PFDI_RET_FAULT_FOUND)
		*ft_id = worker_ctx->smc_ret.a1;

	return pfdi_to_linux_errno(ret);
}

static void pfdi_pe_force_error_fn(struct kthread_work *work)
{
	struct pfdi_cpu_worker *worker_ctx;

	worker_ctx = container_of(work, struct pfdi_cpu_worker, work);
	worker_ctx->result = pfdi_smc_call(PFDI_FN_FORCE_ERROR,
					   worker_ctx->error_info.smc_fid,
					   worker_ctx->error_info.error_id,
					   0, 0).a0;
	complete(&worker_ctx->done);
}

static int pfdi_pe_force_error(unsigned int cpu,
			       const struct pfdi_force_error *error_info)
{
	int ret;
	struct pfdi_cpu_worker *worker_ctx = &per_cpu(pfdi_worker_ctx, cpu);

	if (!pfdi_fid_supported(error_info->smc_fid))
		return -EINVAL;

	if (error_info->error_id < PFDI_RET_TEST_COUNT_ZERO ||
	    error_info->error_id > SMCCC_RET_SUCCESS)
		return -EINVAL;

	worker_ctx->error_info = *error_info;

	ret = pfdi_thread_worker(cpu, pfdi_pe_force_error_fn,
				 MAX_SCHEDULE_TIMEOUT);
	if (ret)
		return ret;

	return pfdi_to_linux_errno(worker_ctx->result);
}

static bool pfdi_ranges_equal(const struct pfdi_test_range *lhs,
			      const struct pfdi_test_range *rhs)
{
	return lhs->start == rhs->start && lhs->end == rhs->end;
}

static bool pfdi_run_available(const struct pfdi_per_cpu_info *cpu_info)
{
	return !atomic_read(&cpu_info->run_inprogress);
}

static int pfdi_validate_test_range(unsigned int cpu,
				    const struct pfdi_test_range *range)
{
	s64 count;
	int ret;

	if (range->start == -1 || range->end == -1)
		return pfdi_ranges_equal(range,
					 &(struct pfdi_test_range){ .start = -1,
								    .end = -1 }) ?
		       0 : -EINVAL;

	if (range->start < 0 || range->end < 0 || range->start > range->end)
		return -EINVAL;

	ret = pfdi_pe_get_test_count(cpu, &count);
	if (ret)
		return ret;

	if (range->start >= count || range->end >= count)
		return -EINVAL;

	return 0;
}

static void pfdi_pe_run_fn(struct kthread_work *work)
{
	struct arm_smccc_res smc_ret;
	struct pfdi_cpu_worker *worker_ctx;

	worker_ctx = container_of(work, struct pfdi_cpu_worker, work);
	smc_ret = pfdi_smc_call(PFDI_FN_PE_TEST_RUN,
				worker_ctx->test_run.range.start,
				worker_ctx->test_run.range.end, 0, 0);

	if ((int)smc_ret.a0 == PFDI_RET_FAULT_FOUND)
		worker_ctx->test_run.ft_id = smc_ret.a1;

	worker_ctx->result = (int)smc_ret.a0;
	complete(&worker_ctx->done);
}

static int pfdi_pe_run_tests(unsigned int cpu, struct pfdi_test_run *test_run)
{
	int ret;
	unsigned long wait_timeout;
	struct pfdi_cpu_worker *worker_ctx = &per_cpu(pfdi_worker_ctx, cpu);
	struct pfdi_per_cpu_info *cpu_info = &per_cpu(pfdi_cpu_info, cpu);

	ret = pfdi_validate_test_range(cpu, &test_run->range);
	if (ret)
		return ret;

	wait_timeout = msecs_to_jiffies(pfdi_wait_timeout_ms);

	mutex_lock(&cpu_info->state_lock);
	while (atomic_read(&cpu_info->run_inprogress)) {
		if (pfdi_ranges_equal(&test_run->range,
				      &cpu_info->ongoing_run_range)) {
			mutex_unlock(&cpu_info->state_lock);
			return -EBUSY;
		}

		mutex_unlock(&cpu_info->state_lock);
			ret = wait_event_interruptible_timeout(cpu_info->run_wait,
							       pfdi_run_available(cpu_info),
							       wait_timeout);
		if (ret <= 0)
			return ret ? ret : -ETIME;

		mutex_lock(&cpu_info->state_lock);
	}

	atomic_set(&cpu_info->run_inprogress, 1);
	cpu_info->ongoing_run_range = test_run->range;
	worker_ctx->test_run = *test_run;
	mutex_unlock(&cpu_info->state_lock);

	ret = pfdi_thread_worker(cpu, pfdi_pe_run_fn, wait_timeout);

	mutex_lock(&cpu_info->state_lock);
	if (worker_ctx->result == PFDI_RET_FAULT_FOUND)
		test_run->ft_id = worker_ctx->test_run.ft_id;
	atomic_set(&cpu_info->run_inprogress, 0);
	wake_up_all(&cpu_info->run_wait);
	mutex_unlock(&cpu_info->state_lock);

	if (ret)
		return ret;

	return pfdi_to_linux_errno(worker_ctx->result);
}

static int arm_pfdi_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc_dev = file->private_data;
	struct pfdi_dev_data *data;
	unsigned int cpu;

	if (!misc_dev || !misc_dev->this_device)
		return -ENODEV;

	data = dev_get_drvdata(misc_dev->this_device);
	if (!data)
		return -ENODEV;

	cpu = get_cpu();
	if (data->cpu_id != cpu) {
		put_cpu();
		return -EPERM;
	}

	if (down_trylock(&per_cpu(pfdi_cpu_info, cpu).open_sem)) {
		put_cpu();
		return -EBUSY;
	}

	put_cpu();
	return 0;
}

static long arm_pfdi_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	int err = 0;
	u32 feature_fid;
	u32 version;
	s64 count;
	s64 ft_id;
	u64 lib_info;
	struct miscdevice *misc_dev = file->private_data;
	struct pfdi_dev_data *device_data;
	struct pfdi_test_run test_run;
	struct pfdi_force_error error_info;
	unsigned int cpu;

	if (!misc_dev || !misc_dev->this_device)
		return -ENODEV;

	device_data = dev_get_drvdata(misc_dev->this_device);
	if (!device_data)
		return -ENODEV;

	cpu = device_data->cpu_id;

	switch (cmd) {
	case PFDI_IOCTL_VERSION:
		err = pfdi_pe_get_version(cpu, &version);
		if (!err && copy_to_user((void __user *)arg, &version,
					 sizeof(version)))
			err = -EFAULT;
		break;
	case PFDI_IOCTL_FEATURES:
		if (copy_from_user(&feature_fid, (void __user *)arg,
				   sizeof(feature_fid)))
			return -EFAULT;

		err = pfdi_pe_get_features(cpu, feature_fid);
		break;
	case PFDI_IOCTL_PE_TEST_ID:
		err = pfdi_pe_get_test_id(cpu, &lib_info);
		if (!err && copy_to_user((void __user *)arg, &lib_info,
					 sizeof(lib_info)))
			err = -EFAULT;
		break;
	case PFDI_IOCTL_PE_TEST_PART_COUNT:
		err = pfdi_pe_get_test_count(cpu, &count);
		if (!err && copy_to_user((void __user *)arg, &count,
					 sizeof(count)))
			err = -EFAULT;
		break;
	case PFDI_IOCTL_PE_TEST_RUN:
		if (copy_from_user(&test_run, (void __user *)arg,
				   sizeof(test_run)))
			return -EFAULT;

		err = pfdi_pe_run_tests(cpu, &test_run);
		if (err == -EFAULT &&
		    copy_to_user((void __user *)arg, &test_run,
				 sizeof(test_run)))
			err = -EFAULT;
		break;
	case PFDI_IOCTL_PE_TEST_RESULT:
		err = pfdi_pe_get_result(cpu, &ft_id);
		if (err == -EFAULT && copy_to_user((void __user *)arg, &ft_id,
						   sizeof(ft_id)))
			err = -EFAULT;
		break;
	case PFDI_IOCTL_FW_CHECK:
		err = pfdi_pe_fw_check(cpu);
		break;
	case PFDI_IOCTL_FORCE_ERROR:
		if (copy_from_user(&error_info, (void __user *)arg,
				   sizeof(error_info)))
			return -EFAULT;
		if (error_info.reserved)
			return -EINVAL;

		err = pfdi_pe_force_error(cpu, &error_info);
		break;
	default:
		err = -ENOTTY;
		break;
	}

	return err;
}

static int arm_pfdi_release(struct inode *inode, struct file *file)
{
	struct miscdevice *misc_dev = file->private_data;
	struct pfdi_dev_data *data;
	unsigned int cpu;

	if (!misc_dev || !misc_dev->this_device)
		return 0;

	data = dev_get_drvdata(misc_dev->this_device);
	cpu = data ? data->cpu_id : raw_smp_processor_id();
	up(&per_cpu(pfdi_cpu_info, cpu).open_sem);

	return 0;
}

static const struct file_operations arm_pfdi_fops = {
	.owner		= THIS_MODULE,
	.open		= arm_pfdi_open,
	.release	= arm_pfdi_release,
	.unlocked_ioctl	= arm_pfdi_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= compat_ptr_ioctl,
#endif
};

static void arm_pfdi_device_init(struct miscdevice *misc_dev,
				 struct pfdi_dev_data *dev_data, unsigned int cpu)
{
	misc_dev->minor = MISC_DYNAMIC_MINOR;
	dev_data->cpu_id = cpu;
	snprintf(dev_data->name, sizeof(dev_data->name), "pfdi%u", cpu);
	snprintf(dev_data->nodename, sizeof(dev_data->nodename),
		 "cpu/%u/pfdi", cpu);
	misc_dev->name = dev_data->name;
	misc_dev->nodename = dev_data->nodename;
	misc_dev->mode = 0600;
	misc_dev->fops = &arm_pfdi_fops;
}

static int arm_pfdi_device_create(unsigned int cpu)
{
	int ret;
	struct miscdevice *misc_dev = &per_cpu(pfdi_misc_device, cpu);
	struct pfdi_dev_data *dev_data = &per_cpu(pfdi_device_data, cpu);
	struct pfdi_cpu_worker *worker_ctx = &per_cpu(pfdi_worker_ctx, cpu);

	memset(misc_dev, 0, sizeof(*misc_dev));
	arm_pfdi_device_init(misc_dev, dev_data, cpu);

	sema_init(&per_cpu(pfdi_cpu_info, cpu).open_sem, 1);
	mutex_init(&worker_ctx->worker_lock);
	atomic_set(&per_cpu(pfdi_cpu_info, cpu).run_inprogress, 0);
	mutex_init(&per_cpu(pfdi_cpu_info, cpu).state_lock);
	init_waitqueue_head(&per_cpu(pfdi_cpu_info, cpu).run_wait);
	init_completion(&worker_ctx->done);
	per_cpu(pfdi_worker, cpu) =
		kthread_create_worker_on_cpu(cpu, 0, "pfdi/%u");
	if (IS_ERR(per_cpu(pfdi_worker, cpu))) {
		ret = PTR_ERR(per_cpu(pfdi_worker, cpu));
		per_cpu(pfdi_worker, cpu) = NULL;
		return ret;
	}

	ret = misc_register(misc_dev);
	if (ret)
		goto destroy_worker;

	if (!misc_dev->this_device) {
		ret = -ENODEV;
		goto deregister_misc;
	}

	dev_set_drvdata(misc_dev->this_device, dev_data);

	return 0;

deregister_misc:
	misc_deregister(misc_dev);
destroy_worker:
	kthread_destroy_worker(per_cpu(pfdi_worker, cpu));
	per_cpu(pfdi_worker, cpu) = NULL;
	return ret;
}

static int arm_pfdi_device_destroy(unsigned int cpu)
{
	struct miscdevice *misc_dev = &per_cpu(pfdi_misc_device, cpu);

	if (atomic_read(&per_cpu(pfdi_cpu_info, cpu).run_inprogress) > 0)
		return -EBUSY;

	if (per_cpu(pfdi_worker, cpu)) {
		kthread_flush_worker(per_cpu(pfdi_worker, cpu));
		kthread_destroy_worker(per_cpu(pfdi_worker, cpu));
		per_cpu(pfdi_worker, cpu) = NULL;
	}

	dev_set_drvdata(misc_dev->this_device, NULL);
	misc_deregister(misc_dev);
	return 0;
}

static int __init arm_pfdi_init(void)
{
	int ret;

	ret = pfdi_validate_cpu_features();
	if (ret)
		return ret;

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "firmware/arm-pfdi:online",
				arm_pfdi_device_create, arm_pfdi_device_destroy);
	if (ret < 0)
		return ret;

	pfdi_cpuhp_state = ret;
	return 0;
}

static void __exit arm_pfdi_exit(void)
{
	if (pfdi_cpuhp_state)
		cpuhp_remove_state(pfdi_cpuhp_state);
}

module_init(arm_pfdi_init);
module_exit(arm_pfdi_exit);

MODULE_DESCRIPTION("Arm Platform Fault Detection Interface driver");
MODULE_AUTHOR("Divin Raj <divin.raj@arm.com>");
MODULE_LICENSE("GPL");
