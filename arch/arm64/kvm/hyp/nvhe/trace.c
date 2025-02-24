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

struct hyp_rb_per_cpu {
	struct trace_buffer_meta	*meta;
	struct hyp_buffer_page		*tail_page;
	struct hyp_buffer_page		*reader_page;
	struct hyp_buffer_page		*head_page;
	struct hyp_buffer_page		*bpages;
	u32				nr_pages;
	u32				status;
	u64				last_overrun;
	u64				write_stamp;
};

#define HYP_RB_UNAVAILABLE	0
#define HYP_RB_READY		1
#define HYP_RB_WRITING		2

static struct hyp_buffer_pages_backing hyp_buffer_pages_backing;
static DEFINE_PER_CPU(struct hyp_rb_per_cpu, trace_rb);
static DEFINE_HYP_SPINLOCK(trace_rb_lock);

#define HYP_BPAGE_LINK_HEAD	1UL
#define HYP_BPAGE_LINK_MASK	~HYP_BPAGE_LINK_HEAD

static bool hyp_bpage_try_shunt_link(struct hyp_buffer_page *bpage, struct hyp_buffer_page *dst,
				     unsigned long old_flags, unsigned long flags)
{
	unsigned long *ptr = (unsigned long *)(&bpage->list.next);
	unsigned long old = (*ptr & HYP_BPAGE_LINK_MASK) | old_flags;
	unsigned long new = (unsigned long)(&dst->list) | flags;

	return cmpxchg(ptr, old, new) == old;
}

static void hyp_bpage_set_link_flag(struct hyp_buffer_page *bpage, unsigned long flag)
{
	bpage->list.next = (struct list_head *)
		(((unsigned long)bpage->list.next & HYP_BPAGE_LINK_MASK) | flag);
}

static struct hyp_buffer_page *hyp_bpage_from_link(struct list_head *list)
{
	unsigned long ptr = (unsigned long)list & HYP_BPAGE_LINK_MASK;

	return container_of((struct list_head *)ptr, struct hyp_buffer_page, list);
}

static struct hyp_buffer_page *hyp_bpage_next_page(struct hyp_buffer_page *bpage)
{
	return hyp_bpage_from_link(bpage->list.next);
}

static bool hyp_bpage_is_head(struct hyp_buffer_page *bpage)
{
	return (unsigned long)bpage->list.prev->next & HYP_BPAGE_LINK_HEAD;
}

static void hyp_bpage_reset(struct hyp_buffer_page *bpage)
{
	bpage->write = 0;
	bpage->entries = 0;

	local_set(&bpage->page->commit, 0);
}

static int hyp_bpage_init(struct hyp_buffer_page *bpage, unsigned long hva)
{
	void *hyp_va = (void *)kern_hyp_va(hva);
	int ret;

	ret = hyp_pin_shared_mem(hyp_va, hyp_va + PAGE_SIZE);
	if (ret)
		return ret;

	INIT_LIST_HEAD(&bpage->list);
	bpage->page = (struct buffer_data_page *)hyp_va;

	hyp_bpage_reset(bpage);

	return 0;
}

#define hyp_rb_meta_inc(__meta, __inc)		\
	WRITE_ONCE((__meta), (__meta + __inc))

static bool hyp_rb_loaded(struct hyp_rb_per_cpu *cpu_buffer)
{
	return !!cpu_buffer->bpages;
}

static int hyp_rb_swap_reader(struct hyp_rb_per_cpu *cpu_buffer)
{
	struct hyp_buffer_page *last, *head, *reader;
	unsigned long overrun;

	if (!hyp_rb_loaded(cpu_buffer))
		return -ENODEV;

	head = cpu_buffer->head_page;
	reader = cpu_buffer->reader_page;

	do {
		/* Run after the writer to find the head */
		while (!hyp_bpage_is_head(head))
			cpu_buffer->head_page = head = hyp_bpage_next_page(head);

		/* Connect the reader page around the header page */
		reader->list.next = head->list.next;
		reader->list.prev = head->list.prev;

		/* The last page before the head */
		last = hyp_bpage_from_link(reader->list.next);

		/* The reader page points to the new header page */
		hyp_bpage_set_link_flag(reader, HYP_BPAGE_LINK_HEAD);

		overrun = smp_load_acquire(&cpu_buffer->meta->overrun);
	} while (!hyp_bpage_try_shunt_link(last, reader, HYP_BPAGE_LINK_HEAD, 0));

	cpu_buffer->head_page = hyp_bpage_from_link(reader->list.next);
	cpu_buffer->head_page->list.prev = &reader->list;
	cpu_buffer->reader_page = head;
	cpu_buffer->meta->reader.lost_events = overrun - cpu_buffer->last_overrun;
	cpu_buffer->meta->reader.id = cpu_buffer->reader_page->id;
	cpu_buffer->last_overrun = overrun;

	return 0;
}

