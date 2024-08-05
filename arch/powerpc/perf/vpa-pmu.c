// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Perf interface to expose Virtual Processor counters.
 *
 * Copyright (C) 2024 Kajol Jain, IBM Corporation
 */

#define pr_fmt(fmt) "vpa-pmu: " fmt

#include <asm/dtl.h>
#include <linux/perf_event.h>
#include <asm/plpar_wrappers.h>

#define EVENT(_name, _code)     enum{_name = _code}

/*
 * Based on Power Architecture Platform Reference(PAPR) documentation,
 * Table 14.14. Per Virtual Processor Area, below Dispatch Trace Log(DTL)
 * Enable Mask used to get corresponding virtual processor dispatch
 * to preempt traces:
 *   DTL_CEDE(0x1): Trace voluntary (OS initiated) virtual
 *   processor waits
 *   DTL_PREEMPT(0x2): Trace time slice preempts
 *   DTLFAULT(0x4): Trace virtual partition memory page
 faults.
 *   DTL_ALL(0x7): Trace all (DTL_CEDE | DTL_PREEMPT | DTL_FAULT)
 *
 * Event codes based on Dispatch Trace Log Enable Mask.
 */
EVENT(DTL_CEDE,         0x1);
EVENT(DTL_PREEMPT,      0x2);
EVENT(DTL_FAULT,        0x4);
EVENT(DTL_ALL,          0x7);

GENERIC_EVENT_ATTR(dtl_cede, DTL_CEDE);
GENERIC_EVENT_ATTR(dtl_preempt, DTL_PREEMPT);
GENERIC_EVENT_ATTR(dtl_fault, DTL_FAULT);
GENERIC_EVENT_ATTR(dtl_all, DTL_ALL);

PMU_FORMAT_ATTR(event, "config:0-7");

static struct attribute *events_attr[] = {
	GENERIC_EVENT_PTR(DTL_CEDE),
	GENERIC_EVENT_PTR(DTL_PREEMPT),
	GENERIC_EVENT_PTR(DTL_FAULT),
	GENERIC_EVENT_PTR(DTL_ALL),
	NULL
};

static struct attribute_group event_group = {
	.name = "events",
	.attrs = events_attr,
};

static struct attribute *format_attrs[] = {
	&format_attr_event.attr,
	NULL,
};

static const struct attribute_group format_group = {
	.name = "format",
	.attrs = format_attrs,
};

static const struct attribute_group *attr_groups[] = {
	&format_group,
	&event_group,
	NULL,
};

struct vpa_dtl {
	struct dtl_entry	*buf;
	u64			last_idx;
	bool			active_lock;
};

static DEFINE_PER_CPU(struct vpa_dtl, vpa_cpu_dtl);

/* variable to capture reference count for the active dtl threads */
static int dtl_global_refc;
static spinlock_t dtl_global_lock = __SPIN_LOCK_UNLOCKED(dtl_global_lock);

/*
 * Function to dump the dispatch trace log buffer data to the
 * perf raw sample.
 */
