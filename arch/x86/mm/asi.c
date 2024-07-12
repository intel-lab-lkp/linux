// SPDX-License-Identifier: GPL-2.0
#include <linux/compiler_types.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>

#include <linux/init.h>
#include <linux/pgtable.h>

#include <asm/cmdline.h>
#include <asm/page.h>
#include <asm/pgalloc.h>
#include <asm/mmu_context.h>
#include <asm/traps.h>

#include "mm_internal.h"
#include "../../../mm/internal.h"

static struct asi_class asi_class[ASI_MAX_NUM];
static DEFINE_SPINLOCK(asi_class_lock);

DEFINE_PER_CPU_ALIGNED(struct asi *, curr_asi);
EXPORT_SYMBOL(curr_asi);

static __aligned(PAGE_SIZE) pgd_t asi_global_nonsensitive_pgd[PTRS_PER_PGD];

struct asi __asi_global_nonsensitive = {
	.pgd = asi_global_nonsensitive_pgd,
	.mm = &init_mm,
};

static inline bool asi_class_registered(int index)
{
	return asi_class[index].name != NULL;
}

static inline bool asi_index_valid(int index)
{
	return index >= 0 && index < ARRAY_SIZE(asi_class);
}

int asi_register_class(const char *name, const struct asi_hooks *ops)
{
	int i;

	if (!boot_cpu_has(X86_FEATURE_ASI))
		return 0;

	VM_BUG_ON(name == NULL);

	spin_lock(&asi_class_lock);

	for (i = 0; i < ARRAY_SIZE(asi_class); i++) {
		if (!asi_class_registered(i)) {
			asi_class[i].name = name;
			if (ops != NULL)
				asi_class[i].ops = *ops;
			break;
		}
	}

	spin_unlock(&asi_class_lock);

	if (i == ARRAY_SIZE(asi_class))
		i = -ENOSPC;

	return i;
}
EXPORT_SYMBOL_GPL(asi_register_class);

void asi_unregister_class(int index)
{
	if (!boot_cpu_has(X86_FEATURE_ASI))
		return;

	BUG_ON(!asi_index_valid(index));

	spin_lock(&asi_class_lock);

	WARN_ON(asi_class[index].name == NULL);
	memset(&asi_class[index], 0, sizeof(struct asi_class));

	spin_unlock(&asi_class_lock);
}
EXPORT_SYMBOL_GPL(asi_unregister_class);

#ifndef mm_inc_nr_p4ds
#define mm_inc_nr_p4ds(mm)	do {} while (false)
#endif

#ifndef mm_dec_nr_p4ds
#define mm_dec_nr_p4ds(mm)	do {} while (false)
#endif

#define pte_offset		pte_offset_kernel

/*
 * asi_p4d_alloc, asi_pud_alloc, asi_pmd_alloc, asi_pte_alloc.
 *
 * These are like the normal xxx_alloc functions, but:
 *
 *  - They use atomic operations instead of taking a spinlock; this allows them
 *    to be used from interrupts. This is necessary because we use the page
 *    allocator from interrupts and the page allocator ultimately calls this
 *    code.
 *  - They support customizing the allocation flags.
 *  - They avoid infinite recursion when the page allocator calls back to
 *    asi_map
 *
 * On the other hand, they do not use the normal page allocation infrastructure,
 * that means that PTE pages do not have the PageTable type nor the PagePgtable
 * flag and we don't increment the meminfo stat (NR_PAGETABLE) as they do.
 *
 * As an optimisation we attempt to map the pagetables in
 * ASI_GLOBAL_NONSENSITIVE, but this can fail, and for simplicity we don't do
 * anything about that. This means it's invalid to access ASI pagetables from a
 * critical section.
 */
