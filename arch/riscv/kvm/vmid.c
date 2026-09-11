// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *     Anup Patel <anup.patel@wdc.com>
 */

#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/cpumask.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/kvm_host.h>
#include <asm/csr.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_tlb.h>
#include <asm/kvm_vmid.h>

static atomic64_t vmid_generation;
static unsigned long *vmid_map;
static unsigned long vmid_bits __ro_after_init;
static unsigned long vmid_cur_idx = 1;

static DEFINE_RAW_SPINLOCK(vmid_lock);

static DEFINE_PER_CPU(atomic64_t, active_vmids);
static DEFINE_PER_CPU(u64, reserved_vmids);

static cpumask_t tlb_flush_pending;

#define VMID_FIRST_VERSION	BIT_ULL(vmid_bits)
#define NUM_VMIDS		BIT(vmid_bits)
#define VMID_HW_MASK		(VMID_FIRST_VERSION - 1)

static bool vmid_gen_match(u64 vmid)
{
	return !((vmid ^ atomic64_read(&vmid_generation)) >> vmid_bits);
}

unsigned long kvm_riscv_gstage_vmid_hwid(u64 vmid)
{
	if (!vmid_bits)
		return 0;

	return (unsigned long)(vmid & VMID_HW_MASK);
}

void __init kvm_riscv_gstage_vmid_detect(void)
{
	/* Figure out the number of VMID bits supported by hardware. */
	csr_write(CSR_HGATP, (kvm_riscv_gstage_mode(kvm_riscv_gstage_max_pgd_levels) <<
		   HGATP_MODE_SHIFT) | HGATP_VMID);
	vmid_bits = csr_read(CSR_HGATP);
	vmid_bits = (vmid_bits & HGATP_VMID) >> HGATP_VMID_SHIFT;
	vmid_bits = fls_long(vmid_bits);
	csr_write(CSR_HGATP, 0);

	/* Flush the local guest TLB after probing HGATP. */
	kvm_riscv_local_hfence_gvma_all();

	/*
	 * VMID 0 is reserved.  During rollover every possible CPU may
	 * reserve one hardware VMID, and we still need at least one VMID
	 * available for a new allocation.
	 */
	if (vmid_bits && NUM_VMIDS - 1 <= num_possible_cpus())
		vmid_bits = 0;
}

unsigned long kvm_riscv_gstage_vmid_bits(void)
{
	return vmid_bits;
}

int __init kvm_riscv_gstage_vmid_alloc_init(void)
{
	int cpu;

	if (!vmid_bits)
		return 0;

	vmid_map = bitmap_zalloc(NUM_VMIDS, GFP_KERNEL);
	if (!vmid_map)
		return -ENOMEM;

	vmid_cur_idx = 1;

	/* Hardware VMID 0 is reserved. */
	__set_bit(0, vmid_map);

	atomic64_set(&vmid_generation, VMID_FIRST_VERSION);

	for_each_possible_cpu(cpu) {
		atomic64_set(&per_cpu(active_vmids, cpu), 0);
		per_cpu(reserved_vmids, cpu) = 0;
	}

	/*
	 * Every CPU performs an initial local invalidation before its
	 * first VMID activation.
	 */
	cpumask_copy(&tlb_flush_pending, cpu_possible_mask);

	return 0;
}

void kvm_riscv_gstage_vmid_alloc_free(void)
{
	bitmap_free(vmid_map);
	vmid_map = NULL;
}

int kvm_riscv_gstage_vmid_init(struct kvm *kvm)
{
	atomic64_set(&kvm->arch.vmid.id, 0);

	return 0;
}

bool kvm_riscv_gstage_vmid_ver_changed(struct kvm_vmid *vmid)
{
	u64 id;

	if (!vmid_bits)
		return false;

	id = atomic64_read(&vmid->id);

	return unlikely(!vmid_gen_match(id));
}

/*
 * Called with vmid_lock held after vmid_generation has already been
 * advanced.
 *
 * No remote CPU is interrupted here.  Instead, preserve every VMID
 * which may still be used by a CPU and queue a local invalidation for
 * that CPU's next VMID activation.
 */
static void flush_context(void)
{
	u64 vmid;
	int cpu;

	bitmap_zero(vmid_map, NUM_VMIDS);
	__set_bit(0, vmid_map);

	for_each_possible_cpu(cpu) {
		vmid = atomic64_xchg(&per_cpu(active_vmids, cpu), 0);

		/*
		 * The CPU may already have been caught by an earlier rollover
		 * without performing another activation since then.  In that
		 * case reserved_vmids is the only record of the old context.
		 */
		if (!vmid)
			vmid = per_cpu(reserved_vmids, cpu);

		if (vmid)
			__set_bit(kvm_riscv_gstage_vmid_hwid(vmid),
				  vmid_map);

		per_cpu(reserved_vmids, cpu) = vmid;
	}

	cpumask_copy(&tlb_flush_pending, cpu_possible_mask);
}

/*
 * Update every reserved copy of an old software VMID.
 *
 * Do not stop after the first match: the same VM may have been active
 * on more than one CPU when rollover occurred.
 */
static bool check_update_reserved_vmid(u64 old_vmid, u64 new_vmid)
{
	bool hit = false;
	int cpu;

	for_each_possible_cpu(cpu) {
		if (per_cpu(reserved_vmids, cpu) == old_vmid) {
			per_cpu(reserved_vmids, cpu) = new_vmid;
			hit = true;
		}
	}

	return hit;
}

