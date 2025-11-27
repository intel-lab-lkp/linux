// SPDX-License-Identifier: GPL-2.0

#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/hugetlb.h>
#include <linux/mmu_notifier.h>
#include <asm/sbi.h>
#include <asm/mmu_context.h>
#include <asm/cpufeature.h>

#define has_svinval()	riscv_has_extension_unlikely(RISCV_ISA_EXT_SVINVAL)

static inline void local_sfence_inval_ir(void)
{
	asm volatile(SFENCE_INVAL_IR() ::: "memory");
}

static inline void local_sfence_w_inval(void)
{
	asm volatile(SFENCE_W_INVAL() ::: "memory");
}

static inline void local_sinval_vma(unsigned long vma, unsigned long asid)
{
	if (asid != FLUSH_TLB_NO_ASID)
		asm volatile(SINVAL_VMA(%0, %1) : : "r" (vma), "r" (asid) : "memory");
	else
		asm volatile(SINVAL_VMA(%0, zero) : : "r" (vma) : "memory");
}

/*
 * Flush entire TLB if number of entries to be flushed is greater
 * than the threshold below.
 */
unsigned long tlb_flush_all_threshold __read_mostly = 64;

static void local_flush_tlb_range_threshold_asid(unsigned long start,
						 unsigned long size,
						 unsigned long stride,
						 unsigned long asid)
{
	unsigned long nr_ptes_in_range = DIV_ROUND_UP(size, stride);
	int i;

	if (nr_ptes_in_range > tlb_flush_all_threshold) {
		local_flush_tlb_all_asid(asid);
		return;
	}

	if (has_svinval()) {
		local_sfence_w_inval();
		for (i = 0; i < nr_ptes_in_range; ++i) {
			local_sinval_vma(start, asid);
			start += stride;
		}
		local_sfence_inval_ir();
		return;
	}

	for (i = 0; i < nr_ptes_in_range; ++i) {
		local_flush_tlb_page_asid(start, asid);
		start += stride;
	}
}

static inline void local_flush_tlb_range_asid(unsigned long start,
		unsigned long size, unsigned long stride, unsigned long asid)
{
	if (size <= stride)
		local_flush_tlb_page_asid(start, asid);
	else if (size == FLUSH_TLB_MAX_SIZE)
		local_flush_tlb_all_asid(asid);
	else
		local_flush_tlb_range_threshold_asid(start, size, stride, asid);
}

/* Flush a range of kernel pages without broadcasting */
void local_flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	local_flush_tlb_range_asid(start, end - start, PAGE_SIZE, FLUSH_TLB_NO_ASID);
}

static void __ipi_flush_tlb_all(void *info)
{
	local_flush_tlb_all();
}

void flush_tlb_all(void)
{
	if (num_online_cpus() < 2)
		local_flush_tlb_all();
	else if (riscv_use_sbi_for_rfence())
		sbi_remote_sfence_vma_asid(NULL, 0, FLUSH_TLB_MAX_SIZE, FLUSH_TLB_NO_ASID);
	else
		on_each_cpu(__ipi_flush_tlb_all, NULL, 1);
}

struct flush_tlb_range_data {
	unsigned long asid;
	unsigned long start;
	unsigned long size;
	unsigned long stride;
};

#ifdef CONFIG_RISCV_LAZY_TLB_FLUSH

DEFINE_PER_CPU_SHARED_ALIGNED(struct tlb_info, tlbinfo) = {
	.rwlock = __RW_LOCK_UNLOCKED(tlbinfo.rwlock),
	.active_mm = NULL,
	.next_gen = 1,
	.contexts = { { NULL, 0, }, },
};

static DEFINE_PER_CPU(mm_context_t *, mmdrop_victims);

static void mmdrop_lazy_mms(struct tasklet_struct *tasklet)
{
	mm_context_t *victim = xchg_relaxed(this_cpu_ptr(&mmdrop_victims), NULL);
	struct mm_struct *mm = NULL;

	while (victim) {
		mm = container_of(victim, struct mm_struct, context);
		while (atomic_dec_return_relaxed(&victim->lazy_tlb_cnt) != 0)
			mmdrop_lazy_tlb(mm);
		victim = victim->next;
	}
}

