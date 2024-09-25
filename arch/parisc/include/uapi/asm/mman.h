/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef __PARISC_MMAN_H__
#define __PARISC_MMAN_H__

/* 0x01 - 0x03 are defined in linux/mman.h */
#define MAP_TYPE	0x2b		/* Mask for type of mapping, includes bits 0x08 and 0x20 */
#define MAP_FIXED	0x04		/* Interpret addr exactly */
#define MAP_ANONYMOUS	0x10		/* don't use a file */

#define MAP_DENYWRITE	0x0800		/* ETXTBSY */
#define MAP_EXECUTABLE	0x1000		/* mark it as an executable */
#define MAP_LOCKED	0x2000		/* pages are locked */
#define MAP_NORESERVE	0x4000		/* don't check for reservations */
#define MAP_GROWSDOWN	0x8000		/* stack-like segment */

#define MAP_POPULATE		0x010000	/* populate (prefault) pagetables */
#define MAP_NONBLOCK		0x020000	/* do not block on IO */
#define MAP_STACK		0x040000	/* give out an address that is best suited for process/thread stacks */
#define MAP_HUGETLB		0x080000	/* create a huge page mapping */
/* MAP_SYNC not supported */
#define MAP_FIXED_NOREPLACE	0x100000	/* MAP_FIXED which doesn't unmap underlying mapping */

/*
 * Flags for mlockall
 */
#define MCL_CURRENT	1		/* lock all current mappings */
#define MCL_FUTURE	2		/* lock all future mappings */
#define MCL_ONFAULT	4		/* lock all pages that are faulted in */

/*
 * Flags for msync, order is different from all others
 */
#define MS_SYNC		1		/* synchronous memory sync */
#define MS_ASYNC	2		/* sync memory asynchronously */
#define MS_INVALIDATE	4		/* invalidate the caches */

#include <asm-generic/mman-common.h>

#endif /* __PARISC_MMAN_H__ */