static struct hyp_buffer_page *hyp_rb_move_tail(struct hyp_rb_per_cpu *cpu_buffer)
{
	struct hyp_buffer_page *tail, *new_tail;

	tail = cpu_buffer->tail_page;
	new_tail = hyp_bpage_next_page(tail);

	if (hyp_bpage_try_shunt_link(tail, new_tail, HYP_BPAGE_LINK_HEAD, 0)) {
		/*
		 * Oh no! we've caught the head. There is none anymore and swap_reader will spin
		 * until we set the new one. Overrun must be written first, to make sure we report
		 * the correct number of lost events.
		 */
		hyp_rb_meta_inc(cpu_buffer->meta->overrun, new_tail->entries);
		hyp_rb_meta_inc(meta_pages_lost(cpu_buffer->meta), 1);

		smp_store_release(&new_tail->list.next,
				  (unsigned long)new_tail->list.next | HYP_BPAGE_LINK_HEAD);
	}

	hyp_bpage_reset(new_tail);
	cpu_buffer->tail_page = new_tail;

	hyp_rb_meta_inc(meta_pages_touched(cpu_buffer->meta), 1);

	return new_tail;
}

static unsigned long rb_event_size(unsigned long length)
{
	struct ring_buffer_event *event;

	return length + RB_EVNT_HDR_SIZE + sizeof(event->array[0]);
}

static struct ring_buffer_event *
rb_add_ts_extend(struct ring_buffer_event *event, u64 delta)
{
	event->type_len = RINGBUF_TYPE_TIME_EXTEND;
	event->time_delta = delta & TS_MASK;
	event->array[0] = delta >> TS_SHIFT;

	return (struct ring_buffer_event *)((unsigned long)event + 8);
}

static struct ring_buffer_event *
hyp_rb_reserve_next(struct hyp_rb_per_cpu *cpu_buffer, unsigned long length)
{
	unsigned long ts_ext_size = 0, event_size = rb_event_size(length);
	struct hyp_buffer_page *tail = cpu_buffer->tail_page;
	struct ring_buffer_event *event;
	u32 write, prev_write;
	u64 ts, time_delta;

	ts = trace_clock();

	time_delta = ts - cpu_buffer->write_stamp;

	if (test_time_stamp(time_delta))
		ts_ext_size = 8;

	prev_write = tail->write;
	write = prev_write + event_size + ts_ext_size;

	if (unlikely(write > BUF_PAGE_SIZE))
		tail = hyp_rb_move_tail(cpu_buffer);

	if (!tail->entries) {
		tail->page->time_stamp = ts;
		time_delta = 0;
		ts_ext_size = 0;
		write = event_size;
		prev_write = 0;
	}

	tail->write = write;
	tail->entries++;

	cpu_buffer->write_stamp = ts;

	event = (struct ring_buffer_event *)(tail->page->data + prev_write);
	if (ts_ext_size) {
		event = rb_add_ts_extend(event, time_delta);
		time_delta = 0;
	}

	event->type_len = 0;
	event->time_delta = time_delta;
	event->array[0] = event_size - RB_EVNT_HDR_SIZE;

	return event;
}

void *tracing_reserve_entry(unsigned long length)
{
	struct hyp_rb_per_cpu *cpu_buffer = this_cpu_ptr(&trace_rb);
	struct ring_buffer_event *rb_event;

	if (cmpxchg(&cpu_buffer->status, HYP_RB_READY, HYP_RB_WRITING) != HYP_RB_READY)
		return NULL;

	rb_event = hyp_rb_reserve_next(cpu_buffer, length);

	return &rb_event->array[1];
}

void tracing_commit_entry(void)
{
	struct hyp_rb_per_cpu *cpu_buffer = this_cpu_ptr(&trace_rb);

	local_set(&cpu_buffer->tail_page->page->commit,
		  cpu_buffer->tail_page->write);
	hyp_rb_meta_inc(cpu_buffer->meta->entries, 1);

	/*
	 * Paired with hyp_rb_disable_writing() to ensure data is
	 * written to the ring-buffer before teardown.
	 */
	smp_store_release(&cpu_buffer->status, HYP_RB_READY);
}

