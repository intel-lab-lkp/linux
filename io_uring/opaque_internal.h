/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IO_URING_OPAQUE_INTERNAL_H
#define IO_URING_OPAQUE_INTERNAL_H

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/bvec.h>
#include <linux/net.h>
#include <linux/sizes.h>
#include <linux/workqueue.h>
#include <uapi/linux/io_uring/opaque.h>

#include "opaque.h"

#define IO_OPAQUE_READ_MAX	SZ_64K
#define IO_OPAQUE_RX_BATCH	SZ_256K
#define IO_OPAQUE_MAX_ORDER	(PAGE_SHIFT < 16 ? 16 - PAGE_SHIFT : 0)
#define IO_OPAQUE_ALLOC_RETRY_MS	50
#define IO_OPAQUE_WAKE_BATCH	32
#define IO_OPAQUE_INDEX_BITS	14
#define IO_OPAQUE_INDEX_MASK	((1U << IO_OPAQUE_INDEX_BITS) - 1)
/* Stream tokens carry the tag bit; object handles and generations never do. */
#define IO_OPAQUE_TOKEN_TAG	BIT_ULL(63)
#define IO_OPAQUE_GENERATION_MAX	(U64_MAX >> (IO_OPAQUE_INDEX_BITS + 1))
/* Unused per-socket TX queues kept per store; the oldest is freed beyond this. */
#define IO_OPAQUE_TX_IDLE_MAX	64
/* Backing segments all ranges of one vectored send may span; more is EMSGSIZE. */
#define IO_OPAQUE_SEND_VEC_SEGS	4096
/*
 * A version of more segments than this records where every this-many-th of
 * them starts, so that a range is found with a walk of about this many steps
 * and a binary search, however large the version is and wherever the walk
 * began. A smaller one is walked, which is as short.
 */
#define IO_OPAQUE_SEEK_STRIDE	16
/* Slots of the table of handles that a policy change walks while it holds tables. */
#define IO_OPAQUE_POLICY_CHUNK	256

/* What io_opaque_tx_join() returns besides an error. */
/* Queued behind another frame and parked, with its references taken. */
#define IO_OPAQUE_TX_WAIT	0
/* The head of its queue, with its references taken. */
#define IO_OPAQUE_TX_HEAD	1
/* The head of its queue, still to take its references. */
#define IO_OPAQUE_TX_SETTLE	2

struct io_opaque_backing {
	refcount_t refs;
	struct page *page;
	u64 charge;
	u32 filled;
	bool copied;
	bool accounted;
};

struct io_opaque_extent {
	struct list_head list;
	struct io_opaque_backing *backing;
	u64 start;
	u32 offset;
	u32 length;
	bool embedded;
};

/* One allocation for the common capture; splits allocate only an extent. */
struct io_opaque_chunk {
	struct io_opaque_backing backing;
	struct io_opaque_extent extent;
};

struct io_opaque_data {
	refcount_t refs;
	struct list_head extents;
	struct bio_vec *bvec;
	/*
	 * Where segments 0, IO_OPAQUE_SEEK_STRIDE, twice that and so on start,
	 * as bytes into the object, when it has more than that many segments;
	 * else NULL. It is allocated with bvec and freed with it.
	 */
	u64 *anchor;
	u64 length;
	u64 charge;
	u32 nr;
	bool dense;
	bool forward_only;
};

struct io_opaque_slot {
	struct io_opaque_data *data;
	u64 generation;
	bool reserved;
	bool compacting;
	struct list_head candidate;
	unsigned long born;
	unsigned long retry;
};

struct io_opaque_stream_slot {
	struct io_opaque_stream *stream;
	u64 generation;
};

struct io_opaque_tx {
	struct rb_node node;
	struct sock *sk;
	/* First-issue order, including pending admissions; only the head sends. */
	struct list_head requests;
	/* On store->txs_idle while no request is queued. */
	struct list_head idle;
	bool failed;
	/* A busy queue owns its socket identity independently of request files. */
	bool sk_held;
};

/*
 * Where one range of a vectored send starts in the version it pins. Admission
 * finds it under tables; the cursor is built from it once tables is dropped.
 */
struct io_opaque_vec_start {
	/* The first segment of the range. */
	u32 seg;
	/* Bytes of that segment before the range. */
	u32 skip;
};

/*
 * What a vectored send needs besides the request: its entries, copied at prep,
 * the version each pins, where each starts in it and the segments of the
 * combined cursor. One block holds the header and the four arrays, so that
 * growing it is one allocation.
 */
struct io_opaque_vec {
	/* Entries, from the SQE. */
	u32 nr;
	/*
	 * Entries of pin[] that admission has set. The version of each is kept
	 * alive by the send, by a reference of the entry's own or, unless the
	 * vector is LAST, by that of the entry before it when both name the
	 * same handle.
	 */
	u32 pinned;
	u32 cap_nr;
	u32 cap_segs;
	struct io_uring_opaque_send_entry *ent;
	struct io_opaque_data **pin;
	struct io_opaque_vec_start *start;
	struct bio_vec *bv;
};

