/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef __ASM_GENERIC_MMAN_COMMON_H
#define __ASM_GENERIC_MMAN_COMMON_H

/*
 Author: Michael S. Tsirkin <mst@mellanox.co.il>, Mellanox Technologies Ltd.
 Based on: asm-xxx/mman.h
*/

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

/*
 * Flags for mlock
 */
#define MLOCK_ONFAULT	0x01		/* Lock pages in range after they are faulted in, do not prefault */

/*
 * Flags for msync
 */
#ifndef MS_ASYNC /* different order on alpha and parisc */
#define MS_ASYNC	1		/* sync memory asynchronously */
#define MS_INVALIDATE	2		/* invalidate the caches */
#define MS_SYNC		4		/* synchronous memory sync */
#endif

#define MADV_NORMAL	0		/* no further special treatment */
#define MADV_RANDOM	1		/* expect random page references */
#define MADV_SEQUENTIAL	2		/* expect sequential page references */
#define MADV_WILLNEED	3		/* will need these pages */
/* 4 through 6 are different on alpha */
#ifndef MADV_DONTNEED
#define MADV_DONTNEED	4		/* don't need these pages */
#endif

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

#endif /* __ASM_GENERIC_MMAN_COMMON_H */