/*
 * Allocate/promote a software VMID.
 *
 * vmid_lock must be held by the caller.
 */
static u64 new_vmid_locked(struct kvm_vmid *kvm_vmid)
{
	u64 vmid = atomic64_read(&kvm_vmid->id);
	u64 generation = atomic64_read(&vmid_generation);
	u64 new_vmid;
	unsigned long idx;

	if (vmid) {
		new_vmid = generation |
			   kvm_riscv_gstage_vmid_hwid(vmid);

		/*
		 * The old VMID is still protected by one or more CPUs.
		 * Keep the same hardware VMID and only promote generation.
		 */
		if (check_update_reserved_vmid(vmid, new_vmid))
			return new_vmid;

		/*
		 * The VM had a VMID in an older generation.  Reuse the same
		 * hardware number if it has not already been claimed.
		 */
		idx = kvm_riscv_gstage_vmid_hwid(vmid);
		if (!__test_and_set_bit(idx, vmid_map))
			return new_vmid;
	}

	/*
	 * Find a free VMID in the current generation.
	 */
	idx = find_next_zero_bit(vmid_map, NUM_VMIDS, vmid_cur_idx);
	if (idx != NUM_VMIDS)
		goto set_vmid;

	/*
	 * No free VMID.  Start a new generation, preserve all CPU-local
	 * users, and defer each CPU's flush until its next activation.
	 */
	generation += VMID_FIRST_VERSION;

	/* Software VMID 0 is reserved as the invalid identifier. */
	if (unlikely(!generation))
		generation = VMID_FIRST_VERSION;

	atomic64_xchg(&vmid_generation, generation);
	flush_context();

	/*
	 * NUM_VMIDS - 1 > num_possible_cpus(), therefore rollover must
	 * leave at least one allocatable hardware VMID.
	 */
	idx = find_next_zero_bit(vmid_map, NUM_VMIDS, 1);

set_vmid:
	__set_bit(idx, vmid_map);
	vmid_cur_idx = idx;

	return generation | idx;
}

void kvm_riscv_gstage_vmid_update(struct kvm_vcpu *vcpu)
{
	struct kvm_vmid *kvm_vmid = &vcpu->kvm->arch.vmid;
	atomic64_t *active;
	unsigned long flags;
	u64 vmid;
	u64 old_active_vmid;
	unsigned int cpu;

	if (!vmid_bits)
		return;

	/*
	 * active_vmids and tlb_flush_pending are per-CPU state.  Keep the
	 * complete VMID activation on the same CPU.
	 */
	lockdep_assert_preemption_disabled();

	cpu = smp_processor_id();
	active = this_cpu_ptr(&active_vmids);

	vmid = atomic64_read(&kvm_vmid->id);
	old_active_vmid = atomic64_read(active);

	/*
	 * Fast path.
	 *
	 * The cmpxchg races with flush_context()'s xchg on the same
	 * per-CPU atomic.  Either this activation is captured by rollover,
	 * or rollover clears active first and the cmpxchg fails.
	 */
	if (old_active_vmid && vmid_gen_match(vmid)) {
		/*
		 * HGATP must contain the same hardware VMID that is published
		 * in active_vmids.
		 */
		if (old_active_vmid != vmid)
			kvm_riscv_mmu_update_hgatp_vmid(vcpu, vmid);

		if (atomic64_cmpxchg(active, old_active_vmid, vmid) ==
		    old_active_vmid)
			return;
	}

	raw_spin_lock_irqsave(&vmid_lock, flags);

	/*
	 * Re-read under the allocator lock because another vCPU of this VM
	 * may already have promoted or allocated the shared VMID.
	 */
	vmid = atomic64_read(&kvm_vmid->id);

	if (!vmid_gen_match(vmid)) {
		vmid = new_vmid_locked(kvm_vmid);
		atomic64_set(&kvm_vmid->id, vmid);
	}

	/*
	 * Complete this CPU's deferred rollover invalidation before
	 * installing the new hardware VMID.
	 */
	if (cpumask_test_and_clear_cpu(cpu, &tlb_flush_pending))
		kvm_riscv_local_hfence_gvma_all();

	/*
	 * Install the current hardware VMID before publishing this CPU as
	 * active with the new software VMID.
	 */
	kvm_riscv_mmu_update_hgatp_vmid(vcpu, vmid);

	/*
	 * Once active_vmids is visible, the required local invalidation and
	 * HGATP update are complete.
	 */
	atomic64_set(active, vmid);

	raw_spin_unlock_irqrestore(&vmid_lock, flags);
}

/*
 * CPU virtualization/CSR state is being discarded.
 *
 * Withdraw fast-path eligibility, but conservatively preserve the last
 * identity instead of making its hardware VMID immediately reusable.
 */
void kvm_riscv_gstage_vmid_cpu_invalidate(void)
{
	unsigned long flags;
	u64 vmid;
	unsigned int cpu;

	if (!vmid_bits)
		return;

	lockdep_assert_preemption_disabled();

	cpu = smp_processor_id();

	raw_spin_lock_irqsave(&vmid_lock, flags);

	vmid = atomic64_xchg(this_cpu_ptr(&active_vmids), 0);

	if (vmid)
		per_cpu(reserved_vmids, cpu) = vmid;

	/*
	 * If active was already zero, retain the old reserved entry.
	 */
	cpumask_set_cpu(cpu, &tlb_flush_pending);

	raw_spin_unlock_irqrestore(&vmid_lock, flags);
}
