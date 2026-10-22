// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/io_uring.h>
#include <linux/jhash.h>
#include <linux/nospec.h>
#include <linux/rculist.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>

#include <uapi/linux/io_uring.h>

#include "../kernel/futex/futex.h"
#include "io_uring.h"
#include "alloc_cache.h"
#include "futex.h"

struct io_futex_member;

struct io_futex {
	struct file	*file;
	void __user	*uaddr;
	unsigned long	futex_val;
	unsigned long	futex_mask;
	u32		futex_flags;
	unsigned int	futex_nr;
	bool		futexv_unqueued;
	u8		iou_flags;	/* sqe->futex_flags */
	/* a persistent wait's membership, NULL for every other request */
	struct io_futex_member	*member;
};

struct io_futex_data {
	struct futex_q	q;
	struct io_kiocb	*req;
};

struct io_futexv_data {
	unsigned long		owned;
	struct futex_vector	futexv[];
};

#define IO_FUTEX_ALLOC_CACHE_MAX	32

bool io_futex_cache_init(struct io_ring_ctx *ctx)
{
	return io_alloc_cache_init(&ctx->futex_cache, IO_FUTEX_ALLOC_CACHE_MAX,
				sizeof(struct io_futex_data), 0);
}

void io_futex_cache_free(struct io_ring_ctx *ctx)
{
	io_alloc_cache_free(&ctx->futex_cache, kfree);
}

static void __io_futex_complete(struct io_tw_req tw_req, io_tw_token_t tw)
{
	hlist_del_init(&tw_req.req->hash_node);
	io_req_task_complete(tw_req, tw);
}

static void io_futex_complete(struct io_tw_req tw_req, io_tw_token_t tw)
{
	struct io_kiocb *req = tw_req.req;
	struct io_ring_ctx *ctx = req->ctx;

	io_tw_lock(ctx, tw);
	io_cache_free(&ctx->futex_cache, req->async_data);
	io_req_async_data_clear(req, 0);
	__io_futex_complete(tw_req, tw);
}

static void io_futexv_complete(struct io_tw_req tw_req, io_tw_token_t tw)
{
	struct io_kiocb *req = tw_req.req;
	struct io_futex *iof = io_kiocb_to_cmd(req, struct io_futex);
	struct io_futexv_data *ifd = req->async_data;

	io_tw_lock(req->ctx, tw);

	if (!iof->futexv_unqueued) {
		int res;

		res = futex_unqueue_multiple(ifd->futexv, iof->futex_nr);
		if (res != -1)
			io_req_set_res(req, res, 0);
	}

	io_req_async_data_free(req);
	__io_futex_complete(tw_req, tw);
}

/*
 * Persistent waits and registered wakes.
 *
 * A persistent wait and a registered waker on the same futex word meet in one
 * io_futex_node, found by the word's futex key in a hash of io_uring's own,
 * sized and hashed as the futex hash is, with a lock per bucket.  A bucket's
 * nodes and their member lists change under its lock, which only arming,
 * registering and teardown take.  Both need write access to the word, as a
 * waker would.  A registered wake already holds the node:
 * it walks the node's members under RCU and takes no lock and does no key
 * lookup, so nothing a preempted low-priority waiter holds can delay it.
 *
 * None of this is in the futex hash.  A persistent wait is not a futex_q,
 * futex(2) wakes don't see it, and a registered wake sees nothing else.
 *
 * Each member posts one CQE per arm.  Its state, changed by cmpxchg but for
 * arming it and ending it:
 *
 *   ARMED	waiting; a wake moves it on			-> QUEUED
 *   QUEUED	to be posted by the ring's task			-> FIRED
 *   FIRED	CQE posted; a wake is remembered		-> MISSED
 *   MISSED	re-arming moves it to QUEUED at once
 *   DEAD	cancelled or ended; the final task work frees it
 *
 * Re-arming moves FIRED to ARMED and MISSED to QUEUED.
 *
 * Every wake that finds a member QUEUED sets the member's bit in its ring's
 * pending bitmap and kicks the ring (io_ring_futex_kick()), not only the wake
 * that moved it there.  Setting the bit is the whole of publishing it, one
 * atomic, so a wake that moved it and was then preempted, a low-priority one
 * on a busy CPU say, leaves nothing half done for later wakes, a real-time
 * one included, to depend on.  The ring's task takes the bits when it runs
 * its local work and posts their members, so persistent waits need
 * IORING_SETUP_DEFER_TASKRUN.  That also keeps the ring alive for a waker
 * still under RCU: such a ring is freed only after a grace period.
 */
struct io_futex_bucket {
	spinlock_t		lock;
	struct hlist_head	nodes;
} ____cacheline_aligned_in_smp;

struct io_futex_node {
	struct hlist_node	hnode;		/* on bucket->nodes */
	struct io_futex_bucket	*bucket;	/* never changes: no rehash */
	union futex_key		key;
	struct hlist_head	members;	/* RCU */
	refcount_t		ref;		/* members and registrations */
	struct rcu_head		rcu;
};

