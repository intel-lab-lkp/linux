// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Vincent Donnefort <vdonnefort@google.com>
 */

#include <linux/trace_remote.h>
#include <linux/tracefs.h>
#include <linux/simple_ring_buffer.h>

#include <asm/kvm_host.h>
#include <asm/kvm_hyptrace.h>

#include "hyp_trace.h"

/* Same 10min used by clocksource when width is more than 32-bits */
#define CLOCK_MAX_CONVERSION_S	600
/*
 * Time to give for the clock init. Long enough to get a good mult/shift
 * estimation. Short enough to not delay the tracing start too much.
 */
#define CLOCK_INIT_MS		100
/*
 * Time between clock checks. Must be small enough to catch clock deviation when
 * it is still tiny.
 */
#define CLOCK_UPDATE_MS		500

static struct hyp_trace_clock {
	u64			cycles;
	u64			cyc_overflow64;
	u64			boot;
	u32			mult;
	u32			shift;
	struct delayed_work	work;
	struct completion	ready;
	struct mutex		lock;
	bool			running;
} hyp_clock;

static void __hyp_clock_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct hyp_trace_clock *hyp_clock;
	struct system_time_snapshot snap;
	u64 rate, delta_cycles;
	u64 boot, delta_boot;

	hyp_clock = container_of(dwork, struct hyp_trace_clock, work);

	ktime_get_snapshot(&snap);
	boot = ktime_to_ns(snap.boot);

	delta_boot = boot - hyp_clock->boot;
	delta_cycles = snap.cycles - hyp_clock->cycles;

	/* Compare hyp clock with the kernel boot clock */
	if (hyp_clock->mult) {
		u64 err, cur = delta_cycles;

		if (WARN_ON_ONCE(cur >= hyp_clock->cyc_overflow64)) {
			__uint128_t tmp = (__uint128_t)cur * hyp_clock->mult;

			cur = tmp >> hyp_clock->shift;
		} else {
			cur *= hyp_clock->mult;
			cur >>= hyp_clock->shift;
		}
		cur += hyp_clock->boot;

		err = abs_diff(cur, boot);
		/* No deviation, only update epoch if necessary */
		if (!err) {
			if (delta_cycles >= (hyp_clock->cyc_overflow64 >> 1))
				goto fast_forward;

			goto resched;
		}

		/* Warn if the error is above tracing precision (1us) */
		if (err > NSEC_PER_USEC)
			pr_warn_ratelimited("hyp trace clock off by %lluus\n",
					    err / NSEC_PER_USEC);
	}

	rate = div64_u64(delta_cycles * NSEC_PER_SEC, delta_boot);

	clocks_calc_mult_shift(&hyp_clock->mult, &hyp_clock->shift,
			       rate, NSEC_PER_SEC, CLOCK_MAX_CONVERSION_S);

	/* Add a comfortable 50% margin */
	hyp_clock->cyc_overflow64 = (U64_MAX / hyp_clock->mult) >> 1;

fast_forward:
	hyp_clock->cycles = snap.cycles;
	hyp_clock->boot = boot;
	kvm_call_hyp_nvhe(__pkvm_update_clock_tracing, hyp_clock->mult,
			  hyp_clock->shift, hyp_clock->boot, hyp_clock->cycles);
	complete(&hyp_clock->ready);

resched:
	schedule_delayed_work(&hyp_clock->work,
			      msecs_to_jiffies(CLOCK_UPDATE_MS));
}

static void hyp_trace_clock_enable(struct hyp_trace_clock *hyp_clock, bool enable)
{
	struct system_time_snapshot snap;

	if (hyp_clock->running == enable)
		return;

	if (!enable) {
		cancel_delayed_work_sync(&hyp_clock->work);
		hyp_clock->running = false;
	}

	ktime_get_snapshot(&snap);

	hyp_clock->boot = ktime_to_ns(snap.boot);
	hyp_clock->cycles = snap.cycles;
	hyp_clock->mult = 0;

	init_completion(&hyp_clock->ready);
	INIT_DELAYED_WORK(&hyp_clock->work, __hyp_clock_work);
	schedule_delayed_work(&hyp_clock->work, msecs_to_jiffies(CLOCK_INIT_MS));
	wait_for_completion(&hyp_clock->ready);
	hyp_clock->running = true;
}