static void hyp_rb_disable_writing(struct hyp_rb_per_cpu *cpu_buffer)
{
	u32 prev_status;

	/* Wait for the buffer to be released */
	do {
		prev_status = cmpxchg_acquire(&cpu_buffer->status,
					      HYP_RB_READY,
					      HYP_RB_UNAVAILABLE);
	} while (prev_status == HYP_RB_WRITING);
}

static int hyp_rb_enable_writing(struct hyp_rb_per_cpu *cpu_buffer)
{
	if (!hyp_rb_loaded(cpu_buffer))
		return -ENODEV;

	cmpxchg(&cpu_buffer->status, HYP_RB_UNAVAILABLE, HYP_RB_READY);

	return 0;
}

static void hyp_rb_teardown(struct hyp_rb_per_cpu *cpu_buffer)
{
	int i;

	if (!hyp_rb_loaded(cpu_buffer))
		return;

	hyp_rb_disable_writing(cpu_buffer);

	hyp_unpin_shared_mem((void *)cpu_buffer->meta,
			     (void *)(cpu_buffer->meta) + PAGE_SIZE);

	for (i = 0; i < cpu_buffer->nr_pages; i++) {
		struct hyp_buffer_page *bpage = &cpu_buffer->bpages[i];

		if (!bpage->page)
			continue;

		hyp_unpin_shared_mem((void *)bpage->page,
				     (void *)bpage->page + PAGE_SIZE);
	}

	cpu_buffer->bpages = 0;
}

static bool hyp_rb_fits_backing(u32 nr_pages, struct hyp_buffer_page *start)
{
	unsigned long max = hyp_buffer_pages_backing.start +
			    hyp_buffer_pages_backing.size;
	struct hyp_buffer_page *end = start + nr_pages;

	return (unsigned long)end <= max;
}

static int hyp_rb_init(struct rb_page_desc *pdesc, struct hyp_buffer_page *start,
		       struct hyp_rb_per_cpu *cpu_buffer)
{
	struct hyp_buffer_page *bpage = start;
	int i, ret;

	/* At least 1 reader page and one head */
	if (pdesc->nr_page_va < 2)
		return -EINVAL;

	/* nr_page_va + 1 must fit nr_pages */
	if (pdesc->nr_page_va >= U32_MAX)
		return -EINVAL;

	if (!hyp_rb_fits_backing(pdesc->nr_page_va, start))
		return -EINVAL;

	if (hyp_rb_loaded(cpu_buffer))
		return -EBUSY;

	cpu_buffer->bpages = start;

	cpu_buffer->meta = (struct trace_buffer_meta *)kern_hyp_va(pdesc->meta_va);
	ret = hyp_pin_shared_mem((void *)cpu_buffer->meta,
				 ((void *)cpu_buffer->meta) + PAGE_SIZE);
	if (ret)
		return ret;

	memset(cpu_buffer->meta, 0, sizeof(*cpu_buffer->meta));
	cpu_buffer->meta->meta_page_size = PAGE_SIZE;
	cpu_buffer->meta->nr_subbufs = cpu_buffer->nr_pages;

	/* The reader page is not part of the ring initially */
	ret = hyp_bpage_init(bpage, pdesc->page_va[0]);
	if (ret)
		goto err;

	cpu_buffer->nr_pages = 1;

	cpu_buffer->reader_page = bpage;
	cpu_buffer->tail_page = bpage + 1;
	cpu_buffer->head_page = bpage + 1;

	for (i = 1; i < pdesc->nr_page_va; i++) {
		ret = hyp_bpage_init(++bpage, pdesc->page_va[i]);
		if (ret)
			goto err;

		bpage->list.next = &(bpage + 1)->list;
		bpage->list.prev = &(bpage - 1)->list;
		bpage->id = i;

		cpu_buffer->nr_pages = i + 1;
	}

	/* Close the ring */
	bpage->list.next = &cpu_buffer->tail_page->list;
	cpu_buffer->tail_page->list.prev = &bpage->list;

	/* The last init'ed page points to the head page */
	hyp_bpage_set_link_flag(bpage, HYP_BPAGE_LINK_HEAD);

	cpu_buffer->last_overrun = 0;

	return 0;

err:
	hyp_rb_teardown(cpu_buffer);

	return ret;
}

static int hyp_setup_bpage_backing(struct hyp_trace_desc *desc)
{
	unsigned long start = kern_hyp_va(desc->backing.start);
	size_t size = desc->backing.size;
	int ret;

	if (hyp_buffer_pages_backing.size)
		return -EBUSY;

	if (!PAGE_ALIGNED(start) || !PAGE_ALIGNED(size))
		return -EINVAL;

	ret = __pkvm_host_donate_hyp(hyp_virt_to_pfn((void *)start), size >> PAGE_SHIFT);
	if (ret)
		return ret;

	memset((void *)start, 0, size);

	hyp_buffer_pages_backing.start = start;
	hyp_buffer_pages_backing.size = size;

	return 0;
}