enum {
	IO_FUTEX_ARMED,
	IO_FUTEX_QUEUED,
	IO_FUTEX_FIRED,
	IO_FUTEX_MISSED,
	IO_FUTEX_DEAD,
};

struct io_futex_member {
	struct hlist_node	mnode;		/* on node->members */
	struct io_futex_node	*node;
	struct io_ring_ctx	*ctx;
	struct io_kiocb		*req;
	/* Its slot in ctx->futex_ring, and the ring's pending bitmap. */
	unsigned int		slot;
	unsigned long		*pending;
	u32			cqe_res;	/* the slot if allocated, else 0 */
	u32			bitset;
	atomic_t		state;
	int			res;		/* the final CQE's */
	struct rcu_head		rcu;
};

/*
 * A ring's persistent waits, for its own task.  Each has a slot, which the
 * SQE names or has allocated as a direct descriptor's is, and which re-arm
 * names: a wake in any task sets the slot's bit in pending[], and the ring's
 * task posts the waits whose bits it takes.  All but pending[] change only
 * under the ring's uring_lock.  Sized when the ring's first persistent wait
 * arms, to its CQ, since each wait needs room for a CQE; freed with the
 * ring, after the grace period its exit waits for, so a waker still under
 * RCU can set a bit in it.
 */
struct io_futex_ring {
	unsigned int		cap;
	struct io_futex_member	**slots;
	unsigned long		*used;
	unsigned long		pending[];
};

/*
 * A ring's registered words, as its registered files are: a table of nr
 * slots, any of them empty, set at registration and changed slot by slot
 * by updates.
 */
struct io_futex_table {
	unsigned int		nr;
	struct rcu_head		rcu;
	struct io_futex_node __rcu	*nodes[] __counted_by(nr);
};

#define IO_FUTEX_REG_MAX	(1U << 16)

static struct io_futex_bucket *io_futex_buckets;
static unsigned int io_futex_hash_mask;

/* As futex_init() sizes the futex hash: 256 buckets per CPU. */
static int __init io_futex_init(void)
{
	unsigned long size, i;

	size = 256 * num_possible_cpus();
	size /= num_possible_nodes();
	size = roundup_pow_of_two(max(4UL, size));
	io_futex_buckets = kvcalloc(size, sizeof(*io_futex_buckets),
				    GFP_KERNEL);
	if (!io_futex_buckets)
		panic("io_uring: no memory for the futex node hash");
	for (i = 0; i < size; i++) {
		spin_lock_init(&io_futex_buckets[i].lock);
		INIT_HLIST_HEAD(&io_futex_buckets[i].nodes);
	}
	io_futex_hash_mask = size - 1;
	return 0;
}
core_initcall(io_futex_init);

static struct io_futex_bucket *io_futex_bucket(union futex_key *key)
{
	u32 hash = jhash2((u32 *)&key->both.ptr,
			  (sizeof(key->both.ptr) + sizeof(key->both.word)) / 4,
			  key->both.offset);

	return &io_futex_buckets[hash & io_futex_hash_mask];
}

/*
 * A key without FUT_OFF_INODE names its mm by pointer.  The node keeps the
 * mm_struct allocated so the pointer can't be reused for another mm while
 * the node can still be found by it.
 */
static bool io_futex_key_has_mm(union futex_key *key)
{
	return !(key->both.offset & FUT_OFF_INODE);
}

static struct io_futex_node *io_futex_node_get(u32 __user *uaddr,
					       unsigned int flags)
{
	union futex_key key = FUTEX_KEY_INIT;
	struct io_futex_node *node, *new = NULL;
	struct io_futex_bucket *b;
	int ret;

	/* Write access, as a waker needs: reading the word is not enough. */
	ret = get_futex_key(uaddr, flags, &key, FUTEX_WRITE);
	if (ret)
		return ERR_PTR(ret);
	b = io_futex_bucket(&key);
again:
	spin_lock(&b->lock);
	hlist_for_each_entry(node, &b->nodes, hnode) {
		if (futex_match(&node->key, &key)) {
			/* A node at zero has already left the bucket. */
			refcount_inc(&node->ref);
			spin_unlock(&b->lock);
			kfree(new);
			return node;
		}
	}
	if (!new) {
		/* Allocate only on a miss, and not under the lock. */
		spin_unlock(&b->lock);
		new = kzalloc_obj(*new, GFP_KERNEL_ACCOUNT);
		if (!new)
			return ERR_PTR(-ENOMEM);
		goto again;
	}
	new->key = key;
	new->bucket = b;
	INIT_HLIST_HEAD(&new->members);
	refcount_set(&new->ref, 1);
	if (io_futex_key_has_mm(&key))
		mmgrab(key.private.mm);
	hlist_add_head(&new->hnode, &b->nodes);
	spin_unlock(&b->lock);
	return new;
}

static void io_futex_node_put(struct io_futex_node *node)
{
	if (!refcount_dec_and_lock(&node->ref, &node->bucket->lock))
		return;
	hlist_del(&node->hnode);
	spin_unlock(&node->bucket->lock);
	if (io_futex_key_has_mm(&node->key))
		mmdrop(node->key.private.mm);
	/* A registered wake may still be walking it. */
	kfree_rcu(node, rcu);
}

