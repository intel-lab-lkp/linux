// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024 Meta Platforms, Inc. and affiliates. */
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/err.h>
#include <linux/btf_ids.h>
#include <linux/vmalloc.h>
#include <linux/pagemap.h>

/*
 * bpf_arena is a sparsely populated shared memory region between bpf program and
 * user space process.
 *
 * For example on x86-64 the values could be:
 * user_vm_start 7f7d26200000     // picked by mmap()
 * kern_vm_start ffffc90001e69000 // picked by get_vm_area()
 * For user space all pointers within the arena are normal 8-byte addresses.
 * In this example 7f7d26200000 is the address of the first page (pgoff=0).
 * The bpf program will access it as: kern_vm_start + lower_32bit_of_user_ptr
 * (u32)7f7d26200000 -> 26200000
 * hence
 * ffffc90001e69000 + 26200000 == ffffc90028069000 is "pgoff=0" within 4Gb
 * kernel memory region.
 *
 * BPF JITs generate the following code to access arena:
 *   mov eax, eax  // eax has lower 32-bit of user pointer
 *   mov word ptr [rax + r12 + off], bx
 * where r12 == kern_vm_start and off is s16.
 * Hence allocate 4Gb + GUARD_SZ/2 on each side.
 *
 * Initially kernel vm_area and user vma are not populated.
 * User space can fault-in any address which will insert the page
 * into kernel and user vma.
 * bpf program can allocate a page via bpf_arena_alloc_pages() kfunc
 * which will insert it into kernel vm_area.
 * The later fault-in from user space will populate that page into user vma.
 */

/* number of bytes addressable by LDX/STX insn with 16-bit 'off' field */
#define GUARD_SZ (1ull << sizeof(((struct bpf_insn *)0)->off) * 8)
#define KERN_VM_SZ ((1ull << 32) + GUARD_SZ)

struct bpf_arena {
	struct bpf_map map;
	u64 user_vm_start;
	u64 user_vm_end;
	struct vm_struct *kern_vm;
	struct maple_tree mt;
	struct list_head vma_list;
	struct mutex lock;
};

u64 bpf_arena_get_kern_vm_start(struct bpf_arena *arena)
{
	return arena ? (u64) (long) arena->kern_vm->addr + GUARD_SZ / 2 : 0;
}

u64 bpf_arena_get_user_vm_start(struct bpf_arena *arena)
{
	return arena ? arena->user_vm_start : 0;
}

static long arena_map_peek_elem(struct bpf_map *map, void *value)
{
	return -EOPNOTSUPP;
}

static long arena_map_push_elem(struct bpf_map *map, void *value, u64 flags)
{
	return -EOPNOTSUPP;
}

static long arena_map_pop_elem(struct bpf_map *map, void *value)
{
	return -EOPNOTSUPP;
}

static long arena_map_delete_elem(struct bpf_map *map, void *value)
{
	return -EOPNOTSUPP;
}

static int arena_map_get_next_key(struct bpf_map *map, void *key, void *next_key)
{
	return -EOPNOTSUPP;
}

static long compute_pgoff(struct bpf_arena *arena, long uaddr)
{
	return (u32)(uaddr - (u32)arena->user_vm_start) >> PAGE_SHIFT;
}

#define MT_ENTRY ((void *)&arena_map_ops) /* unused. has to be valid pointer */

/*
 * Reserve a "zero page", so that bpf prog and user space never see
 * a pointer to arena with lower 32 bits being zero.
 * bpf_cast_user() promotes it to full 64-bit NULL.
 */
static int reserve_zero_page(struct bpf_arena *arena)
{
	long pgoff = compute_pgoff(arena, 0);

	return mtree_insert(&arena->mt, pgoff, MT_ENTRY, GFP_KERNEL);
}

static struct bpf_map *arena_map_alloc(union bpf_attr *attr)
{
	struct vm_struct *kern_vm;
	int numa_node = bpf_map_attr_numa_node(attr);
	struct bpf_arena *arena;
	int err = -ENOMEM;

	if (attr->key_size != 8 || attr->value_size != 8 ||
	    /* BPF_F_MMAPABLE must be set */
	    !(attr->map_flags & BPF_F_MMAPABLE) ||
	    /* No unsupported flags present */
	    (attr->map_flags & ~(BPF_F_SEGV_ON_FAULT | BPF_F_MMAPABLE | BPF_F_NO_USER_CONV)))
		return ERR_PTR(-EINVAL);

	if (attr->map_extra & ~PAGE_MASK)
		/* If non-zero the map_extra is an expected user VMA start address */
		return ERR_PTR(-EINVAL);

	kern_vm = get_vm_area(KERN_VM_SZ, VM_MAP | VM_USERMAP);
	if (!kern_vm)
		return ERR_PTR(-ENOMEM);