static void vpa_dtl_dump_sample_data(struct perf_event *event)
{
	struct perf_sample_data data;
	struct perf_raw_record raw;
	struct pt_regs regs;
	u64 cur_idx, last_idx, i;
	char *buf;

	/* actual number of entries read */
	long n_read = 0, read_size = 0;

	/* number of entries added to dtl buffer */
	long n_req;

	struct vpa_dtl *dtl = &per_cpu(vpa_cpu_dtl, event->cpu);
	int version = 1;

	/* Setup perf sample */
	perf_sample_data_init(&data, 0, event->hw.last_period);
	memset(&regs, 0, sizeof(regs));
	memset(&raw, 0, sizeof(raw));

	cur_idx = be64_to_cpu(lppaca_of(event->cpu).dtl_idx);
	last_idx = dtl->last_idx;

	if (last_idx + N_DISPATCH_LOG <= cur_idx)
		last_idx = cur_idx - N_DISPATCH_LOG + 1;

	n_req = cur_idx - last_idx;

	/* no new entry added to the buffer, return */
	if (n_req <= 0)
		return;

	dtl->last_idx = last_idx + n_req;

	buf = kzalloc((n_req * sizeof(struct dtl_entry)) + sizeof(version) +
			sizeof(tb_ticks_per_sec) + sizeof(n_req), GFP_KERNEL);
	if (!buf) {
		pr_warn("buffer alloc failed for perf raw data for cpu%d\n",
				event->cpu);
		return;
	}
	raw.frag.data = buf;

	/* Save current version of dtl sampling support */
	memcpy(buf, &version, sizeof(version));
	buf += sizeof(version);

	/* Save tb_ticks_per_sec to convert timebase to sec */
	memcpy(buf, &tb_ticks_per_sec, sizeof(tb_ticks_per_sec));
	buf += sizeof(tb_ticks_per_sec);

	/* Save total number of dtl entries added to the dtl buffer */
	memcpy(buf, &n_req, sizeof(n_req));
	buf += sizeof(n_req);

	i = last_idx % N_DISPATCH_LOG;

	/* read the tail of the buffer if we've wrapped */
	if (i + n_req > N_DISPATCH_LOG) {
		read_size = N_DISPATCH_LOG - i;
		memcpy(buf, &dtl->buf[i], read_size * sizeof(struct dtl_entry));
		i = 0;
		n_req -= read_size;
		n_read += read_size;
		buf += read_size * sizeof(struct dtl_entry);
	}

	/* .. and now the head */
	memcpy(buf, &dtl->buf[i], n_req * sizeof(struct dtl_entry));
	n_read += n_req;

	raw.frag.size = n_read * sizeof(struct dtl_entry) + sizeof(version) +
		sizeof(tb_ticks_per_sec) + sizeof(n_req);

	perf_sample_save_raw_data(&data, &raw);
	perf_event_overflow(event, &data, &regs);
}

/*
 * The VPA Dispatch Trace log counters do not interrupt on overflow.
 * Therefore, the kernel needs to poll the counters to avoid missing
 * an overflow using hrtimer. The timer interval is based on sample_period
 * count provided by user, and minimum interval is 1 millisecond.
 */
static enum hrtimer_restart vpa_dtl_hrtimer_handle(struct hrtimer *hrtimer)
{
	struct perf_event *event;
	u64 period;

	event = container_of(hrtimer, struct perf_event, hw.hrtimer);

	if (event->state != PERF_EVENT_STATE_ACTIVE)
		return HRTIMER_NORESTART;

	vpa_dtl_dump_sample_data(event);
	period = max_t(u64, 1000000, event->hw.sample_period);
	hrtimer_forward_now(hrtimer, ns_to_ktime(period));

	return HRTIMER_RESTART;
}

static void vpa_dtl_start_hrtimer(struct perf_event *event)
{
	u64 period;
	struct hw_perf_event *hwc = &event->hw;

	period = max_t(u64, 1000000, hwc->sample_period);
	hrtimer_start(&hwc->hrtimer, ns_to_ktime(period), HRTIMER_MODE_REL_PINNED);
}

static void vpa_dtl_stop_hrtimer(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;

	hrtimer_cancel(&hwc->hrtimer);
}

static void vpa_dtl_reset_global_refc(struct perf_event *event)
{
	spin_lock(&dtl_global_lock);
	dtl_global_refc--;
	if (dtl_global_refc <= 0) {
		dtl_global_refc = 0;
		write_unlock(&dtl_access_lock);
	}
	spin_unlock(&dtl_global_lock);
}

