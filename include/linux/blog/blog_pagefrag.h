/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Binary Logging Page Fragment Management
 */
#ifndef _LINUX_BLOG_PAGEFRAG_H
#define _LINUX_BLOG_PAGEFRAG_H

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/spinlock.h>

#define BLOG_PAGEFRAG_SIZE  (1<<19)  /* 512KB */
#define BLOG_PAGEFRAG_MASK (BLOG_PAGEFRAG_SIZE - 1)

/* Pagefrag allocator structure */
struct blog_pagefrag {
	struct page *pages;
	void *buffer;
	size_t capacity;        /* Buffer capacity for bounds checking */
	spinlock_t lock;        /* protects head */
	unsigned int head;
	unsigned int alloc_count;
	int active_elements;
	void *last_entry;       /* Pointer to the last allocated entry */
};

int blog_pagefrag_init_with_buffer(struct blog_pagefrag *pf, void *buffer, size_t size);
int blog_pagefrag_reserve(struct blog_pagefrag *pf, unsigned int n);
void blog_pagefrag_publish(struct blog_pagefrag *pf, u64 publish_head);
void blog_pagefrag_reset(struct blog_pagefrag *pf);
void *blog_pagefrag_get_ptr(struct blog_pagefrag *pf, u64 val);

#endif /* _LINUX_BLOG_PAGEFRAG_H */