static void io_futex_member_unlink(struct io_futex_member *m)
{
	spin_lock(&m->node->bucket->lock);
	hlist_del_rcu(&m->mnode);
	spin_unlock(&m->node->bucket->lock);
}

/* Under uring_lock: the ring's persistent wait slots, made on first use. */
static struct io_futex_ring *io_futex_ring_get(struct io_ring_ctx *ctx)
{
	struct io_futex_ring *fr = ctx->futex_ring;
	unsigned int cap = ctx->cq_entries;

	if (fr)
		return fr;
	fr = kvzalloc(struct_size(fr, pending, BITS_TO_LONGS(cap)),
		      GFP_KERNEL_ACCOUNT);
	if (!fr)
		return NULL;
	fr->cap = cap;
	fr->slots = kvcalloc(cap, sizeof(*fr->slots), GFP_KERNEL_ACCOUNT);
	fr->used = bitmap_zalloc(cap, GFP_KERNEL_ACCOUNT);
	if (!fr->slots || !fr->used) {
		kvfree(fr->slots);
		bitmap_free(fr->used);
		kvfree(fr);
		return NULL;
	}
	ctx->futex_ring = fr;
	return fr;
}

/*
 * From any task: publish @m to its ring's task.  The bit is set before the
 * kick's full barrier, which the ring's xchg of futex_pending pairs with, so
 * a ring that sees the kick sees the bit.
 */
static void io_futex_mark_pending(struct io_futex_member *m)
{
	set_bit(m->slot, m->pending);
	io_ring_futex_kick(m->ctx);
}

/*
 * From any task, under RCU.  Returns false if @m is going away.  Allocates
 * nothing and takes no lock.
 */
static bool io_futex_notify(struct io_futex_member *m)
{
	int state = atomic_read(&m->state);

	for (;;) {
		switch (state) {
		case IO_FUTEX_ARMED:
			if (!atomic_try_cmpxchg(&m->state, &state,
						IO_FUTEX_QUEUED))
				continue;
			fallthrough;
		case IO_FUTEX_QUEUED:
			/* Whoever moved it may not have published yet: do it too. */
			io_futex_mark_pending(m);
			return true;
		case IO_FUTEX_FIRED:
			if (!atomic_try_cmpxchg(&m->state, &state,
						IO_FUTEX_MISSED))
				continue;
			return true;
		case IO_FUTEX_MISSED:
			/* Posted by the re-arm. */
			return true;
		default:
			return false;
		}
	}
}

/* A persistent wait's end, in its ring's task: the final CQE, then free. */
static void io_futex_persistent_tw(struct io_tw_req tw_req, io_tw_token_t tw)
{
	struct io_kiocb *req = tw_req.req;
	struct io_futex *iof = io_kiocb_to_cmd(req, struct io_futex);
	struct io_futex_member *m = iof->member;

	io_tw_lock(req->ctx, tw);
	/* No wake can reach @m once a grace period has passed. */
	if (m->res < 0)
		req_set_fail(req);
	io_req_set_res(req, m->res, 0);
	iof->member = NULL;
	io_futex_node_put(m->node);
	kfree_rcu(m, rcu);
	__io_futex_complete(tw_req, tw);
}

/*
 * Under uring_lock: takes @req off futex_list, frees its slot, and queues its
 * end.  A bit a waker sets for the slot after this finds it empty, or a
 * later wait that is not QUEUED, and does nothing.
 */
static void io_futex_persistent_end(struct io_kiocb *req, int res)
{
	struct io_futex *iof = io_kiocb_to_cmd(req, struct io_futex);
	struct io_futex_member *m = iof->member;
	struct io_futex_ring *fr = req->ctx->futex_ring;

	hlist_del_init(&req->hash_node);
	fr->slots[m->slot] = NULL;
	__clear_bit(m->slot, fr->used);
	io_futex_member_unlink(m);
	m->res = res;
	atomic_set(&m->state, IO_FUTEX_DEAD);
	io_req_task_work_add(req);
}

/*
 * In the ring's task, from __io_run_local_work(), under uring_lock: post the
 * waits whose bits are set.  Returns how many CQEs it posted.
 */
int io_futex_run_pending(struct io_ring_ctx *ctx)
{
	struct io_futex_ring *fr = ctx->futex_ring;
	unsigned int w;
	int posted = 0;

	lockdep_assert_held(&ctx->uring_lock);
	if (!fr)
		return 0;
	for (w = 0; w < BITS_TO_LONGS(fr->cap); w++) {
		unsigned long bits;

		if (!READ_ONCE(fr->pending[w]))
			continue;
		/* Take them first: a wake after this sets its bit again. */
		bits = xchg(&fr->pending[w], 0);
		while (bits) {
			const unsigned int slot = w * BITS_PER_LONG + __ffs(bits);
			struct io_futex_member *m = fr->slots[slot];
			int state = IO_FUTEX_QUEUED;

			bits &= bits - 1;
			/* An empty slot, or a wait already posted or ended. */
			if (!m || !atomic_try_cmpxchg(&m->state, &state,
						      IO_FUTEX_FIRED))
				continue;
			if (io_req_post_cqe(m->req, m->cqe_res, IORING_CQE_F_MORE)) {
				posted++;
				continue;
			}
			/* No room for it: end the wait, as multishot requests do. */
			io_futex_persistent_end(m->req, -EOVERFLOW);
		}
	}
	return posted;
}

