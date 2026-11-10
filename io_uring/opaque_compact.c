// SPDX-License-Identifier: GPL-2.0
/* Workers copy immutable backing without holding object table locks. */
#include <linux/highmem.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "io_uring.h"
#include "opaque_internal.h"

static void io_opaque_auto_compact(struct io_opaque_store *store);

struct io_opaque_cursor {
	struct io_opaque_extent *extent;
	u32 offset;
};

static int io_opaque_compact_copy(struct io_opaque_req *op,
				  struct io_opaque_cursor *cursor,
				  struct page *page, u32 length)
{
	u32 written = 0;

	while (written < length) {
		struct io_opaque_extent *src = cursor->extent;
		u32 pos = src->offset + cursor->offset;
		u32 in = offset_in_page(pos), out = offset_in_page(written);
		u32 chunk = min(length - written, src->length - cursor->offset);
		void *from, *to;

		if (READ_ONCE(op->canceled) || READ_ONCE(op->store->dead))
			return -ECANCELED;
		chunk = min_t(u32, chunk, PAGE_SIZE - in);
		chunk = min_t(u32, chunk, PAGE_SIZE - out);
		to = kmap_local_page(page + (written >> PAGE_SHIFT));
		from = kmap_local_page(src->backing->page + (pos >> PAGE_SHIFT));
		memcpy(to + out, from + in, chunk);
		kunmap_local(from);
		kunmap_local(to);
		written += chunk;
		cursor->offset += chunk;
		if (cursor->offset == src->length) {
			cursor->offset = 0;
			if (!list_is_last(&src->list, &op->data->extents))
				cursor->extent = list_next_entry(src, list);
			else if (written < length)
				return -EIO;
		}
	}
	return 0;
}

struct page *io_opaque_owned_page(unsigned int *order)
{
	struct page *page;

	for (;;) {
		page = alloc_pages(GFP_KERNEL_ACCOUNT | __GFP_COMP | __GFP_NORETRY |
				   __GFP_NOWARN, *order);
		if (page || !*order)
			return page;
		(*order)--;
	}
}

/*
 * Destination chunk for compaction and CREATE: the largest order within the
 * remaining size, falling back to smaller orders.
 */
struct page *io_opaque_dest_page(struct io_opaque_store *store, u64 pages_left,
				 unsigned int *order)
{
	*order = min_t(unsigned int, IO_OPAQUE_MAX_ORDER, ilog2(pages_left));
	return io_opaque_owned_page(order);
}

static int io_opaque_compact_build(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_cursor cursor;
	u64 done = 0, length, charge;
	struct mem_cgroup *old;
	int ret = 0;

	length = op->data->length;
	charge = ALIGN(length, PAGE_SIZE);
	if (READ_ONCE(op->canceled) || READ_ONCE(store->dead))
		return -ECANCELED;
	ret = io_opaque_compact_reserve(op, charge);
	if (ret)
		return ret;
	op->copy_reserved = true;
	old = set_active_memcg(store->memcg);
	op->replacement = io_opaque_data_alloc();
	if (!op->replacement) {
		ret = -ENOMEM;
		goto out;
	}
	op->replacement->forward_only = op->data->forward_only;
	cursor.extent = list_first_entry(&op->data->extents, struct io_opaque_extent, list);
	cursor.offset = 0;
	while (done < length) {
		u64 pages_left = DIV_ROUND_UP_ULL(length - done, PAGE_SIZE);
		struct io_opaque_extent *extent;
		unsigned int order;
		struct page *page;
		u32 chunk;

		if (READ_ONCE(op->canceled) || READ_ONCE(store->dead)) {
			ret = -ECANCELED;
			goto out;
		}
		page = io_opaque_dest_page(store, pages_left, &order);
		if (!page) {
			ret = -ENOMEM;
			goto out;
		}
		chunk = min_t(u64, length - done, PAGE_SIZE << order);
		extent = io_opaque_extent_new(store, page, 0, chunk, done, true);
		if (IS_ERR(extent)) {
			put_page(page);
			ret = PTR_ERR(extent);
			goto out;
		}
		op->reserved -= page_size(page);
		list_add_tail(&extent->list, &op->replacement->extents);
		ret = io_opaque_compact_copy(op, &cursor, page, chunk);
		if (ret)
			goto out;
		done += chunk;
		op->replacement->length = done;
		cond_resched();
	}
	ret = io_opaque_vector(op->replacement);
	if (!ret)
		op->replacement->dense = true;
out:
	set_active_memcg(old);
	return ret;
}

int io_opaque_compact_publish(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_slot *slot;
	struct io_opaque_data *old;
	int ret = -ESTALE;

	mutex_lock(&store->tables);
	slot = io_opaque_lookup(store, op->handle);
	/* Cancellation shares this bounded publication decision, not the frees. */
	spin_lock(&store->wait_lock);
	if (slot && slot->data == op->data && !store->dead &&
	    !READ_ONCE(op->canceled)) {
		old = slot->data;
		op->published = op->replacement;
		refcount_inc(&op->published->refs);
		slot->data = op->replacement;
		op->replacement = NULL;
		op->copy_committed = true;
		slot->compacting = false;
		list_del_init(&slot->candidate);
		atomic64_add(old->length, &store->compacted);
		atomic64_inc(&store->compactions);
		ret = 0;
	} else {
		old = NULL;
	}
	spin_unlock(&store->wait_lock);
	mutex_unlock(&store->tables);
	io_opaque_data_put(store, old);
	return ret;
}