static int vpa_dtl_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct vpa_dtl *dtl = &per_cpu(vpa_cpu_dtl, event->cpu);

	/* test the event attr type for PMU enumeration */
	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	if (!perfmon_capable())
		return -EACCES;

	/* Return if this is a counting event */
	if (!is_sampling_event(event))
		return -EOPNOTSUPP;

	if (!(event->attr.sample_type & PERF_SAMPLE_RAW)) {
		pr_debug("To enable perf sampling, run with -R/raw-samples option");
		return -EOPNOTSUPP;
	}

	/* Invalid eventcode */
	switch (event->attr.config) {
	case DTL_LOG_CEDE:
	case DTL_LOG_PREEMPT:
	case DTL_LOG_FAULT:
	case DTL_LOG_ALL:
		break;
	default:
		return -EINVAL;
	}

	spin_lock(&dtl_global_lock);

	/*
	 * To ensure there are no other conflicting dtl users
	 * (example: /proc/powerpc/vcpudispatch_stats or debugfs dtl),
	 * below code try to take the dtl_access_lock.
	 * The dtl_access_lock is a rwlock defined in dtl.h, which is used
	 * to unsure there is no conflicting dtl users.
	 * Based on below code, vpa_dtl pmu tries to take write access lock
	 * and also checks for dtl_global_refc, to make sure that the
	 * dtl_access_lock is taken by vpa_dtl pmu interface.
	 */
	if (dtl_global_refc == 0 && !write_trylock(&dtl_access_lock)) {
		spin_unlock(&dtl_global_lock);
		return -EBUSY;
	}

	/*
	 * Increment the number of active vpa_dtl pmu threads. The
	 * dtl_global_refc is used to keep count of cpu threads that
	 * currently capturing dtl data using vpa_dtl pmu interface.
	 */
	dtl_global_refc++;

	/*
	 * active_lock is a per cpu variable which is set if
	 * current cpu is running vpa_pmu perf record session.
	 */
	dtl->active_lock = true;
	spin_unlock(&dtl_global_lock);

	hrtimer_init(&hwc->hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hwc->hrtimer.function = vpa_dtl_hrtimer_handle;

	/*
	 * Since hrtimers have a fixed rate, we can do a static freq->period
	 * mapping and avoid the whole period adjust feedback stuff.
	 */
	if (event->attr.freq) {
		long freq = event->attr.sample_freq;

		event->attr.sample_period = NSEC_PER_SEC / freq;
		hwc->sample_period = event->attr.sample_period;
		local64_set(&hwc->period_left, hwc->sample_period);
		hwc->last_period = hwc->sample_period;
		event->attr.freq = 0;
	}

	event->destroy = vpa_dtl_reset_global_refc;
	return 0;
}

static int vpa_dtl_event_add(struct perf_event *event, int flags)
{
	int ret, hwcpu;
	unsigned long addr;
	struct vpa_dtl *dtl = &per_cpu(vpa_cpu_dtl, event->cpu);

	/*
	 * Register our dtl buffer with the hypervisor. The
	 * HV expects the buffer size to be passed in the second
	 * word of the buffer. Refer section '14.11.3.2. H_REGISTER_VPA'
	 * from PAPR for more information.
	 */
	((u32 *)dtl->buf)[1] = cpu_to_be32(DISPATCH_LOG_BYTES);
	dtl->last_idx = 0;

	hwcpu = get_hard_smp_processor_id(event->cpu);
	addr = __pa(dtl->buf);

	ret = register_dtl(hwcpu, addr);
	if (ret) {
		pr_warn("DTL registration for cpu %d (hw %d) failed with %d\n",
			event->cpu, hwcpu, ret);
		return ret;
	}

	/* set our initial buffer indices */
	lppaca_of(event->cpu).dtl_idx = 0;

	/*
	 * Ensure that our updates to the lppaca fields have
	 * occurred before we actually enable the logging
	 */
	smp_wmb();

	/* enable event logging */
	lppaca_of(event->cpu).dtl_enable_mask = event->attr.config;

	vpa_dtl_start_hrtimer(event);

	return 0;
}