static DEFINE_PER_CPU(struct tasklet_struct, mmdrop_tasklets) = {
	.count = ATOMIC_INIT(0),
	.callback = mmdrop_lazy_mms,
	.use_callback = true,
};

static inline void mmgrab_lazy_mm(struct mm_struct *mm)
{
	mmgrab_lazy_tlb(mm);
	atomic_inc(&mm->context.lazy_tlb_cnt);
}

static inline void mmdrop_lazy_mm(struct mm_struct *mm)
{
	mm_context_t **head, *list, *context = &mm->context;

	if (atomic_inc_return_relaxed(&context->lazy_tlb_cnt) == 1) {
		head = this_cpu_ptr(&mmdrop_victims);

		do {
			list = *head;
			context->next = list;
		} while (cmpxchg_relaxed(head, list, context) != list);

		tasklet_schedule(this_cpu_ptr(&mmdrop_tasklets));
	}
}

#endif /* CONFIG_RISCV_LAZY_TLB_FLUSH */

static void __ipi_flush_tlb_range_asid(void *info)
{
	struct flush_tlb_range_data *d = info;

	local_flush_tlb_range_asid(d->start, d->size, d->stride, d->asid);
}

static void __flush_tlb_range(struct mm_struct *mm,
			      const struct cpumask *cmask,
			      unsigned long start, unsigned long size,
			      unsigned long stride)
{
	unsigned long asid = get_mm_asid(mm);
	unsigned int cpu;

	if (cpumask_empty(cmask))
		return;

	cpu = get_cpu();

	/* Check if the TLB flush needs to be sent to other CPUs. */
	if (cpumask_any_but(cmask, cpu) >= nr_cpu_ids) {
		local_flush_tlb_range_asid(start, size, stride, asid);
	} else if (riscv_use_sbi_for_rfence()) {
		sbi_remote_sfence_vma_asid(cmask, start, size, asid);
	} else {
		struct flush_tlb_range_data ftd;

		ftd.asid = asid;
		ftd.start = start;
		ftd.size = size;
		ftd.stride = stride;
		on_each_cpu_mask(cmask, __ipi_flush_tlb_range_asid, &ftd, 1);
	}

	put_cpu();

	if (mm)
		mmu_notifier_arch_invalidate_secondary_tlbs(mm, start, start + size);
}

void flush_tlb_mm(struct mm_struct *mm)
{
	__flush_tlb_range(mm, mm_cpumask(mm), 0, FLUSH_TLB_MAX_SIZE, PAGE_SIZE);
}

void flush_tlb_mm_range(struct mm_struct *mm,
			unsigned long start, unsigned long end,
			unsigned int page_size)
{
	__flush_tlb_range(mm, mm_cpumask(mm), start, end - start, page_size);
}

void flush_tlb_page(struct vm_area_struct *vma, unsigned long addr)
{
	__flush_tlb_range(vma->vm_mm, mm_cpumask(vma->vm_mm),
			  addr, PAGE_SIZE, PAGE_SIZE);
}

void flush_tlb_range(struct vm_area_struct *vma, unsigned long start,
		     unsigned long end)
{
	unsigned long stride_size;

	if (!is_vm_hugetlb_page(vma)) {
		stride_size = PAGE_SIZE;
	} else {
		stride_size = huge_page_size(hstate_vma(vma));

		/*
		 * As stated in the privileged specification, every PTE in a
		 * NAPOT region must be invalidated, so reset the stride in that
		 * case.
		 */
		if (has_svnapot()) {
			if (stride_size >= PGDIR_SIZE)
				stride_size = PGDIR_SIZE;
			else if (stride_size >= P4D_SIZE)
				stride_size = P4D_SIZE;
			else if (stride_size >= PUD_SIZE)
				stride_size = PUD_SIZE;
			else if (stride_size >= PMD_SIZE)
				stride_size = PMD_SIZE;
			else
				stride_size = PAGE_SIZE;
		}
	}

	__flush_tlb_range(vma->vm_mm, mm_cpumask(vma->vm_mm),
			  start, end - start, stride_size);
}

