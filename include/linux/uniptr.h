/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Support for "universal" pointers that can point to either kernel or userspace
 * memory.
 *
 * Original code from sockptr.h
 *    Copyright (c) 2020 Christoph Hellwig
 */
#ifndef _LINUX_UNIPTR_H
#define _LINUX_UNIPTR_H

#include <linux/err.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

typedef struct {
	union {
		void		*kernel;
		void __user	*user;
	};
	bool		is_kernel : 1;
} uniptr_t;

static inline bool uniptr_is_kernel(uniptr_t uniptr)
{
	return uniptr.is_kernel;
}

static inline uniptr_t KERNEL_UNIPTR(void *p)
{
	return (uniptr_t) { .kernel = p, .is_kernel = true };
}

static inline uniptr_t USER_UNIPTR(void __user *p)
{
	return (uniptr_t) { .user = p };
}

static inline bool uniptr_is_null(uniptr_t uniptr)
{
	if (uniptr_is_kernel(uniptr))
		return !uniptr.kernel;
	return !uniptr.user;
}

static inline int copy_from_uniptr_offset(void *dst, uniptr_t src,
					  size_t offset, size_t size)
{
	if (!uniptr_is_kernel(src))
		return copy_from_user(dst, src.user + offset, size);
	memcpy(dst, src.kernel + offset, size);
	return 0;
}

static inline int copy_from_uniptr(void *dst, uniptr_t src, size_t size)
{
	return copy_from_uniptr_offset(dst, src, 0, size);
}

static inline int copy_to_uniptr_offset(uniptr_t dst, size_t offset,
					const void *src, size_t size)
{
	if (!uniptr_is_kernel(dst))
		return copy_to_user(dst.user + offset, src, size);
	memcpy(dst.kernel + offset, src, size);
	return 0;
}

static inline int copy_to_uniptr(uniptr_t dst, const void *src, size_t size)
{
	return copy_to_uniptr_offset(dst, 0, src, size);
}

static inline void *memdup_uniptr(uniptr_t src, size_t len)
{
	void *p = kmalloc_track_caller(len, GFP_USER | __GFP_NOWARN);

	if (!p)
		return ERR_PTR(-ENOMEM);
	if (copy_from_uniptr(p, src, len)) {
		kfree(p);
		return ERR_PTR(-EFAULT);
	}
	return p;
}

static inline void *memdup_uniptr_nul(uniptr_t src, size_t len)
{
	char *p = kmalloc_track_caller(len + 1, GFP_KERNEL);

	if (!p)
		return ERR_PTR(-ENOMEM);
	if (copy_from_uniptr(p, src, len)) {
		kfree(p);
		return ERR_PTR(-EFAULT);
	}
	p[len] = '\0';
	return p;
}

static inline long strncpy_from_uniptr(char *dst, uniptr_t src, size_t count)
{
	if (uniptr_is_kernel(src)) {
		size_t len = min(strnlen(src.kernel, count - 1) + 1, count);

		memcpy(dst, src.kernel, len);
		return len;
	}
	return strncpy_from_user(dst, src.user, count);
}

static inline int check_zeroed_uniptr(uniptr_t src, size_t offset, size_t size)
{
	if (!uniptr_is_kernel(src))
		return check_zeroed_user(src.user + offset, size);
	return memchr_inv(src.kernel + offset, 0, size) == NULL;
}

#endif /* _LINUX_UNIPTR_H */