/* Under uring_lock. */
static bool io_futex_persistent_cancel(struct io_kiocb *req)
{
	io_futex_persistent_end(req, -ECANCELED);
	return true;
}

static int io_futex_wait_persistent(struct io_kiocb *req,
				    unsigned int issue_flags)
{
	struct io_futex *iof = io_kiocb_to_cmd(req, struct io_futex);
	struct io_ring_ctx *ctx = req->ctx;
	struct io_futex_member *m;
	struct io_futex_node *node;
	struct io_futex_ring *fr;
	unsigned int slot;
	u32 uval;
	int ret;

	m = kzalloc_obj(*m, GFP_KERNEL_ACCOUNT);
	if (!m) {
		ret = -ENOMEM;
		goto fail;
	}
	node = io_futex_node_get(iof->uaddr, iof->futex_flags);
	if (IS_ERR(node)) {
		ret = PTR_ERR(node);
		kfree(m);
		goto fail;
	}
	m->node = node;
	m->ctx = ctx;
	m->req = req;
	m->bitset = iof->futex_mask;
	atomic_set(&m->state, IO_FUTEX_ARMED);
	iof->member = m;
	req->io_task_work.func = io_futex_persistent_tw;

	/*
	 * Cancellation finds it on futex_list, once it is on the node too.
	 * uring_lock is held through the look at the word below: issued from
	 * io-wq, nothing else would keep a cancel from freeing @m under it.
	 */
	io_ring_submit_lock(ctx, issue_flags);
	fr = io_futex_ring_get(ctx);
	if (!fr) {
		ret = -ENOMEM;
		goto fail_unlock;
	}
	/* A slot as a direct descriptor's: the one named, or any free one. */
	if (iof->futex_nr == IORING_FILE_INDEX_ALLOC) {
		slot = find_first_zero_bit(fr->used, fr->cap);
		ret = -ENFILE;
		if (slot >= fr->cap)
			goto fail_unlock;
		m->cqe_res = slot;
	} else {
		slot = iof->futex_nr - 1;
		ret = -EINVAL;
		if (slot >= fr->cap)
			goto fail_unlock;
		slot = array_index_nospec(slot, fr->cap);
		ret = -EBUSY;
		if (test_bit(slot, fr->used))
			goto fail_unlock;
	}
	__set_bit(slot, fr->used);
	fr->slots[slot] = m;
	m->slot = slot;
	m->pending = fr->pending;
	spin_lock(&node->bucket->lock);
	hlist_add_head_rcu(&m->mnode, &node->members);
	spin_unlock(&node->bucket->lock);
	hlist_add_head(&req->hash_node, &ctx->futex_list);
	/* As for a one-shot wait: an io-wq cancel may have missed the list. */
	if (io_wq_cancel_seen(req, issue_flags)) {
		io_futex_persistent_end(req, -ECANCELED);
		io_ring_submit_unlock(ctx, issue_flags);
		return IOU_ISSUE_SKIP_COMPLETE;
	}

	/*
	 * On the list, then look at the word: pairs with the smp_mb() in a
	 * registered wake, between the waker's store and its walk.  Either the
	 * wake finds us or we see its value.  A word we can't read counts as
	 * changed, and user space finds out why.
	 */
	smp_mb();
	if (get_user(uval, (u32 __user *)iof->uaddr) || uval != iof->futex_val)
		io_futex_notify(m);
	io_ring_submit_unlock(ctx, issue_flags);
	return IOU_ISSUE_SKIP_COMPLETE;
fail_unlock:
	io_ring_submit_unlock(ctx, issue_flags);
	iof->member = NULL;
	io_futex_node_put(node);
	kfree(m);
fail:
	req_set_fail(req);
	io_req_set_res(req, ret, 0);
	return IOU_COMPLETE;
}

static int io_futex_member_rearm(struct io_futex_member *m)
{
	int state = atomic_read(&m->state);

	for (;;) {
		switch (state) {
		case IO_FUTEX_FIRED:
			if (!atomic_try_cmpxchg(&m->state, &state,
						IO_FUTEX_ARMED))
				continue;
			return 0;
		case IO_FUTEX_MISSED:
			if (!atomic_try_cmpxchg(&m->state, &state,
						IO_FUTEX_QUEUED))
				continue;
			io_futex_mark_pending(m);
			return 0;
		case IO_FUTEX_DEAD:
			return -ENOENT;
		default:
			return -EBUSY;
		}
	}
}

