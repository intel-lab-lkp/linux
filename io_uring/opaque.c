// SPDX-License-Identifier: GPL-2.0
/* Kernel-resident immutable TCP payloads. */
#include <linux/bitmap.h>
#include <linux/bvec.h>
#include <linux/file.h>
#include <linux/highmem.h>
#include <linux/memcontrol.h>
#include <linux/overflow.h>
#include <linux/rbtree.h>
#include <linux/sched/mm.h>
#include <linux/skbuff.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <net/tcp.h>
#include <uapi/linux/io_uring/opaque.h>

#include "io_uring.h"
#include "cancel.h"
#include "opaque_internal.h"
#include "rsrc.h"
#include "tw.h"

static bool io_opaque_budget_ready(struct io_opaque_req *op, unsigned long now)
{
	struct io_opaque_store *store = op->store;
	u64 limit = io_opaque_rx_limit(store);
	u64 used = atomic64_read(&store->bytes);

	lockdep_assert_held(&store->wait_lock);
	return !time_before(now, op->retry_at) && used <= limit &&
	       op->need_bytes <= limit - used &&
	       atomic_read(&store->extents) < store->config.max_extents - 2;
}

/*
 * Let in the waiters for budget that the budget lets go on, at most
 * IO_OPAQUE_WAKE_BATCH of them in a hold of wait_lock. Those left over are for
 * the retry work, and that is why this says whether there were any: a caller
 * that wants them in now repeats it. The work comes a jiffy later, or as late
 * as a timer that is already pending, which queueing never moves earlier.
 */
static bool io_opaque_wake_budget(struct io_opaque_store *store)
{
	struct io_opaque_req *op, *next;
	unsigned long delay = MAX_JIFFY_OFFSET;
	unsigned long now = jiffies;
	unsigned int woken = 0;
	bool stopped, more = false;

	guard(spinlock)(&store->wait_lock);
	stopped = READ_ONCE(store->dead);
	list_for_each_entry_safe(op, next, &store->budget_waits, wait) {
		struct socket *sock = sock_from_file(op->req->file);

		if (!stopped && !READ_ONCE(op->stream->closed)) {
			if (time_before(now, op->retry_at)) {
				delay = min(delay, op->retry_at - now);
				continue;
			}
			if (!io_opaque_budget_ready(op, now))
				continue;
			if (woken == IO_OPAQUE_WAKE_BATCH) {
				delay = 1;
				more = true;
				continue;
			}
		}
		list_del_init(&op->wait);
		/* A wake is not a reservation; the collector rechecks on entry. */
		woken++;
		/* Native multishot polling also observes FIN, RST and urgent data. */
		wake_up_interruptible_poll(sk_sleep(sock->sk), EPOLLIN);
	}
	if (!stopped && delay != MAX_JIFFY_OFFSET)
		queue_delayed_work(system_dfl_wq, &store->retry_work, delay);
	return more;
}

/* As io_opaque_wake_budget(), until the batch leaves nobody behind. */
static void io_opaque_wake_budget_all(struct io_opaque_store *store)
{
	while (io_opaque_wake_budget(store))
		;
}

int io_opaque_memory_mode(struct io_opaque_store *store)
{
	unsigned int seq;
	int mode, next;
	u64 used, transient;

	if (!store->config.high_watermark)
		return IORING_OPAQUE_MEMORY_NORMAL;
	for (;;) {
		mode = atomic_read(&store->memory_mode);
		/* Watermarks measure payload held: compaction copies and framing are not. */
		do {
			seq = read_seqcount_begin(&store->transient_seq);
			used = atomic64_read(&store->bytes);
			transient = atomic64_read(&store->transient);
		} while (read_seqcount_retry(&store->transient_seq, seq));
		used = used > transient ? used - transient : 0;
		next = mode;
		if (mode == IORING_OPAQUE_MEMORY_NORMAL && used >= store->config.high_watermark)
			next = IORING_OPAQUE_MEMORY_LOWMEM;
		else if (mode == IORING_OPAQUE_MEMORY_LOWMEM && used <= store->config.low_watermark)
			next = IORING_OPAQUE_MEMORY_NORMAL;
		if (next == mode)
			return mode;
		/* Recheck occupancy after winning a concurrent mode transition. */
		atomic_try_cmpxchg(&store->memory_mode, &mode, next);
	}
}

bool io_opaque_charge(struct io_opaque_store *store, u64 bytes, bool rx)
{
	u64 limit = store->config.hard_limit - store->config.reply_reserve;
	s64 used = atomic64_read(&store->bytes);

	if (rx)
		limit -= store->config.compact_headroom;
	for (;;) {
		if (bytes > limit || used > limit - bytes)
			return false;
		if (atomic64_try_cmpxchg(&store->bytes, &used, used + bytes)) {
			io_opaque_memory_mode(store);
			return true;
		}
	}
}

