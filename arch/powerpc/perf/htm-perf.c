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

struct htm_pmu_buf {
	int     nr_pages;
	bool    snapshot;
	void    *base;
	void	**pages;
	u64	head;
	u64     size;
	int     collect_htm_trace;
	u64	trace_records;
	void    *htm_mem_buf;	/* Staging bounce area allocated node-locally */
	void	*emit_buf;	/* Stable buffer for perf raw sample emission */
	u64     mem_start;	/* Hypervisor offset state iterator tracker */
	int     collect_htm_mem;	/* State flag tracking whether memory logging is ongoing */
};

struct htm_pmu_ctx {
	struct perf_output_handle handle;
};

static DEFINE_PER_CPU(struct htm_pmu_ctx, htm_pmu_ctx);

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

#define HTM_MEM_MAX_ENTRIES	2013U
#define HTM_MEM_BUF_SIZE	(32 + HTM_MEM_MAX_ENTRIES * 32)

static int htm_collect_memory_config(struct perf_event *event,
				      struct htm_pmu_buf *aux_buf)
{
	struct perf_sample_data data;
	struct perf_raw_record raw;
	struct pt_regs regs;
	u8 *htm_mem_buf = aux_buf->htm_mem_buf;
	__be64 *num_entries;
	u64 next_start;
	u64 to_copy;
	void *emit_buf = aux_buf->emit_buf;
	long rc;
	int ret = 0;
	int retries;
	int emitted = 0;

	/*
	 * htm_mem_buf is the hcall memory buffer target reused each iteration.
	 * emit_buf is a preallocated stable buffer used for perf raw
	 * sample emission, so raw.frag.data remains valid during
	 * perf_event_overflow().
	 * Size: 32-byte header + HTM_MEM_MAX_ENTRIES * 32-byte entries.
	 */
	while (true) {
		retries = 0;
		do {
			rc = htm_hcall_wrapper(htmflags, 0, 0, 0,
					       0, H_HTM_OP_DUMP_SYSMEM_CONF,
					       virt_to_phys(aux_buf->htm_mem_buf),
					       HTM_MEM_BUF_SIZE, aux_buf->mem_start);
			ret = htm_return_check(rc);
		} while (ret == -EBUSY && ++retries < MAX_RETRIES);

		/* H_NOT_AVAILABLE or error: no more data */
		if (ret <= 0) {
			aux_buf->collect_htm_mem = 0;
			break;
		}

		/*
		 * Read next iterator value from the hcall response BEFORE
		 * emitting, but only commit it to mem_start after a
		 * successful writing of raw data/
		 */
		next_start = be64_to_cpu(*((__be64 *)(htm_mem_buf + 0x8)));

		/*
		 * The hcall was given HTM_MEM_BUF_SIZE bytes of buffer space,
		 * so num_entries is already bounded to HTM_MEM_MAX_ENTRIES.
		 * The response is a complete hcall data:
		 *   [32-byte header][num_entries * 32-byte entries]
		 * Userspace can validate and parse it directly.
		 */
		num_entries = (__be64 *)(htm_mem_buf + 0x10);
		to_copy = 32 + (be64_to_cpu(*num_entries) * 32);
		if (WARN_ON_ONCE(to_copy > HTM_MEM_BUF_SIZE)) {
			ret = -EIO;
			aux_buf->collect_htm_mem = 0;
			break;
		}

		memcpy(emit_buf, aux_buf->htm_mem_buf, to_copy);

		perf_sample_data_init(&data, 0, event->hw.last_period);
		memset(&raw, 0, sizeof(raw));
		raw.frag.data = emit_buf;
		raw.frag.size = to_copy;
		perf_sample_save_raw_data(&data, event, &raw);

		if (perf_event_overflow(event, &data, &regs)) {
			ret = 0;
			break;
		}

		/* Record written: advance next read iterator */
		aux_buf->mem_start = next_start;
		emitted = 1;

		if (!next_start) {
			aux_buf->collect_htm_mem = 0;
			break;
		}
	}

	if (emitted)
		return 1;

	return ret;
}

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

static int htm_dump_sample_data(struct perf_event *event)
{
	struct htm_pmu_ctx *htm_ctx = this_cpu_ptr(&htm_pmu_ctx);
	struct htm_target_id *target = event->pmu_private;
	struct htm_pmu_buf *aux_buf;
	struct htm_config cfg = target->cfg;
	u64 chunk_size, dump_offset, page_index, page_offset;
	u64 max_contiguous_bytes, expected_phys, scan_index, actual_phys;
	u64 hypervisor_target_phys;
	void *target_page_virt;
	int ret = 0, retries = 0;
	long rc;

	/* Start AUX transaction session framework */
	aux_buf = perf_aux_output_begin(&htm_ctx->handle, event);
	if (!aux_buf)
		return 0;

	if (!aux_buf->collect_htm_trace && !aux_buf->collect_htm_mem) {
		perf_aux_output_end(&htm_ctx->handle, 0);
		return 0;
	}

	if (target->tracing_active == HTM_TRACING_ACTIVE) {
		htm_event_stop(event, 0);
		if (target->tracing_active == HTM_TRACING_ACTIVE) {
			perf_aux_output_end(&htm_ctx->handle, 0);
			return -EAGAIN;
		}
	}

	if (!aux_buf->collect_htm_trace) {
		ret = htm_collect_memory_config(event, aux_buf);
		goto out;
	}

	/* Derive the exact target destination point directly out of active ring pointers */
	dump_offset = htm_ctx->handle.head & (aux_buf->size - 1);
	page_index = dump_offset >> PAGE_SHIFT;
	page_offset = dump_offset & (PAGE_SIZE - 1);

	/*
	 * Assess constraints regarding space remaining across the mapping
	 * context boundary
	 */
	chunk_size = htm_ctx->handle.size;
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

	if (!chunk_size) {
		aux_buf->collect_htm_trace = 0;
		perf_aux_output_end(&htm_ctx->handle, 0);
		return 0;
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
		aux_buf->trace_records++;
		perf_aux_output_end(&htm_ctx->handle, chunk_size);
		return ret;
	}

	aux_buf->collect_htm_trace = 0;
	ret = htm_collect_memory_config(event, aux_buf);
out:
	perf_aux_output_end(&htm_ctx->handle, 0);
	return ret;
}

static void htm_event_read(struct perf_event *event)
{
	int ret;

	/*
	 * Update event->count as a binary indicator:
	 * 1 if data was dumped into the
	 * AUX buffer, 0 otherwise. Actual trace record
	 * decoding is left to userspace.
	 */
	ret = htm_dump_sample_data(event);
	if (ret <= 0)
		local64_set(&event->count, 0);
	else
		local64_set(&event->count, 1);
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

	buf->htm_mem_buf = kmalloc_node(PAGE_SIZE, GFP_KERNEL, cpu_to_node(cpu));
	if (!buf->htm_mem_buf) {
		kfree(buf);
		return NULL;
	}

	buf->emit_buf = kmalloc_node(HTM_MEM_BUF_SIZE, GFP_KERNEL,
			cpu_to_node(cpu));
	if (!buf->emit_buf) {
		kfree(buf->htm_mem_buf);
		kfree(buf);
		return NULL;
	}

	buf->collect_htm_trace = 1;
	buf->collect_htm_mem = 1;
	buf->mem_start = 0;
	buf->trace_records = 0;
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

	kfree(buf->emit_buf);
	kfree(buf->htm_mem_buf);
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
