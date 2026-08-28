/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
#ifndef _RSEQ_ABI_H
#define _RSEQ_ABI_H

/*
 * rseq-abi.h
 *
 * Restartable sequences system call API
 *
 * Copyright (c) 2015-2022 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <linux/types.h>
#include <asm/byteorder.h>

enum rseq_abi_cpu_id_state {
	RSEQ_ABI_CPU_ID_UNINITIALIZED			= -1,
	RSEQ_ABI_CPU_ID_REGISTRATION_FAILED		= -2,
};

enum rseq_abi_flags {
	RSEQ_ABI_FLAG_UNREGISTER = (1 << 0),
};

enum rseq_abi_cs_flags_bit {
	RSEQ_ABI_CS_FLAG_NO_RESTART_ON_PREEMPT_BIT	= 0,
	RSEQ_ABI_CS_FLAG_NO_RESTART_ON_SIGNAL_BIT	= 1,
	RSEQ_ABI_CS_FLAG_NO_RESTART_ON_MIGRATE_BIT	= 2,
	RSEQ_ABI_CS_FLAG_SLICE_EXT_AVAILABLE_BIT	= 4,
	RSEQ_ABI_CS_FLAG_SLICE_EXT_ENABLED_BIT		= 5,
	RSEQ_ABI_CS_FLAG_RSEQ_OP_AVAILABLE_BIT		= 6,
	RSEQ_ABI_CS_FLAG_RSEQ_OP_ENABLED_BIT		= 7,
};

enum rseq_abi_cs_flags {
	RSEQ_ABI_CS_FLAG_NO_RESTART_ON_PREEMPT	=
		(1U << RSEQ_ABI_CS_FLAG_NO_RESTART_ON_PREEMPT_BIT),
	RSEQ_ABI_CS_FLAG_NO_RESTART_ON_SIGNAL	=
		(1U << RSEQ_ABI_CS_FLAG_NO_RESTART_ON_SIGNAL_BIT),
	RSEQ_ABI_CS_FLAG_NO_RESTART_ON_MIGRATE	=
		(1U << RSEQ_ABI_CS_FLAG_NO_RESTART_ON_MIGRATE_BIT),
	RSEQ_ABI_CS_FLAG_SLICE_EXT_AVAILABLE	=
		(1U << RSEQ_ABI_CS_FLAG_SLICE_EXT_AVAILABLE_BIT),
	RSEQ_ABI_CS_FLAG_SLICE_EXT_ENABLED	=
		(1U << RSEQ_ABI_CS_FLAG_SLICE_EXT_ENABLED_BIT),
	RSEQ_ABI_CS_FLAG_RSEQ_OP_AVAILABLE	=
		(1U << RSEQ_ABI_CS_FLAG_RSEQ_OP_AVAILABLE_BIT),
	RSEQ_ABI_CS_FLAG_RSEQ_OP_ENABLED	=
		(1U << RSEQ_ABI_CS_FLAG_RSEQ_OP_ENABLED_BIT),
};

/*
 * struct rseq_abi_cs is aligned on 4 * 8 bytes to ensure it is always
 * contained within a single cache-line. It is usually declared as
 * link-time constant data.
 */
struct rseq_abi_cs {
	/* Version of this structure. */
	__u32 version;
	/* enum rseq_abi_cs_flags */
	__u32 flags;
	__u64 start_ip;
	/* Offset from start_ip. */
	__u64 post_commit_offset;
	__u64 abort_ip;
} __attribute__((aligned(4 * sizeof(__u64))));

/**
 * rseq_abi_slice_ctrl - Time slice extension control structure
 * @all:	Compound value
 * @request:	Request for a time slice extension
 * @granted:	Granted time slice extension
 *
 * @request is set by user space and can be cleared by user space or kernel
 * space.  @granted is set and cleared by the kernel and must only be read
 * by user space.
 */
struct rseq_abi_slice_ctrl {
	union {
		__u32		all;
		struct {
			__u8	request;
			__u8	granted;
			__u16	__reserved;
		};
	};
};

union rseq_ptr {
	__u64 ptr64;

	/*
	 * The "arch" field provides architecture accessor for
	 * the ptr field based on architecture pointer size and
	 * endianness.
	 */
	struct {
#ifdef __LP64__
		__u64 ptr;
#elif defined(__BYTE_ORDER) ? (__BYTE_ORDER == __BIG_ENDIAN) : defined(__BIG_ENDIAN)
		__u32 padding;		/* Initialized to zero. */
		__u32 ptr;
#else
		__u32 ptr;
		__u32 padding;		/* Initialized to zero. */
#endif
	} arch;
};

