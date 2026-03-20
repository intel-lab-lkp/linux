/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_MMU_CONTEXT_H
#define _ASM_X86_MMU_CONTEXT_H

#include <linux/atomic.h>
#include <linux/mm_types.h>
#include <linux/pkeys.h>

#include <trace/events/tlb.h>

#include <asm/tlb.h>
#include <asm/tlbflush.h>
#include <asm/paravirt.h>
#include <asm/pgalloc.h>
#include <asm/debugreg.h>
#include <asm/gsseg.h>
#include <asm/desc.h>

extern atomic64_t last_mm_ctx_id;

#ifdef CONFIG_PERF_EVENTS
DECLARE_STATIC_KEY_FALSE(rdpmc_never_available_key);
DECLARE_STATIC_KEY_FALSE(rdpmc_always_available_key);
void cr4_update_pce(void *ignored);
#endif

#ifdef CONFIG_MODIFY_LDT_SYSCALL
/*
 * ldt_structs can be allocated, used, and freed, but they are never
 * modified while live.
 */
struct ldt_struct {
	/*
	 * Xen requires page-aligned LDTs with special permissions.  This is
	 * needed to prevent us from installing evil descriptors such as
	 * call gates.  On native, we could merge the ldt_struct and LDT
	 * allocations, but it's not worth trying to optimize.
	 */
	struct desc_struct	*entries;
	unsigned int		nr_entries;

	/*
	 * If PTI is in use, then the entries array is not mapped while we're
	 * in user mode.  The whole array will be aliased at the addressed
	 * given by ldt_slot_va(slot).  We use two slots so that we can allocate
	 * and map, and enable a new LDT without invalidating the mapping
	 * of an older, still-in-use LDT.
	 *
	 * slot will be -1 if this LDT doesn't have an alias mapping.
	 */
	int			slot;
};

/*
 * Used for LDT copy/destruction.
 */
static inline void init_new_context_ldt(struct mm_struct *mm)
{
	mm->context.ldt = NULL;
	init_rwsem(&mm->context.ldt_usr_sem);
}
int ldt_dup_context(struct mm_struct *oldmm, struct mm_struct *mm);
void destroy_context_ldt(struct mm_struct *mm);
#else	/* CONFIG_MODIFY_LDT_SYSCALL */
static inline void init_new_context_ldt(struct mm_struct *mm) { }
static inline int ldt_dup_context(struct mm_struct *oldmm,
				  struct mm_struct *mm)
{
	return 0;
}
static inline void destroy_context_ldt(struct mm_struct *mm) { }
#endif

#ifdef CONFIG_MODIFY_LDT_SYSCALL
extern void load_mm_ldt(struct mm_struct *mm);
extern void switch_ldt(struct mm_struct *prev, struct mm_struct *next);
#else
static inline void load_mm_ldt(struct mm_struct *mm)
{
	clear_LDT();
}
static inline void switch_ldt(struct mm_struct *prev, struct mm_struct *next)
{
	DEBUG_LOCKS_WARN_ON(preemptible());
}
#endif

#ifdef CONFIG_ADDRESS_MASKING
static inline unsigned long mm_lam_cr3_mask(struct mm_struct *mm)
{
	/*
	 * When switch_mm_irqs_off() is called for a kthread, it may race with
	 * LAM enablement. switch_mm_irqs_off() uses the LAM mask to do two
	 * things: populate CR3 and populate 'cpu_tlbstate.lam'. Make sure it
	 * reads a single value for both.
	 */
	return READ_ONCE(mm->context.lam_cr3_mask);
}

static inline void dup_lam(struct mm_struct *oldmm, struct mm_struct *mm)
{
	mm->context.lam_cr3_mask = oldmm->context.lam_cr3_mask;
	mm->context.untag_mask = oldmm->context.untag_mask;
}

#define mm_untag_mask mm_untag_mask
static inline unsigned long mm_untag_mask(struct mm_struct *mm)
{
	return mm->context.untag_mask;
}

static inline void mm_reset_untag_mask(struct mm_struct *mm)
{
	mm->context.untag_mask = -1UL;
}

#define arch_pgtable_dma_compat arch_pgtable_dma_compat
static inline bool arch_pgtable_dma_compat(struct mm_struct *mm)
{
	return !mm_lam_cr3_mask(mm) ||
		test_bit(MM_CONTEXT_FORCE_TAGGED_SVA, &mm->context.flags);
}
#else

static inline unsigned long mm_lam_cr3_mask(struct mm_struct *mm)
{
	return 0;
}

