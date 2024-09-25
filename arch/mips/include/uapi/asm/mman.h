/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1995, 1999, 2002 by Ralf Baechle
 */
#ifndef _ASM_MMAN_H
#define _ASM_MMAN_H

/*			0x8		   reserved for PROT_EXEC_NOFLUSH */
#define PROT_SEM	0x10		/* page may be used for atomic ops */

/* 0x01 - 0x03 are defined in linux/mman.h */
#define MAP_TYPE	0x0f		/* Mask for type of mapping */
#define MAP_FIXED	0x10		/* Interpret addr exactly */

/* 0x20 through 0x100 originally reserved for other unix compat */
#define MAP_NORESERVE	0x0400		/* don't check for reservations */
#define MAP_ANONYMOUS	0x0800		/* don't use a file */
#define MAP_GROWSDOWN	0x1000		/* stack-like segment */
#define MAP_DENYWRITE	0x2000		/* ETXTBSY */
#define MAP_EXECUTABLE	0x4000		/* mark it as an executable */
#define MAP_LOCKED	0x8000		/* pages are locked */

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

#include <asm-generic/mman-common.h>

#endif /* _ASM_MMAN_H */
