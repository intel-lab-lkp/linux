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

EVENT(HTM_NEST,		0x1);
EVENT(HTM_CORE,         0x2);
EVENT(HTM_LLAT,		0x3);

GENERIC_EVENT_ATTR(htm_nest, HTM_NEST);
GENERIC_EVENT_ATTR(htm_core, HTM_CORE);
GENERIC_EVENT_ATTR(htm_llat, HTM_LLAT);

PMU_FORMAT_ATTR(event, "config:0-27");
PMU_FORMAT_ATTR(htm_type, "config:0-3");
PMU_FORMAT_ATTR(nodeindex, "config:4-11");
PMU_FORMAT_ATTR(nodalchipindex, "config:12-19");
PMU_FORMAT_ATTR(coreindexonchip, "config:20-27");

static struct attribute *events_attr[] = {
	GENERIC_EVENT_PTR(HTM_NEST),
	GENERIC_EVENT_PTR(HTM_CORE),
	GENERIC_EVENT_PTR(HTM_LLAT),
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
	int configured;			/* 1 after H_HTM_OP_CONFIGURE succeeds; 0 otherwise */
	struct list_head list;
	u64 hw_buf_size;	/* hardware buffer size from H_HTM_OP_STATUS (1<<byte[0x01]), 0 if unknown */
};

/* Helper to parse the 28-bit event config into distinct fields */
static inline void parse_htm_config(u64 config, struct htm_config *cfg)
{
	cfg->htmtype = config & 0xf;
	cfg->nodeindex = (config >> 4) & 0xff;
	cfg->nodalchipindex = (config >> 12) & 0xff;
	cfg->coreindexonchip = (config >> 20) & 0xff;
}

struct htm_pmu_buf {
	int     nr_pages;
	bool    snapshot;
	void    *base;
	void	**pages;
	u64	head;
	u64     size;
	int     collect_htm_trace;
};

/*
 * Reset AUX buffer state at the start of a new trace session.
 * collect_htm_trace is cleared on any hard hcall error; without
 * this reset, restarting the event via ioctl(ENABLE) after such an
 * error would permanently drop all future trace data.
 *
 * Uses a stack-local handle to avoid aliasing with an in-progress
 * AUX transaction on the same CPU (e.g. from NMI context).
 */
