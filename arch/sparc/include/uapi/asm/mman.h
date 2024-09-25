/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI__SPARC_MMAN_H__
#define _UAPI__SPARC_MMAN_H__

#include <asm-generic/mman-common.h>

/* SunOS'ified... */

#define PROT_ADI	0x10		/* ADI enabled */

/* 0x01 - 0x03 are defined in linux/mman.h */
#define MAP_TYPE	0x0f		/* Mask for type of mapping */
#define MAP_FIXED	0x10		/* Interpret addr exactly */
#define MAP_ANONYMOUS	0x20		/* don't use a file */

#define MAP_RENAME      MAP_ANONYMOUS   /* In SunOS terminology */
#define MAP_NORESERVE   0x40            /* don't reserve swap pages */
#define MAP_INHERIT     0x80            /* SunOS doesn't do this, but... */
#define MAP_LOCKED      0x100           /* lock the mapping */
#define _MAP_NEW        0x80000000      /* Binary compatibility is fun... */

#define MAP_GROWSDOWN	0x0200		/* stack-like segment */
#define MAP_DENYWRITE	0x0800		/* ETXTBSY */
#define MAP_EXECUTABLE	0x1000		/* mark it as an executable */

#define MAP_POPULATE		0x008000	/* populate (prefault) pagetables */
#define MAP_NONBLOCK		0x010000	/* do not block on IO */
#define MAP_STACK		0x020000	/* give out an address that is best suited for process/thread stacks */
#define MAP_HUGETLB		0x040000	/* create a huge page mapping */
#define MAP_SYNC		0x080000 /* perform synchronous page faults for the mapping */
#define MAP_FIXED_NOREPLACE	0x100000	/* MAP_FIXED which doesn't unmap underlying mapping */

#define MCL_CURRENT     0x2000          /* lock all currently mapped pages */
#define MCL_FUTURE      0x4000          /* lock all additions to address space */
#define MCL_ONFAULT	0x8000		/* lock all pages that are faulted in */

#endif /* _UAPI__SPARC_MMAN_H__ */
