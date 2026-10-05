// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON perf-event source
 *
 * Provides a PMU-agnostic NMI-safe overflow handler that feeds physical- and
 * virtual-address access reports into DAMON via damon_report_access().
 * The PMU is selected at probe creation time via perf_event_attr.
 */

#include <linux/cpuhotplug.h>
#include <linux/damon.h>
#include <linux/init.h>
#include <linux/perf_event.h>
#include <linux/slab.h>
#include "perf_source.h"

/* PMU event attribute for perf-event probe configuration */
struct damon_perf_event_attr {
	u32 type;
	u64 config;
	u64 config1;
	u64 config2;
	bool sample_phys_addr;
	bool sample_weight_struct;
	bool exclude_kernel;
	bool exclude_hv;
	bool freq;
	u64 sample_freq;
	u64 sample_period;
	u32 wakeup_events;
	u32 precise_ip;
};

struct damon_perf_probe_event {
	struct damon_perf_event_attr attr;
	void *priv;		/* struct damon_perf_probe_state * */
	struct hlist_node hlist_node;
	int probe_idx;		/* index into probe_hits[]; set at registration */
	struct damon_ctx *ctx;	/* owning ctx; NULLed at teardown */
};

struct damon_perf_probe_state {
	struct perf_event * __percpu *event;
};

static void damon_perf_overflow(struct perf_event *perf_event,
				struct perf_sample_data *data,
				struct pt_regs *regs)
{
	struct damon_perf_probe_event *event =
		perf_event->overflow_handler_context;
	int probe_idx;
	struct damon_ctx *ctx;
	struct damon_access_report report = {
		.size = PAGE_SIZE,
		.cpu = smp_processor_id(),
	};

	/*
	 * Teardown NULLs event->ctx (with a release barrier) before releasing
	 * the per-CPU perf events, so an in-flight overflow racing the
	 * disable/release observes the torn-down state and drops the sample
	 * instead of reporting into a freed ctx.  Pairs with the
	 * smp_store_release(&event->ctx, NULL) in damon_perf_probe_teardown().
	 */
	ctx = smp_load_acquire(&event->ctx);
	if (!ctx)
		return;
	probe_idx = event->probe_idx;
	report.probe_idx = probe_idx;
	report.ctx = ctx;	/* route to this ctx's per-ctx perf ring */

	/* probe_idx 0 is the zero-init sentinel; a valid index must be >= 1 */
	if (WARN_ONCE(probe_idx == 0,
		      "damon-perf: overflow handler called with probe_idx=0\n"))
		return;

	if (!data)
		return;

	/*
	 * Populate whichever address fields the PMU provides; which ones it
	 * provides depends on the PMU and on the sample_type requested.
	 * Gate on sample_flags rather than testing for zero: the flag is
	 * the authoritative indicator of field validity.
	 */
	if (data->sample_flags & PERF_SAMPLE_PHYS_ADDR)
		report.paddr = data->phys_addr & PAGE_MASK;
	if (data->sample_flags & PERF_SAMPLE_ADDR)
		report.vaddr = data->addr & PAGE_MASK;

	if (!report.paddr && !report.vaddr)
		return;

	if (data->sample_flags & PERF_SAMPLE_DATA_SRC)
		report.is_write = !!(data->data_src.mem_op & PERF_MEM_OP_STORE);
	report.tid = task_pid_vnr(current);
	/* global id: the drain compares it with the target task's tgid */
	report.tgid = task_tgid_nr(current);
	damon_report_access(&report);
}

static enum cpuhp_state damon_perf_cpuhp_state;

/*
 * Per-PMU exclusivity: each PMU type may be owned by at most one damon_ctx.
 * Multiple probes from the same ctx sharing a PMU type are allowed; a second
 * ctx attempting to grab a PMU type already owned returns -EBUSY.
 */
struct damon_pmu_owner {
	struct list_head node;
	u32 pmu_type;
	atomic_long_t owner_ctx;
	atomic_t refcount;
};

static LIST_HEAD(damon_pmu_owner_list);
static DEFINE_SPINLOCK(damon_pmu_owner_lock);

static void damon_perf_event_init_attr(struct damon_perf_probe_event *event,
				       struct perf_event_attr *attr)
{
	u64 stype = PERF_SAMPLE_TIME | PERF_SAMPLE_PERIOD | PERF_SAMPLE_ADDR;