/* Complete accounting in the existing dispatcher, never in deferred ring work. */
static int io_opaque_compact_run(struct io_opaque_req *op, bool automatic)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_slot *slot;
	int ret = io_opaque_compact_build(op);

	if (!ret)
		ret = io_opaque_compact_publish(op);
	mutex_lock(&store->tables);
	slot = io_opaque_lookup(store, op->handle);
	if (slot && slot->data == op->data) {
		slot->compacting = false;
		if (automatic && ret)
			slot->retry = jiffies + HZ;
	}
	mutex_unlock(&store->tables);
	io_opaque_compact_end(op);
	return ret;
}

void io_opaque_compact_work(struct work_struct *work)
{
	struct io_opaque_store *store = container_of(to_delayed_work(work),
						   struct io_opaque_store, compact_work);
	unsigned long delay = MAX_JIFFY_OFFSET;
	unsigned long now;
	unsigned int i;

	/* Once shutdown is observed, drain every accepted request in this run. */
	for (i = 0; i < 8 || READ_ONCE(store->dead); i++) {
		struct io_opaque_req *op;
		int ret;

		scoped_guard(spinlock, &store->wait_lock) {
			op = list_first_entry_or_null(&store->copies, struct io_opaque_req, wait);
			if (op)
				list_del_init(&op->wait);
		}
		if (!op)
			break;
		ret = io_opaque_compact_run(op, false);
		/* Cancellation cannot complete a request still owned by this worker. */
		io_opaque_queue_ready(op, ret);
	}
	io_opaque_auto_compact(store);
	/* Keep automatic scans paced even when manual requests wake us early. */
	guard(mutex)(&store->tables);
	now = jiffies;
	if (!store->dead && (store->policy.flags & IORING_OPAQUE_POLICY_F_AUTO_COMPACT) &&
	    !list_empty(&store->candidates))
		delay = time_before(now, store->auto_at) ? store->auto_at - now : 0;
	guard(spinlock)(&store->wait_lock);
	if (!list_empty(&store->copies))
		delay = 0;
	if (delay != MAX_JIFFY_OFFSET)
		queue_delayed_work(system_dfl_wq, &store->compact_work, delay);
}

int io_opaque_compact_start(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_slot *slot;

	guard(mutex)(&store->tables);
	if (store->dead)
		return -ESHUTDOWN;
	slot = io_opaque_lookup(store, op->handle);
	if (!slot || !slot->data)
		return -ESTALE;
	if (slot->compacting)
		return -EBUSY;
	if (slot->data->dense)
		return 0;
	op->data = slot->data;
	refcount_inc(&op->data->refs);
	slot->compacting = true;
	io_opaque_wait(op, IO_OPAQUE_COPY_WORK);
	scoped_guard(spinlock, &store->wait_lock)
		list_add_tail(&op->wait, &store->copies);
	mod_delayed_work(system_dfl_wq, &store->compact_work, 0);
	return IOU_ISSUE_SKIP_COMPLETE;
}

/*
 * Install @policy and mark the objects it concerns as candidates, or not. Where
 * the table of handles is long this is the longest thing done under tables, so
 * the walk drops it every IO_OPAQUE_POLICY_CHUNK slots and those waiting
 * for it wait for a chunk. What happens between chunks cannot hurt it. The
 * policy is in force from the first one, so an object published meanwhile joins
 * the candidates by itself, or does not, and one freed leaves them; the walk
 * adds a slot only if it is not on the list. A second change that starts
 * meanwhile walks the whole table itself, so the first gives up when it comes
 * back and finds it was overtaken, as it does if the store has stopped.
 */
static int io_opaque_apply_policy(struct io_opaque_store *store,
				  const struct io_uring_opaque_policy *policy)
{
	bool automatic = policy->flags & IORING_OPAQUE_POLICY_F_AUTO_COMPACT;
	u32 i, seq;

	mutex_lock(&store->tables);
	if (store->dead) {
		mutex_unlock(&store->tables);
		return -ESHUTDOWN;
	}
	store->policy = *policy;
	store->tokens = policy->burst_bytes;
	store->token_time = jiffies;
	store->auto_at = jiffies;
	seq = ++store->policy_seq;
	for (i = 0; i < store->config.max_objects; i++) {
		struct io_opaque_slot *slot = &store->objects[i];

		if (automatic && slot->data && !slot->data->dense && !slot->data->forward_only &&
		    list_empty(&slot->candidate)) {
			list_add_tail(&slot->candidate, &store->candidates);
			slot->retry = jiffies;
		} else if (!automatic) {
			list_del_init(&slot->candidate);
		}
		if ((i + 1) % IO_OPAQUE_POLICY_CHUNK || i + 1 == store->config.max_objects)
			continue;
		mutex_unlock(&store->tables);
		cond_resched();
		mutex_lock(&store->tables);
		if (store->dead) {
			mutex_unlock(&store->tables);
			return -ESHUTDOWN;
		}
		if (store->policy_seq != seq) {
			mutex_unlock(&store->tables);
			return 0;
		}
	}
	if (automatic) {
		mod_delayed_work(system_dfl_wq, &store->compact_work, 0);
	} else {
		guard(spinlock)(&store->wait_lock);
		/* Policy changes cannot cancel an accepted manual request. */
		if (list_empty(&store->copies))
			cancel_delayed_work(&store->compact_work);
	}
	mutex_unlock(&store->tables);
	return 0;
}

