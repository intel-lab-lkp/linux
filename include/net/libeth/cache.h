/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2024 Intel Corporation */

#ifndef __LIBETH_CACHE_H
#define __LIBETH_CACHE_H

#include <linux/cache.h>

/* ``__aligned_largest`` is architecture-dependent. Get the actual alignment */
#define ___LIBETH_LARGEST_ALIGN						   \
	sizeof(struct { long __UNIQUE_ID(long_); } __aligned_largest)
#define __LIBETH_LARGEST_ALIGN						   \
	(___LIBETH_LARGEST_ALIGN > SMP_CACHE_BYTES ?			   \
	 ___LIBETH_LARGEST_ALIGN : SMP_CACHE_BYTES)
#define __LIBETH_LARGEST_ALIGNED(sz)					   \
	ALIGN(sz, __LIBETH_LARGEST_ALIGN)

#define __libeth_cacheline_group_begin(grp)				   \
	__cacheline_group_begin(grp) __aligned(__LIBETH_LARGEST_ALIGN)
#define __libeth_cacheline_group_end(grp)				   \
	__cacheline_group_end(grp) __aligned(sizeof(long))

/**
 * libeth_cacheline_group - declare a cacheline-aligned field group
 * @grp: name of the group (usually 'read_mostly', 'read_write', or 'cold')
 * @...: struct fields inside the group
 *
 * Note that the whole group is cacheline-aligned, but the end marker is
 * aligned to long, so that you pass the (almost) actual field size sum to
 * the assertion macros below instead of CL-aligned values.
 * Each cacheline group must be described in struct's kernel-doc.
 */
#define libeth_cacheline_group(grp, ...)				   \
	struct_group(grp,						   \
		__libeth_cacheline_group_begin(grp);			   \
		__VA_ARGS__						   \
		__libeth_cacheline_group_end(grp);			   \
	)

/**
 * libeth_cacheline_group_assert - make sure cacheline group size is expected
 * @type: type of the structure containing the group
 * @grp: group name inside the struct
 * @sz: expected group size
 */
#if defined(CONFIG_64BIT) && SMP_CACHE_BYTES == 64
#define libeth_cacheline_group_assert(type, grp, sz)			   \
	static_assert(offsetof(type, __cacheline_group_end__##grp) -	   \
		      offsetofend(type, __cacheline_group_begin__##grp) == \
		      (sz))
#define __libeth_cacheline_struct_assert(type, sz)			   \
	static_assert(sizeof(type) == (sz))
#else /* !CONFIG_64BIT || SMP_CACHE_BYTES != 64 */
#define libeth_cacheline_group_assert(type, grp, sz)			   \
	static_assert(offsetof(type, __cacheline_group_end__##grp) -	   \
		      offsetofend(type, __cacheline_group_begin__##grp) <= \
		      (sz))
#define __libeth_cacheline_struct_assert(type, sz)			   \
	static_assert(sizeof(type) <= (sz))
#endif /* !CONFIG_64BIT || SMP_CACHE_BYTES != 64 */

#define __libeth_cls1(sz1)						   \
	__LIBETH_LARGEST_ALIGNED(sz1)
#define __libeth_cls2(sz1, sz2)						   \
	(__LIBETH_LARGEST_ALIGNED(sz1) + __LIBETH_LARGEST_ALIGNED(sz2))
#define __libeth_cls3(sz1, sz2, sz3)					   \
	(__LIBETH_LARGEST_ALIGNED(sz1) + __LIBETH_LARGEST_ALIGNED(sz2) +   \
	 __LIBETH_LARGEST_ALIGNED(sz3))
#define __libeth_cls(...)						   \
	CONCATENATE(__libeth_cls, COUNT_ARGS(__VA_ARGS__))(__VA_ARGS__)

/**
 * libeth_cacheline_struct_assert - make sure CL-based struct size is expected
 * @type: type of the struct
 * @...: from 1 to 3 CL group sizes (read-mostly, read-write, cold)
 *
 * When a struct contains several CL groups, it's difficult to predict its size
 * on different architectures. The macro instead takes sizes of all of the
 * groups the structure contains and generates the final struct size.
 */
#define libeth_cacheline_struct_assert(type, ...)			   \
	__libeth_cacheline_struct_assert(type, __libeth_cls(__VA_ARGS__)); \
	static_assert(__alignof(type) >= __LIBETH_LARGEST_ALIGN)

/**
 * libeth_cacheline_set_assert - make sure CL-based struct layout is expected
 * @type: type of the struct
 * @ro: expected size of the read-mostly group
 * @rw: expected size of the read-write group
 * @c: expected size of the cold group
 *
 * Check that each group size is expected and then do final struct size check.
 */
#define libeth_cacheline_set_assert(type, ro, rw, c)			   \
	libeth_cacheline_group_assert(type, read_mostly, ro);		   \
	libeth_cacheline_group_assert(type, read_write, rw);		   \
	libeth_cacheline_group_assert(type, cold, c);			   \
	libeth_cacheline_struct_assert(type, ro, rw, c)

#endif /* __LIBETH_CACHE_H */
