// SPDX-License-Identifier: GPL-2.0
#include <linux/pgtable.h>
#include <asm/facility.h>
#include <kunit/visibility.h>

#define PTE_POISON	0

struct ipte_batch {
	struct mm_struct *mm;
	unsigned long base_addr;
	unsigned long base_end;
	pte_t *base_pte;
	pte_t *start_pte;
	pte_t *end_pte;
	pte_t cache[PTRS_PER_PTE];
};

static DEFINE_PER_CPU(struct ipte_batch, ipte_range);

static int count_contiguous(pte_t *start, pte_t *end, bool *valid)
{
	pte_t *ptep;

	*valid = !(pte_val(*start) & _PAGE_INVALID);

	for (ptep = start + 1; ptep < end; ptep++) {
		if (*valid) {
			if (pte_val(*ptep) & _PAGE_INVALID)
				break;
		} else {
			if (!(pte_val(*ptep) & _PAGE_INVALID))
				break;
		}
	}

	return ptep - start;
}

static void __invalidate_pte_range(struct mm_struct *mm, unsigned long addr,
				   int nr_ptes, pte_t *ptep)
{
	atomic_inc(&mm->context.flush_count);
	if (cpu_has_tlb_lc() &&
	    cpumask_equal(mm_cpumask(mm), cpumask_of(smp_processor_id())))
		__ptep_ipte_range(addr, nr_ptes - 1, ptep, IPTE_LOCAL);
	else
		__ptep_ipte_range(addr, nr_ptes - 1, ptep, IPTE_GLOBAL);
	atomic_dec(&mm->context.flush_count);
}

static int invalidate_pte_range(struct mm_struct *mm, unsigned long addr,
				pte_t *start, pte_t *end)
{
	int nr_ptes;
	bool valid;

	nr_ptes = count_contiguous(start, end, &valid);
	if (valid)
		__invalidate_pte_range(mm, addr, nr_ptes, start);

	return nr_ptes;
}

static void set_pte_range(struct mm_struct *mm, unsigned long addr,
			  pte_t *ptep, pte_t *end, pte_t *cache)
{
	int i, nr_ptes;

	while (ptep < end) {
		nr_ptes = invalidate_pte_range(mm, addr, ptep, end);

		for (i = 0; i < nr_ptes; i++, ptep++, cache++) {
			__set_pte(ptep, *cache);
			*cache = __pte(PTE_POISON);
		}

		addr += nr_ptes * PAGE_SIZE;
	}
}

static void enter_ipte_batch(struct mm_struct *mm,
			     unsigned long addr, unsigned long end, pte_t *pte)
{
	struct ipte_batch *ib;

	ib = &get_cpu_var(ipte_range);

	ib->mm = mm;
	ib->base_addr = addr;
	ib->base_end = end;
	ib->base_pte = pte;
}

static void leave_ipte_batch(void)
{
	pte_t *ptep, *start, *start_cache, *cache;
	unsigned long start_addr, addr;
	struct ipte_batch *ib;
	int start_idx;

	ib = &get_cpu_var(ipte_range);
	if (!ib->mm) {
		put_cpu_var(ipte_range);
		return;
	}
	put_cpu_var(ipte_range);

	lockdep_assert_preemption_disabled();
	if (!ib->start_pte)
		goto done;

	start = ib->start_pte;
	start_idx = ib->start_pte - ib->base_pte;
	start_addr = ib->base_addr + start_idx * PAGE_SIZE;
	addr = start_addr;
	start_cache = &ib->cache[start_idx];
	cache = start_cache;
	for (ptep = start; ptep < ib->end_pte; ptep++, cache++, addr += PAGE_SIZE) {
		if (pte_val(*cache) == PTE_POISON) {
			if (start) {
				set_pte_range(ib->mm, start_addr, start, ptep, start_cache);
				start = NULL;
			}
		} else if (!start) {
			start = ptep;
			start_addr = addr;
			start_cache = cache;
		}
	}
	set_pte_range(ib->mm, start_addr, start, ptep, start_cache);

	ib->start_pte = NULL;
	ib->end_pte = NULL;

done:
	ib->mm = NULL;
	ib->base_addr = 0;
	ib->base_end = 0;
	ib->base_pte = NULL;

	put_cpu_var(ipte_range);
}

static void flush_lazy_mmu_mode(void)
{
	unsigned long addr, end;
	struct ipte_batch *ib;
	struct mm_struct *mm;
	pte_t *pte;

	ib = &get_cpu_var(ipte_range);
	if (ib->mm) {
		mm = ib->mm;
		addr = ib->base_addr;
		end = ib->base_end;
		pte = ib->base_pte;

		leave_ipte_batch();
		enter_ipte_batch(mm, addr, end, pte);
	}
	put_cpu_var(ipte_range);
}

void arch_enter_lazy_mmu_mode_pte(struct mm_struct *mm,
				  unsigned long addr, unsigned long end,
				  pte_t *pte)
{
	if (!test_facility(13))
		return;
	enter_ipte_batch(mm, addr, end, pte);
}
EXPORT_SYMBOL_IF_KUNIT(arch_enter_lazy_mmu_mode_pte);

void arch_leave_lazy_mmu_mode(void)
{
	if (!test_facility(13))
		return;
	leave_ipte_batch();
}
EXPORT_SYMBOL_IF_KUNIT(arch_leave_lazy_mmu_mode);

void arch_flush_lazy_mmu_mode(void)
{
	if (!test_facility(13))
		return;
	flush_lazy_mmu_mode();
}
EXPORT_SYMBOL_IF_KUNIT(arch_flush_lazy_mmu_mode);

