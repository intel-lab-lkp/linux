// SPDX-License-Identifier: GPL-2.0
#include <linux/compiler_types.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>

#include <asm/asi.h>
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
	BUG_ON(!asi_index_valid(index));

	spin_lock(&asi_class_lock);

	WARN_ON(asi_class[index].name == NULL);
	memset(&asi_class[index], 0, sizeof(struct asi_class));

	spin_unlock(&asi_class_lock);
}
EXPORT_SYMBOL_GPL(asi_unregister_class);


static void __asi_destroy(struct asi *asi)
{
	lockdep_assert_held(&asi->mm->asi_init_lock);

}

int asi_init(struct mm_struct *mm, int asi_index, struct asi **out_asi)
{
	struct asi *asi;
	int err = 0;

	*out_asi = NULL;

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

	if (!asi)
		return;

	mm = asi->mm;
	/*
	 * We would need this mutex even if the refcount was atomic as we need
	 * to block concurrent asi_init calls.
	 */
	mutex_lock(&mm->asi_init_lock);
	WARN_ON_ONCE(asi->ref_count <= 0);
	if (--(asi->ref_count) == 0) {
		free_pages((ulong)asi->pgd, PGD_ALLOCATION_ORDER);
		memset(asi, 0, sizeof(struct asi));
	}
	mutex_unlock(&mm->asi_init_lock);
}
EXPORT_SYMBOL_GPL(asi_destroy);

static noinstr void __asi_enter(void)
{
	u64 asi_cr3;
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

	if (!target || target == this_cpu_read(curr_asi))
		return;

	VM_BUG_ON(this_cpu_read(cpu_tlbstate.loaded_mm) ==
		  LOADED_MM_SWITCHING);

	/*
	 * Must update curr_asi before writing CR3 to ensure an interrupting
	 * asi_exit sees that it may need to switch address spaces.
	 */
	this_cpu_write(curr_asi, target);

	asi_cr3 = build_cr3(target->pgd,
			    this_cpu_read(cpu_tlbstate.loaded_mm_asid),
			    tlbstate_lam_cr3_mask());
	write_cr3(asi_cr3);

	if (target->class->ops.post_asi_enter)
		target->class->ops.post_asi_enter();
}

noinstr void asi_enter(struct asi *asi)
{
	VM_WARN_ON_ONCE(!asi);

	asi_set_target(current, asi);
	barrier();

	__asi_enter();
}
EXPORT_SYMBOL_GPL(asi_enter);

inline_or_noinstr void asi_relax(void)
{
	barrier();
	asi_set_target(current, NULL);
}
EXPORT_SYMBOL_GPL(asi_relax);

noinstr void asi_exit(void)
{
	u64 unrestricted_cr3;
	struct asi *asi;

	preempt_disable_notrace();

	VM_BUG_ON(this_cpu_read(cpu_tlbstate.loaded_mm) ==
		  LOADED_MM_SWITCHING);

	asi = this_cpu_read(curr_asi);
	if (asi) {
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
	memset(mm->asi, 0, sizeof(mm->asi));
	mutex_init(&mm->asi_init_lock);
}