static void aux_buf_reset_for_start(struct perf_event *event)
{
	struct perf_output_handle handle;
	struct htm_pmu_buf *aux_buf;

	aux_buf = perf_aux_output_begin(&handle, event);
	if (aux_buf) {
		aux_buf->collect_htm_trace = 1;
		aux_buf->head = 0;
		perf_aux_output_end(&handle, 0);
	}
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
		 * Hypervisor requests a long delay before retry.  All PMU
		 * callbacks (add, del, start, stop) are invoked in an atomic
		 * context with interrupts disabled and hardware context locks
		 * held. sleeping is not possible anywhere in the driver.
		 * Return -EAGAIN so every caller can distinguish this from
		 * transient H_BUSY and treat it as a hard failure without
		 * spinning or sleeping. See the per-callsite comments for how
		 * each caller handles it.
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
 *
 * Note: pmu->add() and pmu->del() are NOT called on every context switch
 * for HTM events.  .task_ctx_nr = perf_invalid_context means HTM events
 * can only be opened CPU-wide (perf_event_open() returns -EINVAL for any
 * task-specific open).  CPU-wide events live in cpuctx->ctx, not in
 * task->perf_event_ctxp.  perf_event_context_sched_out/in() only walks
 * task->perf_event_ctxp and returns immediately when it is NULL, so
 * pmu->add/del are never triggered by a context switch.  The
 * PERF_EF_RELOAD / PERF_EF_UPDATE guards below checks this.
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
	u8 *status_buf;
	long src;
	ssize_t sret;
	int sretries = 0;

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

	/*
	 * Reject sample types whose payload size cannot be statically bounded
	 * or whose maximum size would overflow the 16-bit
	 * perf_event_header.size limit when combined with HTM_MEM_BUF_SIZE:
	 *
	 * PERF_SAMPLE_CALLCHAIN  — (1 + nr) * 8 bytes; nr is runtime-defined,
	 *                          default up to 127, configurable to 1024+.
	 * PERF_SAMPLE_STACK_USER — up to ~65443 bytes depending on header size.
	 * PERF_SAMPLE_REGS_USER  — depends on sample_regs_user bitmask at open
	 *                          time; up to 44 regs × 8 = 360 bytes.
	 * PERF_SAMPLE_REGS_INTR  — same as REGS_USER.
	 * PERF_SAMPLE_AUX        — in-sample AUX snapshot; size is runtime.
	 *
	 * All remaining sample types (IP, TID, TIME, ADDR, PERIOD, CPU, etc.)
	 * contribute exactly one u64 each.  Their worst-case combined overhead
	 * is accounted for in HTM_MEM_BUF_SIZE (patch 4): the buffer is sized
	 * so that HTM_MEM_BUF_SIZE + max fixed overhead <= 65535.
	 */
	if (event->attr.sample_type & (PERF_SAMPLE_CALLCHAIN  |
				       PERF_SAMPLE_STACK_USER  |
				       PERF_SAMPLE_REGS_USER   |
				       PERF_SAMPLE_REGS_INTR   |
				       PERF_SAMPLE_AUX))
		return -EOPNOTSUPP;

	/*
	 * HTM is a continuous bus tracer with no counter or sample period.
	 * Frequency mode makes the core call pmu->stop/start every tick to
	 * adjust the sample period — meaningless for HTM — and would corrupt
	 * event->count semantics (the driver uses it as a record count).
	 * perf record already forces attr.freq=0 in htm_recording_options(),
	 * but reject it here to close the gap for direct perf_event_open() callers.
	 */
	if (event->attr.freq)
		return -EINVAL;

	parse_htm_config(config, &cfg);
	switch (cfg.htmtype) {
	case HTM_CORE:
	case HTM_NEST:
	case HTM_LLAT:
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

	/*
	 * Query the hardware-allocated HTM buffer size via H_HTM_OP_STATUS.
	 * The status output buffer header byte 0x01 holds
	 * CurrentNestHtmBufferSizeInPowerOf2 (for HTM_NEST) or
	 * CurrentCoreHtmBufferSizeInPowerOf2 (for HTM_CORE); both types
	 * use the same offset (0x01) and length (1 byte).
	 * hw_buf_size is used in htm_dump_sample_data() to bound dump
	 * offsets, preventing H_HTM_OP_DUMP_DATA calls past the end of the
	 * hardware buffer.  On failure hw_buf_size stays 0 and the boundary
	 * check is skipped gracefully — the contiguous-window clamp still
	 * applies.
	 */
	status_buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (status_buf) {
		do {
			src = htm_hcall_wrapper(htmflags, cfg.nodeindex,
					cfg.nodalchipindex, cfg.coreindexonchip,
					cfg.htmtype, H_HTM_OP_STATUS,
					virt_to_phys(status_buf), PAGE_SIZE, 0);
			sret = htm_return_check(src);
		} while (sret == -EBUSY && ++sretries < MAX_RETRIES);
		if (sret > 0)
			target->hw_buf_size = 1ULL << status_buf[0x01];
		kfree(status_buf);
	}

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
	 * Ignore context-switch re-enables (PERF_EF_RELOAD).  HTM is a
	 * not a frequency-mode counter PMU. PERF_EF_RELOAD is used by
	 * the core to restart a counter after unthrottling a frequency-based
	 * event; HTM has no period or frequency knob and nothing to unthrottle.
	 * In practice this path is never reached because
	 * .task_ctx_nr = perf_invalid_context prevents task-context placement,
	 * so pmu->start() is never called on context-switch-in.
	 * The guard is kept as check.
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

		if (!(flags & PERF_EF_RELOAD))
			aux_buf_reset_for_start(event);
	}
}