/* Access to this struct within the trace_remote_callbacks are protected by the trace_remote lock */
struct hyp_trace_buffer {
	struct hyp_trace_desc	*desc;
	size_t			desc_size;
} trace_buffer;

static int hyp_trace_buffer_alloc_bpages_backing(struct hyp_trace_buffer *trace_buffer, size_t size)
{
	int nr_bpages = (PAGE_ALIGN(size) / PAGE_SIZE) + 1;
	size_t backing_size;
	void *start;

	backing_size = PAGE_ALIGN(sizeof(struct simple_buffer_page) * nr_bpages *
				  num_possible_cpus());

	start = alloc_pages_exact(backing_size, GFP_KERNEL_ACCOUNT);
	if (!start)
		return -ENOMEM;

	trace_buffer->desc->bpages_backing_start = (unsigned long)start;
	trace_buffer->desc->bpages_backing_size = backing_size;

	return 0;
}

static void hyp_trace_buffer_free_bpages_backing(struct hyp_trace_buffer *trace_buffer)
{
	free_pages_exact((void *)trace_buffer->desc->bpages_backing_start,
			 trace_buffer->desc->bpages_backing_size);
}

static int __load_page(unsigned long va)
{
	return kvm_call_hyp_nvhe(__pkvm_host_share_hyp, virt_to_pfn((void *)va), 1);
}

static void __unload_page(unsigned long va)
{
	WARN_ON(kvm_call_hyp_nvhe(__pkvm_host_unshare_hyp, virt_to_pfn((void *)va), 1));
}

static void hyp_trace_buffer_unload_pages(struct hyp_trace_buffer *trace_buffer, int last_cpu)
{
	struct ring_buffer_desc *rb_desc;
	int cpu, p;

	for_each_ring_buffer_desc(rb_desc, cpu, &trace_buffer->desc->trace_buffer_desc) {
		if (cpu > last_cpu)
			break;

		__unload_page(rb_desc->meta_va);
		for (p = 0; p < rb_desc->nr_page_va; p++)
			__unload_page(rb_desc->page_va[p]);
	}
}

static int hyp_trace_buffer_load_pages(struct hyp_trace_buffer *trace_buffer)
{
	struct ring_buffer_desc *rb_desc;
	int cpu, p, ret = 0;

	for_each_ring_buffer_desc(rb_desc, cpu, &trace_buffer->desc->trace_buffer_desc) {
		ret = __load_page(rb_desc->meta_va);
		if (ret)
			break;

		for (p = 0; p < rb_desc->nr_page_va; p++) {
			ret = __load_page(rb_desc->page_va[p]);
			if (ret)
				break;
		}

		if (ret) {
			for (p--; p >= 0; p--)
				__unload_page(rb_desc->page_va[p]);
			break;
		}
	}

	if (ret)
		hyp_trace_buffer_unload_pages(trace_buffer, cpu--);

	return ret;
}

static struct trace_buffer_desc *hyp_trace_load(unsigned long size, void *priv)
{
	struct hyp_trace_buffer *trace_buffer = priv;
	struct hyp_trace_desc *desc;
	size_t desc_size;
	int ret;

	if (WARN_ON(trace_buffer->desc))
		return ERR_PTR(-EINVAL);

	desc_size = trace_buffer_desc_size(size, num_possible_cpus());
	if (desc_size == SIZE_MAX)
		return ERR_PTR(-E2BIG);

	/*
	 * The hypervisor will unmap the descriptor from the host to protect the reading. Page
	 * granularity for the allocation ensures no other useful data will be unmapped.
	 */
	desc_size = PAGE_ALIGN(desc_size);
	desc = (struct hyp_trace_desc *)alloc_pages_exact(desc_size, GFP_KERNEL);
	if (!desc)
		return ERR_PTR(-ENOMEM);

	trace_buffer->desc = desc;

	ret = hyp_trace_buffer_alloc_bpages_backing(trace_buffer, size);
	if (ret)
		goto err_free_desc;

	ret = trace_remote_alloc_buffer(&desc->trace_buffer_desc, size, cpu_possible_mask);
	if (ret)
		goto err_free_backing;

	ret = hyp_trace_buffer_load_pages(trace_buffer);
	if (ret)
		goto err_free_buffer;

	ret = kvm_call_hyp_nvhe(__pkvm_load_tracing, (unsigned long)desc, desc_size);
	if (ret)
		goto err_unload_pages;

