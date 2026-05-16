// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON IBS (Instruction-Based Sampling) backend for AMD processors.
 *
 * Uses AMD IBS Op sampling via the perf kernel counter infrastructure to
 * deliver PA-keyed access reports to DAMON's shared-layer ring buffer
 * (see damon_report_access()).  This enables physical-address hot-page
 * detection without relying on page-table Accessed bits or page faults.
 *
 * The IBS sampling approach in this file derives from concepts in
 * Bharata B Rao's pghot RFC v5 series for hot page tracking.
 * See: https://lore.kernel.org/linux-mm/20260129144043.231636-1-bharata@amd.com/
 *
 * Author: Ravi Jonnalagadda <ravis.opensrc@gmail.com>
 */

#include <linux/cpu.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/perf_event.h>
#include <linux/slab.h>
#include <linux/smp.h>

#include <linux/damon.h>
#include "ops-common.h"

#define DAMON_IBS_DEFAULT_MAX_CNT	262144	/* ~4K samples/sec/core */
#define IBS_OP_PMU_TYPE_PATH	"/sys/bus/event_source/devices/ibs_op/type"

static unsigned int damon_ibs_max_cnt = DAMON_IBS_DEFAULT_MAX_CNT;

static int damon_ibs_set_sample_rate(unsigned int max_cnt);

static int max_cnt_set(const char *val, const struct kernel_param *kp)
{
	unsigned int new_cnt;
	int ret;

	ret = kstrtouint(val, 0, &new_cnt);
	if (ret)
		return ret;
	if (!new_cnt)
		return -EINVAL;
	return damon_ibs_set_sample_rate(new_cnt);
}
static const struct kernel_param_ops max_cnt_ops = {
	.set = max_cnt_set,
	.get = param_get_uint,
};
module_param_cb(max_cnt, &max_cnt_ops, &damon_ibs_max_cnt, 0644);
MODULE_PARM_DESC(max_cnt,
	"IBS MaxCnt (ops between samples). Writes restart sampling.");

static DEFINE_MUTEX(damon_ibs_lock);
static bool damon_ibs_enabled;
static enum cpuhp_state damon_ibs_cpuhp_state;
static unsigned int ibs_pmu_type;	/* discovered at init */

static DEFINE_PER_CPU(struct perf_event *, damon_ibs_event);

/*
 * Diagnostic counters.  Incremented from NMI context, so use per-CPU
 * counters and sum them on read.
 */
static DEFINE_PER_CPU(unsigned long, ibs_samples_total_pcpu);
static DEFINE_PER_CPU(unsigned long, ibs_samples_filtered_pcpu);

static unsigned long damon_ibs_sum_pcpu(unsigned long __percpu *var)
{
	unsigned long sum = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		sum += per_cpu(*var, cpu);
	return sum;
}

static int samples_total_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%lu\n",
			damon_ibs_sum_pcpu(&ibs_samples_total_pcpu));
}

static int samples_filtered_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%lu\n",
			damon_ibs_sum_pcpu(&ibs_samples_filtered_pcpu));
}

static const struct kernel_param_ops samples_total_ops = {
	.get = samples_total_get,
};
static const struct kernel_param_ops samples_filtered_ops = {
	.get = samples_filtered_get,
};

module_param_cb(samples_total, &samples_total_ops, NULL, 0444);
MODULE_PARM_DESC(samples_total, "Total IBS samples delivered (read-only)");
module_param_cb(samples_filtered, &samples_filtered_ops, NULL, 0444);
MODULE_PARM_DESC(samples_filtered, "IBS samples filtered out (read-only)");

/**
 * damon_ibs_overflow_handler() - IBS overflow callback.
 *
 * Called when an IBS Op counter overflows.  The IBS perf driver fills
 * data->phys_addr from IBSDCPHYSAD when dc_phy_addr_valid is set.
 *
 * Context: NMI — no sleeping, no mutex, no kmalloc.
 */