static void htm_event_stop(struct perf_event *event, int flags)
{
	int rc, ret, retries = 0;
	struct htm_config cfg;
	struct htm_target_id *target = event->pmu_private;

	/*
	 * Ignore context-switch-out stops (PERF_EF_UPDATE).
	 * HTM is a continuous bus tracer; stopping the hardware on
	 * every context switch would break continuous tracing, which is the
	 * entire purpose of this PMU.  PERF_EF_UPDATE is used by the core to
	 * snapshot a counter value on context-switch-out; HTM has no counter
	 * to read (data flows into the AUX buffer via pmu->read).  In
	 * practice this path is never reached because .task_ctx_nr =
	 * perf_invalid_context prevents task-context placement, so
	 * pmu->stop() is never called on context-switch-out.  The guard is
	 * kept as check.
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

	/*
	 * pmu->add() is invoked in an atomic context with interrupts disabled
	 * and hardware context locks held; sleeping is impossible.  Only retry
	 * on transient H_BUSY.  On H_LONG_BUSY_* (-EAGAIN) or any other error,
	 * the loop exits and we call perf_event_disable_inatomic() + return 0.
	 * Returning any non-zero value from pmu->add() would cause
	 * event_sched_in() to convert it to -EAGAIN and leave the event as
	 * PERF_EVENT_STATE_INACTIVE, which the mux retries on every tick,
	 * flooding the hypervisor with H_HTM_OP_CONFIGURE hcalls exactly when
	 * it has requested a long backoff delay.  perf_event_disable_inatomic()
	 * schedules a task-work callback that sets PERF_EVENT_STATE_OFF,
	 * permanently excluding the event from the mux.
	 */
	do {
		rc = htm_hcall_wrapper(htmflags, cfg.nodeindex, cfg.nodalchipindex,
				       cfg.coreindexonchip, cfg.htmtype,
				       H_HTM_OP_CONFIGURE, param1, param2, 0);
		ret = htm_return_check(rc);
	} while (ret == -EBUSY && ++retries < MAX_RETRIES);

	if (ret <= 0) {
		perf_event_disable_inatomic(event);
		return 0;
	}

	/*
	 * htm_event_init() allocated event->pmu_private (struct htm_target_id)
	 * and set event->destroy = reset_htm_active to free it on teardown.
	 * Initialise the tracing state and hw.state before calling start.
	 * Mark configured so htm_event_del() knows a matching DECONFIGURE is
	 * required.  This flag is the only gate; htm_event_del() must not
	 * call H_HTM_OP_DECONFIGURE unless this driver issued the paired
	 * H_HTM_OP_CONFIGURE — doing so would silently destroy a concurrent
	 * trace session that owns the same hardware target.
	 */
	target->configured = 1;
	target->tracing_active = HTM_TRACING_INACTIVE;
	event->hw.state = PERF_HES_STOPPED;

	/*
	 * Start tracing via the .start callback so the standard
	 * PERF_EF_START / ioctl(ENABLE) path is honoured.
	 */
	if (flags & PERF_EF_START) {
		htm_event_start(event, 0);	/* flags=0: not a context switch */
		if (target->tracing_active == HTM_TRACING_INACTIVE) {
			/*
			 * Start failed.  Attempt to deconfigure to avoid leaving
			 * the hypervisor resource permanently reserved.
			 * pmu->add() is atomic; only H_BUSY is retried.  If
			 * H_LONG_BUSY_* or another error is returned here, the
			 * resource cannot be reclaimed in this context; log the
			 * failure so it is visible in the kernel log.
			 * Call perf_event_disable_inatomic() so the event is
			 * permanently disabled (PERF_EVENT_STATE_OFF) rather
			 * than left inactive and retried by the mux.
			 */
			retries = 0;
			do {
				rc = htm_hcall_wrapper(htmflags, cfg.nodeindex,
						       cfg.nodalchipindex, cfg.coreindexonchip,
						       cfg.htmtype, H_HTM_OP_DECONFIGURE, 0, 0, 0);
				ret = htm_return_check(rc);
			} while (ret == -EBUSY && ++retries < MAX_RETRIES);
			if (ret <= 0) {
				pr_err("DECONFIGURE failed in htm event add (ret=%d) node:%u chip:%u core:%u type:%u;\n",
						ret, cfg.nodeindex, cfg.nodalchipindex, cfg.coreindexonchip, cfg.htmtype);
			}
			/*
			 * We already issued DECONFIGURE above; clear configured so
			 * htm_event_del() does not issue a second one.
			 */
			target->configured = 0;
			perf_event_disable_inatomic(event);
			return 0;
		}
	}

	return 0;
}

