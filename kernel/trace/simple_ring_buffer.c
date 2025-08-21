// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 - Google LLC
 * Author: Vincent Donnefort <vdonnefort@google.com>
 */

#include <linux/atomic.h>
#include <linux/simple_ring_buffer.h>

#include <asm/barrier.h>
#include <asm/local.h>

#define SIMPLE_RB_LINK_HEAD	1UL
#define SIMPLE_RB_LINK_MASK	~SIMPLE_RB_LINK_HEAD

static void simple_bpage_set_head_link(struct simple_buffer_page *bpage)
{
	unsigned long link = (unsigned long)bpage->list.next;

	link &= SIMPLE_RB_LINK_MASK;
	link |= SIMPLE_RB_LINK_HEAD;

	/*
	 * Paired with simple_bpage_is_head() to order access between the head link and overrun. It
	 * ensures we always report an up-to-date value after swapping the reader page.
	 */
	smp_store_release(&bpage->list.next, (struct list_head *)link);
}

static bool simple_bpage_is_head(struct simple_buffer_page *bpage)
{
	unsigned long link = (unsigned long)smp_load_acquire(&bpage->list.prev->next);

	return link & SIMPLE_RB_LINK_HEAD;
}

static bool simple_bpage_unset_head_link(struct simple_buffer_page *bpage,
					 struct simple_buffer_page *dst)
{
	unsigned long *link = (unsigned long *)(&bpage->list.next);
	unsigned long old = (*link & SIMPLE_RB_LINK_MASK) | SIMPLE_RB_LINK_HEAD;
	unsigned long new = (unsigned long)(&dst->list);

	return try_cmpxchg(link, &old, new);
}

static struct simple_buffer_page *simple_bpage_from_link(struct list_head *list)
{
	unsigned long ptr = (unsigned long)list & SIMPLE_RB_LINK_MASK;

	return container_of((struct list_head *)ptr, struct simple_buffer_page, list);
}

static struct simple_buffer_page *simple_bpage_next_page(struct simple_buffer_page *bpage)
{
	return simple_bpage_from_link(bpage->list.next);
}

static void simple_bpage_reset(struct simple_buffer_page *bpage)
{
	bpage->write = 0;
	bpage->entries = 0;

	local_set(&bpage->page->commit, 0);
}

static void simple_bpage_init(struct simple_buffer_page *bpage, unsigned long page)
{
	INIT_LIST_HEAD(&bpage->list);
	bpage->page = (struct buffer_data_page *)page;

	simple_bpage_reset(bpage);
}

#define simple_rb_meta_inc(__meta, __inc)		\
	WRITE_ONCE((__meta), (__meta + __inc))

static bool simple_rb_loaded(struct simple_rb_per_cpu *cpu_buffer)
{
	return !!cpu_buffer->bpages;
}

int simple_ring_buffer_swap_reader_page(struct simple_rb_per_cpu *cpu_buffer)
{
	struct simple_buffer_page *last, *head, *reader;
	unsigned long overrun;

	if (!simple_rb_loaded(cpu_buffer))
		return -ENODEV;

	head = cpu_buffer->head_page;
	reader = cpu_buffer->reader_page;

	do {
		/* Run after the writer to find the head */
		if (!simple_bpage_is_head(head))
			head = simple_bpage_next_page(head);

		/* Connect the reader page around the header page */
		reader->list.next = head->list.next;
		reader->list.prev = head->list.prev;

		/* The last page before the head */
		last = simple_bpage_from_link(head->list.prev);

		/* The reader page points to the new header page */
		simple_bpage_set_head_link(reader);

		overrun = cpu_buffer->meta->overrun;
	} while (!simple_bpage_unset_head_link(last, reader));

	cpu_buffer->head_page = simple_bpage_from_link(reader->list.next);
	cpu_buffer->head_page->list.prev = &reader->list;
	cpu_buffer->reader_page = head;
	cpu_buffer->meta->reader.lost_events = overrun - cpu_buffer->last_overrun;
	cpu_buffer->meta->reader.id = cpu_buffer->reader_page->id;
	cpu_buffer->last_overrun = overrun;

	return 0;
}

