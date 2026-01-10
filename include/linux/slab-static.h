/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SLAB_STATIC_H
#define _LINUX_SLAB_STATIC_H

#ifdef MODULE
#error "can't use that in modules"
#endif

#include <generated/kmem_cache_size.h>

/* same size and alignment as struct kmem_cache: */
struct kmem_cache_opaque {
	unsigned char opaque[KMEM_CACHE_SIZE];
} __aligned(KMEM_CACHE_ALIGN);

#define __KMEM_CACHE_SETUP(cache, name, size, flags, ...)	\
		__kmem_cache_create_args((name), (size),	\
			&(struct kmem_cache_args) {		\
				.preallocated = (cache),	\
				__VA_ARGS__}, (flags))

static inline int
kmem_cache_setup_usercopy(struct kmem_cache *s,
			  const char *name, unsigned int size,
			  unsigned int align, slab_flags_t flags,
			  unsigned int useroffset, unsigned int usersize,
			  void (*ctor)(void *))
{
	struct kmem_cache *res;
	res = __KMEM_CACHE_SETUP(s, name, size, flags,
				.align		= align,
				.ctor		= ctor,
				.useroffset	= useroffset,
				.usersize	= usersize);
	if (IS_ERR(res))
		return PTR_ERR(res);
	return 0;
}

static inline int
kmem_cache_setup(struct kmem_cache *s,
		 const char *name, unsigned int size,
		 unsigned int align, slab_flags_t flags,
		 void (*ctor)(void *))
{
	struct kmem_cache *res;
	res = __KMEM_CACHE_SETUP(s, name, size, flags,
				.align		= align,
				.ctor		= ctor);
	if (IS_ERR(res))
		return PTR_ERR(res);
	return 0;
}

#define KMEM_CACHE_SETUP(s, __struct, __flags)                          	\
	__KMEM_CACHE_SETUP((s), #__struct, sizeof(struct __struct), (__flags),	\
			.align	= __alignof__(struct __struct))

#define KMEM_CACHE_SETUP_USERCOPY(s, __struct, __flags, __field)		\
	__KMEM_CACHE_SETUP((s), #__struct, sizeof(struct __struct), (__flags),	\
			.align	= __alignof__(struct __struct),			\
			.useroffset = offsetof(struct __struct, __field),	\
			.usersize = sizeof_field(struct __struct, __field))

#endif
