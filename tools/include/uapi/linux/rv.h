/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * UAPI definitions for Runtime Verification (RV) monitors.
 *
 * All RV monitors that expose an ioctl self-instrumentation interface
 * share the magic byte RV_IOC_MAGIC ('r').
 *
 * Usage examples and design rationale are in:
 *   Documentation/trace/rv/monitor_tlob.rst
 */

#ifndef _UAPI_LINUX_RV_H
#define _UAPI_LINUX_RV_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Magic byte shared by all RV monitor ioctls. */
#define RV_IOC_MAGIC		'r'

/* Maximum monitor name length (including NUL terminator). */
#define RV_MONITOR_NAME_MAX	32

/* Generic /dev/rv ioctls (ioctl numbers 0–15 are reserved for the core) */

/**
 * struct rv_bind_args - arguments for RV_IOCTL_BIND_MONITOR
 * @monitor_name: NUL-terminated name of the monitor to bind (e.g. "tlob").
 */
struct rv_bind_args {
	char monitor_name[RV_MONITOR_NAME_MAX];
};

/*
 * RV_IOCTL_BIND_MONITOR - associate this fd with a specific RV monitor.
 *
 * Must be called once after open() and before any monitor-specific ioctl.
 *
 * Returns 0 on success.
 * Returns -EBUSY  if this fd is already bound to a monitor.
 * Returns -ENOENT if the requested monitor is not registered.
 * Returns -ENOMEM on allocation failure.
 */
#define RV_IOCTL_BIND_MONITOR	_IOW(RV_IOC_MAGIC, 0, struct rv_bind_args)

/* tlob: task latency over budget monitor (ioctl numbers 1–15) */

/**
 * struct tlob_start_args - arguments for TLOB_IOCTL_TRACE_START
 * @threshold_us: Total latency budget for this window, in microseconds.
 *               Must be greater than zero.  Both on-CPU and off-CPU time
 *               (including runqueue wait) count toward this budget.
 */
struct tlob_start_args {
	__u64 threshold_us;
};

/*
 * TLOB_IOCTL_TRACE_START - begin monitoring the calling task.
 *
 * Arms a per-task hrtimer for threshold_us microseconds (CLOCK_MONOTONIC,
 * so both on-CPU and off-CPU time count toward the budget).
 *
 * Returns 0 on success.
 * Returns -EEXIST if TRACE_START was already called on this fd.
 * Returns -ENOSPC if TLOB_MAX_MONITORED tasks are already being tracked.
 * Returns -ENOMEM on allocation failure.
 * Returns -ENODEV if the tlob monitor is not enabled.
 * Returns -ERANGE if threshold_us is 0.
 */
#define TLOB_IOCTL_TRACE_START	_IOW(RV_IOC_MAGIC, 1, struct tlob_start_args)

/*
 * TLOB_IOCTL_TRACE_STOP - end monitoring the calling task.
 *
 * Returns 0 if within budget.
 * Returns -EOVERFLOW if the latency budget was exceeded.
 * Returns -EINVAL if TLOB_IOCTL_TRACE_START was not called on this fd.
 *
 * poll/epoll: after TRACE_START the fd becomes readable (EPOLLIN) when the
 * budget is exceeded.  The caller may then issue TRACE_STOP to retrieve the
 * result, or simply close the fd to clean up.
 */
#define TLOB_IOCTL_TRACE_STOP	_IO(RV_IOC_MAGIC, 2)

#endif /* _UAPI_LINUX_RV_H */
