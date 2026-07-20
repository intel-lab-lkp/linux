// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Perf interface to expose HTM Trace data.
 *
 * Copyright (C) 2026 Athira Rajeev, IBM Corporation
 */

#define pr_fmt(fmt) "htm: " fmt

#include <asm/dtl.h>
#include <linux/perf_event.h>
#include <asm/plpar_wrappers.h>
#include <asm/firmware.h>

#define EVENT(_name, _code)	enum{_name = _code}
#define	MAX_RETRIES	100

EVENT(HTM_CORE,		0x2);
EVENT(HTM_NEST,		0x1);

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

struct htm_config {
	u32 htmtype;
	u32 nodeindex;
	u32 nodalchipindex;
	u32 coreindexonchip;
};

/*
 * Per-event private state.  Allocated in htm_event_init(), freed via the
 * event->destroy callback (reset_htm_active()).
 *
 * cfg stores the HTM target identity parsed from event->attr.config.
 * tracing_active tracks whether H_HTM_OP_START has been successfully issued
 * for this event. It is the source of information used by
 * htm_event_start() and htm_event_stop() to make hcall decisions.
 * event->hw.state is kept in sync for the perf core only.
 */
static LIST_HEAD(htm_active_targets_list);
static DEFINE_MUTEX(htm_targets_lock);

struct htm_target_id {
	struct htm_config cfg;
	int tracing_active;		/* HTM_TRACING_ACTIVE / HTM_TRACING_INACTIVE */
	struct list_head list;
};

/* Helper to parse the 28-bit event config into distinct fields */
static inline void parse_htm_config(u64 config, struct htm_config *cfg)
{
	cfg->htmtype = config & 0xf;
	cfg->nodeindex = (config >> 4) & 0xff;
	cfg->nodalchipindex = (config >> 12) & 0xff;
	cfg->coreindexonchip = (config >> 20) & 0xff;
}

/*
 * Check the return code for H_HTM hcall.
 * Return 1 if either H_PARTIAL or H_SUCCESS is returned.
 * Return 0 if H_NOT_AVAILABLE.
 * Return exact negative error codes for expected issues.
 */
static ssize_t htm_return_check(int rc)
{
	switch (rc) {
	case H_SUCCESS:
	case H_PARTIAL:
		return 1;
	case H_NOT_AVAILABLE:
		return 0;
	case H_BUSY:
		/* Transient busy: retry loop will spin up to MAX_RETRIES */
		return -EBUSY;
	case H_LONG_BUSY_ORDER_1_MSEC:
	case H_LONG_BUSY_ORDER_10_MSEC:
	case H_LONG_BUSY_ORDER_100_MSEC:
	case H_LONG_BUSY_ORDER_1_SEC:
	case H_LONG_BUSY_ORDER_10_SEC:
	case H_LONG_BUSY_ORDER_100_SEC:
		/*
		 * Hypervisor requests a long delay before retry (1ms-100s).
		 * Spinning in a kernel retry loop for this duration risks
		 * deadlocks in atomic contexts.  Return -EAGAIN so callers
		 * exit immediately; the operation will fail and userspace
		 * can retry the perf_event_open() call.
		 * Note: all retry loops check ret == -EBUSY only, so
		 * -EAGAIN exits without retrying, this is intentional.
		 */
		return -EAGAIN;
	case H_PARAMETER:
	case H_P2:
	case H_P3:
	case H_P4:
	case H_P5:
	case H_P6:
		return -EINVAL;
	case H_STATE:
		return -EIO;
	case H_AUTHORITY:
		return -EPERM;
	default:
		/* Prevent silent fallthrough mapping of unhandled errors to 1 */
		return -EIO;
	}
}

/*
 * HTM_TRACING_ACTIVE/INACTIVE: values for htm_target_id.tracing_active.
 * Tracks whether H_HTM_OP_START has been successfully issued.
 * HTM traces at node/chip/core scope, not per-task. Once started,
 * context-switch-triggered stop/start (PERF_EF_UPDATE / PERF_EF_RELOAD)
 * must not stop/restart the hardware. Only explicit API calls
 * (ioctl DISABLE/ENABLE) and event_del should control the hcall.
 */
