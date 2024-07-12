// SPDX-License-Identifier: GPL-2.0
#include <linux/compiler_types.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>

#include <linux/init.h>
#include <asm/asi.h>
#include <asm/cmdline.h>
#include <asm/pgalloc.h>
#include <asm/mmu_context.h>

static struct asi_class asi_class[ASI_MAX_NUM];
static DEFINE_SPINLOCK(asi_class_lock);

DEFINE_PER_CPU_ALIGNED(struct asi *, curr_asi);
EXPORT_SYMBOL(curr_asi);

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
 *
 * On the other hand, they do not use the normal page allocation infrastructure,
 * that means that PTE pages do not have the PageTable type nor the PagePgtable
 * flag and we don't increment the meminfo stat (NR_PAGETABLE) as they do.
 */
static_assert(!IS_ENABLED(CONFIG_PARAVIRT));
#define DEFINE_ASI_PGTBL_ALLOC(base, level)				\
__maybe_unused								\
static level##_t * asi_##level##_alloc(struct asi *asi,			\
				       base##_t *base, ulong addr,	\
				       gfp_t flags)			\
{									\
	if (unlikely(base##_none(*base))) {				\
		ulong pgtbl = get_zeroed_page(flags);			\
		phys_addr_t pgtbl_pa;					\
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