/* Give bytes back to the store, and let nobody in on them yet. */
static void io_opaque_uncharge_quiet(struct io_opaque_store *store, u64 bytes)
{
	atomic64_sub(bytes, &store->bytes);
	io_opaque_memory_mode(store);
}

void io_opaque_uncharge(struct io_opaque_store *store, u64 bytes)
{
	io_opaque_uncharge_quiet(store, bytes);
	io_opaque_wake_budget(store);
}

/* Restore both budgets before waking collectors after a failed reservation. */
static void io_opaque_extent_unreserve(struct io_opaque_store *store, u64 bytes)
{
	atomic_dec(&store->extents);
	if (bytes)
		io_opaque_uncharge_quiet(store, bytes);
	io_opaque_wake_budget(store);
}

/* Charge bytes that watermarks ignore; bytes and transient change in one write. */
bool io_opaque_charge_transient(struct io_opaque_store *store, u64 bytes, u64 limit)
{
	s64 used;
	bool charged = false;

	spin_lock(&store->transient_lock);
	write_seqcount_begin(&store->transient_seq);
	used = atomic64_read(&store->bytes);
	for (;;) {
		if (bytes > limit || used > limit - bytes)
			break;
		if (atomic64_try_cmpxchg(&store->bytes, &used, used + bytes)) {
			atomic64_add(bytes, &store->transient);
			charged = true;
			break;
		}
	}
	write_seqcount_end(&store->transient_seq);
	spin_unlock(&store->transient_lock);
	return charged;
}

/* Uncharge and stop excluding transient bytes; the two amounts may differ. */
void io_opaque_release_transient(struct io_opaque_store *store, u64 uncharge,
				 u64 transient)
{
	spin_lock(&store->transient_lock);
	write_seqcount_begin(&store->transient_seq);
	atomic64_sub(uncharge, &store->bytes);
	atomic64_sub(transient, &store->transient);
	write_seqcount_end(&store->transient_seq);
	spin_unlock(&store->transient_lock);
	io_opaque_memory_mode(store);
	if (uncharge)
		io_opaque_wake_budget(store);
}

static bool io_opaque_extent_reserve(struct io_opaque_store *store)
{
	int nr = atomic_read(&store->extents);

	while (nr < store->config.max_extents) {
		if (atomic_try_cmpxchg(&store->extents, &nr, nr + 1))
			return true;
	}
	return false;
}

static int io_opaque_memcg_charge(struct io_opaque_store *store, u64 bytes)
{
#ifdef CONFIG_MEMCG
	if (store->objcg)
		return obj_cgroup_charge(store->objcg,
					GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN, bytes);
#endif
	return 0;
}

static void io_opaque_memcg_uncharge(struct io_opaque_store *store, u64 bytes)
{
#ifdef CONFIG_MEMCG
	if (store->objcg)
		obj_cgroup_uncharge(store->objcg, bytes);
#endif
}

struct io_opaque_extent *io_opaque_extent_new(struct io_opaque_store *store,
					      struct page *page, u32 offset,
					      u32 length, u64 start,
					      bool reserved)
{
	struct io_opaque_chunk *chunk;
	u64 charge = page_size(compound_head(page));

	if (!io_opaque_extent_reserve(store))
		return ERR_PTR(-ENOBUFS);
	if (!reserved && !io_opaque_charge(store, charge, true))
		goto no_space;
	if (!reserved && io_opaque_memcg_charge(store, charge))
		goto no_memory;
	chunk = kzalloc_obj(*chunk, GFP_KERNEL_ACCOUNT);
	if (!chunk) {
		if (!reserved)
			io_opaque_memcg_uncharge(store, charge);
		goto no_memory;
	}
	refcount_set(&chunk->backing.refs, 1);
	chunk->backing.page = compound_head(page);
	chunk->backing.charge = charge;
	chunk->backing.accounted = !reserved && store->objcg;
	chunk->extent.backing = &chunk->backing;
	chunk->extent.start = start;
	chunk->extent.offset = offset + (page - compound_head(page)) * PAGE_SIZE;
	chunk->extent.length = length;
	chunk->extent.embedded = true;
	INIT_LIST_HEAD(&chunk->extent.list);
	return &chunk->extent;
no_memory:
	io_opaque_extent_unreserve(store, reserved ? 0 : charge);
	return ERR_PTR(-ENOMEM);
no_space:
	io_opaque_extent_unreserve(store, 0);
	return ERR_PTR(-ENOBUFS);
}

/*
 * Let go of @extent, and of its backing if it was the last to use it. Returns
 * the charge of the backing that went, for the caller to give back to the
 * store: giving it back lets those waiting for budget in, which one free does
 * once and a list of them once, not once per extent.
 */
