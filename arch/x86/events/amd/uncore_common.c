// SPDX-License-Identifier: GPL-2.0-only
/*
 * Common uncore PMU helpers for AMD-family x86 processors.
 */

#include <linux/cpumask.h>
#include <linux/hrtimer.h>
#include <linux/percpu.h>
#include <linux/perf_event.h>
#include <linux/slab.h>
#include <linux/smp.h>

#include <asm/msr.h>
#include <asm/perf_event.h>

#include "uncore_common.h"

/* Interval for hrtimer, defaults to 60000 milliseconds */
static unsigned int uncore_update_interval = 60 * MSEC_PER_SEC;

void uncore_common_set_update_interval(unsigned int interval)
{
	uncore_update_interval = interval;
}
EXPORT_SYMBOL_GPL(uncore_common_set_update_interval);

struct uncore_common_pmu *event_to_uncore_common_pmu(struct perf_event *event)
{
	return container_of(event->pmu, struct uncore_common_pmu, pmu);
}
EXPORT_SYMBOL_GPL(event_to_uncore_common_pmu);

static ssize_t cpumask_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	struct pmu *ptr = dev_get_drvdata(dev);
	struct uncore_common_pmu *pmu;

	pmu = container_of(ptr, struct uncore_common_pmu, pmu);

	return cpumap_print_to_pagebuf(true, buf, &pmu->active_mask);
}
static DEVICE_ATTR_RO(cpumask);

static struct attribute *uncore_common_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

struct attribute_group uncore_common_attr_group = {
	.attrs = uncore_common_attrs,
};
EXPORT_SYMBOL_GPL(uncore_common_attr_group);

static enum hrtimer_restart uncore_common_hrtimer(struct hrtimer *hrtimer)
{
	struct uncore_common_ctx *ctx;
	struct perf_event *event;
	int bit;

	ctx = container_of(hrtimer, struct uncore_common_ctx, hrtimer);

	if (!ctx->nr_active || ctx->cpu != smp_processor_id())
		return HRTIMER_NORESTART;

	for_each_set_bit(bit, ctx->active_mask, NUM_COUNTERS_MAX) {
		event = ctx->events[bit];
		event->pmu->read(event);
	}

	hrtimer_forward_now(hrtimer, ns_to_ktime(ctx->hrtimer_duration));

	return HRTIMER_RESTART;
}

void uncore_common_start_hrtimer(struct uncore_common_ctx *ctx)
{
	hrtimer_start(&ctx->hrtimer, ns_to_ktime(ctx->hrtimer_duration),
		      HRTIMER_MODE_REL_PINNED_HARD);
}
EXPORT_SYMBOL_GPL(uncore_common_start_hrtimer);

static void uncore_common_cancel_hrtimer(struct uncore_common_ctx *ctx)
{
	hrtimer_cancel(&ctx->hrtimer);
}

static void uncore_common_init_hrtimer(struct uncore_common_ctx *ctx)
{
	hrtimer_setup(&ctx->hrtimer, uncore_common_hrtimer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_HARD);
}

void uncore_common_read(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	u64 prev, new;
	s64 delta;

	/*
	 * since we do not enable counter overflow interrupts,
	 * we do not have to worry about prev_count changing on us
	 */
	prev = local64_read(&hwc->prev_count);

	/*
	 * Some uncore PMUs do not have RDPMC assignments. In such cases,
	 * read counts directly from the corresponding PERF_CTR.
	 */
	if (hwc->event_base_rdpmc < 0)
		rdmsrq(hwc->event_base, new);
	else
		new = rdpmc(hwc->event_base_rdpmc);

	local64_set(&hwc->prev_count, new);

	delta = (new << COUNTER_SHIFT) - (prev << COUNTER_SHIFT);
	delta >>= COUNTER_SHIFT;

	local64_add(delta, &event->count);
}
EXPORT_SYMBOL_GPL(uncore_common_read);

