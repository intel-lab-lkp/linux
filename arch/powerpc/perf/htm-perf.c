// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Perf interface to expose HTM Trace data.
 *
 * Copyright (C) 2025 Athira Rajeev, IBM Corporation
 */

#define pr_fmt(fmt) "htm: " fmt

#include <asm/dtl.h>
#include <linux/perf_event.h>
#include <asm/plpar_wrappers.h>
#include <linux/vmalloc.h>

extern void perf_event_wakeup(struct perf_event *event);
#define EVENT(_name, _code)     enum{_name = _code}

/*
 * H_HTM (Hardware Trace Macro) hypervisor call is an HCALL to export
 * data from Hardware Trace Macro (HTM) function.
 *
 * Event codes based on HTM type.
 */
EVENT(HTM_CORE,         0x2);
EVENT(HTM_NEST,	        0x1);

GENERIC_EVENT_ATTR(htm_core, HTM_CORE);
GENERIC_EVENT_ATTR(htm_nest, HTM_NEST);

PMU_FORMAT_ATTR(event, "config:0-27");
PMU_FORMAT_ATTR(htm_type, "config:0-3");
PMU_FORMAT_ATTR(nodeindex, "config:4-11");
PMU_FORMAT_ATTR(nodalchipindex, "config:12-19");
PMU_FORMAT_ATTR(coreindexonchip, "config:20-27");

static struct attribute *events_attr[] = {
	GENERIC_EVENT_PTR(HTM_NEST),
	GENERIC_EVENT_PTR(HTM_CORE),
	NULL
};

static struct attribute_group event_group = {
	.name = "events",
	.attrs = events_attr,
};

static struct attribute *format_attrs[] = {
	&format_attr_event.attr,
	&format_attr_htm_type.attr,
	&format_attr_nodeindex.attr,
	&format_attr_nodalchipindex.attr,
	&format_attr_coreindexonchip.attr,
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

static u64 htmflags = H_HTM_FLAGS_NOWRAP;

/*
 * Check the return code for H_HTM hcall.
 * Return non-zero value (1) if either H_PARTIAL or H_SUCCESS
 * is returned. For other return codes:
 * Return zero if H_NOT_AVAILABLE.
 * Return -EBUSY if hcall return busy.
 * Return -EINVAL if any parameter or operation is not valid.
 * Return -EPERM if HTM Virtualization Engine Technology code
 * is not applied.
 * Return -EIO if the HTM state is not valid.
 */
static ssize_t htm_return_check(int rc)
{
	switch (rc) {
	case H_SUCCESS:
		break;
	/* H_PARTIAL for the case where all available data can't be
	 * returned due to buffer size constraint.
	 */
	case H_PARTIAL:
		break;
	/* H_NOT_AVAILABLE indicates reading from an offset outside the range,
	 * i.e. past end of file.
	 */
	case H_NOT_AVAILABLE:
		return 0;
	case H_BUSY:
	case H_LONG_BUSY_ORDER_1_MSEC:
	case H_LONG_BUSY_ORDER_10_MSEC:
	case H_LONG_BUSY_ORDER_100_MSEC:
	case H_LONG_BUSY_ORDER_1_SEC:
	case H_LONG_BUSY_ORDER_10_SEC:
	case H_LONG_BUSY_ORDER_100_SEC:
		return -EBUSY;
	case H_PARAMETER:
		goto out;
	case H_P2:
		goto out;
	case H_P3:
		goto out;
	case H_P4:
		goto out;
	case H_P5:
		goto out;
	case H_P6:
		return -EINVAL;
	case H_STATE:
		return -EIO;
	case H_AUTHORITY:
		return -EPERM;
	}

	/*
	 * Return 1 for H_SUCCESS/H_PARTIAL
	 */
	return 1;
out:
	return -EINVAL;
}

static int htm_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	u64 config = event->attr.config;
	u32 htmtype;

	if (event->attr.inherit)
		return -EOPNOTSUPP;

	/* test the event attr type for PMU enumeration */
	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	if (!perfmon_capable())
		return -EACCES;

	/* Return if this is a counting event */
	if (!is_sampling_event(event))
		return -EOPNOTSUPP;

	/* no branch sampling */
	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	htmtype = config & 0xf;
	/* Invalid eventcode */
	switch (htmtype) {
	case HTM_CORE:
	case HTM_NEST:
		break;
	default:
		return -EINVAL;
	}

	htmflags = H_HTM_FLAGS_NOWRAP;

