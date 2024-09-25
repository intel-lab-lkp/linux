/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef __ALPHA_MMAN_H__
#define __ALPHA_MMAN_H__

#define PROT_READ	0x1		/* page can be read */
#define PROT_WRITE	0x2		/* page can be written */
#define PROT_EXEC	0x4		/* page can be executed */
#ifndef PROT_SEM /* different on mips and xtensa */
#define PROT_SEM	0x8		/* page may be used for atomic ops */
#endif
/*			0x10		   reserved for arch-specific use */
/*			0x20		   reserved for arch-specific use */
#define PROT_NONE	0x0		/* page can not be accessed */
#define PROT_GROWSDOWN	0x01000000	/* mprotect flag: extend change to start of growsdown vma */
#define PROT_GROWSUP	0x02000000	/* mprotect flag: extend change to end of growsup vma */

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

/* MAP_UNINITIALIZED not supported */

/*
 * Flags for mlockall
 */
#define MCL_CURRENT	 8192		/* lock all currently mapped pages */
#define MCL_FUTURE	16384		/* lock all additions to address space */
#define MCL_ONFAULT	32768		/* lock all pages that are faulted in */

/*
 * Flags for mlock
 */
#define MLOCK_ONFAULT	0x01		/* Lock pages in range after they are faulted in, do not prefault */

/*
 * Flags for msync
 */
#define MS_ASYNC	1		/* sync memory asynchronously */
#define MS_SYNC		2		/* synchronous memory sync */
#define MS_INVALIDATE	4		/* invalidate the caches */

#define MADV_NORMAL	0		/* no further special treatment */
#define MADV_RANDOM	1		/* expect random page references */
#define MADV_SEQUENTIAL	2		/* expect sequential page references */
#define MADV_WILLNEED	3		/* will need these pages */
#define MADV_DONTNEED	6		/* don't need these pages */
/* originally MADV_SPACEAVAIL 5 */

/* common parameters: try to keep these consistent across architectures */
#define MADV_FREE	8		/* free pages only if memory pressure */
#define MADV_REMOVE	9		/* remove these pages & resources */
#define MADV_DONTFORK	10		/* don't inherit across fork */
#define MADV_DOFORK	11		/* do inherit across fork */

#define MADV_MERGEABLE   12		/* KSM may merge identical pages */
#define MADV_UNMERGEABLE 13		/* KSM may not merge identical pages */

#define MADV_HUGEPAGE	14		/* Worth backing with hugepages */
#define MADV_NOHUGEPAGE	15		/* Not worth backing with hugepages */

#define MADV_DONTDUMP   16		/* Explicity exclude from the core dump,
					   overrides the coredump filter bits */
#define MADV_DODUMP	17		/* Clear the MADV_DONTDUMP flag */

#define MADV_WIPEONFORK 18		/* Zero memory on fork, child only */
#define MADV_KEEPONFORK 19		/* Undo MADV_WIPEONFORK */

#define MADV_COLD	20		/* deactivate these pages */
#define MADV_PAGEOUT	21		/* reclaim these pages */

#define MADV_POPULATE_READ	22	/* populate (prefault) page tables readable */
#define MADV_POPULATE_WRITE	23	/* populate (prefault) page tables writable */

#define MADV_DONTNEED_LOCKED	24	/* like DONTNEED, but drop locked pages too */

#define MADV_COLLAPSE	25		/* Synchronous hugepage collapse */

#define MADV_HWPOISON	100		/* poison a page for testing */
#define MADV_SOFT_OFFLINE 101		/* soft offline page for testing */

/* compatibility flags */
#define MAP_FILE	0

#define PKEY_DISABLE_ACCESS	0x1
#define PKEY_DISABLE_WRITE	0x2
#define PKEY_ACCESS_MASK	(PKEY_DISABLE_ACCESS |\
				 PKEY_DISABLE_WRITE)

#endif /* __ALPHA_MMAN_H__ */