static int io_futex_rearm(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_futex *iof = io_kiocb_to_cmd(req, struct io_futex);
	struct io_ring_ctx *ctx = req->ctx;
	struct io_futex_ring *fr;
	struct io_futex_member *m;
	int ret;

	/* The slot's errors are a close's by file_index. */
	io_ring_submit_lock(ctx, issue_flags);
	fr = ctx->futex_ring;
	if (!fr) {
		ret = -ENXIO;
	} else if (iof->futex_nr >= fr->cap) {
		ret = -EINVAL;
	} else {
		m = fr->slots[array_index_nospec(iof->futex_nr, fr->cap)];
		ret = m ? io_futex_member_rearm(m) : -EBADF;
	}
	io_ring_submit_unlock(ctx, issue_flags);

	if (ret < 0)
		req_set_fail(req);
	io_req_set_res(req, ret, 0);
	return IOU_COMPLETE;
}

static int io_futex_wake_registered(struct io_kiocb *req)
{
	struct io_futex *iof = io_kiocb_to_cmd(req, struct io_futex);
	struct io_futex_table *table;
	struct io_futex_member *m;
	struct io_futex_node *node;
	unsigned int idx = iof->futex_nr;
	int ret = 0;

	/* The waker's store to the word, then the walk: pairs with arming. */
	smp_mb();
	rcu_read_lock();
	table = rcu_dereference(req->ctx->futex_table);
	node = NULL;
	if (table && idx < table->nr)
		node = rcu_dereference(table->nodes[array_index_nospec(idx,
								       table->nr)]);
	/* As for a fixed file: an empty or missing slot is -EBADF. */
	if (unlikely(!node)) {
		rcu_read_unlock();
		req_set_fail(req);
		io_req_set_res(req, -EBADF, 0);
		return IOU_COMPLETE;
	}
	hlist_for_each_entry_rcu(m, &node->members, mnode)
		if ((m->bitset & iof->futex_mask) && io_futex_notify(m))
			ret++;
	rcu_read_unlock();

	io_req_set_res(req, ret, 0);
	return IOU_COMPLETE;
}

static bool io_futex_reg_flags_valid(unsigned int flags)
{
	/* The word is found by its key; NUMA and MPOL pick a bucket. */
	return futex_flags_valid(flags) && !(flags & (FLAGS_NUMA | FLAGS_MPOL));
}

/* A registered word, or NULL for an empty slot (uaddr 0). */
static struct io_futex_node *
io_futex_reg_node(struct io_uring_futex_reg __user *ureg)
{
	struct io_uring_futex_reg reg;
	unsigned int flags;

	if (copy_from_user(&reg, ureg, sizeof(reg)))
		return ERR_PTR(-EFAULT);
	if (reg.resv || reg.resv2[0] || reg.resv2[1] ||
	    (reg.futex2_flags & ~FUTEX2_VALID_MASK))
		return ERR_PTR(-EINVAL);
	if (!reg.uaddr)
		return reg.futex2_flags ? ERR_PTR(-EINVAL) : NULL;
	flags = futex2_to_flags(reg.futex2_flags);
	if (!io_futex_reg_flags_valid(flags))
		return ERR_PTR(-EINVAL);
	return io_futex_node_get(u64_to_user_ptr(reg.uaddr), flags);
}

static void __io_futex_unregister(struct io_futex_table *table)
{
	unsigned int i;

	/* Each node is freed a grace period after its last put. */
	/* Unpublished, or unpublished by the caller under uring_lock. */
	for (i = 0; i < table->nr; i++) {
		struct io_futex_node *node;

		node = rcu_dereference_protected(table->nodes[i], 1);

		if (node)
			io_futex_node_put(node);
	}
	kvfree_rcu(table, rcu);
}

/* As IORING_REGISTER_FILES2: a table of rr.nr words, sparse if asked. */
int io_futex_register(struct io_ring_ctx *ctx, void __user *arg,
		      unsigned int size)
{
	struct io_uring_futex_reg __user *uregs;
	struct io_uring_rsrc_register rr;
	struct io_futex_table *table;
	unsigned int i;

	if (size != sizeof(rr))
		return -EINVAL;
	if (copy_from_user(&rr, arg, sizeof(rr)))
		return -EFAULT;
	if (!rr.nr || rr.resv2 || rr.tags ||
	    (rr.flags & ~IORING_RSRC_REGISTER_SPARSE))
		return -EINVAL;
	if ((rr.flags & IORING_RSRC_REGISTER_SPARSE) && rr.data)
		return -EINVAL;
	if (rr.nr > IO_FUTEX_REG_MAX)
		return -EMFILE;
	if (rcu_access_pointer(ctx->futex_table))
		return -EBUSY;

	table = kvzalloc(struct_size(table, nodes, rr.nr), GFP_KERNEL_ACCOUNT);
	if (!table)
		return -ENOMEM;
	table->nr = rr.nr;
	uregs = u64_to_user_ptr(rr.data);
	for (i = 0; uregs && i < rr.nr; i++) {
		struct io_futex_node *node = io_futex_reg_node(&uregs[i]);

		if (IS_ERR(node)) {
			__io_futex_unregister(table);
			return PTR_ERR(node);
		}
		RCU_INIT_POINTER(table->nodes[i], node);
	}
	/* Fills the slots, then publishes them: pairs with the wake. */
	rcu_assign_pointer(ctx->futex_table, table);
	return 0;
}