static void damon_ibs_overflow_handler(struct perf_event *event,
				       struct perf_sample_data *data,
				       struct pt_regs *regs)
{
	struct damon_access_report report;
	unsigned long phys_addr;

	if (!data)
		return;

	/*
	 * PERF_SAMPLE_PHYS_ADDR was requested in attr.sample_type, but
	 * the IBS perf driver only populates data->phys_addr when
	 * IBS_OP_DATA3.dc_phy_addr_valid is set.  Skip stale-PA samples
	 * by checking the sample_flags rather than testing phys_addr
	 * for zero (which would also drop legitimate page 0).
	 */
	if (!(data->sample_flags & PERF_SAMPLE_PHYS_ADDR)) {
		this_cpu_inc(ibs_samples_filtered_pcpu);
		return;
	}
	phys_addr = data->phys_addr;

	report = (struct damon_access_report){
		.paddr = phys_addr & PAGE_MASK,
		.size = PAGE_SIZE,
		.cpu = smp_processor_id(),
		.is_write = !!(data->data_src.mem_op & PERF_MEM_OP_STORE),
	};
	damon_report_access(&report);
	this_cpu_inc(ibs_samples_total_pcpu);
}

static int damon_ibs_create_event(int cpu)
{
	struct perf_event_attr attr = {
		.type = ibs_pmu_type,
		.size = sizeof(attr),
		/* config=0: IBS perf driver uses sample_period as MaxCnt. */
		.config = 0,
		.sample_period = damon_ibs_max_cnt,
		.sample_type = PERF_SAMPLE_PHYS_ADDR | PERF_SAMPLE_DATA_SRC,
		.pinned = 1,
	};
	struct perf_event *event;

	event = perf_event_create_kernel_counter(&attr, cpu, NULL,
						 damon_ibs_overflow_handler,
						 NULL);
	if (IS_ERR(event))
		return PTR_ERR(event);

	/*
	 * perf_event_create_kernel_counter() returns the event already
	 * enabled; no perf_event_enable() needed here.
	 */
	per_cpu(damon_ibs_event, cpu) = event;
	return 0;
}

static void damon_ibs_destroy_event(int cpu)
{
	struct perf_event *event = per_cpu(damon_ibs_event, cpu);

	if (!event)
		return;

	perf_event_disable(event);
	perf_event_release_kernel(event);
	per_cpu(damon_ibs_event, cpu) = NULL;
}

static int damon_ibs_cpu_online(unsigned int cpu)
{
	int ret = damon_ibs_create_event(cpu);

	if (ret)
		pr_warn_ratelimited(
			"damon-ibs: failed to create perf_event on cpu %u (err %d); "
			"this cpu will not contribute samples\n", cpu, ret);
	return 0;	/* never block CPU online */
}

static int damon_ibs_cpu_offline(unsigned int cpu)
{
	damon_ibs_destroy_event(cpu);
	return 0;
}

/* Caller must hold damon_ibs_lock. */
static int __damon_ibs_start(void)
{
	int ret;

	if (damon_ibs_enabled)
		return -EBUSY;

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "damon/ibs:online",
				damon_ibs_cpu_online, damon_ibs_cpu_offline);
	if (ret < 0)
		return ret;
	damon_ibs_cpuhp_state = ret;

	damon_ibs_enabled = true;
	pr_info_once("damon-ibs: first start (max_cnt=%u, pmu_type=%u)\n",
		     damon_ibs_max_cnt, ibs_pmu_type);
	return 0;
}

/* Caller must hold damon_ibs_lock. */
static void __damon_ibs_stop(void)
{
	if (!damon_ibs_enabled)
		return;

	cpuhp_remove_state(damon_ibs_cpuhp_state);
	damon_ibs_enabled = false;
}

static int damon_ibs_start(void)
{
	int ret;

	mutex_lock(&damon_ibs_lock);
	ret = __damon_ibs_start();
	mutex_unlock(&damon_ibs_lock);
	return ret;
}

