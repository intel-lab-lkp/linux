// SPDX-License-Identifier: GPL-2.0-only
/*
 * kho_vmalloc.c - KHO vmalloc space serialization/preservation
 * Copyright (C) 2025 Microsoft Corporation, Mike Rapoport <rppt@kernel.org>
 * Copyright (C) 2025 Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/kasan.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/kexec_handover.h>
#include <linux/kho/vmalloc.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>


#include "../../mm/internal.h"
#include "kexec_handover_internal.h"

/* vmalloc flags KHO supports */
#define KHO_VMALLOC_SUPPORTED_FLAGS	(VM_ALLOC | VM_ALLOW_HUGE_VMAP)

/* KHO internal flags for vmalloc preservations */
#define KHO_VMALLOC_ALLOC	0x0001
#define KHO_VMALLOC_HUGE_VMAP	0x0002

static unsigned short vmalloc_flags_to_kho(unsigned int vm_flags)
{
	unsigned short kho_flags = 0;

	if (vm_flags & VM_ALLOC)
		kho_flags |= KHO_VMALLOC_ALLOC;
	if (vm_flags & VM_ALLOW_HUGE_VMAP)
		kho_flags |= KHO_VMALLOC_HUGE_VMAP;

	return kho_flags;
}

static unsigned int kho_flags_to_vmalloc(unsigned short kho_flags)
{
	unsigned int vm_flags = 0;

	if (kho_flags & KHO_VMALLOC_ALLOC)
		vm_flags |= VM_ALLOC;
	if (kho_flags & KHO_VMALLOC_HUGE_VMAP)
		vm_flags |= VM_ALLOW_HUGE_VMAP;

	return vm_flags;
}

static struct kho_vmalloc_chunk *new_vmalloc_chunk(struct kho_vmalloc_chunk *cur)
{
	struct kho_vmalloc_chunk *chunk;
	int err;

	chunk = (struct kho_vmalloc_chunk *)get_zeroed_page(GFP_KERNEL);
	if (!chunk)
		return NULL;

	err = kho_preserve_pages(virt_to_page(chunk), 1);
	if (err)
		goto err_free;
	if (cur)
		KHOSER_STORE_PTR(cur->hdr.next, chunk);
	return chunk;

err_free:
	free_page((unsigned long)chunk);
	return NULL;
}

static void kho_vmalloc_unpreserve_chunk(struct kho_vmalloc_chunk *chunk,
					 unsigned short order)
{
	unsigned long pfn = PHYS_PFN(virt_to_phys(chunk));

	kho_unpreserve_pages(pfn_to_page(pfn), 1);

	for (int i = 0; i < ARRAY_SIZE(chunk->phys) && chunk->phys[i]; i++) {
		pfn = PHYS_PFN(chunk->phys[i]);
		kho_unpreserve_pages(pfn_to_page(pfn), 1 << order);
	}
}

/**
 * kho_preserve_vmalloc - preserve memory allocated with vmalloc() across kexec
 * @ptr: pointer to the area in vmalloc address space
 * @preservation: placeholder for preservation metadata
 *
 * Instructs KHO to preserve the area in vmalloc address space at @ptr. The
 * physical pages mapped at @ptr will be preserved and on successful return
 * @preservation will hold the physical address of a structure that describes
 * the preservation.
 *
 * NOTE: The memory allocated with vmalloc_node() variants cannot be reliably
 * restored on the same node
 *
 * Return: 0 on success, error code on failure
 */
int kho_preserve_vmalloc(void *ptr, struct kho_vmalloc *preservation)
{
	struct kho_vmalloc_chunk *chunk;
	struct vm_struct *vm = find_vm_area(ptr);
	unsigned int order, flags, nr_contig_pages;
	unsigned int idx = 0;
	int err;

	if (!vm)
		return -EINVAL;

	if (vm->flags & ~KHO_VMALLOC_SUPPORTED_FLAGS)
		return -EOPNOTSUPP;

	flags = vmalloc_flags_to_kho(vm->flags);
	order = get_vm_area_page_order(vm);

	chunk = new_vmalloc_chunk(NULL);
	if (!chunk)
		return -ENOMEM;
	KHOSER_STORE_PTR(preservation->first, chunk);

	nr_contig_pages = (1 << order);
	for (int i = 0; i < vm->nr_pages; i += nr_contig_pages) {
		phys_addr_t phys = page_to_phys(vm->pages[i]);

		err = kho_preserve_pages(vm->pages[i], nr_contig_pages);
		if (err)
			goto err_free;

		chunk->phys[idx++] = phys;
		if (idx == ARRAY_SIZE(chunk->phys)) {
			chunk = new_vmalloc_chunk(chunk);
			if (!chunk) {
				err = -ENOMEM;
				goto err_free;
			}
			idx = 0;
		}
	}

	preservation->total_pages = vm->nr_pages;
	preservation->flags = flags;
	preservation->order = order;

	return 0;

err_free:
	kho_unpreserve_vmalloc(preservation);
	return err;
}
EXPORT_SYMBOL_GPL(kho_preserve_vmalloc);