#define HTM_TRACING_ACTIVE	1
#define HTM_TRACING_INACTIVE	0

static void reset_htm_active(struct perf_event *event)
{
	struct htm_target_id *target = event->pmu_private;

	if (!target)
		return;

	mutex_lock(&htm_targets_lock);
	if (!list_empty(&target->list))
		list_del(&target->list);
	mutex_unlock(&htm_targets_lock);

	kfree(target);
	event->pmu_private = NULL;
}

static int htm_event_init(struct perf_event *event)
{
	u64 config = event->attr.config;
	struct htm_config cfg;
	struct htm_target_id *target, *tmp;

	if (event->attr.inherit)
		return -EOPNOTSUPP;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	if (!perfmon_capable())
		return -EACCES;

	if (!is_sampling_event(event))
		return -EOPNOTSUPP;

	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	parse_htm_config(config, &cfg);
	switch (cfg.htmtype) {
	case HTM_CORE:
	case HTM_NEST:
		break;
	default:
		return -EINVAL;
	}

	/* Allocate per-event private state; freed via event->destroy */
	target = kzalloc(sizeof(*target), GFP_KERNEL);
	if (!target)
		return -ENOMEM;

	target->cfg = cfg;
	target->tracing_active = HTM_TRACING_INACTIVE;
	INIT_LIST_HEAD(&target->list);

	mutex_lock(&htm_targets_lock);
	list_for_each_entry(tmp, &htm_active_targets_list, list) {
		if (tmp->cfg.htmtype == cfg.htmtype &&
			tmp->cfg.nodeindex == cfg.nodeindex &&
			tmp->cfg.nodalchipindex == cfg.nodalchipindex &&
			tmp->cfg.coreindexonchip == cfg.coreindexonchip) {
			mutex_unlock(&htm_targets_lock);
			kfree(target);
			return -EBUSY;
		}
	}

	list_add_tail(&target->list, &htm_active_targets_list);
	mutex_unlock(&htm_targets_lock);

	event->pmu_private = target;
	event->destroy = reset_htm_active;
	return 0;
}

static void htm_event_start(struct perf_event *event, int flags)
{
	int rc, ret, retries = 0;
	struct htm_config cfg;
	struct htm_target_id *target = event->pmu_private;

	/*
	 * Ignore context-switch re-enables. The perf core passes
	 * PERF_EF_RELOAD on context-switch-in. HTM tracing is
	 * continuous at hardware level, no hcall needed.
	 */
	if (flags & PERF_EF_RELOAD)
		return;

	/* Already tracing, don't issue a second start hcall */
	if (target->tracing_active == HTM_TRACING_ACTIVE)
		return;

	cfg = target->cfg;

	/* Only retry on transient H_BUSY; H_LONG_BUSY_* (-EAGAIN) exits immediately */
	do {
		rc = htm_hcall_wrapper(htmflags, cfg.nodeindex, cfg.nodalchipindex,
				       cfg.coreindexonchip, cfg.htmtype,
				       H_HTM_OP_START, 0, 0, 0);
		ret = htm_return_check(rc);
	} while (ret == -EBUSY && ++retries < MAX_RETRIES);

	if (ret > 0) {
		target->tracing_active = HTM_TRACING_ACTIVE;
		event->hw.state &= ~PERF_HES_STOPPED;
	}
}

static void htm_event_stop(struct perf_event *event, int flags)
{
	int rc, ret, retries = 0;
	struct htm_config cfg;
	struct htm_target_id *target = event->pmu_private;

	/*
	 * Ignore context-switch-out stops. The perf core passes
	 * PERF_EF_UPDATE on context-switch-out. Do not stop the hcall.
	 */
	if (flags & PERF_EF_UPDATE)
		return;

	/* Not tracing, nothing to stop */
	if (target->tracing_active == HTM_TRACING_INACTIVE)
		return;

	cfg = target->cfg;

	/* Only retry on transient H_BUSY; H_LONG_BUSY_* (-EAGAIN) exits immediately */
	do {
		rc = htm_hcall_wrapper(htmflags, cfg.nodeindex, cfg.nodalchipindex,
				       cfg.coreindexonchip, cfg.htmtype,
				       H_HTM_OP_STOP, 0, 0, 0);
		ret = htm_return_check(rc);
	} while (ret == -EBUSY && ++retries < MAX_RETRIES);

	/*
	 * Only mark stopped if the hcall succeeded.  If the stop failed
	 * (e.g. -EAGAIN on long-busy), leave tracing_active as ACTIVE so
	 * that htm_event_del will retry the stop hcall rather than
	 * skipping it and leaving the hypervisor permanently configured.
	 */
	if (ret > 0) {
		target->tracing_active = HTM_TRACING_INACTIVE;
		event->hw.state |= PERF_HES_STOPPED;
	}
}

