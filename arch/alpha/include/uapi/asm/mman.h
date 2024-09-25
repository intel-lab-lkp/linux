/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef __ALPHA_MMAN_H__
#define __ALPHA_MMAN_H__

/* 0x01 - 0x03 are defined in linux/mman.h */
#define MAP_TYPE	0x0f		/* Mask for type of mapping (OSF/1 is _wrong_) */
#define MAP_FIXED	0x100		/* Interpret addr exactly */
#define MAP_ANONYMOUS	0x10		/* don't use a file */

/* 0x200 through 0x800 originally for OSF-1 compat */
#define MAP_GROWSDOWN	0x1000		/* stack-like segment */
#define MAP_DENYWRITE	0x2000		/* ETXTBSY */
#define MAP_EXECUTABLE	0x4000		/* mark it as an executable */
#define MAP_LOCKED	0x8000		/* pages are locked */
#define MAP_NORESERVE	0x10000		/* don't check for reservations */

#define MAP_POPULATE		0x020000	/* populate (prefault) pagetables */
#define MAP_NONBLOCK		0x040000	/* do not block on IO */
#define MAP_STACK		0x080000	/* give out an address that is best suited for process/thread stacks */
#define MAP_HUGETLB		0x100000	/* create a huge page mapping */
/* MAP_SYNC not supported */
#define MAP_FIXED_NOREPLACE	0x200000/* MAP_FIXED which doesn't unmap underlying mapping */

/*
 * Flags for mlockall
 */
#define MCL_CURRENT	 8192		/* lock all currently mapped pages */
#define MCL_FUTURE	16384		/* lock all additions to address space */
#define MCL_ONFAULT	32768		/* lock all pages that are faulted in */

/*
 * Flags for msync, order is different from all others
 */
#define MS_ASYNC	1		/* sync memory asynchronously */
#define MS_SYNC		2		/* synchronous memory sync */
#define MS_INVALIDATE	4		/* invalidate the caches */

/*
 * Flags for madvise, 1 through 3 are normal
 */
/* originally MADV_SPACEAVAIL 5 */
#define MADV_DONTNEED	6		/* don't need these pages */

#include <asm-generic/mman-common.h>

#endif /* __ALPHA_MMAN_H__ */
