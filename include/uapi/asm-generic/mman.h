/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef __ASM_GENERIC_MMAN_H
#define __ASM_GENERIC_MMAN_H

#include <asm-generic/mman-common.h>

/*
 * 0x01 - 0x03 are defined in linux/mman.h
 * 0x04 - 0x200000 are architecture specific
 * 0x200000 - 0x2000000 are available for common assignments in linux/mman.h
 * 0x4000000 - 0x80000000 are used for hugepage encodings
 */
#define MAP_TYPE	0x0f		/* Mask for type of mapping */
#define MAP_FIXED	0x10		/* Interpret addr exactly */
#define MAP_ANONYMOUS	0x20		/* don't use a file */

#define MAP_GROWSDOWN	0x0100		/* stack-like segment */
#define MAP_DENYWRITE	0x0800		/* ETXTBSY */
#define MAP_EXECUTABLE	0x1000		/* mark it as an executable */
#define MAP_LOCKED	0x2000		/* pages are locked */
#define MAP_NORESERVE	0x4000		/* don't check for reservations */

#define MAP_POPULATE		0x008000	/* populate (prefault) pagetables */
#define MAP_NONBLOCK		0x010000	/* do not block on IO */
#define MAP_STACK		0x020000	/* give out an address that is best suited for process/thread stacks */
#define MAP_HUGETLB		0x040000	/* create a huge page mapping */
#define MAP_SYNC		0x080000 /* perform synchronous page faults for the mapping */
#define MAP_FIXED_NOREPLACE	0x100000	/* MAP_FIXED which doesn't unmap underlying mapping */

/*
 * Bits [26:31] are reserved, see asm-generic/hugetlb_encode.h
 * for MAP_HUGETLB usage
 */

#define MCL_CURRENT	1		/* lock all current mappings */
#define MCL_FUTURE	2		/* lock all future mappings */
#define MCL_ONFAULT	4		/* lock all pages that are faulted in */

#endif /* __ASM_GENERIC_MMAN_H */