static u64 io_opaque_extent_release(struct io_opaque_store *store, struct io_opaque_extent *extent)
{
	struct io_opaque_backing *backing = extent->backing;
	u64 charge = 0;

	list_del(&extent->list);
	if (!extent->embedded)
		kfree(extent);
	atomic_dec(&store->extents);
	if (refcount_dec_and_test(&backing->refs)) {
		charge = backing->charge;

		if (backing->accounted)
			io_opaque_memcg_uncharge(store, charge);
		put_page(backing->page);
		kfree(container_of(backing, struct io_opaque_chunk, backing));
	}
	return charge;
}

/*
 * Free the extents of @list. What they held is returned to the store in one
 * step, and the waiters for budget are looked at in one go: each look takes
 * wait_lock and walks the list of them, and a 1 MB object that is not one
 * mapping can have 700 extents. The look is repeated while it leaves ready
 * waiters behind, so that they are let in by the free and not by the retry
 * work.
 */
static void io_opaque_extents_free(struct io_opaque_store *store,
				   struct list_head *list)
{
	struct io_opaque_extent *extent, *next;
	bool any = false;
	u64 charge = 0;

	list_for_each_entry_safe(extent, next, list, list) {
		charge += io_opaque_extent_release(store, extent);
		any = true;
	}
	if (charge)
		io_opaque_uncharge_quiet(store, charge);
	if (any)
		io_opaque_wake_budget_all(store);
}

struct io_opaque_data *io_opaque_data_alloc(void)
{
	struct io_opaque_data *data = kzalloc_obj(*data, GFP_KERNEL_ACCOUNT);

	if (data) {
		refcount_set(&data->refs, 1);
		INIT_LIST_HEAD(&data->extents);
	}
	return data;
}

void io_opaque_data_put(struct io_opaque_store *store,
			struct io_opaque_data *data)
{
	if (data && refcount_dec_and_test(&data->refs)) {
		io_opaque_extents_free(store, &data->extents);
		kvfree(data->bvec);
		kfree(data);
	}
}

/*
 * Build the version's segments, and for a version of more than
 * IO_OPAQUE_SEEK_STRIDE of them the anchors that find one without walking
 * from the first, in one allocation. Both are made once, when the version is,
 * and never change. The anchors follow the segments at the next multiple of
 * eight bytes: a segment is 16 bytes, but only 12 on 32 bits, where an odd
 * number of them would leave the anchors four-byte aligned.
 */
int io_opaque_vector(struct io_opaque_data *data)
{
	struct io_opaque_extent *extent;
	struct io_opaque_backing *previous = NULL;
	u32 nr = 0, anchors;
	size_t bytes;
	u64 at = 0;

	data->charge = 0;
	list_for_each_entry(extent, &data->extents, list) {
		nr++;
		if (extent->backing != previous)
			data->charge += extent->backing->charge;
		previous = extent->backing;
	}
	anchors = nr > IO_OPAQUE_SEEK_STRIDE ? DIV_ROUND_UP(nr, IO_OPAQUE_SEEK_STRIDE) : 0;
	/* Where the anchors start. An overflow saturates, and the allocation fails. */
	bytes = size_add(array_size(nr, sizeof(*data->bvec)), sizeof(*data->anchor) - 1);
	bytes &= ~(sizeof(*data->anchor) - 1);
	data->bvec = kvmalloc(size_add(bytes, array_size(anchors, sizeof(*data->anchor))),
			      GFP_KERNEL_ACCOUNT);
	if (!data->bvec)
		return -ENOMEM;
	data->anchor = anchors ? (void *)data->bvec + bytes : NULL;
	data->nr = nr;
	nr = 0;
	list_for_each_entry(extent, &data->extents, list) {
		if (anchors && !(nr % IO_OPAQUE_SEEK_STRIDE))
			data->anchor[nr / IO_OPAQUE_SEEK_STRIDE] = at;
		bvec_set_page(&data->bvec[nr++], extent->backing->page,
			      extent->length, extent->offset);
		at += extent->length;
	}
	return 0;
}

/* Handle encoding is internal; zero is never a published handle. */
u64 io_opaque_handle(u32 index, u64 generation)
{
	return generation << IO_OPAQUE_INDEX_BITS | index;
}

static u32 io_opaque_index(u64 handle)
{
	return handle & IO_OPAQUE_INDEX_MASK;
}

struct io_opaque_slot *io_opaque_lookup(struct io_opaque_store *store,
					u64 handle)
{
	u32 index = io_opaque_index(handle);

	lockdep_assert_held(&store->tables);
	if (!handle || index >= store->config.max_objects)
		return NULL;
	if (store->objects[index].generation != handle >> IO_OPAQUE_INDEX_BITS)
		return NULL;
	return &store->objects[index];
}