static inline void dup_lam(struct mm_struct *oldmm, struct mm_struct *mm)
{
}

static inline void mm_reset_untag_mask(struct mm_struct *mm)
{
}
#endif

extern void mm_init_global_asid(struct mm_struct *mm);
extern void mm_free_global_asid(struct mm_struct *mm);

/*
 * Init a new mm.  Used on mm copies, like at fork()
 * and on mm's that are brand-new, like at execve().
 */
#define init_new_context init_new_context
static inline int init_new_context(struct task_struct *tsk,
				   struct mm_struct *mm)
{
	mutex_init(&mm->context.lock);

	mm->context.ctx_id = atomic64_inc_return(&last_mm_ctx_id);
	atomic64_set(&mm->context.tlb_gen, 0);
	mm->context.next_trim_cpumask = jiffies + HZ;

#ifdef CONFIG_X86_INTEL_MEMORY_PROTECTION_KEYS
	if (cpu_feature_enabled(X86_FEATURE_OSPKE)) {
		/* pkey 0 is the default and allocated implicitly */
		mm->context.pkey_allocation_map = 0x1;
		/* -1 means unallocated or invalid */
		mm->context.execute_only_pkey = -1;
	}
#endif

	mm_init_global_asid(mm);
	mm_reset_untag_mask(mm);
	init_new_context_ldt(mm);
	return 0;
}

#define destroy_context destroy_context
static inline void destroy_context(struct mm_struct *mm)
{
	destroy_context_ldt(mm);
	mm_free_global_asid(mm);
}

extern void switch_mm(struct mm_struct *prev, struct mm_struct *next,
		      struct task_struct *tsk);

extern void switch_mm_irqs_off(struct mm_struct *prev, struct mm_struct *next,
			       struct task_struct *tsk);
#define switch_mm_irqs_off switch_mm_irqs_off

#define activate_mm(prev, next)			\
do {						\
	paravirt_enter_mmap(next);		\
	switch_mm_irqs_off((prev), (next), NULL);	\
} while (0);

#ifdef CONFIG_X86_32
#define deactivate_mm(tsk, mm)			\
do {						\
	loadsegment(gs, 0);			\
} while (0)
#else
#define deactivate_mm(tsk, mm)			\
do {						\
	shstk_free(tsk);			\
	load_gs_index(0);			\
	loadsegment(fs, 0);			\
} while (0)
#endif

static inline void arch_dup_pkeys(struct mm_struct *oldmm,
				  struct mm_struct *mm)
{
#ifdef CONFIG_X86_INTEL_MEMORY_PROTECTION_KEYS
	if (!cpu_feature_enabled(X86_FEATURE_OSPKE))
		return;

	/* Duplicate the oldmm pkey state in mm: */
	mm->context.pkey_allocation_map = oldmm->context.pkey_allocation_map;
	mm->context.execute_only_pkey   = oldmm->context.execute_only_pkey;
#endif
}

static inline int arch_dup_mmap(struct mm_struct *oldmm, struct mm_struct *mm)
{
	arch_dup_pkeys(oldmm, mm);
	paravirt_enter_mmap(mm);
	dup_lam(oldmm, mm);
	return ldt_dup_context(oldmm, mm);
}

#ifdef CONFIG_MM_LOCAL_REGION
static inline void mm_local_region_free(struct mm_struct *mm)
{
	if (mm_local_region_used(mm)) {
		struct mmu_gather tlb;
		unsigned long start = MM_LOCAL_BASE_ADDR;
		unsigned long end = MM_LOCAL_END_ADDR;

		/*
		 * Although free_pgd_range() is intended for freeing user
		 * page-tables, it also works out for kernel mappings on x86.
		 * We use tlb_gather_mmu_fullmm() to avoid confusing the
		 * range-tracking logic in __tlb_adjust_range().
		 */
		tlb_gather_mmu_fullmm(&tlb, mm);
		free_pgd_range(&tlb, start, end, start, end);
		tlb_finish_mmu(&tlb);

		mm_flags_clear(MMF_LOCAL_REGION_USED, mm);
	}
}

#if defined(CONFIG_MITIGATION_PAGE_TABLE_ISOLATION) && defined(CONFIG_X86_PAE)
static inline pmd_t *pgd_to_pmd_walk(pgd_t *pgd, unsigned long va)
{
	p4d_t *p4d;
	pud_t *pud;

	if (pgd->pgd == 0)
		return NULL;

	p4d = p4d_offset(pgd, va);
	if (p4d_none(*p4d))
		return NULL;

	pud = pud_offset(p4d, va);
	if (pud_none(*pud))
		return NULL;

	return pmd_offset(pud, va);
}

