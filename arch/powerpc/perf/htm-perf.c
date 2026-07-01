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

struct htm_pmu_buf {
	int     nr_pages;
	bool    snapshot;
	void     *base;
	u64     size;
	u64     head;
	u64     head_size;
	bool    full;
	int	htm_stopped;
	int	collect_htm_trace;
	u64	mem_head;
	void	*htm_mem_buf;
	u64	mem_start;
	int	collect_htm_mem;
};

struct htm_pmu_ctx {
	struct perf_output_handle handle;
};

static DEFINE_PER_CPU(struct htm_pmu_ctx, htm_pmu_ctx);
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

static int htm_collect_memory_config(struct perf_event *event,
					struct htm_pmu_buf *aux_buf)
{
	struct perf_sample_data data;
	struct perf_raw_record raw;
	struct pt_regs regs;
	u64 *num_entries;
	u64 to_copy = 0;
	int htm_val;
	long rc;
	int ret;
	int retries = 0;
	size_t size;
	size_t space_to_end = aux_buf->size - aux_buf->mem_head;

	/* Capture HTM system memory configuration in aux buffer */
	do {
		rc = htm_hcall_wrapper(htmflags, 0, 0, 0,
				0, H_HTM_OP_DUMP_SYSMEM_CONF, virt_to_phys(aux_buf->htm_mem_buf),
				PAGE_SIZE, aux_buf->mem_start);
		ret = htm_return_check(rc);
	} while (ret == -EBUSY && ++retries < 100);

	/* Return once there is no more data in HTM buffer */
	if (ret <= 0) {
		perf_sample_data_init(&data, 0, event->hw.last_period);
		memset(&raw, 0, sizeof(raw));
		memset(&regs, 0, sizeof(regs));

		htm_val = (aux_buf->head/((aux_buf->nr_pages * PAGE_SIZE)));
		raw.frag.data = &htm_val;
		raw.frag.size = sizeof(htm_val);

		aux_buf->collect_htm_mem = 0;
		perf_sample_save_raw_data(&data, event, &raw);
		perf_event_overflow(event, &data, &regs);
		return 0;
	}

	/*
	 * Find how much data to copy to aux buffer
	 * If hcall returned H_PARTIAL, set mem_start to
	 * indicate next offset of memory to read from
	 */
	num_entries = aux_buf->htm_mem_buf + 0x10;
	aux_buf->mem_start = be64_to_cpu(*(u64 *)(aux_buf->htm_mem_buf + 0x8));

	to_copy = 32 + (be64_to_cpu(*num_entries) * 32);

	if (to_copy <= space_to_end) {
		if ((to_copy + aux_buf->mem_head) >= ((aux_buf->nr_pages * PAGE_SIZE)/2)) {
			/*
			 * Crossing 50% threshold - flush and wrap.
			 * Write current chunk, then pad to end of buffer.
			 * This ensures next write starts at beginning with
			 * perf head also at beginning (synchronized).
			 */
			memcpy(aux_buf->base + aux_buf->mem_head, aux_buf->htm_mem_buf, to_copy);
			aux_buf->mem_head = 0;

			/*
			 * Return space_to_end to include padding.
			 * Perf will advance head to end (wrapping to 0),
			 * matching our mem_head position.
			 */
			size = space_to_end;
		} else {
			/* Normal case - chunk fits without crossing threshold */
			memcpy(aux_buf->base + aux_buf->mem_head, aux_buf->htm_mem_buf, to_copy);
			aux_buf->mem_head += to_copy;
			size = to_copy;
		}
	} else {
		return 0;
	}

	/* Return non-zero to indicate that one record is written to aux buffer */
	return size;
}