static struct simple_buffer_page *simple_rb_move_tail(struct simple_rb_per_cpu *cpu_buffer)
{
	struct simple_buffer_page *tail, *new_tail;

	tail = cpu_buffer->tail_page;
	new_tail = simple_bpage_next_page(tail);

	if (simple_bpage_unset_head_link(tail, new_tail)) {
		/*
		 * Oh no! we've caught the head. There is none anymore and swap_reader will spin
		 * until we set the new one. Overrun must be written first, to make sure we report
		 * the correct number of lost events.
		 */
		simple_rb_meta_inc(cpu_buffer->meta->overrun, new_tail->entries);
		simple_rb_meta_inc(cpu_buffer->meta->pages_lost, 1);

		simple_bpage_set_head_link(new_tail);
	}

	simple_bpage_reset(new_tail);
	cpu_buffer->tail_page = new_tail;

	simple_rb_meta_inc(cpu_buffer->meta->pages_touched, 1);

	return new_tail;
}

static unsigned long rb_event_size(unsigned long length)
{
	struct ring_buffer_event *event;

	return length + RB_EVNT_HDR_SIZE + sizeof(event->array[0]);
}

static struct ring_buffer_event *
rb_event_add_ts_extend(struct ring_buffer_event *event, u64 delta)
{
	event->type_len = RINGBUF_TYPE_TIME_EXTEND;
	event->time_delta = delta & TS_MASK;
	event->array[0] = delta >> TS_SHIFT;

	return (struct ring_buffer_event *)((unsigned long)event + 8);
}

static struct ring_buffer_event *
simple_rb_reserve_next(struct simple_rb_per_cpu *cpu_buffer, unsigned long length, u64 timestamp)
{
	unsigned long ts_ext_size = 0, event_size = rb_event_size(length);
	struct simple_buffer_page *tail = cpu_buffer->tail_page;
	struct ring_buffer_event *event;
	u32 write, prev_write;
	u64 time_delta;

	time_delta = timestamp - cpu_buffer->write_stamp;

	if (test_time_stamp(time_delta))
		ts_ext_size = 8;

	prev_write = tail->write;
	write = prev_write + event_size + ts_ext_size;

	if (unlikely(write > (PAGE_SIZE - BUF_PAGE_HDR_SIZE)))
		tail = simple_rb_move_tail(cpu_buffer);

	if (!tail->entries) {
		tail->page->time_stamp = timestamp;
		time_delta = 0;
		ts_ext_size = 0;
		write = event_size;
		prev_write = 0;
	}

	tail->write = write;
	tail->entries++;

	cpu_buffer->write_stamp = timestamp;

	event = (struct ring_buffer_event *)(tail->page->data + prev_write);
	if (ts_ext_size) {
		event = rb_event_add_ts_extend(event, time_delta);
		time_delta = 0;
	}

	event->type_len = 0;
	event->time_delta = time_delta;
	event->array[0] = event_size - RB_EVNT_HDR_SIZE;

	return event;
}

void *simple_ring_buffer_reserve(struct simple_rb_per_cpu *cpu_buffer, unsigned long length,
				 u64 timestamp)
{
	struct ring_buffer_event *rb_event;

	if (cmpxchg(&cpu_buffer->status, SIMPLE_RB_READY, SIMPLE_RB_WRITING) != SIMPLE_RB_READY)
		return NULL;

	rb_event = simple_rb_reserve_next(cpu_buffer, length, timestamp);

	return &rb_event->array[1];
}

void simple_ring_buffer_commit(struct simple_rb_per_cpu *cpu_buffer)
{
	local_set(&cpu_buffer->tail_page->page->commit,
		  cpu_buffer->tail_page->write);
	simple_rb_meta_inc(cpu_buffer->meta->entries, 1);

	/*
	 * Paired with simple_rb_enable_tracing() to ensure data is
	 * written to the ring-buffer before teardown.
	 */
	smp_store_release(&cpu_buffer->status, SIMPLE_RB_READY);
}