static void htm_event_del(struct perf_event *event, int flags)
{
	int rc, ret, retries = 0;
	struct htm_target_id *target = event->pmu_private;
	struct htm_config cfg = target->cfg;

	/*
	 * pmu->del() is called by the perf core after pmu->stop(), whether
	 * triggered by ioctl(PERF_EVENT_IOC_DISABLE) or event destruction.
	 * pmu->del() is invoked in an atomic context with IRQs disabled,
	 * sleeping is impossible.
	 *
	 * If a prior htm_event_stop() call returned with tracing_active still
	 * set to HTM_TRACING_ACTIVE (because H_LONG_BUSY_* caused an immediate
	 * exit), calling htm_event_stop() again here with flags=0 retries the
	 * H_HTM_OP_STOP hcall.  This del path is the guaranteed retry point:
	 * the perf core will always reach del after stop, so the trace is not
	 * permanently left running in the hypervisor.  Only H_BUSY is retried
	 * here; H_LONG_BUSY_* or any errors on stop is treated as a best-effort,
	 * ie the deconfigure that follows will still be attempted and error logged.
	 */
	htm_event_stop(event, 0);

	/*
	 * Only issue H_HTM_OP_DECONFIGURE if this driver successfully issued
	 * the paired H_HTM_OP_CONFIGURE.  If htm_event_add() failed before or
	 * during CONFIGURE (configured == 0), there is nothing to tear down.
	 * Issuing DECONFIGURE without a prior CONFIGURE would silently destroy
	 * a concurrent trace session that owns the same hardware target and
	 * produce spurious pr_err() noise for an expected H_STATE / H_NOT_AVAILABLE
	 * response.
	 */
	if (!target->configured)
		return;

	/*
	 * Deconfigure the hardware resource.  Only H_BUSY is retried.
	 * If H_LONG_BUSY_* or any other error is returned, the resource
	 * cannot be reclaimed in this atomic context; log the failure so it
	 * is visible in the kernel log.
	 */
	do {
		rc = htm_hcall_wrapper(htmflags, cfg.nodeindex, cfg.nodalchipindex,
				       cfg.coreindexonchip, cfg.htmtype,
				       H_HTM_OP_DECONFIGURE, 0, 0, 0);
		ret = htm_return_check(rc);
	} while (ret == -EBUSY && ++retries < MAX_RETRIES);
	if (ret <= 0) {
		pr_err("DECONFIGURE failed in htm event del (ret=%d) node:%u chip:%u core:%u type:%u;\n",
				ret, cfg.nodeindex, cfg.nodalchipindex, cfg.coreindexonchip, cfg.htmtype);
	}
	/* pmu_private freed by event->destroy = reset_htm_active */
}