static inline int mm_local_map_to_user(struct mm_struct *mm)
{
	BUILD_BUG_ON(!PREALLOCATED_PMDS);
	pgd_t *k_pgd = pgd_offset(mm, MM_LOCAL_BASE_ADDR);
	pgd_t *u_pgd = kernel_to_user_pgdp(k_pgd);
	pmd_t *k_pmd, *u_pmd;
	int err;

	k_pmd = pgd_to_pmd_walk(k_pgd, MM_LOCAL_BASE_ADDR);
	u_pmd = pgd_to_pmd_walk(u_pgd, MM_LOCAL_BASE_ADDR);

	BUILD_BUG_ON(MM_LOCAL_END_ADDR - MM_LOCAL_BASE_ADDR > PMD_SIZE);

	/* Preallocate the PTE table so it can be shared. */
	err = pte_alloc(mm, k_pmd);
	if (err)
		return err;

	/* Point the userspace PMD at the same PTE as the kernel PMD. */
	set_pmd(u_pmd, *k_pmd);
	return 0;
}
#elif defined(CONFIG_MITIGATION_PAGE_TABLE_ISOLATION)
static inline int mm_local_map_to_user(struct mm_struct *mm)
{
	pgd_t *pgd;
	int err;

	err = preallocate_sub_pgd(mm, MM_LOCAL_BASE_ADDR);
	if (err)
		return err;

	pgd = pgd_offset(mm, MM_LOCAL_BASE_ADDR);
	set_pgd(kernel_to_user_pgdp(pgd), *pgd);
	return 0;
}
#else
static inline int mm_local_map_to_user(struct mm_struct *mm)
{
	WARN_ONCE(1, "mm_local_map_to_user() not implemented");
	return -EINVAL;
}
#endif

/*
 * Do initial setup of the user-local region. Call from process context.
 *
 * Under PTI, userspace shares the pagetables for the mm-local region with the
 * kernel (if you map stuff here, it's immediately mapped into userspace too).
 * LDT remap. It's assuming nothing gets mapped in here that needs to be
 * protected from Meltdown-type attacks from the current process.
 */
static inline int mm_local_region_init(struct mm_struct *mm)
{
	int err;

	if (boot_cpu_has(X86_FEATURE_PTI)) {
		err = mm_local_map_to_user(mm);
		if (err)
			return err;
	}

	mm_flags_set(MMF_LOCAL_REGION_USED, mm);

	return 0;
}

#else
static inline void mm_local_region_free(struct mm_struct *mm) { }
#endif /* CONFIG_MM_LOCAL_REGION */

static inline void arch_exit_mmap(struct mm_struct *mm)
{
	paravirt_arch_exit_mmap(mm);
	mm_local_region_free(mm);
}

#ifdef CONFIG_X86_64
static inline bool is_64bit_mm(struct mm_struct *mm)
{
	return	!IS_ENABLED(CONFIG_IA32_EMULATION) ||
		!test_bit(MM_CONTEXT_UPROBE_IA32, &mm->context.flags);
}
#else
static inline bool is_64bit_mm(struct mm_struct *mm)
{
	return false;
}
#endif

static inline bool is_notrack_mm(struct mm_struct *mm)
{
	return test_bit(MM_CONTEXT_NOTRACK, &mm->context.flags);
}

static inline void set_notrack_mm(struct mm_struct *mm)
{
	set_bit(MM_CONTEXT_NOTRACK, &mm->context.flags);
}

/*
 * We only want to enforce protection keys on the current process
 * because we effectively have no access to PKRU for other
 * processes or any way to tell *which * PKRU in a threaded
 * process we could use.
 *
 * So do not enforce things if the VMA is not from the current
 * mm, or if we are in a kernel thread.
 */
static inline bool arch_vma_access_permitted(struct vm_area_struct *vma,
		bool write, bool execute, bool foreign)
{
	/* pkeys never affect instruction fetches */
	if (execute)
		return true;
	/* allow access if the VMA is not one from this process */
	if (foreign || vma_is_foreign(vma))
		return true;
	return __pkru_allows_pkey(vma_pkey(vma), write);
}

unsigned long __get_current_cr3_fast(void);

#include <asm-generic/mmu_context.h>

extern struct mm_struct *use_temporary_mm(struct mm_struct *temp_mm);
extern void unuse_temporary_mm(struct mm_struct *prev_mm);

#endif /* _ASM_X86_MMU_CONTEXT_H */