	if (event->attr.sample_phys_addr)
		stype |= PERF_SAMPLE_PHYS_ADDR;
	if (event->attr.sample_weight_struct)
		stype |= PERF_SAMPLE_WEIGHT_STRUCT;
	stype |= PERF_SAMPLE_DATA_SRC;

	*attr = (struct perf_event_attr) {
		.size = sizeof(*attr),
		.type = event->attr.type,
		.config = event->attr.config,
		.config1 = event->attr.config1,
		.config2 = event->attr.config2,
		.freq = event->attr.freq,
		.sample_type = stype,
		.precise_ip = event->attr.precise_ip,
		.pinned = 1,
		/*
		 * Created disabled, and enabled by the caller once the counter
		 * is fully set up.
		 */
		.disabled = 1,
		.wakeup_events = event->attr.wakeup_events,
		.exclude_kernel = event->attr.exclude_kernel,
		.exclude_hv = event->attr.exclude_hv,
	};
	if (event->attr.freq)
		attr->sample_freq = event->attr.sample_freq;
	else
		attr->sample_period = event->attr.sample_period;
}

static int damon_perf_cpu_online(unsigned int cpu, struct hlist_node *node)
{
	struct damon_perf_probe_event *event = hlist_entry(node,
			struct damon_perf_probe_event, hlist_node);
	struct damon_perf_probe_state *perf = event->priv;
	struct perf_event_attr attr;
	struct perf_event *perf_event;

	if (!perf)
		return 0;

	damon_perf_event_init_attr(event, &attr);

	perf_event = perf_event_create_kernel_counter(&attr, cpu, NULL,
						      damon_perf_overflow,
						      event);
	if (IS_ERR(perf_event)) {
		pr_warn_ratelimited("damon-perf: cpu %u event create failed: %ld\n",
				    cpu, PTR_ERR(perf_event));
		return 0;
	}
	per_cpu(*perf->event, cpu) = perf_event;

	perf_event_enable(perf_event);
	return 0;
}

static int damon_perf_cpu_offline(unsigned int cpu, struct hlist_node *node)
{
	struct damon_perf_probe_event *event = hlist_entry(node,
			struct damon_perf_probe_event, hlist_node);
	struct damon_perf_probe_state *perf = event->priv;
	struct perf_event *perf_event;

	if (!perf)
		return 0;

	perf_event = per_cpu(*perf->event, cpu);
	if (perf_event) {
		perf_event_disable(perf_event);
		perf_event_release_kernel(perf_event);
		per_cpu(*perf->event, cpu) = NULL;
	}
	return 0;
}

/**
 * damon_perf_probe_setup - arm perf_events for a DAMON probe.
 * @ctx:   DAMON context that owns the probe.
 * @probe: the damon_probe being armed; its list position in ctx->probes
 *         determines the probe_idx stored in ring entries.
 * @event: perf event descriptor (caller fills .attr fields)
 *
 * Computes probe_idx by walking ctx->probes so the caller does not need
 * to track it externally.
 *
 * Return: 0 on success, negative errno on failure.
 */
int damon_perf_probe_setup(struct damon_ctx *ctx,
			   struct damon_probe *probe,
			   struct damon_perf_probe_event *event)
{
	struct damon_perf_probe_state *perf;
	struct damon_pmu_owner *owner, *found = NULL;
	struct damon_probe *p;
	int idx = 0;
	int err = -ENOMEM;

	/*
	 * Per-PMU exclusivity: find or create an owner slot for this PMU type.
	 * Multiple probes from the same ctx sharing a PMU type are allowed;
	 * a second ctx attempting the same PMU type returns -EBUSY.
	 */
	spin_lock(&damon_pmu_owner_lock);
	list_for_each_entry(owner, &damon_pmu_owner_list, node) {
		if (owner->pmu_type == event->attr.type) {
			long cur = atomic_long_read(&owner->owner_ctx);

			if (cur != 0L && cur != (long)ctx) {
				spin_unlock(&damon_pmu_owner_lock);
				return -EBUSY;
			}
			atomic_long_set(&owner->owner_ctx, (long)ctx);
			atomic_inc(&owner->refcount);
			found = owner;
			break;
		}
	}
	if (!found) {
		/*
		 * GFP_ATOMIC: this allocation runs while holding
		 * damon_pmu_owner_lock (a spinlock), so it must not sleep.
		 */
		owner = kzalloc_obj(*owner, GFP_ATOMIC);
		if (!owner) {
			spin_unlock(&damon_pmu_owner_lock);
			return -ENOMEM;
		}
		owner->pmu_type = event->attr.type;
		atomic_long_set(&owner->owner_ctx, (long)ctx);
		atomic_set(&owner->refcount, 1);
		list_add(&owner->node, &damon_pmu_owner_list);
		found = owner;
	}
	spin_unlock(&damon_pmu_owner_lock);