/*
 * As IORING_REGISTER_FILES_UPDATE2: replace up.nr slots from up.offset, and
 * clear those whose uaddr is 0.  Returns how many were updated.
 */
int io_futex_register_update(struct io_ring_ctx *ctx, void __user *arg,
			     unsigned int size)
{
	struct io_uring_futex_reg __user *uregs;
	struct io_uring_rsrc_update2 up;
	struct io_futex_table *table;
	unsigned int done, end;
	int err = 0;

	if (size != sizeof(up))
		return -EINVAL;
	if (copy_from_user(&up, arg, sizeof(up)))
		return -EFAULT;
	if (!up.nr || up.resv || up.resv2 || up.tags)
		return -EINVAL;
	if (check_add_overflow(up.offset, up.nr, &end))
		return -EOVERFLOW;
	table = rcu_dereference_protected(ctx->futex_table,
					  lockdep_is_held(&ctx->uring_lock));
	if (!table)
		return -ENXIO;
	if (end > table->nr)
		return -EINVAL;

	uregs = u64_to_user_ptr(up.data);
	for (done = 0; done < up.nr; done++) {
		unsigned int i = array_index_nospec(up.offset + done, table->nr);
		struct io_futex_node *node, *old;

		node = io_futex_reg_node(&uregs[done]);
		if (IS_ERR(node)) {
			err = PTR_ERR(node);
			break;
		}
		old = rcu_replace_pointer(table->nodes[i], node,
					  lockdep_is_held(&ctx->uring_lock));
		/* A wake still walking it holds RCU; it is freed after. */
		if (old)
			io_futex_node_put(old);
	}
	return done ? done : err;
}

int io_futex_unregister(struct io_ring_ctx *ctx)
{
	struct io_futex_table *table;

	table = rcu_replace_pointer(ctx->futex_table, NULL,
				    lockdep_is_held(&ctx->uring_lock));
	if (!table)
		return -ENXIO;
	__io_futex_unregister(table);
	return 0;
}

void io_futex_unregister_all(struct io_ring_ctx *ctx)
{
	struct io_futex_ring *fr = ctx->futex_ring;

	io_futex_unregister(ctx);
	/*
	 * Every persistent wait has ended, and the ring's exit has waited a
	 * grace period since, so no waker can still set a bit.
	 */
	if (fr) {
		ctx->futex_ring = NULL;
		kvfree(fr->slots);
		bitmap_free(fr->used);
		kvfree(fr);
	}
}

static bool io_futexv_claim(struct io_futexv_data *ifd)
{
	if (test_bit(0, &ifd->owned) || test_and_set_bit_lock(0, &ifd->owned))
		return false;
	return true;
}

static bool __io_futex_cancel(struct io_kiocb *req)
{
	struct io_futex *iof = io_kiocb_to_cmd(req, struct io_futex);

	if (iof->member)
		return io_futex_persistent_cancel(req);

	/* futex wake already done or in progress */
	if (req->opcode == IORING_OP_FUTEX_WAIT) {
		struct io_futex_data *ifd = req->async_data;

		if (!futex_unqueue(&ifd->q))
			return false;
		req->io_task_work.func = io_futex_complete;
	} else {
		struct io_futexv_data *ifd = req->async_data;

		if (!io_futexv_claim(ifd))
			return false;
		req->io_task_work.func = io_futexv_complete;
	}

	hlist_del_init(&req->hash_node);
	io_req_set_res(req, -ECANCELED, 0);
	io_req_task_work_add(req);
	return true;
}

int io_futex_cancel(struct io_ring_ctx *ctx, struct io_cancel_data *cd,
		    unsigned int issue_flags)
{
	return io_cancel_remove(ctx, cd, issue_flags, &ctx->futex_list, __io_futex_cancel);
}

bool io_futex_remove_all(struct io_ring_ctx *ctx, struct io_uring_task *tctx,
			 bool cancel_all)
{
	return io_cancel_remove_all(ctx, tctx, &ctx->futex_list, cancel_all, __io_futex_cancel);
}