static_assert(!IS_ENABLED(CONFIG_PARAVIRT));
#define DEFINE_ASI_PGTBL_ALLOC(base, level)				\
static level##_t * asi_##level##_alloc(struct asi *asi,			\
				       base##_t *base, ulong addr,	\
				       gfp_t flags)			\
{									\
	if (unlikely(base##_none(*base))) {				\
		/* Stop asi_map calls causing recursive allocation */	\
		gfp_t pgtbl_gfp = flags | __GFP_SENSITIVE;		\
		ulong pgtbl = get_zeroed_page(pgtbl_gfp);		\
		phys_addr_t pgtbl_pa;					\
		int err;						\
									\
		if (!pgtbl)						\
			return NULL;					\
									\
		pgtbl_pa = __pa(pgtbl);					\
									\
		if (cmpxchg((ulong *)base, 0,				\
			    pgtbl_pa | _PAGE_TABLE) != 0) {		\
			free_page(pgtbl);				\
			goto out;					\
		}							\
									\
		mm_inc_nr_##level##s(asi->mm);				\
									\
		err = asi_map_gfp(ASI_GLOBAL_NONSENSITIVE,		\
				  (void *)pgtbl, PAGE_SIZE, flags);	\
		if (err)						\
			/* Should be rare. Spooky. */			\
			pr_warn_ratelimited("Created sensitive ASI %s (%pK, maps %luK).\n",\
				#level, (void *)pgtbl, addr);		\
		else							\
			__SetPageGlobalNonSensitive(virt_to_page(pgtbl));\
									\
	}								\
out:									\
	VM_BUG_ON(base##_leaf(*base));					\
	return level##_offset(base, addr);				\
}

DEFINE_ASI_PGTBL_ALLOC(pgd, p4d)
DEFINE_ASI_PGTBL_ALLOC(p4d, pud)
DEFINE_ASI_PGTBL_ALLOC(pud, pmd)
DEFINE_ASI_PGTBL_ALLOC(pmd, pte)

void __init asi_check_boottime_disable(void)
{
	bool enabled = IS_ENABLED(CONFIG_MITIGATION_ADDRESS_SPACE_ISOLATION_DEFAULT_ON);
	char arg[4];
	int ret;

	ret = cmdline_find_option(boot_command_line, "asi", arg, sizeof(arg));
	if (ret == 3 && !strncmp(arg, "off", 3)) {
		enabled = false;
		pr_info("ASI disabled through kernel command line.\n");
	} else if (ret == 2 && !strncmp(arg, "on", 2)) {
		enabled = true;
		pr_info("Ignoring asi=on param while ASI implementation is incomplete.\n");
	} else {
		pr_info("ASI %s by default.\n",
			enabled ? "enabled" : "disabled");
	}

	if (enabled)
		pr_info("ASI enablement ignored due to incomplete implementation.\n");
}

/*
 * Map data by sharing sub-PGD pagetables with the unrestricted mapping. This is
 * more efficient than asi_map, but only works when you know the whole top-level
 * page needs to be mapped in the restricted tables. Note that the size of the
 * mappings this creates differs between 4 and 5-level paging.
 */
static void asi_clone_pgd(pgd_t *dst_table, pgd_t *src_table, size_t addr)
{
	pgd_t *src = pgd_offset_pgd(src_table, addr);
	pgd_t *dst = pgd_offset_pgd(dst_table, addr);

	if (!pgd_val(*dst))
		set_pgd(dst, *src);
	else
		WARN_ON_ONCE(pgd_val(*dst) != pgd_val(*src));
}

/*
 * For 4-level paging this is exactly the same as asi_clone_pgd. For 5-level
 * paging it clones one level lower. So this always creates a mapping of the
 * same size.
 */
static void asi_clone_p4d(pgd_t *dst_table, pgd_t *src_table, size_t addr)
{
	pgd_t *src_pgd = pgd_offset_pgd(src_table, addr);
	pgd_t *dst_pgd = pgd_offset_pgd(dst_table, addr);
	p4d_t *src_p4d = p4d_alloc(&init_mm, src_pgd, addr);
	p4d_t *dst_p4d = p4d_alloc(&init_mm, dst_pgd, addr);

	if (!p4d_val(*dst_p4d))
		set_p4d(dst_p4d, *src_p4d);
	else
		WARN_ON_ONCE(p4d_val(*dst_p4d) != p4d_val(*src_p4d));
}

/*
 * percpu_addr is where the linker put the percpu variable. asi_map_percpu finds
 * the place where the percpu allocator copied the data during boot.
 *
 * This is necessary even when the page allocator defaults to
 * global-nonsensitive, because the percpu allocator uses the memblock allocator
 * for early allocations.
 */
static int asi_map_percpu(struct asi *asi, void *percpu_addr, size_t len)
{
	int cpu, err;
	void *ptr;

	for_each_possible_cpu(cpu) {
		ptr = per_cpu_ptr(percpu_addr, cpu);
		err = asi_map(asi, ptr, len);
		if (err)
			return err;
	}

	return 0;
}

static int __init asi_global_init(void)
{
	int err;

	if (!boot_cpu_has(X86_FEATURE_ASI))
		return 0;

	/*
	 * Lower-level pagetables for global nonsensitive mappings are shared,
	 * but the PGD has to be copied into each domain during asi_init. To
	 * avoid needing to synchronize new mappings into pre-existing domains
	 * we just pre-allocate all of the relevant level N-1 entries so that
	 * the global nonsensitive PGD already has pointers that can be copied
	 * when new domains get asi_init()ed.
	 */
	preallocate_sub_pgd_pages(asi_global_nonsensitive_pgd,
				  PAGE_OFFSET,
				  PAGE_OFFSET + PFN_PHYS(max_pfn) - 1,
				  "ASI Global Non-sensitive direct map");
	preallocate_sub_pgd_pages(asi_global_nonsensitive_pgd,
				  VMALLOC_START, VMALLOC_END,
				  "ASI Global Non-sensitive vmalloc");

	/* Map all kernel text and static data */
	err = asi_map(ASI_GLOBAL_NONSENSITIVE, (void *)__START_KERNEL,
		      (size_t)_end - __START_KERNEL);
	if (WARN_ON(err))
		return err;
	err = asi_map(ASI_GLOBAL_NONSENSITIVE, (void *)FIXADDR_START,
		      FIXADDR_SIZE);
	if (WARN_ON(err))
		return err;
	/* Map all static percpu data */
	err = asi_map_percpu(
		ASI_GLOBAL_NONSENSITIVE,
		__per_cpu_start, __per_cpu_end - __per_cpu_start);
	if (WARN_ON(err))
		return err;

	/*
	 * The next areas are mapped using shared sub-P4D paging structures
	 * (asi_clone_p4d instead of asi_map), since we know the whole P4D will
	 * be mapped.
	 */
	asi_clone_p4d(asi_global_nonsensitive_pgd, init_mm.pgd,
		      CPU_ENTRY_AREA_BASE);
#ifdef CONFIG_X86_ESPFIX64
	asi_clone_p4d(asi_global_nonsensitive_pgd, init_mm.pgd,
		      ESPFIX_BASE_ADDR);
#endif
	/*
	 * The vmemmap area actually _must_ be cloned via shared paging
	 * structures, since mappings can potentially change dynamically when
	 * hugetlbfs pages are created or broken down.
	 *
	 * We always clone 2 PGDs, this is a corrolary of the sizes of struct
	 * page, a page, and the physical address space.
	 */
	WARN_ON(sizeof(struct page) * MAXMEM / PAGE_SIZE != 2 * (1UL << PGDIR_SHIFT));
	asi_clone_pgd(asi_global_nonsensitive_pgd, init_mm.pgd, VMEMMAP_START);
	asi_clone_pgd(asi_global_nonsensitive_pgd, init_mm.pgd,
		      VMEMMAP_START + (1UL << PGDIR_SHIFT));

	return 0;
}
subsys_initcall(asi_global_init)

static void __asi_destroy(struct asi *asi)
{
	WARN_ON_ONCE(asi->ref_count <= 0);
	if (--(asi->ref_count) > 0)
		return;

	free_pages((ulong)asi->pgd, PGD_ALLOCATION_ORDER);
	memset(asi, 0, sizeof(struct asi));
}

int asi_init(struct mm_struct *mm, int asi_index, struct asi **out_asi)
{
	struct asi *asi;
	int err = 0;
	uint i;

	*out_asi = NULL;

	if (!boot_cpu_has(X86_FEATURE_ASI))
		return 0;

	BUG_ON(!asi_index_valid(asi_index));

	asi = &mm->asi[asi_index];

	BUG_ON(!asi_class_registered(asi_index));

	mutex_lock(&mm->asi_init_lock);

	if (asi->ref_count++ > 0)
		goto exit_unlock; /* err is 0 */

	BUG_ON(asi->pgd != NULL);

	/*
	 * For now, we allocate 2 pages to avoid any potential problems with
	 * KPTI code. This won't be needed once KPTI is folded into the ASI
	 * framework.
	 */
	asi->pgd = (pgd_t *)__get_free_pages(
		GFP_KERNEL_ACCOUNT | __GFP_ZERO, PGD_ALLOCATION_ORDER);
	if (!asi->pgd) {
		err = -ENOMEM;
		goto exit_unlock;
	}

	asi->class = &asi_class[asi_index];
	asi->mm = mm;
	asi->index = asi_index;

	for (i = KERNEL_PGD_BOUNDARY; i < PTRS_PER_PGD; i++)
		set_pgd(asi->pgd + i, asi_global_nonsensitive_pgd[i]);

exit_unlock:
	if (err)
		__asi_destroy(asi);
	else
		*out_asi = asi;

	mutex_unlock(&mm->asi_init_lock);

	return err;
}
EXPORT_SYMBOL_GPL(asi_init);

void asi_destroy(struct asi *asi)
{
	struct mm_struct *mm;

	if (!boot_cpu_has(X86_FEATURE_ASI) || !asi)
		return;

	mm = asi->mm;
	/*
	 * We would need this mutex even if the refcount was atomic as we need
	 * to block concurrent asi_init calls.
	 */
	mutex_lock(&mm->asi_init_lock);
	__asi_destroy(asi);
	mutex_unlock(&mm->asi_init_lock);
}
EXPORT_SYMBOL_GPL(asi_destroy);

noinstr void __asi_enter(void)
{
	u64 asi_cr3;
	u16 pcid;
	struct asi *target = asi_get_target(current);

	/*
	 * This is actually false restriction, it should be fine to be
	 * preemptible during the critical section. But we haven't tested it. We
	 * will also need to disable preemption during this function itself and
	 * perhaps elsewhere. This false restriction shouldn't create any
	 * additional burden for ASI clients anyway: the critical section has
	 * to be as short as possible to avoid unnecessary ASI transitions so
	 * disabling preemption should be fine.
	 */
	VM_BUG_ON(preemptible());
	VM_BUG_ON(current->thread.asi_state.intr_nest_depth != 0);

	if (!target || target == this_cpu_read(curr_asi))
		return;

	VM_BUG_ON(this_cpu_read(cpu_tlbstate.loaded_mm) ==
		  LOADED_MM_SWITCHING);

	/*
	 * Must update curr_asi before writing CR3 to ensure an interrupting
	 * asi_exit sees that it may need to switch address spaces.
	 */
	this_cpu_write(curr_asi, target);

	pcid = asi_pcid(target, this_cpu_read(cpu_tlbstate.loaded_mm_asid));
	asi_cr3 = build_cr3_pcid(target->pgd, pcid, tlbstate_lam_cr3_mask(), false);
	write_cr3(asi_cr3);

	if (target->class->ops.post_asi_enter)
		target->class->ops.post_asi_enter();
}

noinstr void asi_enter(struct asi *asi)
{
	if (!static_asi_enabled())
		return;

	VM_WARN_ON_ONCE(!asi);

	asi_set_target(current, asi);
	barrier();

	__asi_enter();
}
EXPORT_SYMBOL_GPL(asi_enter);

inline_or_noinstr void asi_relax(void)
{
	if (static_asi_enabled()) {
		barrier();
		asi_set_target(current, NULL);
	}
}
EXPORT_SYMBOL_GPL(asi_relax);

noinstr void asi_exit(void)
{
	u64 unrestricted_cr3;
	struct asi *asi;

	if (!static_asi_enabled())
		return;

	preempt_disable_notrace();

	VM_BUG_ON(this_cpu_read(cpu_tlbstate.loaded_mm) ==
		  LOADED_MM_SWITCHING);

	asi = this_cpu_read(curr_asi);
	if (asi) {
		WARN_ON_ONCE(asi_in_critical_section());

		if (asi->class->ops.pre_asi_exit)
			asi->class->ops.pre_asi_exit();

		unrestricted_cr3 =
			build_cr3(this_cpu_read(cpu_tlbstate.loaded_mm)->pgd,
				  this_cpu_read(cpu_tlbstate.loaded_mm_asid),
				  tlbstate_lam_cr3_mask());

		write_cr3(unrestricted_cr3);
		/*
		 * Must not update curr_asi until after CR3 write, otherwise a
		 * re-entrant call might not enter this branch. (This means we
		 * might do unnecessary CR3 writes).
		 */
		this_cpu_write(curr_asi, NULL);
	}

	preempt_enable_notrace();
}
EXPORT_SYMBOL_GPL(asi_exit);

void asi_init_mm_state(struct mm_struct *mm)
{
	if (!boot_cpu_has(X86_FEATURE_ASI))
		return;

	memset(mm->asi, 0, sizeof(mm->asi));
	mutex_init(&mm->asi_init_lock);
}

static bool is_page_within_range(unsigned long addr, unsigned long page_size,
				 unsigned long range_start, unsigned long range_end)
{
	unsigned long page_start = ALIGN_DOWN(addr, page_size);
	unsigned long page_end = page_start + page_size;

	return page_start >= range_start && page_end <= range_end;
}

static bool follow_physaddr(
	pgd_t *pgd_table, unsigned long virt,
	phys_addr_t *phys, unsigned long *page_size, ulong *flags)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	/* This may be written using lookup_address_in_*, see kcl/675039. */

	*page_size = PGDIR_SIZE;
	pgd = pgd_offset_pgd(pgd_table, virt);
	if (!pgd_present(*pgd))
		return false;
	if (pgd_leaf(*pgd)) {
		*phys = PFN_PHYS(pgd_pfn(*pgd)) | (virt & ~PGDIR_MASK);
		*flags = pgd_flags(*pgd);
		return true;
	}

	*page_size = P4D_SIZE;
	p4d = p4d_offset(pgd, virt);
	if (!p4d_present(*p4d))
		return false;
	if (p4d_leaf(*p4d)) {
		*phys = PFN_PHYS(p4d_pfn(*p4d)) | (virt & ~P4D_MASK);
		*flags = p4d_flags(*p4d);
		return true;
	}

	*page_size = PUD_SIZE;
	pud = pud_offset(p4d, virt);
	if (!pud_present(*pud))
		return false;
	if (pud_leaf(*pud)) {
		*phys = PFN_PHYS(pud_pfn(*pud)) | (virt & ~PUD_MASK);
		*flags = pud_flags(*pud);
		return true;
	}

	*page_size = PMD_SIZE;
	pmd = pmd_offset(pud, virt);
	if (!pmd_present(*pmd))
		return false;
	if (pmd_leaf(*pmd)) {
		*phys = PFN_PHYS(pmd_pfn(*pmd)) | (virt & ~PMD_MASK);
		*flags = pmd_flags(*pmd);
		return true;
	}

	*page_size = PAGE_SIZE;
	pte = pte_offset_map(pmd, virt);
	if (!pte)
		return false;

	if (!pte_present(*pte)) {
		pte_unmap(pte);
		return false;
	}

	*phys = PFN_PHYS(pte_pfn(*pte)) | (virt & ~PAGE_MASK);
	*flags = pte_flags(*pte);

	pte_unmap(pte);
	return true;
}

/*
 * Map the given range into the ASI page tables. The source of the mapping is
 * the regular unrestricted page tables. Can be used to map any kernel memory.
 *
 * In contrast to some internal ASI logic (asi_clone_pgd and asi_clone_p4d) this
 * never shares pagetables between restricted and unrestricted address spaces,
 * instead it creates wholly new equivalent mappings.
 *
 * The caller MUST ensure that the source mapping will not change during this
 * function. For dynamic kernel memory, this is generally ensured by mapping the
 * memory within the allocator.
 *
 * If this fails, it may leave partial mappings behind. You must asi_unmap them,
 * bearing in mind asi_unmap's requirements on the calling context. Part of the
 * reason for this is that we don't want to unexpectedly undo mappings that
 * weren't created by the present caller.
 *
 * This must not be called from the critical section, as ASI's pagetables are
 * not guaranteed to be mapped in the restricted address space.
 *
 * If the source mapping is a large page and the range being mapped spans the
 * entire large page, then it will be mapped as a large page in the ASI page
 * tables too. If the range does not span the entire huge page, then it will be
 * mapped as smaller pages. In that case, the implementation is slightly
 * inefficient, as it will walk the source page tables again for each small
 * destination page, but that should be ok for now, as usually in such cases,
 * the range would consist of a small-ish number of pages.
 *
 * Note that upstream
 * (https://lore.kernel.org/all/20210317155843.c15e71f966f1e4da508dea04@linux-foundation.org/)
 * vmap_p4d_range supports huge mappings. It is probably possible to use that
 * logic instead of custom mapping duplication logic in later versions of ASI.
 */
int __must_check asi_map_gfp(struct asi *asi, void *addr, unsigned long len, gfp_t gfp_flags)
{
	unsigned long virt;
	unsigned long start = (size_t)addr;
	unsigned long end = start + len;
	unsigned long page_size;

	if (!static_asi_enabled())
		return 0;

	/* ASI pagetables might be sensitive. */
	WARN_ON_ONCE(asi_in_critical_section());

	VM_BUG_ON(!IS_ALIGNED(start, PAGE_SIZE));
	VM_BUG_ON(!IS_ALIGNED(len, PAGE_SIZE));
	VM_BUG_ON(!fault_in_kernel_space(start)); /* Misnamed, ignore "fault_" */

	gfp_flags &= GFP_RECLAIM_MASK;

	if (asi->mm != &init_mm)
		gfp_flags |= __GFP_ACCOUNT;

	for (virt = start; virt < end; virt = ALIGN(virt + 1, page_size)) {
		pgd_t *pgd;
		p4d_t *p4d;
		pud_t *pud;
		pmd_t *pmd;
		pte_t *pte;
		phys_addr_t phys;
		ulong flags;

		if (!follow_physaddr(asi->mm->pgd, virt, &phys, &page_size, &flags))
			continue;

#define MAP_AT_LEVEL(base, BASE, level, LEVEL) {				\
			if (base##_leaf(*base)) {				\
				if (WARN_ON_ONCE(PHYS_PFN(phys & BASE##_MASK) !=\
						 base##_pfn(*base)))		\
					return -EBUSY;				\
				continue;					\
			}							\
										\
			level = asi_##level##_alloc(asi, base, virt, gfp_flags);\
			if (!level)						\
				return -ENOMEM;					\
										\
			if (page_size >= LEVEL##_SIZE &&			\
			    (level##_none(*level) || level##_leaf(*level)) &&	\
			    is_page_within_range(virt, LEVEL##_SIZE,		\
						 start, end)) {			\
				page_size = LEVEL##_SIZE;			\
				phys &= LEVEL##_MASK;				\
										\
				if (!level##_none(*level)) {			\
					if (WARN_ON_ONCE(level##_pfn(*level) != \
							 PHYS_PFN(phys))) {	\
						return -EBUSY;			\
					}					\
				} else {					\
					set_##level(level,			\
						    __##level(phys | flags));	\
				}						\
				continue;					\
			}							\
		}

		pgd = pgd_offset_pgd(asi->pgd, virt);

		MAP_AT_LEVEL(pgd, PGDIR, p4d, P4D);
		MAP_AT_LEVEL(p4d, P4D, pud, PUD);
		MAP_AT_LEVEL(pud, PUD, pmd, PMD);
		/*
		 * If a large page is going to be partially mapped
		 * in 4k pages, convert the PSE/PAT bits.
		 */
		if (page_size >= PMD_SIZE)
			flags = protval_large_2_4k(flags);
		MAP_AT_LEVEL(pmd, PMD, pte, PAGE);

		VM_BUG_ON(true); /* Should never reach here. */
	}

	return 0;
#undef MAP_AT_LEVEL
}

int __must_check asi_map(struct asi *asi, void *addr, unsigned long len)
{
	return asi_map_gfp(asi, addr, len, GFP_KERNEL);
}

/*
 * Unmap a kernel address range previously mapped into the ASI page tables.
 *
 * The area being unmapped must be a whole previously mapped region (or regions)
 * Unmapping a partial subset of a previously mapped region is not supported.
 * That will work, but may end up unmapping more than what was asked for, if
 * the mapping contained huge pages. A later patch will remove this limitation
 * by splitting the huge mapping in the ASI page table in such a case. For now,
 * vunmap_pgd_range() will just emit a warning if this situation is detected.
 *
 * This might sleep, and cannot be called with interrupts disabled.
 */
void asi_unmap(struct asi *asi, void *addr, size_t len)
{
	size_t start = (size_t)addr;
	size_t end = start + len;
	pgtbl_mod_mask mask = 0;

	if (!static_asi_enabled() || !len)
		return;

	/* ASI pagetables might be sensitive. */
	WARN_ON_ONCE(asi_in_critical_section());

	VM_BUG_ON(start & ~PAGE_MASK);
	VM_BUG_ON(len & ~PAGE_MASK);
	VM_BUG_ON(!fault_in_kernel_space(start)); /* Misnamed, ignore "fault_" */

	vunmap_pgd_range(asi->pgd, start, end, &mask);

	/* We don't support partial unmappings - b/270310049 */
	if (mask & PGTBL_P4D_MODIFIED) {
		VM_WARN_ON(!IS_ALIGNED((ulong)addr, P4D_SIZE));
		VM_WARN_ON(!IS_ALIGNED((ulong)len, P4D_SIZE));
	} else if (mask & PGTBL_PUD_MODIFIED) {
		VM_WARN_ON(!IS_ALIGNED((ulong)addr, PUD_SIZE));
		VM_WARN_ON(!IS_ALIGNED((ulong)len, PUD_SIZE));
	} else if (mask & PGTBL_PMD_MODIFIED) {
		VM_WARN_ON(!IS_ALIGNED((ulong)addr, PMD_SIZE));
		VM_WARN_ON(!IS_ALIGNED((ulong)len, PMD_SIZE));
	}

	asi_flush_tlb_range(asi, addr, len);
}
