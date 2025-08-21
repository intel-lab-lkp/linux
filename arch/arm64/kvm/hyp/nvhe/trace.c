// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Vincent Donnefort <vdonnefort@google.com>
 */

#include <nvhe/clock.h>
#include <nvhe/mem_protect.h>
#include <nvhe/mm.h>
#include <nvhe/trace.h>

#include <asm/percpu.h>
#include <asm/kvm_mmu.h>
#include <asm/local.h>

#include <linux/simple_ring_buffer.h>

DEFINE_PER_CPU(struct simple_rb_per_cpu, __simple_rbs);

static struct hyp_trace_buffer {
	struct simple_rb_per_cpu		*simple_rbs;
	unsigned long				bpages_backing_start;
	size_t					bpages_backing_size;
	hyp_spinlock_t				lock;
} trace_buffer = {
	.simple_rbs = &__simple_rbs,
	.lock = __HYP_SPIN_LOCK_UNLOCKED,
};

static bool hyp_trace_buffer_loaded(struct hyp_trace_buffer *trace_buffer)
{
	return trace_buffer->bpages_backing_size > 0;
}

void *tracing_reserve_entry(unsigned long length)
{
	return simple_ring_buffer_reserve(this_cpu_ptr(trace_buffer.simple_rbs), length,
					  trace_clock());
}

void tracing_commit_entry(void)
{
	simple_ring_buffer_commit(this_cpu_ptr(trace_buffer.simple_rbs));
}

static int hyp_trace_buffer_load_bpage_backing(struct hyp_trace_buffer *trace_buffer,
					       struct hyp_trace_desc *desc)
{
	unsigned long start = kern_hyp_va(desc->bpages_backing_start);
	size_t size = desc->bpages_backing_size;
	int ret;

	if (!PAGE_ALIGNED(start) || !PAGE_ALIGNED(size))
		return -EINVAL;

	ret = __pkvm_host_donate_hyp(hyp_virt_to_pfn((void *)start), size >> PAGE_SHIFT);
	if (ret)
		return ret;

	memset((void *)start, 0, size);

	trace_buffer->bpages_backing_start = start;
	trace_buffer->bpages_backing_size = size;

	return 0;
}

static void hyp_trace_buffer_unload_bpage_backing(struct hyp_trace_buffer *trace_buffer)
{
	unsigned long start = trace_buffer->bpages_backing_start;
	size_t size = trace_buffer->bpages_backing_size;

	if (!size)
		return;

	memset((void *)start, 0, size);

	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(start), size >> PAGE_SHIFT));

	trace_buffer->bpages_backing_start = 0;
	trace_buffer->bpages_backing_size = 0;
}

static void *__pin_shared_page(unsigned long kern_va)
{
	void *va = kern_hyp_va((void *)kern_va);

	return hyp_pin_shared_mem(va, va + PAGE_SIZE) ? NULL : va;
}

static void __unpin_shared_page(void *va)
{
	hyp_unpin_shared_mem(va, va + PAGE_SIZE);
}

static void hyp_trace_buffer_unload(struct hyp_trace_buffer *trace_buffer)
{
	int cpu;

	hyp_assert_lock_held(&trace_buffer->lock);

	if (!hyp_trace_buffer_loaded(trace_buffer))
		return;

	for (cpu = 0; cpu < hyp_nr_cpus; cpu++)
		__simple_ring_buffer_unload(per_cpu_ptr(trace_buffer->simple_rbs, cpu),
					    __unpin_shared_page);

	hyp_trace_buffer_unload_bpage_backing(trace_buffer);
}

static int hyp_trace_buffer_load(struct hyp_trace_buffer *trace_buffer,
				 struct hyp_trace_desc *desc)
{
	struct simple_buffer_page *bpages;
	struct ring_buffer_desc *rb_desc;
	int ret, cpu;

	hyp_assert_lock_held(&trace_buffer->lock);

	if (hyp_trace_buffer_loaded(trace_buffer))
		return -EINVAL;

	ret = hyp_trace_buffer_load_bpage_backing(trace_buffer, desc);
	if (ret)
		return ret;