static void __ipte_batch_set_pte(struct ipte_batch *ib, pte_t *ptep, pte_t pte)
{
	unsigned int idx = ptep - ib->base_pte;

	lockdep_assert_preemption_disabled();
	ib->cache[idx] = pte;

	if (!ib->start_pte) {
		ib->start_pte = ptep;
		ib->end_pte = ptep + 1;
	} else if (ptep < ib->start_pte) {
		ib->start_pte = ptep;
	} else if (ptep + 1 > ib->end_pte) {
		ib->end_pte = ptep + 1;
	}
}

static pte_t __ipte_batch_ptep_get(struct ipte_batch *ib, pte_t *ptep)
{
	unsigned int idx = ptep - ib->base_pte;

	lockdep_assert_preemption_disabled();
	if (pte_val(ib->cache[idx]) == PTE_POISON)
		return __ptep_get(ptep);
	return ib->cache[idx];
}

static bool lazy_mmu_mode(struct ipte_batch *ib, struct mm_struct *mm, pte_t *ptep)
{
	unsigned int nr_ptes;

	lockdep_assert_preemption_disabled();
	if (!is_lazy_mmu_mode_active())
		return false;
	if (!mm)
		return false;
	if (!ib->mm)
		return false;
	if (ptep < ib->base_pte)
		return false;
	nr_ptes = (ib->base_end - ib->base_addr) / PAGE_SIZE;
	if (ptep >= ib->base_pte + nr_ptes)
		return false;
	return true;
}

static struct ipte_batch *get_ipte_batch_nomm(pte_t *ptep)
{
	struct ipte_batch *ib;

	ib = &get_cpu_var(ipte_range);
	if (!lazy_mmu_mode(ib, ib->mm, ptep)) {
		put_cpu_var(ipte_range);
		return NULL;
	}

	return ib;
}

static struct ipte_batch *get_ipte_batch(struct mm_struct *mm, pte_t *ptep)
{
	struct ipte_batch *ib;

	ib = &get_cpu_var(ipte_range);
	if (!lazy_mmu_mode(ib, mm, ptep)) {
		put_cpu_var(ipte_range);
		return NULL;
	}

	return ib;
}

static void put_ipte_batch(struct ipte_batch *ib)
{
	put_cpu_var(ipte_range);
}

bool ipte_batch_set_pte(pte_t *ptep, pte_t pte)
{
	struct ipte_batch *ib;

	ib = get_ipte_batch_nomm(ptep);
	if (!ib)
		return false;
	__ipte_batch_set_pte(ib, ptep, pte);
	put_ipte_batch(ib);

	return true;
}

bool ipte_batch_ptep_get(pte_t *ptep, pte_t *res)
{
	struct ipte_batch *ib;

	ib = get_ipte_batch_nomm(ptep);
	if (!ib)
		return false;
	*res = __ipte_batch_ptep_get(ib, ptep);
	put_ipte_batch(ib);

	return true;
}

bool ipte_batch_ptep_test_and_clear_young(struct vm_area_struct *vma,
					  unsigned long addr, pte_t *ptep,
					  int *res)
{
	struct ipte_batch *ib;
	pte_t pte, old;

	ib = get_ipte_batch(vma->vm_mm, ptep);
	if (!ib)
		return false;

	old = __ipte_batch_ptep_get(ib, ptep);
	pte = pte_mkold(old);
	__ipte_batch_set_pte(ib, ptep, pte);

	put_ipte_batch(ib);

	*res = pte_young(old);

	return true;
}

bool ipte_batch_ptep_get_and_clear(struct mm_struct *mm,
				   unsigned long addr, pte_t *ptep, pte_t *res)
{
	struct ipte_batch *ib;
	pte_t pte, old;

	ib = get_ipte_batch(mm, ptep);
	if (!ib)
		return false;

	old = __ipte_batch_ptep_get(ib, ptep);
	pte = __pte(_PAGE_INVALID);
	__ipte_batch_set_pte(ib, ptep, pte);

	put_ipte_batch(ib);

	*res = old;

	return true;
}

bool ipte_batch_ptep_get_and_clear_full(struct mm_struct *mm,
					unsigned long addr, pte_t *ptep,
					int full, pte_t *res)
{
	struct ipte_batch *ib;
	pte_t pte, old;

	ib = get_ipte_batch(mm, ptep);
	if (!ib)
		return false;

	old = __ipte_batch_ptep_get(ib, ptep);
	pte = __pte(_PAGE_INVALID);
	__ipte_batch_set_pte(ib, ptep, pte);

	put_ipte_batch(ib);

	*res = old;

	return true;
}

bool ipte_batch_ptep_modify_prot_start(struct vm_area_struct *vma,
				       unsigned long addr, pte_t *ptep, pte_t *res)
{
	return ipte_batch_ptep_get_and_clear(vma->vm_mm, addr, ptep, res);
}

bool ipte_batch_ptep_modify_prot_commit(struct vm_area_struct *vma,
					unsigned long addr, pte_t *ptep,
					pte_t old_pte, pte_t pte)
{
	struct ipte_batch *ib;

	ib = get_ipte_batch(vma->vm_mm, ptep);
	if (!ib)
		return false;
	__ipte_batch_set_pte(ib, ptep, pte);
	put_ipte_batch(ib);

	return true;
}

bool ipte_batch_ptep_set_wrprotect(struct mm_struct *mm,
				   unsigned long addr, pte_t *ptep)
{
	struct ipte_batch *ib;
	pte_t pte, old;

	ib = get_ipte_batch(mm, ptep);
	if (!ib)
		return false;

	old = __ipte_batch_ptep_get(ib, ptep);
	pte = pte_wrprotect(old);
	__ipte_batch_set_pte(ib, ptep, pte);

	put_ipte_batch(ib);

	return true;
}