	arena = bpf_map_area_alloc(sizeof(*arena), numa_node);
	if (!arena)
		goto err;

	INIT_LIST_HEAD(&arena->vma_list);
	arena->kern_vm = kern_vm;
	arena->user_vm_start = attr->map_extra;
	bpf_map_init_from_attr(&arena->map, attr);
	mt_init_flags(&arena->mt, MT_FLAGS_ALLOC_RANGE);
	mutex_init(&arena->lock);
	if (arena->user_vm_start) {
		err = reserve_zero_page(arena);
		if (err) {
			bpf_map_area_free(arena);
			goto err;
		}
	}

	return &arena->map;
err:
	free_vm_area(kern_vm);
	return ERR_PTR(err);
}

static int for_each_pte(pte_t *ptep, unsigned long addr, void *data)
{
	struct page *page;
	pte_t pte;

	pte = ptep_get(ptep);
	if (!pte_present(pte))
		return 0;
	page = pte_page(pte);
	/*
	 * We do not update pte here:
	 * 1. Nobody should be accessing bpf_arena's range outside of a kernel bug
	 * 2. TLB flushing is batched or deferred. Even if we clear pte,
	 * the TLB entries can stick around and continue to permit access to
	 * the freed page. So it all relies on 1.
	 */
	__free_page(page);
	return 0;
}

static void arena_map_free(struct bpf_map *map)
{
	struct bpf_arena *arena = container_of(map, struct bpf_arena, map);

	/*
	 * Check that user vma-s are not around when bpf map is freed.
	 * mmap() holds vm_file which holds bpf_map refcnt.
	 * munmap() must have happened on vma followed by arena_vm_close()
	 * which would clear arena->vma_list.
	 */
	if (WARN_ON_ONCE(!list_empty(&arena->vma_list)))
		return;

	/*
	 * free_vm_area() calls remove_vm_area() that calls free_unmap_vmap_area().
	 * It unmaps everything from vmalloc area and clears pgtables.
	 * Call apply_to_existing_page_range() first to find populated ptes and
	 * free those pages.
	 */
	apply_to_existing_page_range(&init_mm, bpf_arena_get_kern_vm_start(arena),
				     KERN_VM_SZ - GUARD_SZ / 2, for_each_pte, NULL);
	free_vm_area(arena->kern_vm);
	mtree_destroy(&arena->mt);
	bpf_map_area_free(arena);
}

static void *arena_map_lookup_elem(struct bpf_map *map, void *key)
{
	return ERR_PTR(-EINVAL);
}

static long arena_map_update_elem(struct bpf_map *map, void *key,
				  void *value, u64 flags)
{
	return -EOPNOTSUPP;
}

static int arena_map_check_btf(const struct bpf_map *map, const struct btf *btf,
			       const struct btf_type *key_type, const struct btf_type *value_type)
{
	return 0;
}

static u64 arena_map_mem_usage(const struct bpf_map *map)
{
	return 0;
}

struct vma_list {
	struct vm_area_struct *vma;
	struct list_head head;
};

static int remember_vma(struct bpf_arena *arena, struct vm_area_struct *vma)
{
	struct vma_list *vml;

	vml = kmalloc(sizeof(*vml), GFP_KERNEL);
	if (!vml)
		return -ENOMEM;
	vma->vm_private_data = vml;
	vml->vma = vma;
	list_add(&vml->head, &arena->vma_list);
	return 0;
}

static void arena_vm_close(struct vm_area_struct *vma)
{
	struct vma_list *vml;

	vml = vma->vm_private_data;
	list_del(&vml->head);
	vma->vm_private_data = NULL;
	kfree(vml);
}

static vm_fault_t arena_vm_fault(struct vm_fault *vmf)
{
	struct bpf_map *map = vmf->vma->vm_file->private_data;
	struct bpf_arena *arena = container_of(map, struct bpf_arena, map);
	struct page *page;
	long kbase, kaddr;
	int ret;

	kbase = bpf_arena_get_kern_vm_start(arena);
	kaddr = kbase + (u32)(vmf->address & PAGE_MASK);

	guard(mutex)(&arena->lock);
	page = vmalloc_to_page((void *)kaddr);
	if (page)
		/* already have a page vmap-ed */
		goto out;

	if (arena->map.map_flags & BPF_F_SEGV_ON_FAULT)
		/* User space requested to segfault when page is not allocated by bpf prog */
		return VM_FAULT_SIGSEGV;

	ret = mtree_insert(&arena->mt, vmf->pgoff, MT_ENTRY, GFP_KERNEL);
	if (ret == -EEXIST)
		return VM_FAULT_RETRY;
	if (ret)
		return VM_FAULT_SIGSEGV;

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page) {
		mtree_erase(&arena->mt, vmf->pgoff);
		return VM_FAULT_SIGSEGV;
	}

	ret = vmap_pages_range(kaddr, kaddr + PAGE_SIZE, PAGE_KERNEL, &page, PAGE_SHIFT);
	if (ret) {
		mtree_erase(&arena->mt, vmf->pgoff);
		__free_page(page);
		return VM_FAULT_SIGSEGV;
	}