struct io_opaque_stream {
	refcount_t refs;
	/* Consumed only on entry to a collector pass, never by native poll. */
	atomic_t collect_ready;
	/* Extents, range decisions and collector state. */
	struct mutex lock;
	struct io_opaque_store *store;
	struct file *file;
	struct sock_rx_owner owner;
	struct list_head extents;
	struct list_head reads;
	struct rb_root_cached decisions;
	struct io_opaque_req *collector;
	u64 token;
	u64 next;
	u64 undecided;
	u64 need_bytes;
	int error;
	bool eof;
	bool closed;
	/* Collection pass may not take undecided bytes off the socket. */
	bool hold_undecided;
};

struct io_opaque_store {
	refcount_t refs;
	refcount_t users;
	struct io_uring_opaque_config config;
	/*
	 * Handles and candidates; never held during copies or I/O, and for a time
	 * that the request bounds: what admission does under it is a lookup per
	 * entry, the segments its ranges span up to the limit, and for each range
	 * a walk that the version's anchors keep to about two strides and a
	 * binary search. Attach and the admission of a send take it with the
	 * socket owned, so nothing may take a socket lock while holding it, or
	 * wait for anything that holds one for long.
	 */
	struct mutex tables;
	struct io_opaque_slot *objects;
	struct io_opaque_stream_slot *streams;
	unsigned long *object_used;
	unsigned long *stream_used;
	u32 object_cursor;
	u32 stream_cursor;
	u32 nr_objects;
	u32 nr_streams;
	struct rb_root txs;
	struct list_head txs_idle;
	u32 nr_txs_idle;
	struct user_struct *user;
	struct mm_struct *mm;
	struct mem_cgroup *memcg;
	struct obj_cgroup *objcg;
	atomic64_t bytes;
	atomic64_t copied;
	atomic64_t compacted;
	atomic64_t compactions;
	atomic64_t framing;
	atomic64_t framing_copied;
	/* Framing held in the reply reserve, outside bytes. */
	atomic64_t reserve_framing;
	/*
	 * Charged in bytes but not payload held: unpublished compaction copies
	 * and framing in ordinary capacity. Watermarks subtract it. Changes
	 * that move bytes and transient together are one seqcount write.
	 */
	atomic64_t transient;
	/* Serializes transient_seq writers. */
	spinlock_t transient_lock;
	seqcount_spinlock_t transient_seq;
	atomic_t extents;
	atomic_t memory_mode;
	/* Budget waiters and remote wakeup/cancellation arbitration. */
	spinlock_t wait_lock;
	/* TX queues: the tree, every queue's requests and failed flag. Never with tables. */
	spinlock_t tx_lock;
	struct list_head budget_waits;
	struct list_head copies;
	/* One dispatcher serializes manual and automatic copies. */
	struct delayed_work compact_work;
	struct delayed_work retry_work;
	struct list_head candidates;
	struct io_uring_opaque_policy policy;
	/* Counts policy changes; a walk of the table that is overtaken by another stops. */
	u32 policy_seq;
	u64 tokens;
	unsigned long token_time;
	unsigned long auto_at;
	int dead;
};

bool io_opaque_charge(struct io_opaque_store *store, u64 bytes, bool rx);
void io_opaque_uncharge(struct io_opaque_store *store, u64 bytes);
bool io_opaque_charge_transient(struct io_opaque_store *store, u64 bytes, u64 limit);
void io_opaque_release_transient(struct io_opaque_store *store, u64 uncharge,
				 u64 transient);
bool io_opaque_charge_frame(struct io_opaque_store *store, u64 bytes, bool *reserved);
void io_opaque_uncharge_frame(struct io_opaque_store *store, u64 bytes, bool reserved);
int io_opaque_memory_mode(struct io_opaque_store *store);

static inline u64 io_opaque_rx_limit(struct io_opaque_store *store)
{
	return store->config.hard_limit - store->config.compact_headroom -
	       store->config.reply_reserve;
}

struct io_opaque_extent *io_opaque_extent_new(struct io_opaque_store *store,
					      struct page *page, u32 offset,
					      u32 length, u64 start,
					      bool reserved);
struct io_opaque_data *io_opaque_data_alloc(void);
void io_opaque_data_put(struct io_opaque_store *store,
			struct io_opaque_data *data);
int io_opaque_vector(struct io_opaque_data *data);
struct page *io_opaque_owned_page(unsigned int *order);
struct page *io_opaque_dest_page(struct io_opaque_store *store, u64 pages_left,
				 unsigned int *order);
u64 io_opaque_handle(u32 index, u64 generation);
struct io_opaque_slot *io_opaque_lookup(struct io_opaque_store *store,
					u64 handle);
void io_opaque_wait(struct io_opaque_req *op, enum io_opaque_phase phase);
void io_opaque_queue_ready(struct io_opaque_req *op, int result);
#endif