void uncore_common_start(struct perf_event *event, int flags)
{
	struct uncore_common_pmu *pmu = event_to_uncore_common_pmu(event);
	struct uncore_common_ctx *ctx = *per_cpu_ptr(pmu->ctx, event->cpu);
	struct hw_perf_event *hwc = &event->hw;

	if (!ctx->nr_active++)
		uncore_common_start_hrtimer(ctx);

	if (flags & PERF_EF_RELOAD)
		wrmsrq(hwc->event_base, (u64)local64_read(&hwc->prev_count));

	hwc->state = 0;

	__set_bit(hwc->idx, ctx->active_mask);
	wrmsrq(hwc->config_base, (hwc->config | ARCH_PERFMON_EVENTSEL_ENABLE));

	perf_event_update_userpage(event);
}
EXPORT_SYMBOL_GPL(uncore_common_start);

void uncore_common_stop(struct perf_event *event, int flags)
{
	struct uncore_common_pmu *pmu = event_to_uncore_common_pmu(event);
	struct uncore_common_ctx *ctx = *per_cpu_ptr(pmu->ctx, event->cpu);
	struct hw_perf_event *hwc = &event->hw;

	wrmsrq(hwc->config_base, hwc->config);
	hwc->state |= PERF_HES_STOPPED;

	if ((flags & PERF_EF_UPDATE) && !(hwc->state & PERF_HES_UPTODATE)) {
		event->pmu->read(event);
		hwc->state |= PERF_HES_UPTODATE;
	}

	if (!--ctx->nr_active)
		uncore_common_cancel_hrtimer(ctx);

	__clear_bit(hwc->idx, ctx->active_mask);
}
EXPORT_SYMBOL_GPL(uncore_common_stop);

int uncore_common_event_init(struct perf_event *event)
{
	struct uncore_common_pmu *pmu;
	struct uncore_common_ctx *ctx;
	struct hw_perf_event *hwc = &event->hw;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	if (event->cpu < 0)
		return -EINVAL;

	pmu = event_to_uncore_common_pmu(event);
	ctx = *per_cpu_ptr(pmu->ctx, event->cpu);
	if (!ctx)
		return -ENODEV;

	hwc->config = event->attr.config;
	hwc->idx = -1;

	event->cpu = ctx->cpu;

	return 0;
}
EXPORT_SYMBOL_GPL(uncore_common_event_init);

int uncore_common_add(struct perf_event *event, int flags)
{
	struct uncore_common_pmu *pmu = event_to_uncore_common_pmu(event);
	struct uncore_common_ctx *ctx = *per_cpu_ptr(pmu->ctx, event->cpu);
	struct hw_perf_event *hwc = &event->hw;
	int i;

	/* are we already assigned? */
	if (hwc->idx != -1 && ctx->events[hwc->idx] == event)
		goto out;

	for (i = 0; i < pmu->num_counters; i++) {
		if (ctx->events[i] == event) {
			hwc->idx = i;
			goto out;
		}
	}

	/* if not, take the first available counter */
	hwc->idx = -1;

	for (i = 0; i < pmu->num_counters; i++) {
		struct perf_event *tmp = NULL;

		if (try_cmpxchg(&ctx->events[i], &tmp, event)) {
			hwc->idx = i;
			break;
		}
	}

out:
	if (hwc->idx == -1)
		return -EBUSY;

	hwc->config_base = pmu->msr_base + (2 * hwc->idx);
	hwc->event_base = pmu->msr_base + 1 + (2 * hwc->idx);
	hwc->event_base_rdpmc = pmu->rdpmc_base + hwc->idx;
	hwc->state = PERF_HES_UPTODATE | PERF_HES_STOPPED;

	if (pmu->rdpmc_base < 0)
		hwc->event_base_rdpmc = -1;

	if (flags & PERF_EF_START)
		event->pmu->start(event, PERF_EF_RELOAD);

	return 0;
}
EXPORT_SYMBOL_GPL(uncore_common_add);

void uncore_common_del(struct perf_event *event, int flags)
{
	struct uncore_common_pmu *pmu = event_to_uncore_common_pmu(event);
	struct uncore_common_ctx *ctx = *per_cpu_ptr(pmu->ctx, event->cpu);
	struct hw_perf_event *hwc = &event->hw;
	int i;

	event->pmu->stop(event, PERF_EF_UPDATE);

	for (i = 0; i < pmu->num_counters; i++) {
		struct perf_event *tmp = event;

		if (try_cmpxchg(&ctx->events[i], &tmp, NULL))
			break;
	}

	hwc->idx = -1;
}
EXPORT_SYMBOL_GPL(uncore_common_del);