static ssize_t htm_dump_sample_data(struct perf_event *event)
{
	struct perf_output_handle handle;
	struct htm_target_id *target = event->pmu_private;
	struct htm_pmu_buf *aux_buf;
	struct htm_config cfg = target->cfg;
	u64 chunk_size, dump_offset, page_index, page_offset;
	u64 max_contiguous_bytes, expected_phys, scan_index, actual_phys;
	u64 hypervisor_target_phys;
	void *target_page_virt;
	ssize_t ret = 0;
	int retries = 0;
	long rc;

	/* Start AUX transaction; use a stack-local handle to prevent
	 * NMI reentrancy from corrupting an outer transaction's handle.
	 */
	aux_buf = perf_aux_output_begin(&handle, event);
	if (!aux_buf)
		return 0;

	if (!aux_buf->collect_htm_trace) {
		/*
		 * collect_htm_trace is cleared on hcall error and reset to 1
		 * in htm_event_start() when tracing restarts.  If it is still
		 * 0 here the event was disabled due to a prior hard error;
		 * end the AUX transaction and return 0 so the drain loop stops.
		 */
		perf_aux_output_end(&handle, 0);
		return 0;
	}

	if (target->tracing_active == HTM_TRACING_ACTIVE) {
		/*
		 * First pmu->read call of the drain sequence: stop the hardware
		 * to freeze the HTM buffer before reading it.  This is a
		 * one-shot dump — the buffer is static for the entire drain;
		 * the hardware is not restarted between chunks or after the
		 * dump completes.  Restarting mid-drain would overwrite buffer
		 * content that has not yet been transferred to the AUX ring.
		 * A new perf record session issues H_HTM_OP_START afresh.
		 */
		htm_event_stop(event, 0);
		if (target->tracing_active == HTM_TRACING_ACTIVE) {
			/*
			 * H_HTM_OP_STOP failed — cannot dump a live buffer.
			 * Abort the dump; event->count = 0 stops the drain loop.
			 * The hardware is still running; htm_event_del() will
			 * call H_HTM_OP_DECONFIGURE which implicitly stops it,
			 * preventing a resource leak.
			 */
			perf_aux_output_end(&handle, 0);
			return -EIO;
		}
	}

	/* Derive the exact target destination point directly out of active ring pointers */
	dump_offset = handle.head & (aux_buf->size - 1);
	page_index = dump_offset >> PAGE_SHIFT;
	page_offset = dump_offset & (PAGE_SIZE - 1);

	/*
	 * Assess constraints regarding space remaining across the mapping
	 * context boundary.
	 * handle.size is always page-aligned: perf_aux_output_begin() computes
	 * it as the distance from the write pointer to the wakeup boundary,
	 * rounded to PAGE_SIZE.  Masking with PAGE_MASK is therefore a no-op
	 * but is kept to make the page-granularity contract explicit.
	 */
	chunk_size = handle.size;
	chunk_size &= PAGE_MASK;

	if (chunk_size > (aux_buf->size - dump_offset))
		chunk_size = aux_buf->size - dump_offset;

	/*
	 * HTM driver uses these capabilities:
	 * PERF_PMU_CAP_AUX_NO_SG | PERF_PMU_CAP_AUX_PREFER_LARGE
	 * the core perf ring-buffer allocator (rb_alloc_aux) tries to allocate
	 * physically contiguous page block. If not available, it tries to allocate
	 * largest possible contiguous block.
	 *
	 * Example: If we ask perf for 1024 pages (64MB), the kernel executes a loop
	 * inside rb_alloc_aux() to fulfill that request using the buddy allocator. it
	 * always tries to grab the largest possible contiguous block of memory it can
	 * find first, then takes the next largest, and repeats until request is
	 * completely filled. Here while writing to aux buffer, to eliminate any
	 * virtual or physical boundary overruns, check for the page boundary.
	 *
	 * Dynamically scan forward page-by-page from our active page_index to
	 * calculate the absolute boundary limit of this current physically
	 * contiguous block chunk. Prevents hypervisor macro overruns across
	 * asymmetrical fragmentation gaps.
	 */
	max_contiguous_bytes = PAGE_SIZE - page_offset;
	scan_index = page_index + 1;
	expected_phys = (u64)virt_to_phys(aux_buf->pages[page_index]) + PAGE_SIZE;

	while (scan_index < aux_buf->nr_pages && max_contiguous_bytes < chunk_size) {
		actual_phys = (u64)virt_to_phys(aux_buf->pages[scan_index]);

		if (actual_phys != expected_phys)
			break; /* Intersected a fragmentation boundary block link! */

		max_contiguous_bytes += PAGE_SIZE;
		expected_phys += PAGE_SIZE;
		scan_index++;
	}

	/* Bound transfer length tightly within the validated contiguous window */
	if (chunk_size > max_contiguous_bytes)
		chunk_size = max_contiguous_bytes;

	/*
	 * Bound the dump to the hardware-allocated buffer size reported by
	 * H_HTM_OP_STATUS (stored in target->hw_buf_size at configure time).
	 * aux_buf->head is the byte offset of the next chunk to dump; once
	 * it reaches hw_buf_size the hardware buffer is exhausted and no
	 * further H_HTM_OP_DUMP_DATA calls should be issued.
	 * hw_buf_size == 0 means the status hcall failed at configure time;
	 * skip the check and let the hcall itself signal end-of-data.
	 */
	if (target->hw_buf_size) {
		if (aux_buf->head >= target->hw_buf_size) {
			aux_buf->collect_htm_trace = 0;
			perf_aux_output_end(&handle, 0);
			return 0;
		}
		if (chunk_size > target->hw_buf_size - aux_buf->head)
			chunk_size = target->hw_buf_size - aux_buf->head;
	}

	if (!chunk_size) {
		/*
		 * No space in the AUX ring buffer right now.  Leave
		 * collect_htm_trace set. Return -ENOSPC so htm_event_read()
		 * keeps event->count non-zero, signalling to the caller that
		 * collection is still ongoing and another pmu->read pass
		 * should be attempted once the consumer has drained the buffer.
		 */
		perf_aux_output_end(&handle, 0);
		return -ENOSPC;
	}

	/*
	 * Compute the precise base target address using
	 * localized page offset rules
	 */
	target_page_virt = aux_buf->pages[page_index];
	hypervisor_target_phys = (u64)virt_to_phys(target_page_virt) + page_offset;

	do {
		/*
		 * Invoke H_HTM call with:
		 * - operation as htm dump (H_HTM_OP_DUMP_DATA)
		 * - last three values are address, size and offset
		 */
		rc = htm_hcall_wrapper(htmflags, cfg.nodeindex, cfg.nodalchipindex,
				cfg.coreindexonchip, cfg.htmtype, H_HTM_OP_DUMP_DATA,
				hypervisor_target_phys, chunk_size, aux_buf->head);
		ret = htm_return_check(rc);
	} while (ret == -EBUSY && ++retries < MAX_RETRIES);

	if (ret > 0) {
		aux_buf->head += chunk_size;
		perf_aux_output_end(&handle, chunk_size);
		/*
		 * Return the number of 128-byte HTM trace records written.
		 * Dividing here keeps htm_event_read() free of format
		 * knowledge: it can simply use the returned count directly,
		 * regardless of which data path (AUX trace or memory config)
		 * produced it.
		 */
		return (ssize_t)(chunk_size / 128);
	}

	/*
	 * Hcall failed.  All non-success paths end collection for this
	 * buffer session.
	 */
	aux_buf->collect_htm_trace = 0;
	perf_aux_output_end(&handle, 0);
	return ret;
}

