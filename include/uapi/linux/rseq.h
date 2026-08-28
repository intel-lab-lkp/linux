/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_RSEQ_H
#define _UAPI_LINUX_RSEQ_H

/*
 * linux/rseq.h
 *
 * Restartable sequences system call API
 *
 * Copyright (c) 2015-2018 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <linux/types.h>
#include <asm/byteorder.h>

/*
 * Maximum number of nodes walked in the rseq operation list.
 */
#define RSEQ_OP_LIST_LIMIT	2048

enum rseq_cpu_id_state {
	RSEQ_CPU_ID_UNINITIALIZED		= -1,
	RSEQ_CPU_ID_REGISTRATION_FAILED		= -2,
};

enum rseq_flags {
	RSEQ_FLAG_UNREGISTER			= (1 << 0),
	RSEQ_FLAG_SLICE_EXT_DEFAULT_ON		= (1 << 1),
};

enum rseq_cs_flags_bit {
	/* Historical and unsupported bits */
	RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT_BIT	= 0,
	RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL_BIT	= 1,
	RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE_BIT	= 2,
	/* (3) Intentional gap to keep new bits separate */

	/* User read only feature flags */
	RSEQ_CS_FLAG_SLICE_EXT_AVAILABLE_BIT	= 4,
	RSEQ_CS_FLAG_SLICE_EXT_ENABLED_BIT	= 5,
	RSEQ_CS_FLAG_RSEQ_OP_AVAILABLE_BIT	= 6,
	RSEQ_CS_FLAG_RSEQ_OP_ENABLED_BIT	= 7,
};

enum rseq_cs_flags {
	RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT	=
		(1U << RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT_BIT),
	RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL	=
		(1U << RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL_BIT),
	RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE	=
		(1U << RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE_BIT),

	RSEQ_CS_FLAG_SLICE_EXT_AVAILABLE	=
		(1U << RSEQ_CS_FLAG_SLICE_EXT_AVAILABLE_BIT),
	RSEQ_CS_FLAG_SLICE_EXT_ENABLED		=
		(1U << RSEQ_CS_FLAG_SLICE_EXT_ENABLED_BIT),
	RSEQ_CS_FLAG_RSEQ_OP_AVAILABLE		=
		(1U << RSEQ_CS_FLAG_RSEQ_OP_AVAILABLE_BIT),
	RSEQ_CS_FLAG_RSEQ_OP_ENABLED		=
		(1U << RSEQ_CS_FLAG_RSEQ_OP_ENABLED_BIT),
};

/*
 * struct rseq_cs is aligned on 4 * 8 bytes to ensure it is always
 * contained within a single cache-line. It is usually declared as
 * link-time constant data.
 */
struct rseq_cs {
	/* Version of this structure. */
	__u32 version;
	/* enum rseq_cs_flags */
	__u32 flags;
	__u64 start_ip;
	/* Offset from start_ip. */
	__u64 post_commit_offset;
	__u64 abort_ip;
} __attribute__((aligned(4 * sizeof(__u64))));

/**
 * rseq_slice_ctrl - Time slice extension control structure
 * @all:	Compound value
 * @request:	Request for a time slice extension
 * @granted:	Granted time slice extension
 *
 * @request is set by user space and can be cleared by user space or kernel
 * space.  @granted is set and cleared by the kernel and must only be read
 * by user space.
 */
struct rseq_slice_ctrl {
	union {
		__u32		all;
		struct {
			__u8	request;
			__u8	granted;
			__u16	__reserved;
		};
	};
};

/*
 * enum rseq_op_type - Type of an rseq operation
 * @RSEQ_OP_RESET:			Plain reset. Uses struct rseq_op_reset.
 * @RSEQ_OP_RESET_WITH_STRIDE_CPUID:	Reset indexed by the current CPU ID.
 *					Uses struct rseq_op_reset_with_stride.
 * @RSEQ_OP_RESET_WITH_STRIDE_MMCID:	Reset indexed by the current MM CID.
 *					Uses struct rseq_op_reset_with_stride.
 */
enum rseq_op_type {
	RSEQ_OP_RESET,
	RSEQ_OP_RESET_WITH_STRIDE_CPUID,
	RSEQ_OP_RESET_WITH_STRIDE_MMCID,
	RSEQ_OP_NR,
};

/*
 * struct rseq_op_node - Common header linking an rseq operation into the list
 * @next:	Address of the next node. Owned by the kernel.
 * @prev:	Address of the previous node. Owned by the kernel.
 * @type:	Operation type. See enum rseq_op_type.
 * @reserved:	Must be zero on registration.
 *
 * User space allocates the node, sets @type and zeroes @next, @prev and
 * @reserved before passing it to prctl(PR_RSEQ_OP, PR_RSEQ_OP_REGISTER, node).
 * The kernel owns @next and @prev for the lifetime of the registration and
 * links the node into a circular doubly-linked list anchored by an internal
 * sentinel in struct rseq. User space must not touch @next or @prev while the
 * node is registered.
 */
struct rseq_op_node {
	__u64 next;
	__u64 prev;
	struct {
		__u8  type; /* enum rseq_op_type */
		__u8  reserved[7];
	};
};

/*
 * struct rseq_op_reset - Reset one word to a value on return to user space
 * @node:	Operation list node.
 * @src:	Address of the source word, or 0 to reset @dst to zero.
 * @dst:	Address of the destination word.
 * @len:	Word length in bytes. Must be 4 or 8 (8 is 64-bit only).
 */
struct rseq_op_reset {
	struct rseq_op_node	node;
	__u64			src;
	__u64			dst;
	__u32			len;
};