int uncore_common_ctx_init(struct uncore_common *uncore, unsigned int cpu)
{
	struct uncore_common_ctx *curr, *prev;
	struct uncore_common_pmu *pmu;
	int node, cid, gid;
	int i, j;

	if (!uncore->init_done || !uncore->num_pmus)
		return 0;

	cid = uncore_common_ctx_cid(uncore, cpu);
	gid = uncore_common_ctx_gid(uncore, cpu);

	for (i = 0; i < uncore->num_pmus; i++) {
		pmu = &uncore->pmus[i];
		*per_cpu_ptr(pmu->ctx, cpu) = NULL;
		curr = NULL;

		if (gid != pmu->group)
			continue;

		for_each_online_cpu(j) {
			if (cpu == j)
				continue;

			prev = *per_cpu_ptr(pmu->ctx, j);
			if (!prev)
				continue;

			if (cid == uncore_common_ctx_cid(uncore, j)) {
				curr = prev;
				break;
			}
		}

		if (!curr) {
			node = cpu_to_node(cpu);

			curr = kzalloc_node(sizeof(*curr), GFP_KERNEL, node);
			if (!curr)
				goto fail;

			curr->cpu = cpu;
			curr->events = kzalloc_node(sizeof(*curr->events) *
						    pmu->num_counters,
						    GFP_KERNEL, node);
			if (!curr->events) {
				kfree(curr);
				goto fail;
			}

			uncore_common_init_hrtimer(curr);
			curr->hrtimer_duration = (u64)uncore_update_interval * NSEC_PER_MSEC;

			cpumask_set_cpu(cpu, &pmu->active_mask);
		}

		curr->refcnt++;
		*per_cpu_ptr(pmu->ctx, cpu) = curr;
	}

	return 0;

fail:
	uncore_common_ctx_free(uncore, cpu);

	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(uncore_common_ctx_init);

void uncore_common_ctx_free(struct uncore_common *uncore, unsigned int cpu)
{
	struct uncore_common_pmu *pmu;
	struct uncore_common_ctx *ctx;
	int i;

	if (!uncore->init_done)
		return;

	for (i = 0; i < uncore->num_pmus; i++) {
		pmu = &uncore->pmus[i];

		if (!pmu->ctx)
			continue;

		ctx = *per_cpu_ptr(pmu->ctx, cpu);
		if (!ctx)
			continue;

		if (cpu == ctx->cpu)
			cpumask_clear_cpu(cpu, &pmu->active_mask);

		if (!--ctx->refcnt) {
			kfree(ctx->events);
			kfree(ctx);
		}

		*per_cpu_ptr(pmu->ctx, cpu) = NULL;
	}
}
EXPORT_SYMBOL_GPL(uncore_common_ctx_free);

void uncore_common_ctx_move(struct uncore_common *uncore, unsigned int cpu)
{
	struct uncore_common_ctx *curr, *next;
	struct uncore_common_pmu *pmu;
	int i, j;

	if (!uncore->init_done)
		return;

	for (i = 0; i < uncore->num_pmus; i++) {
		pmu = &uncore->pmus[i];
		if (!pmu->ctx)
			continue;

		curr = *per_cpu_ptr(pmu->ctx, cpu);
		if (!curr)
			continue;

		for_each_online_cpu(j) {
			if (cpu == j)
				continue;

			next = *per_cpu_ptr(pmu->ctx, j);
			if (!next)
				continue;

			if (curr == next) {
				perf_pmu_migrate_context(&pmu->pmu, cpu, j);
				cpumask_clear_cpu(cpu, &pmu->active_mask);
				cpumask_set_cpu(j, &pmu->active_mask);
				next->cpu = j;
				break;
			}
		}
	}
}
EXPORT_SYMBOL_GPL(uncore_common_ctx_move);

MODULE_DESCRIPTION("x86 Uncore common support");
MODULE_LICENSE("GPL");