static u32 simple_rb_enable_tracing(struct simple_rb_per_cpu *cpu_buffer, bool enable)
{
	u32 prev_status;

	if (enable)
		return cmpxchg(&cpu_buffer->status, SIMPLE_RB_UNAVAILABLE, SIMPLE_RB_READY);

	/* Wait for the buffer to be released */
	do {
		prev_status = cmpxchg_acquire(&cpu_buffer->status,
					      SIMPLE_RB_READY,
					      SIMPLE_RB_UNAVAILABLE);
	} while (prev_status == SIMPLE_RB_WRITING);

	return prev_status;
}

int simple_ring_buffer_reset(struct simple_rb_per_cpu *cpu_buffer)
{
	struct simple_buffer_page *bpage;
	u32 prev_status;

	if (!simple_rb_loaded(cpu_buffer))
		return -ENODEV;

	prev_status = simple_rb_enable_tracing(cpu_buffer, false);

	while (!simple_bpage_is_head(cpu_buffer->head_page))
		cpu_buffer->head_page = simple_bpage_next_page(cpu_buffer->head_page);

	bpage = cpu_buffer->tail_page = cpu_buffer->head_page;
	do {
		simple_bpage_reset(bpage);
		bpage = simple_bpage_next_page(bpage);
	} while (bpage != cpu_buffer->head_page);

	simple_bpage_reset(cpu_buffer->reader_page);

	cpu_buffer->last_overrun = 0;
	cpu_buffer->write_stamp = 0;

	cpu_buffer->meta->reader.read = 0;
	cpu_buffer->meta->reader.lost_events = 0;
	cpu_buffer->meta->entries = 0;
	cpu_buffer->meta->overrun = 0;
	cpu_buffer->meta->read = 0;
	cpu_buffer->meta->pages_lost = 0;
	cpu_buffer->meta->pages_touched = 0;

	if (prev_status == SIMPLE_RB_READY)
		simple_rb_enable_tracing(cpu_buffer, true);

	return 0;
}

int simple_ring_buffer_init(struct simple_rb_per_cpu *cpu_buffer, struct simple_buffer_page *bpages,
			    const struct ring_buffer_desc *desc)
{
	struct simple_buffer_page *bpage = bpages;
	int i;

	/* At least 1 reader page and one head */
	if (desc->nr_page_va < 2)
		return -EINVAL;

	memset(cpu_buffer, 0, sizeof(*cpu_buffer));

	cpu_buffer->bpages = bpages;

	cpu_buffer->meta = (void *)desc->meta_va;
	memset(cpu_buffer->meta, 0, sizeof(*cpu_buffer->meta));
	cpu_buffer->meta->meta_page_size = PAGE_SIZE;
	cpu_buffer->meta->nr_subbufs = cpu_buffer->nr_pages;

	/* The reader page is not part of the ring initially */
	simple_bpage_init(bpage, desc->page_va[0]);
	bpage->id = 0;

	cpu_buffer->nr_pages = 1;

	cpu_buffer->reader_page = bpage;
	cpu_buffer->tail_page = bpage + 1;
	cpu_buffer->head_page = bpage + 1;

	for (i = 1; i < desc->nr_page_va; i++) {
		simple_bpage_init(++bpage, desc->page_va[i]);

		bpage->list.next = &(bpage + 1)->list;
		bpage->list.prev = &(bpage - 1)->list;
		bpage->id = i;

		cpu_buffer->nr_pages = i + 1;
	}

	/* Close the ring */
	bpage->list.next = &cpu_buffer->tail_page->list;
	cpu_buffer->tail_page->list.prev = &bpage->list;

	/* The last init'ed page points to the head page */
	simple_bpage_set_head_link(bpage);

	return 0;
}

void simple_ring_buffer_unload(struct simple_rb_per_cpu *cpu_buffer)
{
	if (!simple_rb_loaded(cpu_buffer))
		return;

	simple_rb_enable_tracing(cpu_buffer, false);

	cpu_buffer->bpages = 0;
}

int simple_ring_buffer_enable_tracing(struct simple_rb_per_cpu *cpu_buffer, bool enable)
{
	if (!simple_rb_loaded(cpu_buffer))
		return -ENODEV;

	simple_rb_enable_tracing(cpu_buffer, enable);

	return 0;
}