static int htm_event_add(struct perf_event *event, int flags)
{
	int rc, ret, retries = 0;
	unsigned long param1 = -1, param2 = -1;
	struct htm_target_id *target = event->pmu_private;
	struct htm_config cfg = target->cfg;

	/* Configure the hardware, only retry on transient H_BUSY */
	do {
		rc = htm_hcall_wrapper(htmflags, cfg.nodeindex, cfg.nodalchipindex,
				       cfg.coreindexonchip, cfg.htmtype,
				       H_HTM_OP_CONFIGURE, param1, param2, 0);
		ret = htm_return_check(rc);
	} while (ret == -EBUSY && ++retries < MAX_RETRIES);

	if (ret <= 0)
		return (ret == 0) ? -ENODEV : ret;

	/*
	 * htm_event_init() allocated event->pmu_private (struct htm_target_id)
	 * and set event->destroy = reset_htm_active to free it on teardown.
	 * Initialise the tracing state and hw.state before calling start.
	 */
	target->tracing_active = HTM_TRACING_INACTIVE;
	event->hw.state = PERF_HES_STOPPED;

	/*
	 * Start tracing via the .start callback so the standard
	 * PERF_EF_START / ioctl(ENABLE) path is honoured.
	 */
	if (flags & PERF_EF_START) {
		htm_event_start(event, 0);	/* flags=0: not a context switch */
		if (target->tracing_active == HTM_TRACING_INACTIVE) {
			/* Start failed, deconfigure to avoid resource leak */
			retries = 0;
			do {
				rc = htm_hcall_wrapper(htmflags, cfg.nodeindex,
						       cfg.nodalchipindex, cfg.coreindexonchip,
						       cfg.htmtype, H_HTM_OP_DECONFIGURE, 0, 0, 0);
				ret = htm_return_check(rc);
			} while (ret == -EBUSY && ++retries < MAX_RETRIES);
			return -EIO;
		}
	}

	return 0;
}

static void htm_event_del(struct perf_event *event, int flags)
{
	int rc, ret, retries = 0;
	struct htm_target_id *target = event->pmu_private;
	struct htm_config cfg = target->cfg;

	/* Stop tracing, flags=0 so htm_event_stop issues the hcall */
	htm_event_stop(event, 0);

	/* Deconfigure */
	do {
		rc = htm_hcall_wrapper(htmflags, cfg.nodeindex, cfg.nodalchipindex,
				       cfg.coreindexonchip, cfg.htmtype,
				       H_HTM_OP_DECONFIGURE, 0, 0, 0);
		ret = htm_return_check(rc);
	} while (ret == -EBUSY && ++retries < MAX_RETRIES);

	/* pmu_private freed by event->destroy = reset_htm_active */
}

static void htm_event_read(struct perf_event *event)
{
}

static struct pmu htm_pmu = {
	.task_ctx_nr  = perf_invalid_context,
	.name         = "htm",
	.attr_groups  = attr_groups,
	.event_init   = htm_event_init,
	.add          = htm_event_add,
	.del          = htm_event_del,
	.read         = htm_event_read,
	.start        = htm_event_start,
	.stop         = htm_event_stop,
	.capabilities = PERF_PMU_CAP_NO_EXCLUDE | PERF_PMU_CAP_EXCLUSIVE,
};

static int htm_init(void)
{
	int r;

	if (!firmware_has_feature(FW_FEATURE_LPAR)) {
		pr_debug("Only supported on LPAR platforms running under a Hypervisor\n");
		return -ENODEV;
	}

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