	return &desc->trace_buffer_desc;

err_unload_pages:
	hyp_trace_buffer_unload_pages(trace_buffer, INT_MAX);

err_free_buffer:
	trace_remote_free_buffer(&desc->trace_buffer_desc);

err_free_backing:
	hyp_trace_buffer_free_bpages_backing(trace_buffer);

err_free_desc:
	free_pages_exact(desc, desc_size);
	trace_buffer->desc = NULL;

	return ERR_PTR(ret);
}

static void hyp_trace_unload(struct trace_buffer_desc *desc, void *priv)
{
	struct hyp_trace_buffer *trace_buffer = priv;

	if (WARN_ON(desc != &trace_buffer->desc->trace_buffer_desc))
		return;

	kvm_call_hyp_nvhe(__pkvm_unload_tracing);
	hyp_trace_buffer_unload_pages(trace_buffer, INT_MAX);
	trace_remote_free_buffer(desc);
	hyp_trace_buffer_free_bpages_backing(trace_buffer);
	free_pages_exact(trace_buffer->desc, trace_buffer->desc_size);
	trace_buffer->desc = NULL;
}

static int hyp_trace_enable_tracing(bool enable, void *priv)
{
	hyp_trace_clock_enable(&hyp_clock, enable);

	return kvm_call_hyp_nvhe(__pkvm_enable_tracing, enable);
}

static int hyp_trace_swap_reader_page(unsigned int cpu, void *priv)
{
	return kvm_call_hyp_nvhe(__pkvm_swap_reader_tracing, cpu);
}

static int hyp_trace_reset(unsigned int cpu, void *priv)
{
	return kvm_call_hyp_nvhe(__pkvm_reset_tracing, cpu);
}

static int hyp_trace_enable_event(unsigned short id, bool enable, void *priv)
{
	return kvm_call_hyp_nvhe(__pkvm_enable_event, id, enable);
}

static int hyp_trace_clock_show(struct seq_file *m, void *v)
{
	seq_puts(m, "[boot]\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hyp_trace_clock);

#ifdef CONFIG_PKVM_SELFTESTS
static ssize_t hyp_trace_write_event_write(struct file *f, const char __user *ubuf,
					   size_t cnt, loff_t *pos)
{
	unsigned long val;
	int ret;

	ret = kstrtoul_from_user(ubuf, cnt, 10, &val);
	if (ret)
		return ret;

	ret = kvm_call_hyp_nvhe(__pkvm_write_event, val);
	if (ret)
		return ret;

	return cnt;
}

static const struct file_operations hyp_trace_write_event_fops = {
	.write	= hyp_trace_write_event_write,
};
#endif

static int hyp_trace_init_tracefs(struct dentry *d, void *priv)
{
#ifdef CONFIG_PKVM_SELFTESTS
	tracefs_create_file("write_event", 0200, d, NULL, &hyp_trace_write_event_fops);
#endif
	return tracefs_create_file("trace_clock", 0440, d, NULL, &hyp_trace_clock_fops) ?
		0 : -ENOMEM;
}

static struct trace_remote_callbacks trace_remote_callbacks = {
	.init			= hyp_trace_init_tracefs,
	.load_trace_buffer	= hyp_trace_load,
	.unload_trace_buffer	= hyp_trace_unload,
	.enable_tracing		= hyp_trace_enable_tracing,
	.swap_reader_page	= hyp_trace_swap_reader_page,
	.reset			= hyp_trace_reset,
	.enable_event		= hyp_trace_enable_event,
};

#include <asm/kvm_define_hypevents.h>

static void hyp_trace_init_events(void)
{
	struct hyp_event_id *hyp_event_id = __hyp_event_ids_start;
	struct remote_event *event = __hyp_events_start;
	int id = 0;

	/* Events on both sides hypervisor are sorted */
	for (; (unsigned long)event < (unsigned long)__hyp_events_end;
		event++, hyp_event_id++, id++)
		event->id = hyp_event_id->id = id;
}

int hyp_trace_init(void)
{
	if (!is_protected_kvm_enabled())
		return 0;

	hyp_trace_init_events();

	return trace_remote_register("hypervisor", &trace_remote_callbacks, &trace_buffer,
				     __hyp_events_start, __hyp_events_end - __hyp_events_start);
}
