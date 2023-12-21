/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_TRACE_MMAP_H_
#define _UAPI_TRACE_MMAP_H_

#include <linux/types.h>

struct trace_buffer_meta {
	unsigned long	entries;
	unsigned long	overrun;
	unsigned long	read;

	unsigned long	subbufs_touched;
	unsigned long	subbufs_lost;
	unsigned long	subbufs_read;

	struct {
		unsigned long	lost_events;	/* Events lost at the time of the reader swap */
		__u32		id;		/* Reader subbuf ID from 0 to nr_subbufs - 1 */
		__u32		read;		/* Number of bytes read on the reader subbuf */
	} reader;

	__u32		subbuf_size;		/* Size of each subbuf including the header */
	__u32		nr_subbufs;		/* Number of subbufs in the ring-buffer */

	__u32		meta_page_size;		/* Size of the meta-page */
	__u32		meta_struct_len;	/* Len of this struct */
};

#define TRACE_MMAP_IOCTL_GET_READER		_IO('T', 0x1)

#endif /* _UAPI_TRACE_MMAP_H_ */
