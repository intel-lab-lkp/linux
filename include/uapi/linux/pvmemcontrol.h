/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Userspace interface for /dev/pvmemcontrol
 * pvmemcontrol Guest Memory Service Module
 *
 * Copyright (c) 2024, Google LLC.
 * Yuanchu Xie <yuanchu@google.com>
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#ifndef _UAPI_PVMEMCONTROL_H
#define _UAPI_PVMEMCONTROL_H

#include <linux/wait.h>
#include <linux/types.h>
#include <asm/param.h>

/* Contains the function code and arguments for specific function */
struct pvmemcontrol_vmm_call {
	__u64 func_code;	/* pvmemcontrol set function code */
	__u64 addr;		/* hyper. page size aligned guest phys. addr */
	__u64 length;		/* hyper. page size aligned length */
	__u64 arg;		/* function code specific argument */
};

/* Is filled on return to guest from VMM from most function calls */
struct pvmemcontrol_vmm_ret {
	__u32 ret_errno;	/* on error, value of errno */
	__u32 ret_code;		/* pvmemcontrol internal error code, on success 0 */
	__u64 ret_value;	/* return value from the function call */
	__u64 arg0;		/* major version for func_code INFO */
	__u64 arg1;		/* minor version for func_code INFO */
};

struct pvmemcontrol_buf {
	union {
		struct pvmemcontrol_vmm_call call;
		struct pvmemcontrol_vmm_ret ret;
	};
};

/* The ioctl type, documented in ioctl-number.rst */
#define PVMEMCONTROL_IOCTL_TYPE		0xDA

#define PVMEMCONTROL_IOCTL_VMM _IOWR(PVMEMCONTROL_IOCTL_TYPE, 0x00, struct pvmemcontrol_buf)

/*
 * Returns the host page size in ret_value.
 * major version in arg0.
 * minor version in arg1.
 */
#define PVMEMCONTROL_INFO		0

/* Pvmemcontrol calls, pvmemcontrol_vmm_return is returned */
#define PVMEMCONTROL_DONTNEED		1 /* madvise(addr, len, MADV_DONTNEED); */
#define PVMEMCONTROL_REMOVE		2 /* madvise(addr, len, MADV_MADV_REMOVE); */
#define PVMEMCONTROL_FREE		3 /* madvise(addr, len, MADV_FREE); */
#define PVMEMCONTROL_PAGEOUT		4 /* madvise(addr, len, MADV_PAGEOUT); */
#define PVMEMCONTROL_DONTDUMP		5 /* madvise(addr, len, MADV_DONTDUMP); */

/* prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, addr, len, arg) */
#define PVMEMCONTROL_SET_VMA_ANON_NAME  6

#define PVMEMCONTROL_MLOCK		7 /* mlock2(addr, len, 0) */
#define PVMEMCONTROL_MUNLOCK		8 /* munlock(addr, len) */

#define PVMEMCONTROL_MPROTECT_NONE	9 /* mprotect(addr, len, PROT_NONE) */
#define PVMEMCONTROL_MPROTECT_R	       10 /* mprotect(addr, len, PROT_READ) */
#define PVMEMCONTROL_MPROTECT_W	       11 /* mprotect(addr, len, PROT_WRITE) */
/* mprotect(addr, len, PROT_READ | PROT_WRITE) */
#define PVMEMCONTROL_MPROTECT_RW       12

#define PVMEMCONTROL_MERGEABLE         13 /* madvise(addr, len, MADV_MERGEABLE); */
#define PVMEMCONTROL_UNMERGEABLE       14 /* madvise(addr, len, MADV_UNMERGEABLE); */

#endif /* _UAPI_PVMEMCONTROL_H */
