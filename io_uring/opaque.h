/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IO_URING_OPAQUE_H
#define IO_URING_OPAQUE_H

#include <linux/io_uring_types.h>
#include <linux/rbtree.h>
#include <linux/uio.h>
#include <uapi/linux/io_uring/opaque.h>

struct io_opaque_store;
struct io_cancel_data;
struct io_uring_opaque_config;

#ifdef CONFIG_IO_URING_OPAQUE_OBJ
struct io_opaque_stream;
struct io_opaque_data;
struct io_opaque_tx;
struct io_opaque_vec;

enum io_opaque_phase {
	IO_OPAQUE_IDLE,
	IO_OPAQUE_RANGE_WAIT,
	IO_OPAQUE_BUDGET_POLL,
	IO_OPAQUE_SEND_WAIT,
	IO_OPAQUE_COPY_WORK,
	IO_OPAQUE_QUEUED,
};

struct io_opaque_req {
	struct io_kiocb *req;
	struct io_opaque_store *store;
	struct io_opaque_stream *stream;
	struct io_opaque_data *data;
	struct list_head wait;
	struct list_head cancel;
	u64 handle;
	u64 offset;
	u64 progress;
	u64 need_bytes;
	/* Unallocated CREATE charge; must coexist with the reserved slot. */
	u64 create_reserved;
	unsigned long retry_at;
	void __user *addr;
	u32 length;
	int result;
	u16 op;
	u16 collect_flags;
	enum io_opaque_phase phase;
	bool canceled;
	bool slot_reserved;
	bool consumed;
	bool resolved;
	bool budget_reported;
	union {
		struct {
			struct rb_node decision;
			struct list_head read;
			u32 slot;
		};
		struct {
			struct iov_iter iter;
			struct list_head send;
			struct io_opaque_tx *tx;
			void *framing;
			u64 prefix;
			u64 suffix;
			u32 frame_charge;
			u32 prefix_length;
			u32 suffix_length;
			u32 total_length;
			u32 msg_flags;
			u16 send_flags;
			bool frame_reserved;
		};
		struct {
			struct io_opaque_data *replacement;
			u64 reserved;
			u64 transient;
		};
		struct io_uring_opaque_policy policy;
	};
	/*
	 * Block of a vectored send, kept when the request returns to the ring's
	 * cache so that the next one has none to allocate. It is not part of the
	 * union and is the last member: allocation clears everything before it.
	 * Only a request with IORING_OPAQUE_SEND_VECTOR uses it, whatever it holds.
	 */
	struct io_opaque_vec *vec;
};

bool io_opaque_cache_init(struct io_ring_ctx *ctx);
void io_opaque_cache_free(struct io_ring_ctx *ctx);
struct io_opaque_store *io_opaque_alloc(struct io_ring_ctx *ctx,
					const struct io_uring_opaque_config *config);
void io_opaque_get(struct io_opaque_store *store);
void io_opaque_user_put(struct io_opaque_store *store);
void io_opaque_put(struct io_opaque_store *store);
int io_register_opaque(struct io_ring_ctx *ctx, void __user *arg);
void io_terminate_opaque(struct io_ring_ctx *ctx);
void io_unregister_opaque(struct io_ring_ctx *ctx);
void io_opaque_stop(struct io_opaque_store *store);
void io_opaque_free(struct io_opaque_store *store);
int io_opaque_recv_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_opaque_recv(struct io_kiocb *req, unsigned int issue_flags);
__poll_t io_opaque_collect_poll(struct io_kiocb *req);
int io_opaque_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_opaque_issue(struct io_kiocb *req, unsigned int issue_flags);
int io_opaque_send_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_opaque_send(struct io_kiocb *req, unsigned int issue_flags);
void io_opaque_cleanup(struct io_kiocb *req);
void io_opaque_fail(struct io_kiocb *req);
int io_opaque_cancel(struct io_ring_ctx *ctx, struct io_cancel_data *cd,
		     unsigned int issue_flags);
bool io_opaque_cancel_all(struct io_ring_ctx *ctx, struct io_uring_task *tctx,
			  bool cancel_all);
#else
static inline __poll_t io_opaque_collect_poll(struct io_kiocb *req)
{
	return 0;
}

static inline bool io_opaque_cache_init(struct io_ring_ctx *ctx)
{
	return false;
}

static inline void io_opaque_cache_free(struct io_ring_ctx *ctx)
{
}

static inline int io_register_opaque(struct io_ring_ctx *ctx, void __user *arg)
{
	return -EOPNOTSUPP;
}

static inline void io_terminate_opaque(struct io_ring_ctx *ctx)
{
}

static inline void io_unregister_opaque(struct io_ring_ctx *ctx)
{
}

static inline void io_opaque_cleanup(struct io_kiocb *req)
{
}

static inline int io_opaque_cancel(struct io_ring_ctx *ctx,
				   struct io_cancel_data *cd,
				   unsigned int issue_flags)
{
	return -ENOENT;
}

static inline bool io_opaque_cancel_all(struct io_ring_ctx *ctx,
					struct io_uring_task *tctx, bool cancel_all)
{
	return false;
}
#endif
#endif
