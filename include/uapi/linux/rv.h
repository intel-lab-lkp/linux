/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * UAPI definitions for Runtime Verification (RV) monitors.
 *
 * All RV monitors that expose an ioctl self-instrumentation interface
 * share the magic byte RV_IOC_MAGIC (0xB9), registered in
 * Documentation/userspace-api/ioctl/ioctl-number.rst.
 *
 * A single /dev/rv misc device serves as the entry point.  ioctl numbers
 * encode both the monitor identity and the operation:
 *
 *   0x01 - 0x1F  tlob (task latency over budget)
 *   0x20 - 0x3F  reserved for future RV monitors
 *
 * Usage examples and design rationale are in:
 *   Documentation/trace/rv/monitor_tlob.rst
 */

#ifndef _UAPI_LINUX_RV_H
#define _UAPI_LINUX_RV_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Magic byte shared by all RV monitor ioctls. */
#define RV_IOC_MAGIC	0xB9

/* -----------------------------------------------------------------------
 * tlob: task latency over budget monitor  (nr 0x01 - 0x1F)
 * -----------------------------------------------------------------------
 */

/**
 * struct tlob_start_args - arguments for TLOB_IOCTL_TRACE_START
 * @threshold_us: Latency budget for this critical section, in microseconds.
 *               Must be greater than zero.
 * @tag:         Opaque 64-bit cookie supplied by the caller.  Echoed back
 *               verbatim in the tlob_budget_exceeded ftrace event and in any
 *               tlob_event record delivered via @notify_fd.  Use it to identify
 *               which code region triggered a violation when the same thread
 *               monitors multiple regions sequentially.  Set to 0 if not
 *               needed.
 * @notify_fd:   File descriptor that will receive a tlob_event record on
 *               violation.  Must refer to an open /dev/rv fd.  May equal
 *               the calling fd (self-notification, useful for retrieving the
 *               on_cpu_us / off_cpu_us breakdown after TRACE_STOP returns
 *               -EOVERFLOW).  Set to -1 to disable fd notification; in that
 *               case violations are only signalled via the TRACE_STOP return
 *               value and the tlob_budget_exceeded ftrace event.
 * @flags:       Must be 0.  Reserved for future extensions.
 */
struct tlob_start_args {
	__u64 threshold_us;
	__u64 tag;
	__s32 notify_fd;
	__u32 flags;
};

/**
 * struct tlob_event - one budget-exceeded event
 *
 * Consumed by read() on the notify_fd registered at TLOB_IOCTL_TRACE_START.
 * Each record describes a single budget exceedance for one task.
 *
 * @tid:          Thread ID (task_pid_vnr) of the violating task.
 * @threshold_us: Budget that was exceeded, in microseconds.
 * @on_cpu_us:    Cumulative on-CPU time at violation time, in microseconds.
 * @off_cpu_us:   Cumulative off-CPU (scheduling + I/O wait) time at
 *               violation time, in microseconds.
 * @switches:     Number of context switches since TRACE_START.
 * @state:        DA state at violation: 1 = on_cpu, 0 = off_cpu.
 * @tag:          Cookie from tlob_start_args.tag; for the tracefs uprobe path
 *               this is the offset_start value.  Zero when not set.
 */
struct tlob_event {
	__u32 tid;
	__u32 pad;
	__u64 threshold_us;
	__u64 on_cpu_us;
	__u64 off_cpu_us;
	__u32 switches;
	__u32 state;   /* 1 = on_cpu, 0 = off_cpu */
	__u64 tag;
};