int io_futex_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_futex *iof = io_kiocb_to_cmd(req, struct io_futex);
	u32 flags, uflags;

	if (unlikely(sqe->len))
		return -EINVAL;

	uflags = READ_ONCE(sqe->futex_flags);
	iof->iou_flags = uflags;
	iof->member = NULL;
	iof->uaddr = u64_to_user_ptr(READ_ONCE(sqe->addr));
	iof->futex_val = READ_ONCE(sqe->addr2);
	iof->futex_mask = READ_ONCE(sqe->addr3);
	flags = READ_ONCE(sqe->fd);

	if (req->opcode == IORING_OP_FUTEX_WAIT) {
		if (uflags & ~(IORING_FUTEX_PERSISTENT | IORING_FUTEX_REARM) ||
		    uflags == (IORING_FUTEX_PERSISTENT | IORING_FUTEX_REARM) ||
		    sqe->buf_index)
			return -EINVAL;
		/*
		 * A persistent wait's slot, as a direct descriptor's: index + 1,
		 * or IORING_FILE_INDEX_ALLOC.  A re-arm names the slot the same
		 * way, as a close does.
		 */
		iof->futex_nr = READ_ONCE(sqe->file_index);
		if (!(uflags & (IORING_FUTEX_PERSISTENT | IORING_FUTEX_REARM)) !=
		    !iof->futex_nr)
			return -EINVAL;
		if (uflags & IORING_FUTEX_REARM) {
			if (iof->uaddr || iof->futex_val || iof->futex_mask ||
			    flags || iof->futex_nr == IORING_FILE_INDEX_ALLOC)
				return -EINVAL;
			iof->futex_nr--;
			return 0;
		}
	} else if (sqe->file_index) {
		return -EINVAL;
	} else if (uflags) {
		/* Woken by slot: no word, no count, only the bitset. */
		if (uflags != IORING_FUTEX_REGISTERED || iof->uaddr ||
		    iof->futex_val || flags || !iof->futex_mask ||
		    iof->futex_mask > U32_MAX)
			return -EINVAL;
		iof->futex_nr = READ_ONCE(sqe->buf_index);
		return 0;
	} else if (sqe->buf_index) {
		return -EINVAL;
	}

	if (flags & ~FUTEX2_VALID_MASK)
		return -EINVAL;

	iof->futex_flags = futex2_to_flags(flags);
	if (!futex_flags_valid(iof->futex_flags))
		return -EINVAL;
	/*
	 * Persistent: links would wait for the wait's end, and a skipped
	 * success would be every notification.
	 */
	if ((uflags & IORING_FUTEX_PERSISTENT) &&
	    (!io_futex_reg_flags_valid(iof->futex_flags) ||
	     !(req->ctx->flags & IORING_SETUP_DEFER_TASKRUN) ||
	     (req->flags & (REQ_F_LINK | REQ_F_HARDLINK | REQ_F_CQE_SKIP))))
		return -EINVAL;

	if (!futex_validate_input(iof->futex_flags, iof->futex_val) ||
	    !futex_validate_input(iof->futex_flags, iof->futex_mask))
		return -EINVAL;

	return 0;
}

/*
 * A wait on a private futex does not block the submitter. Even a single
 * threaded task can go on and have the mm private hash installed by clone()
 * or prctl(), which does not pick up waiters from the global hash. Make
 * sure the private hash exists before the first waiter is queued.
 */
static int io_futex_prep_private(struct io_kiocb *req)
{
	int ret = futex_hash_allocate_default();

	if (unlikely(ret))
		return ret;

	/* inflight tracking only needed for mm private hash */
	io_req_track_inflight(req);
	return 0;
}

int io_futex_wait_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_futex *iof = io_kiocb_to_cmd(req, struct io_futex);
	int ret;

	ret = io_futex_prep(req, sqe);
	if (unlikely(ret))
		return ret;

	/*
	 * Persistent waits never enter the futex hash, and a re-arm doesn't
	 * set futex_flags at all.
	 */
	if (iof->iou_flags & (IORING_FUTEX_PERSISTENT | IORING_FUTEX_REARM))
		return 0;
	if (!(iof->futex_flags & FLAGS_SHARED))
		return io_futex_prep_private(req);
	return 0;
}

static void io_futex_wakev_fn(struct wake_q_head *wake_q, struct futex_q *q)
{
	struct io_kiocb *req = q->wake_data;
	struct io_futexv_data *ifd = req->async_data;

	if (!io_futexv_claim(ifd)) {
		__futex_wake_mark(q);
		return;
	}
	if (unlikely(!__futex_wake_mark(q)))
		return;

	io_req_set_res(req, 0, 0);
	req->io_task_work.func = io_futexv_complete;
	__io_req_task_work_add(req, IOU_F_TWQ_IN_WAKE);
}

int io_futexv_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_futex *iof = io_kiocb_to_cmd(req, struct io_futex);
	struct io_futexv_data *ifd;
	unsigned int i;
	int ret;

	/* No flags or mask supported for waitv */
	if (unlikely(sqe->fd || sqe->buf_index || sqe->file_index ||
		     sqe->addr2 || sqe->futex_flags || sqe->addr3))
		return -EINVAL;

	iof->uaddr = u64_to_user_ptr(READ_ONCE(sqe->addr));
	iof->member = NULL;
	iof->futex_nr = READ_ONCE(sqe->len);
	if (!iof->futex_nr || iof->futex_nr > FUTEX_WAITV_MAX)
		return -EINVAL;

	ifd = kzalloc_flex(struct io_futexv_data, futexv, iof->futex_nr,
			   GFP_KERNEL_ACCOUNT);
	if (!ifd)
		return -ENOMEM;

	ret = futex_parse_waitv(ifd->futexv, iof->uaddr, iof->futex_nr,
				io_futex_wakev_fn, req);
	if (ret) {
		kfree(ifd);
		return ret;
	}

	for (i = 0; i < iof->futex_nr; i++) {
		if (!(ifd->futexv[i].w.flags & FLAGS_SHARED)) {
			ret = io_futex_prep_private(req);
			if (ret) {
				kfree(ifd);
				return ret;
			}
			break;
		}
	}

	iof->futexv_unqueued = 0;
	req->flags |= REQ_F_ASYNC_DATA;
	req->async_data = ifd;
	return 0;
}