/* Copy and check the policy at prep, so its struct need not outlive submission. */
int io_opaque_policy_prep(struct io_opaque_req *op)
{
	struct io_uring_opaque_policy *policy = &op->policy;
	u64 hard_limit = op->store->config.hard_limit;
	int ret;

	if (op->length < offsetof(struct io_uring_opaque_policy, __resv) ||
	    op->length > PAGE_SIZE)
		return -EINVAL;
	ret = copy_struct_from_user(policy, sizeof(*policy), op->addr, op->length);
	if (ret)
		return ret;
	if ((policy->flags & ~IORING_OPAQUE_POLICY_F_AUTO_COMPACT) ||
	    policy->__resv0 || !mem_is_zero(policy->__resv, sizeof(policy->__resv)) ||
	    policy->bytes_per_second > SZ_1T || policy->burst_bytes > hard_limit ||
	    policy->max_temporary_bytes > hard_limit)
		return -EINVAL;
	if ((policy->flags & IORING_OPAQUE_POLICY_F_AUTO_COMPACT) &&
	    (!policy->bytes_per_second || !policy->burst_bytes ||
	     policy->max_temporary_bytes < PAGE_SIZE))
		return -EINVAL;
	return 0;
}

int io_opaque_set_policy(struct io_opaque_req *op)
{
	return io_opaque_apply_policy(op->store, &op->policy);
}

/* The same dispatcher handles bounded scans and the store-wide token bucket. */
static void io_opaque_auto_compact(struct io_opaque_store *store)
{
	struct io_opaque_req op = { .store = store };
	struct io_opaque_slot *slot = NULL;
	struct io_uring_opaque_policy policy;
	u64 minted, elapsed;
	u64 length = 0;
	u32 policy_seq;
	unsigned int scanned = 0;
	int ret;

	mutex_lock(&store->tables);
	policy = store->policy;
	policy_seq = store->policy_seq;
	if (store->dead || !(policy.flags & IORING_OPAQUE_POLICY_F_AUTO_COMPACT) ||
	    time_before(jiffies, store->auto_at))
		goto unlock;
	store->auto_at = jiffies + msecs_to_jiffies(100);
	elapsed = jiffies - store->token_time;
	if (check_mul_overflow(elapsed, policy.bytes_per_second, &minted))
		minted = policy.burst_bytes;
	else
		minted = div_u64(minted, HZ);
	store->tokens = min(policy.burst_bytes, store->tokens + min(minted, policy.burst_bytes));
	store->token_time = jiffies;
	while (!list_empty(&store->candidates) && scanned++ < 32) {
		u64 dense, saving;

		slot = list_first_entry(&store->candidates, struct io_opaque_slot, candidate);
		list_move_tail(&slot->candidate, &store->candidates);
		if (!slot->data || slot->data->dense) {
			list_del_init(&slot->candidate);
			continue;
		}
		if (slot->compacting || time_before(jiffies, slot->retry) ||
		    time_before(jiffies, slot->born + msecs_to_jiffies(100)))
			continue;
		dense = ALIGN(slot->data->length, PAGE_SIZE);
		saving = slot->data->charge > dense ? slot->data->charge - dense : 0;
		/* Selection heuristics are implementation details, not UAPI. */
		if ((!saving && slot->data->nr == 1) ||
		    (saving && saving * 100 < slot->data->charge * 25) ||
		    dense > policy.max_temporary_bytes || slot->data->length > policy.burst_bytes) {
			/* Immutable backing cannot become eligible without a new policy. */
			list_del_init(&slot->candidate);
			continue;
		}
		if (slot->data->length > store->tokens)
			continue;
		slot->compacting = true;
		store->tokens -= slot->data->length;
		op.data = slot->data;
		length = op.data->length;
		refcount_inc(&op.data->refs);
		op.handle = io_opaque_handle(slot - store->objects, slot->generation);
		break;
	}
unlock:
	mutex_unlock(&store->tables);
	if (op.data) {
		ret = io_opaque_compact_run(&op, true);
		/* No reservation means no copy was attempted; return its tokens. */
		if (ret && !op.copy_reserved) {
			guard(mutex)(&store->tables);

			if (store->policy_seq == policy_seq)
				store->tokens = min(policy.burst_bytes, store->tokens + length);
		}
	}
}