static void vpa_dtl_event_del(struct perf_event *event, int flags)
{
	int hwcpu = get_hard_smp_processor_id(event->cpu);
	struct vpa_dtl *dtl = &per_cpu(vpa_cpu_dtl, event->cpu);

	vpa_dtl_dump_sample_data(event);
	vpa_dtl_stop_hrtimer(event);
	unregister_dtl(hwcpu);
	lppaca_of(event->cpu).dtl_enable_mask = 0x0;
	dtl->active_lock = false;
}

static void vpa_dtl_event_read(struct perf_event *event)
{
	/*
	 * This function defination is empty as vpa_dtl_dump_sample_data
	 * is used to parse and dump the dispatch trace log data,
	 * to perf raw sample.
	 */
}

/* Allocate dtl buffer memory for given cpu. */
static int vpa_dtl_mem_alloc(int cpu)
{
	struct vpa_dtl *dtl = &per_cpu(vpa_cpu_dtl, cpu);
	struct dtl_entry *buf = NULL;

	if (dtl->buf)
		return 0;
	dtl->active_lock = false;

	/* Check for dispatch trace log buffer cache */
	if (!dtl_cache)
		return -ENOMEM;

	buf = kmem_cache_alloc_node(dtl_cache, GFP_KERNEL, cpu_to_node(cpu));
	if (!buf) {
		pr_warn("buffer allocation failed for cpu %d\n", cpu);
		return -ENOMEM;
	}
	dtl->buf = buf;
	return 0;
}

static int vpa_dtl_cpu_online(unsigned int cpu)
{
	return vpa_dtl_mem_alloc(cpu);
}

static int vpa_dtl_cpu_offline(unsigned int cpu)
{
	struct vpa_dtl *dtl = &per_cpu(vpa_cpu_dtl, cpu);

	/* Reduce the ref count if dtl event running on this cpu */
	spin_lock(&dtl_global_lock);
	if (dtl_global_refc && dtl->active_lock)
		dtl_global_refc--;
	if (dtl_global_refc <= 0) {
		dtl_global_refc = 0;
		write_unlock(&dtl_access_lock);
	}
	spin_unlock(&dtl_global_lock);
	return 0;
}

static int vpa_dtl_cpu_hotplug_init(void)
{
	return cpuhp_setup_state(CPUHP_AP_PERF_POWERPC_VPA_DTL_ONLINE,
			  "perf/powerpc/vpa_pmu:online",
			  vpa_dtl_cpu_online,
			  vpa_dtl_cpu_offline);
}

static void vpa_dtl_clear_memory(void)
{
	int i;

	for_each_online_cpu(i) {
		struct vpa_dtl *dtl = &per_cpu(vpa_cpu_dtl, i);

		kmem_cache_free(dtl_cache, dtl->buf);
		dtl->buf = NULL;
	}
}

static struct pmu vpa_dtl_pmu = {
	.task_ctx_nr = perf_invalid_context,

	.name = "vpa_dtl",
	.attr_groups = attr_groups,
	.event_init  = vpa_dtl_event_init,
	.add         = vpa_dtl_event_add,
	.del         = vpa_dtl_event_del,
	.read        = vpa_dtl_event_read,
	.capabilities = PERF_PMU_CAP_NO_EXCLUDE | PERF_PMU_CAP_EXCLUSIVE,
};

static int vpa_pmu_init(void)
{
	int r;

	if (!firmware_has_feature(FW_FEATURE_SPLPAR)) {
		pr_debug("not a shared virtualized system, not enabling\n");
		return -ENODEV;
	}

	/* This driver is intended only for L1 host */
	if (is_kvm_guest()) {
		pr_debug("Only supported for L1 host system\n");
		return -ENODEV;
	}

	/* init cpuhotplug */
	r = vpa_dtl_cpu_hotplug_init();
	if (r) {
		vpa_dtl_clear_memory();
		return r;
	}

	r = perf_pmu_register(&vpa_dtl_pmu, vpa_dtl_pmu.name, -1);
	if (r)
		return r;

	return 0;
}

device_initcall(vpa_pmu_init);