out:
	page_ref_add(page, 1);
	vmf->page = page;
	return 0;
}

static const struct vm_operations_struct arena_vm_ops = {
	.close		= arena_vm_close,
	.fault          = arena_vm_fault,
};

static int arena_map_mmap(struct bpf_map *map, struct vm_area_struct *vma)
{
	struct bpf_arena *arena = container_of(map, struct bpf_arena, map);
	int err;

	if (arena->user_vm_start && arena->user_vm_start != vma->vm_start)
		/*
		 * 1st user process can do mmap(NULL, ...) to pick user_vm_start
		 * 2nd user process must pass the same addr to mmap(addr, MAP_FIXED..);
		 *   or
		 * specify addr in map_extra at map creation time and
		 * use the same addr later with mmap(addr, MAP_FIXED..);
		 */
		return -EBUSY;

	if (arena->user_vm_end && arena->user_vm_end != vma->vm_end)
		/* all user processes must have the same size of mmap-ed region */
		return -EBUSY;

	if (vma->vm_end - vma->vm_start > 1ull << 32)
		/* Must not be bigger than 4Gb */
		return -E2BIG;

	if (remember_vma(arena, vma))
		return -ENOMEM;

	if (!arena->user_vm_start) {
		arena->user_vm_start = vma->vm_start;
		err = reserve_zero_page(arena);
		if (err)
			return err;
	}
	arena->user_vm_end = vma->vm_end;
	/*
	 * bpf_map_mmap() checks that it's being mmaped as VM_SHARED and
	 * clears VM_MAYEXEC. Set VM_DONTEXPAND as well to avoid
	 * potential change of user_vm_start.
	 */
	vm_flags_set(vma, VM_DONTEXPAND);
	vma->vm_ops = &arena_vm_ops;
	return 0;
}

BTF_ID_LIST_SINGLE(bpf_arena_map_btf_ids, struct, bpf_arena)
const struct bpf_map_ops arena_map_ops = {
	.map_meta_equal = bpf_map_meta_equal,
	.map_alloc = arena_map_alloc,
	.map_free = arena_map_free,
	.map_mmap = arena_map_mmap,
	.map_get_next_key = arena_map_get_next_key,
	.map_push_elem = arena_map_push_elem,
	.map_peek_elem = arena_map_peek_elem,
	.map_pop_elem = arena_map_pop_elem,
	.map_lookup_elem = arena_map_lookup_elem,
	.map_update_elem = arena_map_update_elem,
	.map_delete_elem = arena_map_delete_elem,
	.map_check_btf = arena_map_check_btf,
	.map_mem_usage = arena_map_mem_usage,
	.map_btf_id = &bpf_arena_map_btf_ids[0],
};

static u64 clear_lo32(u64 val)
{
	return val & ~(u64)~0U;
}

/*
 * Allocate pages and vmap them into kernel vmalloc area.
 * Later the pages will be mmaped into user space vma.
 */