static void hyp_teardown_bpage_backing(void)
{
	unsigned long start = hyp_buffer_pages_backing.start;
	size_t size = hyp_buffer_pages_backing.size;

	if (!size)
		return;

	memset((void *)start, 0, size);

	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(start), size >> PAGE_SHIFT));

	hyp_buffer_pages_backing.start = 0;
	hyp_buffer_pages_backing.size = 0;
}

int __pkvm_swap_reader_tracing(unsigned int cpu)
{
	int ret = 0;

	if (cpu >= hyp_nr_cpus)
		return -EINVAL;

	hyp_spin_lock(&trace_rb_lock);
	ret = hyp_rb_swap_reader(per_cpu_ptr(&trace_rb, cpu));
	hyp_spin_unlock(&trace_rb_lock);

	return ret;
}

static void __pkvm_teardown_tracing_locked(void)
{
	int cpu;

	hyp_assert_lock_held(&trace_rb_lock);

	for (cpu = 0; cpu < hyp_nr_cpus; cpu++) {
		struct hyp_rb_per_cpu *cpu_buffer = per_cpu_ptr(&trace_rb, cpu);

		hyp_rb_teardown(cpu_buffer);
	}

	hyp_teardown_bpage_backing();
}

void __pkvm_teardown_tracing(void)
{
	hyp_spin_lock(&trace_rb_lock);
	__pkvm_teardown_tracing_locked();
	hyp_spin_unlock(&trace_rb_lock);
}

static bool rb_page_desc_fits_desc(struct rb_page_desc *pdesc,
				   unsigned long desc_end)
{
	unsigned long *end;

	/* Check we can at least read nr_pages */
	if ((unsigned long)&pdesc->nr_page_va >= desc_end)
		return false;

	end = &pdesc->page_va[pdesc->nr_page_va];

	return (unsigned long)end <= desc_end;
}

int __pkvm_load_tracing(unsigned long desc_hva, size_t desc_size)
{
	struct hyp_trace_desc *desc = (struct hyp_trace_desc *)kern_hyp_va(desc_hva);
	struct trace_page_desc *trace_pdesc = &desc->page_desc;
	struct hyp_buffer_page *bpage_backing_start;
	struct rb_page_desc *pdesc;
	int ret, cpu;

	if (!desc_size || !PAGE_ALIGNED(desc_hva) || !PAGE_ALIGNED(desc_size))
		return -EINVAL;

	ret = __pkvm_host_donate_hyp(hyp_virt_to_pfn((void *)desc),
				     desc_size >> PAGE_SHIFT);
	if (ret)
		return ret;

	hyp_spin_lock(&trace_rb_lock);

	ret = hyp_setup_bpage_backing(desc);
	if (ret)
		goto err;

	bpage_backing_start = (struct hyp_buffer_page *)hyp_buffer_pages_backing.start;

	for_each_rb_page_desc(pdesc, cpu, trace_pdesc) {
		struct hyp_rb_per_cpu *cpu_buffer;
		int cpu;

		ret = -EINVAL;
		if (!rb_page_desc_fits_desc(pdesc, desc_hva + desc_size))
			break;

		cpu = pdesc->cpu;
		if (cpu >= hyp_nr_cpus)
			break;

		cpu_buffer = per_cpu_ptr(&trace_rb, cpu);

		ret = hyp_rb_init(pdesc, bpage_backing_start, cpu_buffer);
		if (ret)
			break;

		bpage_backing_start += pdesc->nr_page_va;
	}

err:
	if (ret)
		__pkvm_teardown_tracing_locked();

	hyp_spin_unlock(&trace_rb_lock);

	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn((void *)desc),
				       desc_size >> PAGE_SHIFT));
	return ret;
}

int __pkvm_enable_tracing(bool enable)
{
	int cpu, ret = enable ? -EINVAL : 0;

	hyp_spin_lock(&trace_rb_lock);
	for (cpu = 0; cpu < hyp_nr_cpus; cpu++) {
		struct hyp_rb_per_cpu *cpu_buffer = per_cpu_ptr(&trace_rb, cpu);

		if (enable) {
			if (!hyp_rb_enable_writing(cpu_buffer))
				ret = 0;
		} else {
			hyp_rb_disable_writing(cpu_buffer);
		}

	}
	hyp_spin_unlock(&trace_rb_lock);

	return ret;
}
