/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 *  include/linux/alloc_tag.h
 */

#ifndef _UAPI_ALLOC_TAG_H
#define _UAPI_ALLOC_TAG_H

#include <linux/types.h>

#define ALLOCINFO_STR_SIZE	64

struct allocinfo_content_id {
	__u64 id;
};

struct allocinfo_tag {
	/* Longer names are trimmed */
	char modname[ALLOCINFO_STR_SIZE];
	char function[ALLOCINFO_STR_SIZE];
	char filename[ALLOCINFO_STR_SIZE];
	__u64 lineno;
};

/* The alignment ensures 32-bit compatible interfaces are not broken */
struct allocinfo_counter {
	__u64 bytes;
	__u64 calls;
	__u8 accurate;
} __attribute__((aligned(8)));

struct allocinfo_tag_data {
	struct allocinfo_tag tag;
	struct allocinfo_counter counter;
};

struct allocinfo_get_at {
	__u64 pos;	/* input */
	struct allocinfo_tag_data data;
};

#define _ALLOCINFO_IOC_CONTENT_ID	0
#define _ALLOCINFO_IOC_GET_AT		1
#define _ALLOCINFO_IOC_GET_NEXT		2

#define ALLOCINFO_IOC_BASE		0xA6
#define ALLOCINFO_IOC_CONTENT_ID	_IOR(ALLOCINFO_IOC_BASE, _ALLOCINFO_IOC_CONTENT_ID,	\
					     struct allocinfo_content_id)
#define ALLOCINFO_IOC_GET_AT		_IOWR(ALLOCINFO_IOC_BASE, _ALLOCINFO_IOC_GET_AT,	\
					      struct allocinfo_get_at)
#define ALLOCINFO_IOC_GET_NEXT		_IOR(ALLOCINFO_IOC_BASE, _ALLOCINFO_IOC_GET_NEXT,	\
					     struct allocinfo_tag_data)

#endif /* _UAPI_ALLOC_TAG_H */