	if (event->attr.freq) {
		hwc->sample_period = event->attr.sample_period;
		local64_set(&hwc->period_left, hwc->sample_period);
		hwc->last_period = hwc->sample_period;
		event->attr.freq = 0;
	}

	return 0;
}

static int htm_event_add(struct perf_event *event, int flags)
{
	int rc, ret;
	unsigned long param1 = -1, param2 = -1;
	int retries = 0;
	u64 config = event->attr.config;
	u32 htmtype, nodeindex, nodalchipindex, coreindexonchip;

	/*
	 * Invoke H_HTM call with:
	 * operation as htm configure (H_HTM_OP_CONFIGURE)
	 * last three values are unused, hence set to zero
	 */
	htmtype = config & 0xf;
	nodeindex = (config >> 4) & 0xff;
	nodalchipindex = (config >> 12) & 0xff;
	coreindexonchip = (config >> 20) & 0xff;
	do {
		rc = htm_hcall_wrapper(htmflags, nodeindex, nodalchipindex, coreindexonchip,
			htmtype, H_HTM_OP_CONFIGURE, param1, param2, 0);
		ret = htm_return_check(rc);
	} while (ret <= 0 && ++retries < 100);
	if (ret <= 0)
		return -1;

	/* Reset retries */
	retries = 0;

	/*
	 * Invoke H_HTM call with:
	 * operation as htm  start (H_HTM_OP_START)
	 * last three values are unused, hence set to zero
	 */
	do {
		rc = htm_hcall_wrapper(htmflags, nodeindex, nodalchipindex, coreindexonchip,
				htmtype, H_HTM_OP_START, 0, 0, 0);
		ret = htm_return_check(rc);
	} while (ret == -EBUSY && ++retries < 100);

	if (htm_return_check(rc) <= 0)
		return -1;

	return 0;
}

static void htm_event_del(struct perf_event *event, int flags)
{
	long rc;
	int ret;
	int retries = 0;
	u64 config = event->attr.config;
	u32 htmtype, nodeindex, nodalchipindex, coreindexonchip;

	/*
	 * Invoke H_HTM call with:
	 * operation as htm  stop (H_HTM_OP_STOP)
	 * last three values are unused, hence set to zero
	 */
	htmtype = config & 0xf;
	nodeindex = (config >> 4) & 0xff;
	nodalchipindex = (config >> 12) & 0xff;
	coreindexonchip = (config >> 20) & 0xff;
	do {
		rc = htm_hcall_wrapper(htmflags, nodeindex, nodalchipindex, coreindexonchip,
				htmtype, H_HTM_OP_STOP, 0, 0, 0);
		ret = htm_return_check(rc);
	} while (ret == -EBUSY && ++retries < 100);

	/* Reset retries */
	retries = 0;

	/*
	 * Invoke H_HTM call with:
	 * operation as htm configure (H_HTM_OP_DECONFIGURE)
	 * last three values are unused, hence set to zero
	 */
	do {
		rc = htm_hcall_wrapper(htmflags, nodeindex, nodalchipindex, coreindexonchip,
			htmtype, H_HTM_OP_DECONFIGURE, 0, 0, 0);
		ret = htm_return_check(rc);
	} while (ret <= 0 && ++retries < 100);
}

/*
 * This function definition is empty as htm_dump_sample_data
 * is used to parse and dump the HTM trace data,
 * to perf data.
 */
static void htm_event_read(struct perf_event *event)
{
	return;
}

static void htm_event_start(struct perf_event *event, int flags)
{
}

static void htm_event_stop(struct perf_event *event, int flags)
{
}

static struct pmu htm_pmu = {
	.task_ctx_nr = perf_invalid_context,

	.name = "htm",
	.attr_groups = attr_groups,
	.event_init  = htm_event_init,
	.add         = htm_event_add,
	.del         = htm_event_del,
	.read        = htm_event_read,
	.start	     = htm_event_start,
	.stop	     = htm_event_stop,
	.capabilities = PERF_PMU_CAP_NO_EXCLUDE | PERF_PMU_CAP_EXCLUSIVE,
};

static int htm_init(void)
{
	int r;

	/* This driver is intended only for L1 host. */
	if (is_kvm_guest()) {
		pr_debug("Only supported for L1 host system\n");
		return -ENODEV;
	}

	r = perf_pmu_register(&htm_pmu, htm_pmu.name, -1);
	if (r)
		return r;

	return 0;
}

device_initcall(htm_init);
