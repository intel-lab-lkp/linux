/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _TRACE_MMAP_H_
#define _TRACE_MMAP_H_

#include <linux/types.h>

/**
 * struct trace_buffer_meta - Ring-buffer Meta-page description
 * @entries:		Number of entries in the ring-buffer.
 * @overrun:		Number of entries lost in the ring-buffer.
 * @read:		Number of entries that have been read.
 * @subbufs_touched:	Number of subbufs that have been filled.
 * @subbufs_lost:	Number of subbufs lost to overrun.
 * @subbufs_read:	Number of subbufs that have been read.
 * @reader.lost_events:	Number of events lost at the time of the reader swap.
 * @reader.id:		subbuf ID of the current reader. From 0 to @nr_subbufs - 1
 * @reader.read:	Number of bytes read on the reader subbuf.
 * @subbuf_size:	Size of each subbuf, including the header.
 * @nr_subbufs:		Number of subbfs in the ring-buffer.
 * @meta_page_size:	Size of this meta-page.
 * @meta_struct_len:	Size of this structure.
 */
struct trace_buffer_meta {
	unsigned long	entries;
	unsigned long	overrun;
	unsigned long	read;

	unsigned long	subbufs_touched;
	unsigned long	subbufs_lost;
	unsigned long	subbufs_read;

	struct {
		unsigned long	lost_events;
		__u32		id;
		__u32		read;
	} reader;

	__u32		subbuf_size;
	__u32		nr_subbufs;

	__u32		meta_page_size;
	__u32		meta_struct_len;
};

#define TRACE_MMAP_IOCTL_GET_READER		_IO('T', 0x1)

#endif /* _TRACE_MMAP_H_ */