static int htm_dump_sample_data(struct perf_event *event)
{
	struct htm_pmu_ctx *htm_ctx = this_cpu_ptr(&htm_pmu_ctx);
	struct htm_pmu_buf *aux_buf;
	u64 config = event->attr.config;
	u32 htmtype, nodeindex, nodalchipindex, coreindexonchip;
	long rc;
	int ret = 0;
	int retries = 0;

	htmtype = config & 0xf;
	nodeindex = (config >> 4) & 0xff;
	nodalchipindex = (config >> 12) & 0xff;
	coreindexonchip = (config >> 20) & 0xff;

	aux_buf = perf_aux_output_begin(&htm_ctx->handle, event);
	if (!aux_buf)
		return -1;

	if (!aux_buf->collect_htm_mem && !aux_buf->collect_htm_trace) {
		perf_aux_output_end(&htm_ctx->handle, 0);
		return 0;
	}

	if (!aux_buf->htm_stopped) {
		do {
			rc = htm_hcall_wrapper(htmflags, nodeindex, nodalchipindex, coreindexonchip,
				htmtype, H_HTM_OP_STOP, 0, 0, 0);
			ret = htm_return_check(rc);
		} while (ret == -EBUSY && ++retries < 100);

		if (ret > 0) {
			/* HTM stopped trace collection */
			aux_buf->htm_stopped = 1;
		} else {
			/* Failed to stop tracing, don't proceed to trace collection */
			perf_aux_output_end(&htm_ctx->handle, 0);
			return ret;
		}
		/* Reset the retries */
		retries = 0;
	}

	/*
	 * Invoke H_HTM call with:
	 * - operation as htm dump (H_HTM_OP_DUMP_DATA)
	 * - last three values are address, size and offset
	 */
	if (aux_buf->collect_htm_trace) {
		do {
			rc = htm_hcall_wrapper(htmflags, nodeindex, nodalchipindex, coreindexonchip,
				htmtype, H_HTM_OP_DUMP_DATA, virt_to_phys(aux_buf->base),
				(aux_buf->nr_pages * PAGE_SIZE), aux_buf->head);
			ret = htm_return_check(rc);
		} while (ret == -EBUSY && ++retries < 100);

		if (ret > 0) {
			aux_buf->head += (aux_buf->nr_pages * PAGE_SIZE);
			perf_aux_output_end(&htm_ctx->handle, (aux_buf->nr_pages * PAGE_SIZE));
			return ret;
		} else {
			aux_buf->collect_htm_trace = 0;
		}
	}

	if (aux_buf->collect_htm_mem) {
		ret = htm_collect_memory_config(event, aux_buf);
		perf_aux_output_end(&htm_ctx->handle, ret);
	}

	return ret;
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
	int ret;

	if (event->state != PERF_EVENT_STATE_ACTIVE)
		return;

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
 * and returns pointer to each page address. Map these pages to
 * contiguous space using vmap and use that as base address.
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

	/* We need at least one page for this to work. */
	if (!nr_pages)
		return NULL;

	if (cpu == -1)
		cpu = raw_smp_processor_id();

	buf = kzalloc_node(sizeof(*buf), GFP_KERNEL, cpu_to_node(cpu));
	if (!buf)
		return NULL;

	buf->base = pages[0];

	if (!buf->base) {
		kfree(buf);
		return NULL;
	}

	buf->htm_mem_buf = kmalloc_node(PAGE_SIZE, GFP_KERNEL, cpu_to_node(cpu));
	if (!buf->htm_mem_buf) {
		kfree(buf);
		pr_err("Failed to allocate htm mem buf\n");
		return NULL;
	}

	buf->nr_pages = nr_pages;
	buf->snapshot = false;
	buf->size = nr_pages << PAGE_SHIFT;
	buf->head = 0;
	buf->head_size = 0;
	buf->htm_stopped = 0;
	buf->collect_htm_trace = 1;
	buf->mem_head = 0;
	buf->collect_htm_mem = 1;
	buf->mem_start = 0;
	return buf;
}

/*
 * free pmu-private AUX data structures
 */
static void htm_free_aux(void *aux)
{
	struct htm_pmu_buf *buf = aux;
	void *free_mem;

	if (!buf)
		return;

	free_mem = buf->htm_mem_buf;
	buf->htm_mem_buf = NULL;

	smp_mb();

	kfree(free_mem);
	kfree(buf);
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
	.setup_aux   = htm_setup_aux,
	.free_aux    = htm_free_aux,
	.capabilities = PERF_PMU_CAP_NO_EXCLUDE | PERF_PMU_CAP_EXCLUSIVE
			| PERF_PMU_CAP_AUX_NO_SG | PERF_PMU_CAP_AUX_PREFER_LARGE,
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