	/* Compute probe_idx by walking ctx->probes list */
	damon_for_each_probe(p, ctx) {
		if (p == probe)
			break;
		idx++;
	}
	if (idx >= DAMON_MAX_PROBES) {
		err = -ENOSPC;
		goto release_owner;
	}
	event->probe_idx = idx + 1;	/* 1-based; 0 is reserved sentinel */
	event->ctx = ctx;		/* route overflow reports to this ctx */

	/*
	 * Allocate the ctx's per-ctx perf report ring before arming any event,
	 * so the overflow handler always finds a ready ring.  Idempotent across
	 * a ctx's multiple probes.
	 */
	err = damon_ctx_alloc_perf_ring(ctx);
	if (err)
		goto release_owner;

	err = -ENOMEM;
	perf = kzalloc_obj(*perf, GFP_KERNEL);
	if (!perf)
		goto release_owner;

	perf->event = alloc_percpu(typeof(*perf->event));
	if (!perf->event)
		goto free_perf;

	event->priv = perf;
	INIT_HLIST_NODE(&event->hlist_node);

	err = cpuhp_state_add_instance(damon_perf_cpuhp_state,
				       &event->hlist_node);
	if (err)
		goto free_event;

	return 0;

free_event:
	free_percpu(perf->event);
free_perf:
	kfree(perf);
	event->priv = NULL;
release_owner:
	spin_lock(&damon_pmu_owner_lock);
	if (atomic_dec_and_test(&found->refcount)) {
		list_del(&found->node);
		spin_unlock(&damon_pmu_owner_lock);
		kfree(found);
	} else {
		spin_unlock(&damon_pmu_owner_lock);
	}
	return err;
}

/**
 * damon_perf_probe_teardown - disarm perf_events.
 * @ctx: DAMON context that owns the probe (used to release per-PMU ownership).
 * @event: perf event descriptor previously passed to damon_perf_probe_setup()
 */
void damon_perf_probe_teardown(struct damon_ctx *ctx,
			       struct damon_perf_probe_event *event)
{
	struct damon_perf_probe_state *perf = event->priv;
	struct damon_pmu_owner *owner, *tmp;

	if (!perf)
		return;

	/* Stop in-flight overflows from reporting; see damon_perf_overflow(). */
	smp_store_release(&event->ctx, NULL);
	cpuhp_state_remove_instance(damon_perf_cpuhp_state,
				    &event->hlist_node);
	free_percpu(perf->event);
	kfree(perf);
	event->priv = NULL;

	/*
	 * Release per-PMU ownership when the last probe for this
	 * ctx/PMU-type pair is torn down.
	 */
	spin_lock(&damon_pmu_owner_lock);
	list_for_each_entry_safe(owner, tmp, &damon_pmu_owner_list, node) {
		if (owner->pmu_type == event->attr.type &&
		    atomic_long_read(&owner->owner_ctx) == (long)ctx) {
			/*
			 * Free under the lock so a concurrent same-PMU teardown
			 * cannot observe and free the same owner.
			 */
			if (atomic_dec_and_test(&owner->refcount)) {
				list_del(&owner->node);
				kfree(owner);
			}
			break;
		}
	}
	spin_unlock(&damon_pmu_owner_lock);
}

static int __init damon_perf_source_init(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN,
				      "mm/damon/perf_source:online",
				      damon_perf_cpu_online,
				      damon_perf_cpu_offline);
	if (ret < 0)
		return ret;
	damon_perf_cpuhp_state = ret;
	return 0;
}

device_initcall(damon_perf_source_init);