void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	__flush_tlb_range(NULL, cpu_online_mask,
			  start, end - start, PAGE_SIZE);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
void flush_pmd_tlb_range(struct vm_area_struct *vma, unsigned long start,
			unsigned long end)
{
	__flush_tlb_range(vma->vm_mm, mm_cpumask(vma->vm_mm),
			  start, end - start, PMD_SIZE);
}

void flush_pud_tlb_range(struct vm_area_struct *vma, unsigned long start,
			 unsigned long end)
{
	__flush_tlb_range(vma->vm_mm, mm_cpumask(vma->vm_mm),
			  start, end - start, PUD_SIZE);
}
#endif

bool arch_tlbbatch_should_defer(struct mm_struct *mm)
{
	return true;
}

void arch_tlbbatch_add_pending(struct arch_tlbflush_unmap_batch *batch,
		struct mm_struct *mm, unsigned long start, unsigned long end)
{
	cpumask_or(&batch->cpumask, &batch->cpumask, mm_cpumask(mm));
	mmu_notifier_arch_invalidate_secondary_tlbs(mm, start, end);
}

void arch_tlbbatch_flush(struct arch_tlbflush_unmap_batch *batch)
{
	__flush_tlb_range(NULL, &batch->cpumask,
			  0, FLUSH_TLB_MAX_SIZE, PAGE_SIZE);
	cpumask_clear(&batch->cpumask);
}

#ifdef CONFIG_RISCV_LAZY_TLB_FLUSH

static inline unsigned int new_tlb_gen(struct tlb_info *info)
{
	unsigned int gen = info->next_gen++;
	unsigned int i;

	if (unlikely(!info->next_gen)) {
		for (i = 0; i < MAX_LOADED_MM; i++) {
			if (info->contexts[i].gen)
				info->contexts[i].gen = 1;
		}
		info->next_gen = 1;
		gen = info->next_gen++;
	}

	return gen;
}

void local_load_tlb_mm(struct mm_struct *mm)
{
	struct tlb_info *info = this_cpu_ptr(&tlbinfo);
	struct tlb_context *contexts = info->contexts;
	struct mm_struct *victim = NULL;
	unsigned int i, pos = 0, min = UINT_MAX;

	for (i = 0; i < MAX_LOADED_MM; i++) {
		if (contexts[i].mm == mm) {
			pos = i;
			break;
		}
		if (min > contexts[i].gen) {
			min = contexts[i].gen;
			pos = i;
		}
	}

	write_lock(&info->rwlock);

	info->active_mm = mm;

	if (contexts[pos].mm != mm) {
		mmgrab_lazy_mm(mm);
		victim = contexts[pos].mm;
		contexts[pos].mm = mm;
	}
	contexts[pos].gen = new_tlb_gen(info);

	write_unlock(&info->rwlock);

	if (victim) {
		cpumask_clear_cpu(raw_smp_processor_id(), mm_cpumask(victim));
		local_flush_tlb_all_asid(get_mm_asid(victim));
		mmdrop_lazy_mm(victim);
	}
}

void local_flush_tlb_mm(struct mm_struct *mm)
{
	struct tlb_info *info = this_cpu_ptr(&tlbinfo);
	struct tlb_context *contexts = info->contexts;
	unsigned long asid = get_mm_asid(mm);
	unsigned int i;

	if (!mm || mm == info->active_mm) {
		local_flush_tlb_all_asid(asid);
		return;
	}

	for (i = 0; i < MAX_LOADED_MM; i++) {
		if (contexts[i].mm != mm)
			continue;

		write_lock(&info->rwlock);
		contexts[i].mm = NULL;
		contexts[i].gen = 0;
		write_unlock(&info->rwlock);

		cpumask_clear_cpu(raw_smp_processor_id(), mm_cpumask(mm));
		mmdrop_lazy_mm(mm);
		break;
	}

	local_flush_tlb_all_asid(asid);
}

#endif /* CONFIG_RISCV_LAZY_TLB_FLUSH */