/*
 * struct rseq_op_reset_with_stride - Reset one word in a strided array
 * @node:	Operation list node.
 * @src:	Address of the source word, or 0 to reset the slot to zero.
 * @dst:	Base address of the strided destination array.
 * @dst_stride:	Stride in bytes between consecutive array slots.
 * @len:	Word length in bytes. Must be 4 or 8 (8 is 64-bit only).
 *
 * The destination slot is @dst + @dst_stride * index, where index is the
 * current CPU ID or MM CID depending on the operation type.
 */
struct rseq_op_reset_with_stride {
	struct rseq_op_node	node;
	__u64			src;
	__u64			dst;
	__u64			dst_stride;
	__u32			len;
};

/*
 * The original size and alignment of the allocation for struct rseq is
 * 32 bytes.
 *
 * The allocation size needs to be greater or equal to
 * max(getauxval(AT_RSEQ_FEATURE_SIZE), 32), and the allocation needs to
 * be aligned on max(getauxval(AT_RSEQ_ALIGN), 32).
 *
 * As an alternative, userspace is allowed to use both the original size
 * and alignment of 32 bytes for backward compatibility.
 *
 * A single active struct rseq registration per thread is allowed.
 */
struct rseq {
	/*
	 * Restartable sequences cpu_id_start field. Updated by the
	 * kernel. Read by user-space with single-copy atomicity
	 * semantics. This field should only be read by the thread which
	 * registered this data structure. Aligned on 32-bit. Always
	 * contains a value in the range of possible CPUs, although the
	 * value may not be the actual current CPU (e.g. if rseq is not
	 * initialized). This CPU number value should always be compared
	 * against the value of the cpu_id field before performing a rseq
	 * commit or returning a value read from a data structure indexed
	 * using the cpu_id_start value.
	 */
	__u32 cpu_id_start;
	/*
	 * Restartable sequences cpu_id field. Updated by the kernel.
	 * Read by user-space with single-copy atomicity semantics. This
	 * field should only be read by the thread which registered this
	 * data structure. Aligned on 32-bit. Values
	 * RSEQ_CPU_ID_UNINITIALIZED and RSEQ_CPU_ID_REGISTRATION_FAILED
	 * have a special semantic: the former means "rseq uninitialized",
	 * and latter means "rseq initialization failed". This value is
	 * meant to be read within rseq critical sections and compared
	 * with the cpu_id_start value previously read, before performing
	 * the commit instruction, or read and compared with the
	 * cpu_id_start value before returning a value loaded from a data
	 * structure indexed using the cpu_id_start value.
	 */
	__u32 cpu_id;
	/*
	 * Restartable sequences rseq_cs field.
	 *
	 * Contains NULL when no critical section is active for the current
	 * thread, or holds a pointer to the currently active struct rseq_cs.
	 *
	 * Updated by user-space, which sets the address of the currently
	 * active rseq_cs at the beginning of assembly instruction sequence
	 * block, and set to NULL by the kernel when it restarts an assembly
	 * instruction sequence block, as well as when the kernel detects that
	 * it is preempting or delivering a signal outside of the range
	 * targeted by the rseq_cs. Also needs to be set to NULL by user-space
	 * before reclaiming memory that contains the targeted struct rseq_cs.
	 *
	 * Read and set by the kernel. Set by user-space with single-copy
	 * atomicity semantics. This field should only be updated by the
	 * thread which registered this data structure. Aligned on 64-bit.
	 *
	 * 32-bit architectures should update the low order bits of the
	 * rseq_cs field, leaving the high order bits initialized to 0.
	 */
	__u64 rseq_cs;

	/*
	 * Restartable sequences flags field.
	 *
	 * This field was initially intended to allow event masking for
	 * single-stepping through rseq critical sections with debuggers.
	 * The kernel does not support this anymore and the relevant bits
	 * are checked for being always false:
	 *	- RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT
	 *	- RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL
	 *	- RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE
	 *
	 * It is now used for feature status advertisement by the kernel.
	 * See: enum rseq_cs_flags_bit for further information.
	 */
	__u32 flags;

	/*
	 * Restartable sequences node_id field. Updated by the kernel. Read by
	 * user-space with single-copy atomicity semantics. This field should
	 * only be read by the thread which registered this data structure.
	 * Aligned on 32-bit. Contains the current NUMA node ID.
	 */
	__u32 node_id;

	/*
	 * Restartable sequences mm_cid field. Updated by the kernel. Read by
	 * user-space with single-copy atomicity semantics. This field should
	 * only be read by the thread which registered this data structure.
	 * Aligned on 32-bit. Contains the current thread's concurrency ID
	 * (allocated uniquely within a memory map).
	 */
	__u32 mm_cid;

	/*
	 * Time slice extension control structure. CPU local updates from
	 * kernel and user space.
	 */
	struct rseq_slice_ctrl slice_ctrl;

	/*
	 * Sentinel of the circular doubly-linked list of rseq operations
	 * registered via prctl(PR_RSEQ_OP, ...). Fully owned and maintained by
	 * the kernel: it is initialized to point to itself on registration and
	 * user space must never read or write it directly.
	 *
	 * The kernel only use next and prev from rseq_op_list.  The rest of the
	 * bytes are reserved for later usage and should be zeroed.
	 */
	union {
		struct rseq_op_node rseq_op_list;
		struct {
			__u64	op_used[2];
			__u64	reserved;
		};
	};

	/*
	 * Flexible array member at end of structure, after last feature field.
	 */
	char end[];
} __attribute__((aligned(32)));

#endif /* _UAPI_LINUX_RSEQ_H */