static long arena_alloc_pages(struct bpf_arena *arena, long uaddr, long page_cnt, int node_id)
{
	long page_cnt_max = (arena->user_vm_end - arena->user_vm_start) >> PAGE_SHIFT;
	u64 kern_vm_start = bpf_arena_get_kern_vm_start(arena);
	long pgoff = 0, kaddr, nr_pages = 0;
	struct page **pages;
	int ret, i;

	if (page_cnt >= page_cnt_max)
		return 0;

	if (uaddr) {
		if (uaddr & ~PAGE_MASK)
			return 0;
		pgoff = compute_pgoff(arena, uaddr);
		if (pgoff + page_cnt > page_cnt_max)
			/* requested address will be outside of user VMA */
			return 0;
	}

	/* zeroing is needed, since alloc_pages_bulk_array() only fills in non-zero entries */
	pages = kvcalloc(page_cnt, sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		return 0;

	guard(mutex)(&arena->lock);

	if (uaddr)
		ret = mtree_insert_range(&arena->mt, pgoff, pgoff + page_cnt,
					 MT_ENTRY, GFP_KERNEL);
	else
		ret = mtree_alloc_range(&arena->mt, &pgoff, MT_ENTRY,
					page_cnt, 0, page_cnt_max, GFP_KERNEL);
	if (ret)
		goto out_free_pages;

	nr_pages = alloc_pages_bulk_array_node(GFP_KERNEL | __GFP_ZERO, node_id, page_cnt, pages);
	if (nr_pages != page_cnt)
		goto out;

	kaddr = kern_vm_start + (u32)(arena->user_vm_start + pgoff * PAGE_SIZE);
	ret = vmap_pages_range(kaddr, kaddr + PAGE_SIZE * page_cnt, PAGE_KERNEL,
			       pages, PAGE_SHIFT);
	if (ret)
		goto out;
	kvfree(pages);
	return clear_lo32(arena->user_vm_start) + (u32)(kaddr - kern_vm_start);
out:
	mtree_erase(&arena->mt, pgoff);
out_free_pages:
	if (pages)
		for (i = 0; i < nr_pages; i++)
			__free_page(pages[i]);
	kvfree(pages);
	return 0;
}

/*
 * If page is present in vmalloc area, unmap it from vmalloc area,
 * unmap it from all user space vma-s,
 * and free it.
 */
static void zap_pages(struct bpf_arena *arena, long uaddr, long page_cnt)
{
	struct vma_list *vml;

	list_for_each_entry(vml, &arena->vma_list, head)
		zap_page_range_single(vml->vma, uaddr,
				      PAGE_SIZE * page_cnt, NULL);
}

static void arena_free_pages(struct bpf_arena *arena, long uaddr, long page_cnt)
{
	u64 full_uaddr, uaddr_end;
	long kaddr, pgoff, i;
	struct page *page;

	/* only aligned lower 32-bit are relevant */
	uaddr = (u32)uaddr;
	uaddr &= PAGE_MASK;
	full_uaddr = clear_lo32(arena->user_vm_start) + uaddr;
	uaddr_end = min(arena->user_vm_end, full_uaddr + (page_cnt << PAGE_SHIFT));
	if (full_uaddr >= uaddr_end)
		return;

	page_cnt = (uaddr_end - full_uaddr) >> PAGE_SHIFT;

	kaddr = bpf_arena_get_kern_vm_start(arena) + uaddr;

	guard(mutex)(&arena->lock);

	pgoff = compute_pgoff(arena, uaddr);
	/* clear range */
	mtree_store_range(&arena->mt, pgoff, pgoff + page_cnt, NULL, GFP_KERNEL);

	if (page_cnt > 1)
		/* bulk zap if multiple pages being freed */
		zap_pages(arena, full_uaddr, page_cnt);

	for (i = 0; i < page_cnt; i++, kaddr += PAGE_SIZE, full_uaddr += PAGE_SIZE) {
		page = vmalloc_to_page((void *)kaddr);
		if (!page)
			continue;
		if (page_cnt == 1 && page_mapped(page)) /* mapped by some user process */
			zap_pages(arena, full_uaddr, 1);
		vunmap_range(kaddr, kaddr + PAGE_SIZE);
		__free_page(page);
	}
}

__bpf_kfunc_start_defs();

__bpf_kfunc void *bpf_arena_alloc_pages(void *p__map, void *addr__ign, u32 page_cnt,
					int node_id, u64 flags)
{
	struct bpf_map *map = p__map;
	struct bpf_arena *arena = container_of(map, struct bpf_arena, map);

	if (map->map_type != BPF_MAP_TYPE_ARENA || !arena->user_vm_start || flags)
		return NULL;

	return (void *)arena_alloc_pages(arena, (long)addr__ign, page_cnt, node_id);
}

__bpf_kfunc void bpf_arena_free_pages(void *p__map, void *ptr__ign, u32 page_cnt)
{
	struct bpf_map *map = p__map;
	struct bpf_arena *arena = container_of(map, struct bpf_arena, map);

	if (map->map_type != BPF_MAP_TYPE_ARENA || !arena->user_vm_start)
		return;
	arena_free_pages(arena, (long)ptr__ign, page_cnt);
}
__bpf_kfunc_end_defs();

BTF_KFUNCS_START(arena_kfuncs)
BTF_ID_FLAGS(func, bpf_arena_alloc_pages, KF_TRUSTED_ARGS | KF_SLEEPABLE)
BTF_ID_FLAGS(func, bpf_arena_free_pages, KF_TRUSTED_ARGS | KF_SLEEPABLE)
BTF_KFUNCS_END(arena_kfuncs)

static const struct btf_kfunc_id_set common_kfunc_set = {
	.owner = THIS_MODULE,
	.set   = &arena_kfuncs,
};

static int __init kfunc_init(void)
{
	return register_btf_kfunc_id_set(BPF_PROG_TYPE_UNSPEC, &common_kfunc_set);
}
late_initcall(kfunc_init);
