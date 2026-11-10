/* SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR MIT */
#ifndef LINUX_IO_URING_OPAQUE_H
#define LINUX_IO_URING_OPAQUE_H

#include <linux/types.h>

/* IORING_REGISTER_OPAQUE_STORE, nr_args == 1. size includes reserved bytes. */
struct io_uring_opaque_register {
	__u32 size;
	__u32 flags;
	__s32 fd;
	__u32 store_id;
	__u64 config;
	__u32 config_size;
	__u32 __resv0;
	__u64 __resv[2];
};

#define IORING_OPAQUE_REG_IMPORT	(1U << 0)
#define IORING_OPAQUE_REG_EXPORT	(1U << 1)

/* config/config_size select a size-versioned configuration on creation. */
struct io_uring_opaque_config {
	__u64 hard_limit;
	__u64 compact_headroom;
	__u32 max_object_size;
	__u32 max_objects;
	__u32 max_streams;
	__u32 max_extents;
	__u32 flags;
	__u32 __resv0;
	__u64 __resv[2];
	__u64 high_watermark;
	__u64 low_watermark;
	__u64 reply_reserve;
	__u32 __resv2;
	__u32 __resv1;
};

/* IORING_OP_OPAQUE_OBJ: sqe->ioprio selects the operation. */
enum io_uring_opaque_op {
	/* Nonconsuming available window; len is capacity, never WAITALL. */
	IORING_OPAQUE_INSPECT,
	/* Complete-only object-ready CQE for the declared future stream range. */
	IORING_OPAQUE_RECV_OBJECT,
	IORING_OPAQUE_DISCARD,
	/* Copy from an already complete immutable object. */
	IORING_OPAQUE_READ_OBJECT,
	IORING_OPAQUE_FREE,
	IORING_OPAQUE_STAT,
	IORING_OPAQUE_OBJECT_STAT,
	IORING_OPAQUE_STREAM_STAT,
	IORING_OPAQUE_STREAM_CLOSE,
	IORING_OPAQUE_COMPACT,
	IORING_OPAQUE_SET_POLICY,
	/* Copy userspace bytes into a new complete immutable object. */
	IORING_OPAQUE_CREATE,
	/* Nonconsuming contiguous byte count, without copying payload. */
	IORING_OPAQUE_AVAILABLE,
};

/* IORING_OP_OPAQUE_OBJ_SEND: sqe->ioprio contains these flags. */
#define IORING_OPAQUE_SEND_LAST	(1U << 0)
/* addr points at len struct io_uring_opaque_send_entry, off is zero: one send, many ranges. */
#define IORING_OPAQUE_SEND_VECTOR	(1U << 1)
/* Entries of one vectored send, as many as SEND_ZC takes iovecs (UIO_MAXIOV). */
#define IORING_OPAQUE_SEND_VEC_MAX	1024

/* One range of a vectored send, named as addr, off and len name it in a single one. */
struct io_uring_opaque_send_entry {
	__u64 handle;
	__u64 off;
	__u32 len;
	__u32 __resv;
};

/* Optional SEND sqe->addr3 framing; size includes zero reserved extensions. */
struct io_uring_opaque_frame {
	__u32 size;
	__u32 flags;
	__u64 prefix;
	__u64 suffix;
	__u32 prefix_length;
	__u32 suffix_length;
	__u64 __resv[2];
};

/* Combined copied prefix and suffix, independently of object body length. */
#define IORING_OPAQUE_FRAME_MAX	(64U * 1024U)

#define IORING_OPAQUE_OBJECT_F_COMPACTED	(1U << 0)
#define IORING_OPAQUE_OBJECT_F_COMPACTING	(1U << 1)
#define IORING_OPAQUE_OBJECT_F_FORWARD_ONLY (1U << 2)
#define IORING_OPAQUE_STREAM_F_EOF		(1U << 0)

enum io_uring_opaque_memory_mode {
	IORING_OPAQUE_MEMORY_NORMAL,
	IORING_OPAQUE_MEMORY_LOWMEM,
};

struct io_uring_opaque_stat {
	__u32 size;
	__u32 __resv;
	__u64 backing_bytes;
	__u64 hard_limit;
	__u64 copied_bytes;
	__u64 compacted_bytes;
	__u64 compact_count;
	__u32 objects;
	__u32 streams;
	__u32 extents;
	__u32 memory_mode;
	__u64 framing_bytes;
	__u64 framing_copied_bytes;
};

struct io_uring_opaque_object_stat {
	__u32 size;
	__u32 flags;
	__u64 length;
	__u64 backing_bytes;
	__u32 extents;
	__u32 __resv;
};

struct io_uring_opaque_stream_stat {
	__u32 size;
	__u32 flags;
	__u64 rx_next;
	__u64 undecided_bytes;
	__s32 error;
	__u32 __resv;
};

struct io_uring_opaque_policy {
	__u32 flags;
	__u32 __resv0;
	__u64 bytes_per_second;
	__u64 burst_bytes;
	__u64 max_temporary_bytes;
	__u64 __resv[2];
};

#endif /* LINUX_IO_URING_OPAQUE_H */