static void htm_event_read(struct perf_event *event)
{
	ssize_t ret;

	/*
	 * Refuse to run from NMI context.  htm_dump_sample_data() issues
	 * hypervisor calls, mutates target->tracing_active, and writes
	 * aux_buf->head — none of which are NMI-safe.
	 *
	 * perf_event_read_local() (used by BPF helpers such as
	 * bpf_perf_event_read_value()) does not check PERF_PMU_CAP_NO_NMI,
	 * so an explicit in_nmi() guard here is required.
	 *
	 * Returning without touching event->count preserves whatever value
	 * was set by the previous pmu->read() call.  If the drain is still
	 * in progress, event->count remains non-zero and the drain loop
	 * keeps retrying.  The next scheduled pmu->read() — which will not
	 * be in NMI context — will proceed normally and complete the dump.
	 * This mirrors the stack-local handle fix: both let the NMI path
	 * return without mutating any shared state.
	 */
	if (in_nmi())
		return;

	ret = htm_dump_sample_data(event);
	/*
	 * htm_dump_sample_data() returns the record count directly
	 * (already divided by the per-format record size):
	 *   AUX trace path:   chunk_size / 128  (128-byte HTM records)
	 *   Memory cfg path:  to_copy    /  32  (32-byte entries)
	 *
	 * ret > 0:        record count written; use directly as event->count.
	 * ret == -ENOSPC: AUX buffer full, hypervisor stream intact;
	 *                 count = 1 so the drain loop keeps retrying
	 *                 once the consumer has drained the buffer.
	 * ret <= 0:       EOF, stop failed, or hard error; count = 0
	 *                 so the drain loop stops cleanly.
	 *
	 * event->count does not represent an instruction or cycle count;
	 * actual trace records are decoded in userspace by the perf tool.
	 */
	if (ret > 0)
		local64_set(&event->count, ret);
	else if (ret == -ENOSPC)
		local64_set(&event->count, 1);
	else
		local64_set(&event->count, 0);
}

