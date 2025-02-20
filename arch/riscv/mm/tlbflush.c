// SPDX-License-Identifier: GPL-2.0

#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/hugetlb.h>
#include <asm/sbi.h>
#include <asm/mmu_context.h>

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

static void __ipi_flush_tlb_range_asid(void *info)
{
	struct flush_tlb_range_data *d = info;

	local_flush_tlb_range_asid(d->start, d->size, d->stride, d->asid);
}

static void __flush_tlb_range(const struct cpumask *cmask, unsigned long asid,
			      unsigned long start, unsigned long size,
			      unsigned long stride)
{
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
}

static inline unsigned long get_mm_asid(struct mm_struct *mm)
{
	return cntx2asid(atomic_long_read(&mm->context.id));
}

void flush_tlb_mm(struct mm_struct *mm)
{
	__flush_tlb_range(mm_cpumask(mm), get_mm_asid(mm),
			  0, FLUSH_TLB_MAX_SIZE, PAGE_SIZE);
}

void flush_tlb_mm_range(struct mm_struct *mm,
			unsigned long start, unsigned long end,
			unsigned int page_size)
{
	__flush_tlb_range(mm_cpumask(mm), get_mm_asid(mm),
			  start, end - start, page_size);
}

void flush_tlb_page(struct vm_area_struct *vma, unsigned long addr)
{
	__flush_tlb_range(mm_cpumask(vma->vm_mm), get_mm_asid(vma->vm_mm),
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

	__flush_tlb_range(mm_cpumask(vma->vm_mm), get_mm_asid(vma->vm_mm),
			  start, end - start, stride_size);
}

void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	__flush_tlb_range(cpu_online_mask, FLUSH_TLB_NO_ASID,
			  start, end - start, PAGE_SIZE);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
void flush_pmd_tlb_range(struct vm_area_struct *vma, unsigned long start,
			unsigned long end)
{
	__flush_tlb_range(mm_cpumask(vma->vm_mm), get_mm_asid(vma->vm_mm),
			  start, end - start, PMD_SIZE);
}
#endif

bool arch_tlbbatch_should_defer(struct mm_struct *mm)
{
	return true;
}

void arch_tlbbatch_add_pending(struct arch_tlbflush_unmap_batch *batch,
			       struct mm_struct *mm,
			       unsigned long uaddr)
{
	cpumask_or(&batch->cpumask, &batch->cpumask, mm_cpumask(mm));
}

void arch_flush_tlb_batched_pending(struct mm_struct *mm)
{
	flush_tlb_mm(mm);
}

void arch_tlbbatch_flush(struct arch_tlbflush_unmap_batch *batch)
{
	__flush_tlb_range(&batch->cpumask, FLUSH_TLB_NO_ASID, 0,
			  FLUSH_TLB_MAX_SIZE, PAGE_SIZE);
}

static DEFINE_PER_CPU(atomic_long_t, ugen_done);

static int __init luf_init_arch(void)
{
	int cpu;

	for_each_cpu(cpu, cpu_possible_mask)
		atomic_long_set(per_cpu_ptr(&ugen_done, cpu), LUF_UGEN_INIT - 1);

	return 0;
}
early_initcall(luf_init_arch);

/*
 * batch will not be updated.
 */
bool arch_tlbbatch_check_done(struct arch_tlbflush_unmap_batch *batch,
			unsigned long ugen)
{
	int cpu;

	if (!ugen)
		goto out;

	for_each_cpu(cpu, &batch->cpumask) {
		unsigned long done;

		done = atomic_long_read(per_cpu_ptr(&ugen_done, cpu));
		if (ugen_before(done, ugen))
			return false;
	}
	return true;
out:
	return cpumask_empty(&batch->cpumask);
}

bool arch_tlbbatch_diet(struct arch_tlbflush_unmap_batch *batch,
			unsigned long ugen)
{
	int cpu;

	if (!ugen)
		goto out;

	for_each_cpu(cpu, &batch->cpumask) {
		unsigned long done;

		done = atomic_long_read(per_cpu_ptr(&ugen_done, cpu));
		if (!ugen_before(done, ugen))
			cpumask_clear_cpu(cpu, &batch->cpumask);
	}
out:
	return cpumask_empty(&batch->cpumask);
}

void arch_tlbbatch_mark_ugen(struct arch_tlbflush_unmap_batch *batch,
			     unsigned long ugen)
{
	int cpu;

	if (!ugen)
		return;

	for_each_cpu(cpu, &batch->cpumask) {
		atomic_long_t *done = per_cpu_ptr(&ugen_done, cpu);
		unsigned long old = atomic_long_read(done);

		/*
		 * It's racy.  The race results in unnecessary tlb flush
		 * because of the smaller ugen_done than it should be.
		 * However, it's okay in terms of correctness.
		 */
		if (!ugen_before(old, ugen))
			continue;

		/*
		 * It's for optimization.  Just skip on fail than retry.
		 */
		atomic_long_cmpxchg(done, old, ugen);
	}
}

void arch_mm_mark_ugen(struct mm_struct *mm, unsigned long ugen)
{
	int cpu;

	if (!ugen)
		return;

	for_each_cpu(cpu, mm_cpumask(mm)) {
		atomic_long_t *done = per_cpu_ptr(&ugen_done, cpu);
		unsigned long old = atomic_long_read(done);

		/*
		 * It's racy.  The race results in unnecessary tlb flush
		 * because of the smaller ugen_done than it should be.
		 * However, it's okay in terms of correctness.
		 */
		if (!ugen_before(old, ugen))
			continue;

		/*
		 * It's for optimization.  Just skip on fail than retry.
		 */
		atomic_long_cmpxchg(done, old, ugen);
	}
}