/**
 * struct tlob_mmap_page - control page for the mmap'd violation ring buffer
 *
 * Mapped at offset 0 of the mmap region returned by mmap(2) on a /dev/rv fd.
 * The data array of struct tlob_event records begins at offset @data_offset
 * (always one page from the mmap base; use this field rather than hard-coding
 * PAGE_SIZE so the code remains correct across architectures).
 *
 * Ring layout:
 *
 *   mmap base + 0             : struct tlob_mmap_page  (one page)
 *   mmap base + data_offset   : struct tlob_event[capacity]
 *
 * The mmap length determines the ring capacity.  Compute it as:
 *
 *   raw    = sysconf(_SC_PAGESIZE) + capacity * sizeof(struct tlob_event)
 *   length = (raw + sysconf(_SC_PAGESIZE) - 1) & ~(sysconf(_SC_PAGESIZE) - 1)
 *
 * i.e. round the raw byte count up to the next page boundary before
 * passing it to mmap(2).  The kernel requires a page-aligned length.
 * capacity must be a power of 2.  Read @capacity after a successful
 * mmap(2) for the actual value.
 *
 * Producer/consumer ordering contract:
 *
 *   Kernel (producer):
 *     data[data_head & (capacity - 1)] = event;
 *     // pairs with load-acquire in userspace:
 *     smp_store_release(&page->data_head, data_head + 1);
 *
 *   Userspace (consumer):
 *     // pairs with store-release in kernel:
 *     head = __atomic_load_n(&page->data_head, __ATOMIC_ACQUIRE);
 *     for (tail = page->data_tail; tail != head; tail++)
 *         handle(&data[tail & (capacity - 1)]);
 *     __atomic_store_n(&page->data_tail, tail, __ATOMIC_RELEASE);
 *
 * @data_head and @data_tail are monotonically increasing __u32 counters
 * in units of records.  Unsigned 32-bit wrap-around is handled correctly
 * by modular arithmetic; the ring is full when
 * (data_head - data_tail) == capacity.
 *
 * When the ring is full the kernel drops the incoming record and increments
 * @dropped.  The consumer should check @dropped periodically to detect loss.
 *
 * read() and mmap() share the same ring buffer.  Do not use both
 * simultaneously on the same fd.
 *
 * @data_head:   Next write slot index.  Updated by the kernel with
 *               store-release ordering.  Read by userspace with load-acquire.
 * @data_tail:   Next read slot index.  Updated by userspace.  Read by the
 *               kernel to detect overflow.
 * @capacity:    Actual ring capacity in records (power of 2).  Written once
 *               by the kernel at mmap time; read-only for userspace thereafter.
 * @version:     Ring buffer ABI version; currently 1.
 * @data_offset: Byte offset from the mmap base to the data array.
 *               Always equal to sysconf(_SC_PAGESIZE) on the running kernel.
 * @record_size: sizeof(struct tlob_event) as seen by the kernel.  Verify
 *               this matches userspace's sizeof before indexing the array.
 * @dropped:     Number of events dropped because the ring was full.
 *               Monotonically increasing; read with __ATOMIC_RELAXED.
 */
struct tlob_mmap_page {
	__u32  data_head;
	__u32  data_tail;
	__u32  capacity;
	__u32  version;
	__u32  data_offset;
	__u32  record_size;
	__u64  dropped;
};

/*
 * TLOB_IOCTL_TRACE_START - begin monitoring the calling task.
 *
 * Arms a per-task hrtimer for threshold_us microseconds.  If args.notify_fd
 * is >= 0, a tlob_event record is pushed into that fd's ring buffer on
 * violation in addition to the tlob_budget_exceeded ftrace event.
 * args.notify_fd == -1 disables fd notification.
 *
 * Violation records are consumed by read() on the notify_fd (blocking or
 * non-blocking depending on O_NONBLOCK).  On violation, TLOB_IOCTL_TRACE_STOP
 * also returns -EOVERFLOW regardless of whether notify_fd is set.
 *
 * args.flags must be 0.
 */
#define TLOB_IOCTL_TRACE_START		_IOW(RV_IOC_MAGIC, 0x01, struct tlob_start_args)

/*
 * TLOB_IOCTL_TRACE_STOP - end monitoring the calling task.
 *
 * Returns 0 if within budget, -EOVERFLOW if the budget was exceeded.
 */
#define TLOB_IOCTL_TRACE_STOP		_IO(RV_IOC_MAGIC,  0x02)

#endif /* _UAPI_LINUX_RV_H */