/*
 * Set up pmu-private data structures for an AUX area
 * **pages contains the aux buffer allocated for this event
 * for the corresponding cpu. rb_alloc_aux uses "alloc_pages_node"
 * and returns pointer to each page address.
 * PMU capabilities: PERF_PMU_CAP_AUX_NO_SG | PERF_PMU_CAP_AUX_PREFER_LARGE
 * to try get closest possible physically contiguous page blocks.
 *
 * The aux private data structure ie, "struct htm_pmu_buf" mainly
 * saves
 * - buf->base: aux buffer base address
 * - buf->head: offset from base address where data will be written to.
 * - buf->size: Size of allocated memory
 */
static void *htm_setup_aux(struct perf_event *event, void **pages,
		int nr_pages, bool snapshot)
{
	int cpu = event->cpu;
	struct htm_pmu_buf *buf;

	if (!nr_pages)
		return NULL;

	/*
	 * Snapshot (overwrite) mode is not supported.  In overwrite mode
	 * perf_aux_output_begin() leaves handle->size = 0, which would
	 * cause htm_dump_sample_data() to return ENOSPC on every call,
	 * setting event->count = 1 and creating an infinite drain loop
	 * in userspace.  Reject it here so perf_event_open() returns
	 * EINVAL to the caller.
	 */
	if (snapshot)
		return NULL;

	if (cpu == -1)
		cpu = raw_smp_processor_id();

	buf = kzalloc_node(sizeof(*buf), GFP_KERNEL, cpu_to_node(cpu));
	if (!buf)
		return NULL;

	buf->nr_pages = nr_pages;
	buf->snapshot = snapshot;
	buf->size = (u64)nr_pages << PAGE_SHIFT;
	buf->pages = pages;

	buf->base = pages[0];
	if (!buf->base) {
		kfree(buf);
		return NULL;
	}

	buf->collect_htm_trace = 1;
	buf->head = 0;
	return buf;
}

/*
 * free pmu-private AUX data structures
 */
static void htm_free_aux(void *aux)
{
	struct htm_pmu_buf *buf = aux;

	if (!buf)
		return;

	kfree(buf);
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
	.setup_aux    = htm_setup_aux,
	.free_aux     = htm_free_aux,
	.capabilities = PERF_PMU_CAP_NO_EXCLUDE | PERF_PMU_CAP_EXCLUSIVE
			| PERF_PMU_CAP_AUX_NO_SG | PERF_PMU_CAP_AUX_PREFER_LARGE,
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