static void damon_ibs_stop(void)
{
	mutex_lock(&damon_ibs_lock);
	__damon_ibs_stop();
	mutex_unlock(&damon_ibs_lock);
}

/**
 * damon_ibs_set_sample_rate() - Set IBS sampling interval.
 * @max_cnt: IBS Op MaxCnt value (ops between samples).
 *           Higher = fewer samples/sec.
 *
 * If IBS is already running, restart it with the new rate.
 *
 * Return: 0 on success; if a restart was required and failed,
 * propagate the error so callers (e.g. the max_cnt module-param
 * .set callback) surface it to userspace instead of silently
 * leaving sampling stopped.
 */
static int damon_ibs_set_sample_rate(unsigned int max_cnt)
{
	int ret = 0;

	mutex_lock(&damon_ibs_lock);
	damon_ibs_max_cnt = max_cnt ? max_cnt : DAMON_IBS_DEFAULT_MAX_CNT;

	if (damon_ibs_enabled) {
		__damon_ibs_stop();
		ret = __damon_ibs_start();
		if (ret)
			pr_warn("damon-ibs: restart failed: %d\n", ret);
	}
	mutex_unlock(&damon_ibs_lock);
	return ret;
}


static void damon_ibs_init_ctx(struct damon_ctx *ctx)
{
	int ret;

	/* IBS is the access-detection source for this ctx. */
	ctx->sample_control.primitives_enabled.page_table = false;

	ret = damon_ibs_start();
	if (ret && ret != -EBUSY)
		pr_warn("damon-ibs: failed to start IBS sampling: %d\n", ret);
}

/**
 * damon_ibs_discover_pmu_type() - Discover IBS Op PMU type from sysfs.
 *
 * Reads /sys/bus/event_source/devices/ibs_op/type to get the PMU type
 * identifier needed for perf_event_attr.type.
 *
 * TODO: replace sysfs-read with a PMU lookup API when one becomes
 * available.
 *
 * Return: 0 on success, negative error code otherwise.
 */
static int damon_ibs_discover_pmu_type(void)
{
	struct file *f;
	char buf[16];
	loff_t pos = 0;
	ssize_t len;
	int ret;

	f = filp_open(IBS_OP_PMU_TYPE_PATH, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	len = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	filp_close(f, NULL);
	if (len <= 0)
		return -EIO;

	buf[len] = '\0';
	ret = kstrtouint(strim(buf), 10, &ibs_pmu_type);
	if (ret)
		return ret;

	pr_info("damon-ibs: discovered ibs_op PMU type=%u\n", ibs_pmu_type);
	return 0;
}

static int __init damon_ibs_init(void)
{
	struct damon_operations ops = {
		.id = DAMON_OPS_PADDR_IBS,
		.owner = THIS_MODULE,
		.init = damon_ibs_init_ctx,
		.prepare_access_checks = damon_pa_prepare_access_checks,
		.check_accesses = damon_pa_check_accesses,
		.apply_probes = damon_pa_apply_probes,
		.apply_scheme = damon_pa_apply_scheme,
		.get_scheme_score = damon_pa_scheme_score,
	};
	int err;

	if (!boot_cpu_has(X86_FEATURE_IBS))
		return -ENODEV;

	err = damon_ibs_discover_pmu_type();
	if (err) {
		pr_err("damon-ibs: failed to discover IBS PMU type: %d\n", err);
		return err;
	}

	err = damon_register_ops(&ops);
	if (err)
		return err;

	pr_info("damon-ibs: AMD IBS backend registered (max_cnt=%u, pmu_type=%u)\n",
		damon_ibs_max_cnt, ibs_pmu_type);
	return 0;
}

static void __exit damon_ibs_exit(void)
{
	damon_ibs_stop();
	damon_unregister_ops(DAMON_OPS_PADDR_IBS);
}

module_init(damon_ibs_init);
module_exit(damon_ibs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ravi Jonnalagadda <ravis.opensrc@gmail.com>");
MODULE_DESCRIPTION("AMD IBS-based access sampling backend for DAMON");
