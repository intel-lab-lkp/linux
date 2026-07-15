// SPDX-License-Identifier: GPL-2.0
#include <linux/pgtable.h>
#include <linux/kasan.h>
#include <linux/slab.h>
#include <asm/facility.h>
#include <asm/lazy_mmu.h>
#include <kunit/visibility.h>

#define PTE_POISON	_PAGE_LARGE

struct ipte_range {
	struct mm_struct *mm;
	unsigned long base_addr;
	unsigned long base_end;
	pte_t *base_pte;
	pte_t *start_pte;
	pte_t *end_pte;
	pte_t cache[PTRS_PER_PTE];
};

static DEFINE_PER_CPU(struct ipte_range *, ipte_range);

static int count_contiguous(pte_t *start, pte_t *end, bool *valid)
{
	unsigned long page_invalid_bit;
	pte_t *ptep, pte;

	pte = __ptep_get(start);
	page_invalid_bit = pte_val(pte) & _PAGE_INVALID;

	for (ptep = start + 1; ptep < end; ptep++) {
		pte = __ptep_get(ptep);
		if ((pte_val(pte) & _PAGE_INVALID) != page_invalid_bit)
			break;
	}

	*valid = !(page_invalid_bit);
	return ptep - start;
}

static void __invalidate_pte_range(struct mm_struct *mm, unsigned long addr,
				   int nr_ptes, pte_t *ptep)
{
	atomic_inc(&mm->context.flush_count);
	if (cpu_has_tlb_lc() && cpumask_equal(mm_cpumask(mm), cpumask_of(smp_processor_id())))
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

static void enter_ipte_norange(void)
{
	struct ipte_range __maybe_unused *range;

	if (!test_facility(13))
		return;

	range = get_cpu_var(ipte_range);
	get_lowcore()->lazy_mmu_count++;
}

static void enter_ipte_range(struct mm_struct *mm,
			     unsigned long addr, unsigned long end, pte_t *pte)
{
	struct ipte_range *range;

	if (!test_facility(13))
		return;

	range = get_cpu_var(ipte_range);
	get_lowcore()->lazy_mmu_count++;

	if (mm_is_protected(mm))
		return;

	range->mm = mm;
	range->base_addr = addr;
	range->base_end = end;
	range->base_pte = pte;
}

static void leave_ipte_range(void)
{
	pte_t *ptep, *start, *start_cache, *cache;
	unsigned long start_addr, addr;
	struct ipte_range *range;
	int start_idx;

	if (!test_facility(13))
		return;

	lockdep_assert_preemption_disabled();
	range = this_cpu_read(ipte_range);
	if (!range->mm)
		goto norange;
	if (!range->start_pte)
		goto done;

	start = range->start_pte;
	start_idx = range->start_pte - range->base_pte;
	start_addr = range->base_addr + start_idx * PAGE_SIZE;
	addr = start_addr;
	start_cache = &range->cache[start_idx];
	cache = start_cache;
	for (ptep = start; ptep < range->end_pte; ptep++, cache++, addr += PAGE_SIZE) {
		if (pte_val(*cache) == PTE_POISON) {
			if (start) {
				set_pte_range(range->mm, start_addr, start, ptep, start_cache);
				start = NULL;
			}
		} else if (!start) {
			start = ptep;
			start_addr = addr;
			start_cache = cache;
		}
	}
	set_pte_range(range->mm, start_addr, start, ptep, start_cache);

	range->start_pte = NULL;
	range->end_pte = NULL;

done:
	range->mm = NULL;
	range->base_addr = 0;
	range->base_end = 0;
	range->base_pte = NULL;

norange:
	get_lowcore()->lazy_mmu_count--;
	put_cpu_var(ipte_range);
}

static void flush_lazy_mmu_mode(void)
{
	unsigned long addr, end;
	struct ipte_range *range;
	struct mm_struct *mm;
	pte_t *pte;

	if (!test_facility(13))
		return;

	range = get_cpu_var(ipte_range);
	if (range->mm) {
		mm = range->mm;
		addr = range->base_addr;
		end = range->base_end;
		pte = range->base_pte;

		leave_ipte_range();
		enter_ipte_range(mm, addr, end, pte);
	}
	put_cpu_var(ipte_range);
}

void arch_enter_lazy_mmu_mode(void)
{
	enter_ipte_norange();
}
EXPORT_SYMBOL_IF_KUNIT(arch_enter_lazy_mmu_mode);

void arch_enter_lazy_mmu_mode_with_ptes(struct mm_struct *mm,
					unsigned long addr, unsigned long end,
					pte_t *pte)
{
	enter_ipte_range(mm, addr, end, pte);
}
EXPORT_SYMBOL_IF_KUNIT(arch_enter_lazy_mmu_mode_with_ptes);

void arch_leave_lazy_mmu_mode(void)
{
	leave_ipte_range();
}
EXPORT_SYMBOL_IF_KUNIT(arch_leave_lazy_mmu_mode);

void arch_flush_lazy_mmu_mode(void)
{
	flush_lazy_mmu_mode();
}
EXPORT_SYMBOL_IF_KUNIT(arch_flush_lazy_mmu_mode);

static void __ipte_range_set_pte(struct ipte_range *range, pte_t *ptep, pte_t pte)
{
	unsigned int idx = ptep - range->base_pte;

	lockdep_assert_preemption_disabled();
	range->cache[idx] = pte;

	if (!range->start_pte) {
		range->start_pte = ptep;
		range->end_pte = ptep + 1;
	} else if (ptep < range->start_pte) {
		range->start_pte = ptep;
	} else if (ptep + 1 > range->end_pte) {
		range->end_pte = ptep + 1;
	}
}

static pte_t __ipte_range_ptep_get(struct ipte_range *range, pte_t *ptep)
{
	unsigned int idx = ptep - range->base_pte;

	lockdep_assert_preemption_disabled();
	if (pte_val(range->cache[idx]) == PTE_POISON)
		return __ptep_get(ptep);
	return range->cache[idx];
}

static struct ipte_range *this_ipte_range(pte_t *ptep)
{
	struct ipte_range *range;
	unsigned int nr_ptes;

	range = this_cpu_read(ipte_range);
	if (ptep < range->base_pte)
		return NULL;
	nr_ptes = (range->base_end - range->base_addr) / PAGE_SIZE;
	if (ptep >= range->base_pte + nr_ptes)
		return NULL;

	return range;
}

bool __lazy_mmu_set_pte(pte_t *ptep, pte_t pte)
{
	struct ipte_range *range;

	range = this_ipte_range(ptep);
	if (!range)
		return false;

	__ipte_range_set_pte(range, ptep, pte);

	return true;
}

bool __lazy_mmu_ptep_get(pte_t *ptep, pte_t *res)
{
	struct ipte_range *range;

	range = this_ipte_range(ptep);
	if (!range)
		return false;

	*res = __ipte_range_ptep_get(range, ptep);

	return true;
}

bool __lazy_mmu_ptep_test_and_clear_young(unsigned long addr, pte_t *ptep, int *res)
{
	struct ipte_range *range;
	pte_t pte, old;

	range = this_ipte_range(ptep);
	if (!range)
		return false;

	old = __ipte_range_ptep_get(range, ptep);
	pte = pte_mkold(old);
	__ipte_range_set_pte(range, ptep, pte);
	*res = pte_young(old);

	return true;
}

bool __lazy_mmu_ptep_get_and_clear(unsigned long addr, pte_t *ptep, pte_t *res)
{
	struct ipte_range *range;
	pte_t pte, old;

	range = this_ipte_range(ptep);
	if (!range)
		return false;

	old = __ipte_range_ptep_get(range, ptep);
	pte = __pte(_PAGE_INVALID);
	__ipte_range_set_pte(range, ptep, pte);
	*res = old;

	return true;
}

bool __lazy_mmu_ptep_modify_prot_start(unsigned long addr, pte_t *ptep, pte_t *res)
{
	return __lazy_mmu_ptep_get_and_clear(addr, ptep, res);
}

bool __lazy_mmu_ptep_modify_prot_commit(unsigned long addr, pte_t *ptep,
					pte_t old_pte, pte_t pte)
{
	struct ipte_range *range;

	range = this_ipte_range(ptep);
	if (!range)
		return false;

	__ipte_range_set_pte(range, ptep, pte);

	return true;
}

bool __lazy_mmu_ptep_set_wrprotect(unsigned long addr, pte_t *ptep)
{
	struct ipte_range *range;
	pte_t pte;

	range = this_ipte_range(ptep);
	if (!range)
		return false;

	pte = __ipte_range_ptep_get(range, ptep);
	if (pte_write(pte)) {
		pte = pte_wrprotect(pte);
		__ipte_range_set_pte(range, ptep, pte);
	}

	return true;
}

int lazy_mmu_online_cpu(gfp_t gfp, unsigned int cpu)
{
	struct ipte_range *range;
	int i;

	if (!test_facility(13))
		return 0;

	range = kzalloc_obj(*range, gfp);
	if (!range)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(range->cache); i++)
		range->cache[i] = __pte(PTE_POISON);
	per_cpu(ipte_range, cpu) = range;

	return 0;
}

void lazy_mmu_offline_cpu(unsigned int cpu)
{
	struct ipte_range *range;

	if (!test_facility(13))
		return;

	range = per_cpu(ipte_range, cpu);
	per_cpu(ipte_range, cpu) = NULL;
	kfree(range);
}

void __init lazy_mmu_online_boot_cpu(void)
{
	lazy_mmu_online_cpu(GFP_ATOMIC, 0);
}
