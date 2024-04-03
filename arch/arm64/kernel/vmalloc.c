// SPDX-License-Identifier: GPL-2.0-only
/*
 * AArch64 vmap area management code
 *
 * Author: Maxwell Bland <mbland@motorola.com>
 */

#include <linux/vmalloc.h>
#include <linux/elf.h>

#include <asm/module.h>

/*
 * Prevents the allocation of new vmap_areas from dynamic code
 * region if the virtual address requested is not explicitly the
 * module region.
 */
inline bool arch_skip_va(struct vmap_area *va, unsigned long vstart)
{
	return (vstart != MODULES_ASLR_START &&
			va->va_start >= MODULES_ASLR_START &&
			va->va_end <= MODULES_ASLR_END);
}

/*
 * Splits a vmap area in two and allocates a new area if needed
 */
inline struct vmap_area *
try_split_alloc_vmap_area(struct rb_root *root,
		struct list_head *head,
		struct kmem_cache *vmap_area_cachep,
		unsigned long addr)
{
	struct vmap_area *va;
	int ret;
	struct vmap_area *lva = NULL;

	va = __find_vmap_area(addr, root);
	if (!va) {
		pr_err("%s: could not find vmap\n", __func__);
		return NULL;
	}

	lva = kmem_cache_alloc(vmap_area_cachep, GFP_NOWAIT);
	if (!lva) {
		pr_err("%s: unable to allocate va for range\n", __func__);
		return NULL;
	}
	lva->va_start = addr;
	lva->va_end = va->va_end;
	ret = va_clip(root, head, va, addr, va->va_end - addr);
	if (WARN_ON_ONCE(ret)) {
		pr_err("%s: unable to clip code base region\n", __func__);
		kmem_cache_free(vmap_area_cachep, lva);
		return NULL;
	}
	insert_vmap_area_augment(lva, NULL, root, head);
	return lva;
}

/*
 * Run during vmalloc_init, ensures that there exist explicit rb tree
 * node delineations between code and data
 */
inline void arch_refine_vmap_space(struct rb_root *root,
		struct list_head *head,
		struct kmem_cache *cachep)
{
	try_split_alloc_vmap_area(root, head, cachep, MODULES_ASLR_START);
	try_split_alloc_vmap_area(root, head, cachep, MODULES_ASLR_END);
}