static void io_futex_wake_fn(struct wake_q_head *wake_q, struct futex_q *q)
{
	struct io_futex_data *ifd = container_of(q, struct io_futex_data, q);
	struct io_kiocb *req = ifd->req;

	if (unlikely(!__futex_wake_mark(q)))
		return;

	io_req_set_res(req, 0, 0);
	req->io_task_work.func = io_futex_complete;
	__io_req_task_work_add(req, IOU_F_TWQ_IN_WAKE);
}

int io_futexv_wait(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_futex *iof = io_kiocb_to_cmd(req, struct io_futex);
	struct io_futexv_data *ifd = req->async_data;
	struct io_ring_ctx *ctx = req->ctx;
	int ret, woken = -1;

	io_ring_submit_lock(ctx, issue_flags);

	ret = futex_wait_multiple_setup(ifd->futexv, iof->futex_nr, &woken);

	/*
	 * Error case, ret is < 0. Mark the request as failed.
	 */
	if (unlikely(ret < 0)) {
		io_ring_submit_unlock(ctx, issue_flags);
		req_set_fail(req);
		io_req_set_res(req, ret, 0);
		io_req_async_data_free(req);
		return IOU_COMPLETE;
	}

	/*
	 * 0 return means that we successfully setup the waiters, and that
	 * nobody triggered a wakeup while we were doing so. If the wakeup
	 * happened post setup, the task_work will be run post this issue and
	 * under the submission lock. 1 means We got woken while setting up,
	 * let that side do the completion. Note that
	 * futex_wait_multiple_setup() will have unqueued all the futexes in
	 * this case. Mark us as having done that already, since this is
	 * different from normal wakeup.
	 */
	if (!ret) {
		/*
		 * If futex_wait_multiple_setup() returns 0 for a
		 * successful setup, then the task state will not be
		 * runnable. This is fine for the sync syscall, as
		 * it'll be blocking unless we already got one of the
		 * futexes woken, but it obviously won't work for an
		 * async invocation. Mark us runnable again.
		 */
		__set_current_state(TASK_RUNNING);
		hlist_add_head(&req->hash_node, &ctx->futex_list);
		if (io_wq_cancel_seen(req, issue_flags))
			__io_futex_cancel(req);
	} else {
		iof->futexv_unqueued = 1;
		if (woken != -1)
			io_req_set_res(req, woken, 0);
	}

	io_ring_submit_unlock(ctx, issue_flags);
	return IOU_ISSUE_SKIP_COMPLETE;
}

int io_futex_wait(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_futex *iof = io_kiocb_to_cmd(req, struct io_futex);
	struct io_ring_ctx *ctx = req->ctx;
	struct io_futex_data *ifd = NULL;
	int ret;

	if (iof->iou_flags & IORING_FUTEX_REARM)
		return io_futex_rearm(req, issue_flags);

	if (!iof->futex_mask) {
		ret = -EINVAL;
		goto done;
	}

	if (iof->iou_flags & IORING_FUTEX_PERSISTENT)
		return io_futex_wait_persistent(req, issue_flags);

	io_ring_submit_lock(ctx, issue_flags);
	ifd = io_cache_alloc(&ctx->futex_cache, GFP_NOWAIT);
	if (!ifd) {
		ret = -ENOMEM;
		goto done_unlock;
	}

	req->flags |= REQ_F_ASYNC_DATA;
	req->async_data = ifd;
	ifd->q = futex_q_init;
	ifd->q.bitset = iof->futex_mask;
	ifd->q.wake = io_futex_wake_fn;
	ifd->req = req;

	ret = futex_wait_setup(iof->uaddr, iof->futex_val, iof->futex_flags,
			       &ifd->q, NULL, NULL);
	if (!ret) {
		hlist_add_head(&req->hash_node, &ctx->futex_list);
		if (io_wq_cancel_seen(req, issue_flags))
			__io_futex_cancel(req);
		io_ring_submit_unlock(ctx, issue_flags);

		return IOU_ISSUE_SKIP_COMPLETE;
	}

done_unlock:
	io_ring_submit_unlock(ctx, issue_flags);
done:
	if (ret < 0)
		req_set_fail(req);
	io_req_set_res(req, ret, 0);
	io_req_async_data_free(req);
	return IOU_COMPLETE;
}

int io_futex_wake(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_futex *iof = io_kiocb_to_cmd(req, struct io_futex);
	int ret;

	if (iof->iou_flags & IORING_FUTEX_REGISTERED)
		return io_futex_wake_registered(req);

	/*
	 * Strict flags - ensure that waking 0 futexes yields a 0 result.
	 * See commit 43adf8449510 ("futex: FLAGS_STRICT") for details.
	 */
	ret = futex_wake(iof->uaddr, FLAGS_STRICT | iof->futex_flags, NULL,
			 iof->futex_val, iof->futex_mask);
	if (ret < 0)
		req_set_fail(req);
	io_req_set_res(req, ret, 0);
	return IOU_COMPLETE;
}