/**
 * kho_unpreserve_vmalloc - unpreserve memory allocated with vmalloc()
 * @preservation: preservation metadata returned by kho_preserve_vmalloc()
 *
 * Instructs KHO to unpreserve the area in vmalloc address space that was
 * previously preserved with kho_preserve_vmalloc().
 */
void kho_unpreserve_vmalloc(struct kho_vmalloc *preservation)
{
	struct kho_vmalloc_chunk *chunk = KHOSER_LOAD_PTR(preservation->first);

	while (chunk) {
		struct kho_vmalloc_chunk *tmp = chunk;

		kho_vmalloc_unpreserve_chunk(chunk, preservation->order);

		chunk = KHOSER_LOAD_PTR(chunk->hdr.next);
		free_page((unsigned long)tmp);
	}
}
EXPORT_SYMBOL_GPL(kho_unpreserve_vmalloc);

/**
 * kho_restore_vmalloc - recreates and populates an area in vmalloc address
 * space from the preserved memory.
 * @preservation: preservation metadata.
 *
 * Recreates an area in vmalloc address space and populates it with memory that
 * was preserved using kho_preserve_vmalloc().
 *
 * Return: pointer to the area in the vmalloc address space, NULL on failure.
 */
void *kho_restore_vmalloc(const struct kho_vmalloc *preservation)
{
	struct kho_vmalloc_chunk *chunk = KHOSER_LOAD_PTR(preservation->first);
	kasan_vmalloc_flags_t kasan_flags = KASAN_VMALLOC_PROT_NORMAL;
	unsigned int align, order, shift, vm_flags;
	unsigned long total_pages, contig_pages;
	unsigned long addr, size;
	struct vm_struct *area;
	struct page **pages;
	unsigned int idx = 0;
	int err;

	vm_flags = kho_flags_to_vmalloc(preservation->flags);
	if (vm_flags & ~KHO_VMALLOC_SUPPORTED_FLAGS)
		return NULL;

	total_pages = preservation->total_pages;
	pages = kvmalloc_objs(*pages, total_pages);
	if (!pages)
		return NULL;
	order = preservation->order;
	contig_pages = (1 << order);
	shift = PAGE_SHIFT + order;
	align = 1 << shift;

	while (chunk) {
		struct page *page;

		for (int i = 0; i < ARRAY_SIZE(chunk->phys) && chunk->phys[i]; i++) {
			phys_addr_t phys = chunk->phys[i];

			if (idx + contig_pages > total_pages)
				goto err_free_pages_array;

			page = kho_restore_pages(phys, contig_pages);
			if (!page)
				goto err_free_pages_array;

			for (int j = 0; j < contig_pages; j++)
				pages[idx++] = page + j;

			phys += contig_pages * PAGE_SIZE;
		}

		page = kho_restore_pages(virt_to_phys(chunk), 1);
		if (!page)
			goto err_free_pages_array;
		chunk = KHOSER_LOAD_PTR(chunk->hdr.next);
		__free_page(page);
	}

	if (idx != total_pages)
		goto err_free_pages_array;

	area = __get_vm_area_node(total_pages * PAGE_SIZE, align, shift,
				  vm_flags | VM_UNINITIALIZED,
				  VMALLOC_START, VMALLOC_END,
				  NUMA_NO_NODE, GFP_KERNEL,
				  __builtin_return_address(0));
	if (!area)
		goto err_free_pages_array;

	addr = (unsigned long)area->addr;
	size = get_vm_area_size(area);
	err = vmap_pages_range(addr, addr + size, PAGE_KERNEL, pages, shift);
	if (err)
		goto err_free_vm_area;

	area->nr_pages = total_pages;
	area->pages = pages;

	if (vm_flags & VM_ALLOC)
		kasan_flags |= KASAN_VMALLOC_VM_ALLOC;

	area->addr = kasan_unpoison_vmalloc(area->addr, total_pages * PAGE_SIZE,
					    kasan_flags);
	clear_vm_uninitialized_flag(area);

	return area->addr;

err_free_vm_area:
	free_vm_area(area);
err_free_pages_array:
	kvfree(pages);
	return NULL;
}
EXPORT_SYMBOL_GPL(kho_restore_vmalloc);