/*
 * Maximum number of nodes walked in the rseq operation list.
 */
#define RSEQ_ABI_OP_LIST_LIMIT	2048

/*
 * enum rseq_abi_op_type - Type of an rseq operation
 * @RSEQ_ABI_OP_RESET:			Plain reset. Uses struct rseq_abi_op_reset.
 * @RSEQ_ABI_OP_RESET_WITH_STRIDE_CPUID:	Reset indexed by the current CPU ID.
 *					Uses struct rseq_abi_op_reset_with_stride.
 * @RSEQ_ABI_OP_RESET_WITH_STRIDE_MMCID:	Reset indexed by the current MM CID.
 *					Uses struct rseq_abi_op_reset_with_stride.
 */
enum rseq_abi_op_type {
	RSEQ_ABI_OP_RESET,
	RSEQ_ABI_OP_RESET_WITH_STRIDE_CPUID,
	RSEQ_ABI_OP_RESET_WITH_STRIDE_MMCID,
	RSEQ_ABI_OP_NR,
};

/*
 * struct rseq_abi_op_node - Common header linking an rseq operation into the list
 * @next:	Address of the next node. Owned by the kernel.
 * @prev:	Address of the previous node. Owned by the kernel.
 * @type:	Operation type. See enum rseq_abi_op_type.
 * @reserved:	Must be zero on registration.
 *
 * User space allocates the node, sets @type and zeroes @next, @prev and
 * @reserved before passing it to prctl(PR_RSEQ_OP, PR_RSEQ_OP_REGISTER, node).
 * The kernel owns @next and @prev for the lifetime of the registration and
 * links the node into a circular doubly-linked list anchored by an internal
 * sentinel in struct rseq_abi. User space must not touch @next or @prev while
 * the node is registered.
 */
struct rseq_abi_op_node {
	__u64 next;
	__u64 prev;
	struct {
		__u8  type; /* enum rseq_abi_op_type */
		__u8  reserved[7];
	};
};

/*
 * struct rseq_abi_op_reset - Reset one word to a value on return to user space
 * @node:	Operation list node.
 * @src:	Address of the source word, or 0 to reset @dst to zero.
 * @dst:	Address of the destination word.
 * @len:	Word length in bytes. Must be 4 or 8.
 */
struct rseq_abi_op_reset {
	struct rseq_abi_op_node	node;
	__u64			src;
	__u64			dst;
	__u32			len;
};

/*
 * struct rseq_abi_op_reset_with_stride - Reset one word in a strided array
 * @node:	Operation list node.
 * @src:	Address of the source word, or 0 to reset the slot to zero.
 * @dst:	Base address of the strided destination array.
 * @dst_stride:	Stride in bytes between consecutive array slots.
 * @len:	Word length in bytes. Must be 4 or 8.
 *
 * The destination slot is @dst + @dst_stride * index, where index is the
 * current CPU ID or MM CID depending on the operation type.
 */
struct rseq_abi_op_reset_with_stride {
	struct rseq_abi_op_node	node;
	__u64			src;
	__u64			dst;
	__u64			dst_stride;
	__u32			len;
};

/*
 * struct rseq_abi is aligned on 4 * 8 bytes to ensure it is always
 * contained within a single cache-line.
 *
 * A single struct rseq_abi per thread is allowed.
 */
struct rseq_abi {
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
	 */
	union rseq_ptr rseq_cs;

	/*
	 * Restartable sequences flags field.
	 *
	 * This field should only be updated by the thread which
	 * registered this data structure. Read by the kernel.
	 * Mainly used for single-stepping through rseq critical sections
	 * with debuggers.
	 *
	 * - RSEQ_ABI_CS_FLAG_NO_RESTART_ON_PREEMPT
	 *     Inhibit instruction sequence block restart on preemption
	 *     for this thread.
	 * - RSEQ_ABI_CS_FLAG_NO_RESTART_ON_SIGNAL
	 *     Inhibit instruction sequence block restart on signal
	 *     delivery for this thread.
	 * - RSEQ_ABI_CS_FLAG_NO_RESTART_ON_MIGRATE
	 *     Inhibit instruction sequence block restart on migration for
	 *     this thread.
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
	struct rseq_abi_slice_ctrl slice_ctrl;

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
		struct rseq_abi_op_node rseq_op_list;
		struct {
			__u64	op_used[2];
			__u64	reserved;
		};
	};

	/*
	 * Flexible array member at end of structure, after last feature field.
	 */
	char end[];
} __attribute__((aligned(256)));

#endif /* _RSEQ_ABI_H */