	bpages = (struct simple_buffer_page *)trace_buffer->bpages_backing_start;
	for_each_ring_buffer_desc(rb_desc, cpu, &desc->trace_buffer_desc) {
		ret = __simple_ring_buffer_init(per_cpu_ptr(trace_buffer->simple_rbs, cpu),
						bpages, rb_desc, __pin_shared_page,
						__unpin_shared_page);
		if (ret)
			break;

		bpages += rb_desc->nr_page_va;
	}

	if (ret)
		hyp_trace_buffer_unload(trace_buffer);

	return ret;
}

static bool hyp_trace_desc_validate(struct hyp_trace_desc *desc, size_t desc_size)
{
	struct simple_buffer_page *bpages = (struct simple_buffer_page *)desc->bpages_backing_start;
	struct ring_buffer_desc *rb_desc;
	void *bpages_end, *desc_end;
	int cpu;

	desc_end = (void *)desc + desc_size;
	bpages_end = (void *)desc->bpages_backing_start + desc->bpages_backing_size;

	for_each_ring_buffer_desc(rb_desc, cpu, &desc->trace_buffer_desc) {
		/* Can we read nr_page_va? */
		if ((void *)(&rb_desc->nr_page_va + sizeof(rb_desc->nr_page_va)) > desc_end)
			return false;

		/* Overflow desc? */
		if ((void *)(rb_desc->page_va + rb_desc->nr_page_va + 1) > desc_end)
			return false;

		/* Overflow bpages backing memory? */
		if ((void *)(bpages + rb_desc->nr_page_va + 1) > bpages_end)
			return false;

		if (cpu >= hyp_nr_cpus)
			return false;

		bpages += rb_desc->nr_page_va;
	}

	return true;
}

int __pkvm_load_tracing(unsigned long desc_hva, size_t desc_size)
{
	struct hyp_trace_desc *desc = (struct hyp_trace_desc *)kern_hyp_va(desc_hva);
	int ret;

	if (!desc_size || !PAGE_ALIGNED(desc_hva) || !PAGE_ALIGNED(desc_size))
		return -EINVAL;

	ret = __pkvm_host_donate_hyp(hyp_virt_to_pfn((void *)desc),
				     desc_size >> PAGE_SHIFT);
	if (ret)
		return ret;

	if (!hyp_trace_desc_validate(desc, desc_size))
		goto err_donate_desc;

	hyp_spin_lock(&trace_buffer.lock);

	ret = hyp_trace_buffer_load(&trace_buffer, desc);

	hyp_spin_unlock(&trace_buffer.lock);

err_donate_desc:
	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn((void *)desc),
				       desc_size >> PAGE_SHIFT));
	return ret;
}

void __pkvm_unload_tracing(void)
{
	hyp_spin_lock(&trace_buffer.lock);
	hyp_trace_buffer_unload(&trace_buffer);
	hyp_spin_unlock(&trace_buffer.lock);
}

int __pkvm_enable_tracing(bool enable)
{
	int cpu, ret = enable ? -EINVAL : 0;

	hyp_spin_lock(&trace_buffer.lock);

	if (!hyp_trace_buffer_loaded(&trace_buffer))
		goto unlock;

	for (cpu = 0; cpu < hyp_nr_cpus; cpu++)
		simple_ring_buffer_enable_tracing(per_cpu_ptr(trace_buffer.simple_rbs, cpu),
						  enable);

	ret = 0;

unlock:
	hyp_spin_unlock(&trace_buffer.lock);

	return ret;
}

int __pkvm_swap_reader_tracing(unsigned int cpu)
{
	int ret;

	if (cpu >= hyp_nr_cpus)
		return -EINVAL;

	hyp_spin_lock(&trace_buffer.lock);

	if (hyp_trace_buffer_loaded(&trace_buffer))
		ret = simple_ring_buffer_swap_reader_page(
				per_cpu_ptr(trace_buffer.simple_rbs, cpu));
	else
		ret = -ENODEV;

	hyp_spin_unlock(&trace_buffer.lock);

	return ret;
}
