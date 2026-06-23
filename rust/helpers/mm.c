// SPDX-License-Identifier: GPL-2.0

#include <linux/list_lru.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>

__rust_helper void rust_helper_mmgrab(struct mm_struct *mm)
{
	mmgrab(mm);
}

__rust_helper unsigned long rust_helper_list_lru_count(struct list_lru *lru)
{
	return list_lru_count(lru);
}

__rust_helper unsigned long rust_helper_list_lru_walk(struct list_lru *lru,
						      list_lru_walk_cb isolate,
						      void *cb_arg,
						      unsigned long nr_to_walk)
{
	return list_lru_walk(lru, isolate, cb_arg, nr_to_walk);
}

__rust_helper void rust_helper_mmdrop(struct mm_struct *mm)
{
	mmdrop(mm);
}

__rust_helper void rust_helper_mmget(struct mm_struct *mm)
{
	mmget(mm);
}

__rust_helper bool rust_helper_mmget_not_zero(struct mm_struct *mm)
{
	return mmget_not_zero(mm);
}

__rust_helper void rust_helper_mmap_read_lock(struct mm_struct *mm)
{
	mmap_read_lock(mm);
}

__rust_helper bool rust_helper_mmap_read_trylock(struct mm_struct *mm)
{
	return mmap_read_trylock(mm);
}

__rust_helper void rust_helper_mmap_read_unlock(struct mm_struct *mm)
{
	mmap_read_unlock(mm);
}

__rust_helper struct vm_area_struct *
rust_helper_vma_lookup(struct mm_struct *mm, unsigned long addr)
{
	return vma_lookup(mm, addr);
}

__rust_helper void rust_helper_vma_end_read(struct vm_area_struct *vma)
{
	vma_end_read(vma);
}
