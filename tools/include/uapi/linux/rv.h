/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * UAPI definitions for Runtime Verification (RV) monitors.
 *
 * This is a tools-friendly copy of include/uapi/linux/rv.h.
 * Keep in sync with the kernel header.
 */

#ifndef _UAPI_LINUX_RV_H
#define _UAPI_LINUX_RV_H

#include <linux/types.h>
#include <sys/ioctl.h>

/* Magic byte shared by all RV monitor ioctls. */
#define RV_IOC_MAGIC	0xB9

/* -----------------------------------------------------------------------
 * tlob: task latency over budget monitor  (nr 0x01 - 0x1F)
 * -----------------------------------------------------------------------
 */

struct tlob_start_args {
	__u64 threshold_us;
	__u64 tag;
	__s32 notify_fd;
	__u32 flags;
};

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

struct tlob_mmap_page {
	__u32  data_head;
	__u32  data_tail;
	__u32  capacity;
	__u32  version;
	__u32  data_offset;
	__u32  record_size;
	__u64  dropped;
};

#define TLOB_IOCTL_TRACE_START	_IOW(RV_IOC_MAGIC, 0x01, struct tlob_start_args)
#define TLOB_IOCTL_TRACE_STOP	_IO(RV_IOC_MAGIC,  0x02)

#endif /* _UAPI_LINUX_RV_H */
