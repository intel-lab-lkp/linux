// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 Arm Limited
 * Author: Andrew Murray <Andrew.Murray@arm.com>
 */
#include <linux/kvm_host.h>
#include <linux/perf_event.h>
#include <linux/perf/arm_pmu.h>
#include <linux/perf/arm_pmuv3.h>

#include <kvm/arm_pmu.h>
#include <kvm/arm_vgic.h>

#include <asm/kvm_emulate.h>

static LIST_HEAD(arm_pmus);
static DEFINE_MUTEX(arm_pmus_lock);
static DEFINE_PER_CPU(struct kvm_pmu_events, kvm_pmu_events);

#define kvm_arm_pmu_irq_initialized(v)	((v)->arch.pmu.irq_num >= VGIC_NR_SGIS)

bool kvm_supports_guest_pmuv3(void)
{
	guard(mutex)(&arm_pmus_lock);
	return !list_empty(&arm_pmus);
}

/*
 * Given the perf event attributes and system type, determine
 * if we are going to need to switch counters at guest entry/exit.
 */
static bool kvm_pmu_switch_needed(struct perf_event_attr *attr)
{
	/**
	 * With VHE the guest kernel runs at EL1 and the host at EL2,
	 * where user (EL0) is excluded then we have no reason to switch
	 * counters.
	 */
	if (has_vhe() && attr->exclude_user)
		return false;

	/* Only switch if attributes are different */
	return (attr->exclude_host != attr->exclude_guest);
}

struct kvm_pmu_events *kvm_get_pmu_events(void)
{
	return this_cpu_ptr(&kvm_pmu_events);
}

/*
 * Add events to track that we may want to switch at guest entry/exit
 * time.
 */
void kvm_set_pmu_events(u64 set, struct perf_event_attr *attr)
{
	struct kvm_pmu_events *pmu = kvm_get_pmu_events();

	if (!system_supports_pmuv3() || !kvm_pmu_switch_needed(attr))
		return;

	if (!attr->exclude_host)
		pmu->events_host |= set;
	if (!attr->exclude_guest)
		pmu->events_guest |= set;
}

/*
 * Stop tracking events
 */
void kvm_clr_pmu_events(u64 clr)
{
	struct kvm_pmu_events *pmu = kvm_get_pmu_events();

	if (!system_supports_pmuv3())
		return;

	pmu->events_host &= ~clr;
	pmu->events_guest &= ~clr;
}

/*
 * Read a value direct from PMEVTYPER<idx> where idx is 0-30
 * or PMxCFILTR_EL0 where idx is 31-32.
 */
static u64 kvm_vcpu_pmu_read_evtype_direct(int idx)
{
	if (idx == ARMV8_PMU_CYCLE_IDX)
		return read_pmccfiltr();
	else if (idx == ARMV8_PMU_INSTR_IDX)
		return read_pmicfiltr();

	return read_pmevtypern(idx);
}

/*
 * Write a value direct to PMEVTYPER<idx> where idx is 0-30
 * or PMxCFILTR_EL0 where idx is 31-32.
 */
static void kvm_vcpu_pmu_write_evtype_direct(int idx, u32 val)
{
	if (idx == ARMV8_PMU_CYCLE_IDX)
		write_pmccfiltr(val);
	else if (idx == ARMV8_PMU_INSTR_IDX)
		write_pmicfiltr(val);
	else
		write_pmevtypern(idx, val);
}

/*
 * Modify ARMv8 PMU events to include EL0 counting
 */
static void kvm_vcpu_pmu_enable_el0(unsigned long events)
{
	u64 typer;
	u32 counter;

	for_each_set_bit(counter, &events, ARMPMU_MAX_HWEVENTS) {
		typer = kvm_vcpu_pmu_read_evtype_direct(counter);
		typer &= ~ARMV8_PMU_EXCLUDE_EL0;
		kvm_vcpu_pmu_write_evtype_direct(counter, typer);
	}
}

/*
 * Modify ARMv8 PMU events to exclude EL0 counting
 */
static void kvm_vcpu_pmu_disable_el0(unsigned long events)
{
	u64 typer;
	u32 counter;

	for_each_set_bit(counter, &events, ARMPMU_MAX_HWEVENTS) {
		typer = kvm_vcpu_pmu_read_evtype_direct(counter);
		typer |= ARMV8_PMU_EXCLUDE_EL0;
		kvm_vcpu_pmu_write_evtype_direct(counter, typer);
	}
}

/*
 * On VHE ensure that only guest events have EL0 counting enabled.
 * This is called from both vcpu_{load,put} and the sysreg handling.
 * Since the latter is preemptible, special care must be taken to
 * disable preemption.
 */
void kvm_vcpu_pmu_restore_guest(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu_events *pmu;
	u64 events_guest, events_host;

	if (!system_supports_pmuv3() || !has_vhe())
		return;

	preempt_disable();
	pmu = kvm_get_pmu_events();
	events_guest = pmu->events_guest;
	events_host = pmu->events_host;

	kvm_vcpu_pmu_enable_el0(events_guest);
	kvm_vcpu_pmu_disable_el0(events_host);
	preempt_enable();
}

/*
 * On VHE ensure that only host events have EL0 counting enabled
 */
void kvm_vcpu_pmu_restore_host(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu_events *pmu;
	u64 events_guest, events_host;

	if (!system_supports_pmuv3() || !has_vhe())
		return;

	pmu = kvm_get_pmu_events();
	events_guest = pmu->events_guest;
	events_host = pmu->events_host;

	kvm_vcpu_pmu_enable_el0(events_host);
	kvm_vcpu_pmu_disable_el0(events_guest);
}

/*
 * With VHE, keep track of the PMUSERENR_EL0 value for the host EL0 on the pCPU
 * where PMUSERENR_EL0 for the guest is loaded, since PMUSERENR_EL0 is switched
 * to the value for the guest on vcpu_load().  The value for the host EL0
 * will be restored on vcpu_put(), before returning to userspace.
 * This isn't necessary for nVHE, as the register is context switched for
 * every guest enter/exit.
 *
 * Return true if KVM takes care of the register. Otherwise return false.
 */
bool kvm_set_pmuserenr(u64 val)
{
	struct kvm_cpu_context *hctxt;
	struct kvm_vcpu *vcpu;

	if (!system_supports_pmuv3() || !has_vhe())
		return false;

	vcpu = kvm_get_running_vcpu();
	if (!vcpu || !vcpu_get_flag(vcpu, PMUSERENR_ON_CPU))
		return false;

	hctxt = host_data_ptr(host_ctxt);
	ctxt_sys_reg(hctxt, PMUSERENR_EL0) = val;
	return true;
}

/*
 * If we interrupted the guest to update the host PMU context, make
 * sure we re-apply the guest EL0 state.
 */
void kvm_vcpu_pmu_resync_el0(void)
{
	struct kvm_vcpu *vcpu;

	if (!has_vhe() || !in_interrupt())
		return;

	vcpu = kvm_get_running_vcpu();
	if (!vcpu)
		return;

	kvm_make_request(KVM_REQ_RESYNC_PMU_EL0, vcpu);
}

void kvm_host_pmu_init(struct arm_pmu *pmu)
{
	struct arm_pmu_entry *entry;

	/*
	 * Check the sanitised PMU version for the system, as KVM does not
	 * support implementations where PMUv3 exists on a subset of CPUs.
	 */
	if (!pmuv3_implemented(kvm_arm_pmu_get_pmuver_limit()))
		return;

	guard(mutex)(&arm_pmus_lock);

	entry = kmalloc_obj(*entry);
	if (!entry)
		return;

	entry->arm_pmu = pmu;
	list_add_tail(&entry->entry, &arm_pmus);
}

static struct arm_pmu *kvm_pmu_probe_armpmu(void)
{
	struct arm_pmu_entry *entry;
	struct arm_pmu *pmu;
	int cpu;

	guard(mutex)(&arm_pmus_lock);

	/*
	 * It is safe to use a stale cpu to iterate the list of PMUs so long as
	 * the same value is used for the entirety of the loop. Given this, and
	 * the fact that no percpu data is used for the lookup there is no need
	 * to disable preemption.
	 *
	 * It is still necessary to get a valid cpu, though, to probe for the
	 * default PMU instance as userspace is not required to specify a PMU
	 * type. In order to uphold the preexisting behavior KVM selects the
	 * PMU instance for the core during vcpu init. A dependent use
	 * case would be a user with disdain of all things big.LITTLE that
	 * affines the VMM to a particular cluster of cores.
	 *
	 * In any case, userspace should just do the sane thing and use the UAPI
	 * to select a PMU type directly. But, be wary of the baggage being
	 * carried here.
	 */
	cpu = raw_smp_processor_id();
	list_for_each_entry(entry, &arm_pmus, entry) {
		pmu = entry->arm_pmu;

		if (cpumask_test_cpu(cpu, &pmu->supported_cpus))
			return pmu;
	}

	return NULL;
}

static u64 __compute_pmceid(struct arm_pmu *pmu, bool pmceid1)
{
	u32 hi[2], lo[2];

	bitmap_to_arr32(lo, pmu->pmceid_bitmap, ARMV8_PMUV3_MAX_COMMON_EVENTS);
	bitmap_to_arr32(hi, pmu->pmceid_ext_bitmap, ARMV8_PMUV3_MAX_COMMON_EVENTS);

	return ((u64)hi[pmceid1] << 32) | lo[pmceid1];
}

static u64 compute_pmceid0(struct arm_pmu *pmu)
{
	u64 val = __compute_pmceid(pmu, 0);

	/* always support SW_INCR */
	val |= BIT(ARMV8_PMUV3_PERFCTR_SW_INCR);
	/* always support CHAIN */
	val |= BIT(ARMV8_PMUV3_PERFCTR_CHAIN);
	return val;
}

static u64 compute_pmceid1(struct arm_pmu *pmu)
{
	u64 val = __compute_pmceid(pmu, 1);

	/*
	 * Don't advertise STALL_SLOT*, as PMMIR_EL0 is handled
	 * as RAZ
	 */
	val &= ~(BIT_ULL(ARMV8_PMUV3_PERFCTR_STALL_SLOT - 32) |
		 BIT_ULL(ARMV8_PMUV3_PERFCTR_STALL_SLOT_FRONTEND - 32) |
		 BIT_ULL(ARMV8_PMUV3_PERFCTR_STALL_SLOT_BACKEND - 32));
	return val;
}

u64 kvm_pmu_get_pmceid(struct kvm_vcpu *vcpu, bool pmceid1)
{
	struct arm_pmu *cpu_pmu = vcpu->kvm->arch.arm_pmu;
	unsigned long *bmap = vcpu->kvm->arch.pmu_filter;
	u64 val, mask = 0;
	int base, i, nr_events;

	if (!pmceid1) {
		val = compute_pmceid0(cpu_pmu);
		base = 0;
	} else {
		val = compute_pmceid1(cpu_pmu);
		base = 32;
	}

	if (!bmap)
		return val;

	nr_events = kvm_pmu_event_mask(vcpu->kvm) + 1;

	for (i = 0; i < 32; i += 8) {
		u64 byte;

		byte = bitmap_get_value8(bmap, base + i);
		mask |= byte << i;
		if (nr_events >= (0x4000 + base + 32)) {
			byte = bitmap_get_value8(bmap, 0x4000 + base + i);
			mask |= byte << (32 + i);
		}
	}

	return val & mask;
}

/*
 * When perf interrupt is an NMI, we cannot safely notify the vcpu corresponding
 * to the event.
 * This is why we need a callback to do it once outside of the NMI context.
 */
static void kvm_pmu_perf_overflow_notify_vcpu(struct irq_work *work)
{
	struct kvm_vcpu *vcpu;

	vcpu = container_of(work, struct kvm_vcpu, arch.pmu.overflow_work);
	kvm_vcpu_kick(vcpu);
}

static u32 __kvm_pmu_event_mask(unsigned int pmuver)
{
	switch (pmuver) {
	case ID_AA64DFR0_EL1_PMUVer_IMP:
		return GENMASK(9, 0);
	case ID_AA64DFR0_EL1_PMUVer_V3P1:
	case ID_AA64DFR0_EL1_PMUVer_V3P4:
	case ID_AA64DFR0_EL1_PMUVer_V3P5:
	case ID_AA64DFR0_EL1_PMUVer_V3P7:
		return GENMASK(15, 0);
	default:		/* Shouldn't be here, just for sanity */
		WARN_ONCE(1, "Unknown PMU version %d\n", pmuver);
		return 0;
	}
}

u32 kvm_pmu_event_mask(struct kvm *kvm)
{
	u64 dfr0 = kvm_read_vm_id_reg(kvm, SYS_ID_AA64DFR0_EL1);
	u8 pmuver = SYS_FIELD_GET(ID_AA64DFR0_EL1, PMUVer, dfr0);

	return __kvm_pmu_event_mask(pmuver);
}

u64 kvm_pmu_evtyper_mask(struct kvm *kvm)
{
	u64 mask = ARMV8_PMU_EXCLUDE_EL1 | ARMV8_PMU_EXCLUDE_EL0 |
		   kvm_pmu_event_mask(kvm);

	if (kvm_has_feat(kvm, ID_AA64PFR0_EL1, EL2, IMP))
		mask |= ARMV8_PMU_INCLUDE_EL2;

	if (kvm_has_feat(kvm, ID_AA64PFR0_EL1, EL3, IMP))
		mask |= ARMV8_PMU_EXCLUDE_NS_EL0 |
			ARMV8_PMU_EXCLUDE_NS_EL1 |
			ARMV8_PMU_EXCLUDE_EL3;

	return mask;
}

static void kvm_pmu_update_state(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu *pmu = &vcpu->arch.pmu;
	bool overflow;

	if (kvm_pmu_is_partitioned(vcpu->kvm))
		overflow = kvm_pmu_part_overflow_status(vcpu);
	else
		overflow = kvm_pmu_emul_overflow_status(vcpu);

	if (pmu->irq_level == overflow)
		return;

	pmu->irq_level = overflow;

	if (likely(irqchip_in_kernel(vcpu->kvm))) {
		int ret = kvm_vgic_inject_irq(vcpu->kvm, vcpu,
					      pmu->irq_num, overflow, pmu);
		WARN_ON(ret);
	}
}

/**
 * kvm_pmu_flush_hwstate - flush pmu state to cpu
 * @vcpu: The vcpu pointer
 *
 * Check if the PMU has overflowed while we were running in the host, and inject
 * an interrupt if that was the case.
 */
void kvm_pmu_flush_hwstate(struct kvm_vcpu *vcpu)
{
	kvm_pmu_update_state(vcpu);
}

/**
 * kvm_pmu_sync_hwstate - sync pmu state from cpu
 * @vcpu: The vcpu pointer
 *
 * Check if the PMU has overflowed while we were running in the guest, and
 * inject an interrupt if that was the case.
 */
void kvm_pmu_sync_hwstate(struct kvm_vcpu *vcpu)
{
	kvm_pmu_update_state(vcpu);
}

int kvm_arm_pmu_v3_enable(struct kvm_vcpu *vcpu)
{
	if (!vcpu->arch.pmu.created)
		return -EINVAL;

	/*
	 * A valid interrupt configuration for the PMU is either to have a
	 * properly configured interrupt number and using an in-kernel
	 * irqchip, or to not have an in-kernel GIC and not set an IRQ.
	 */
	if (irqchip_in_kernel(vcpu->kvm)) {
		int irq = vcpu->arch.pmu.irq_num;
		/*
		 * If we are using an in-kernel vgic, at this point we know
		 * the vgic will be initialized, so we can check the PMU irq
		 * number against the dimensions of the vgic and make sure
		 * it's valid.
		 */
		if (!irq_is_ppi(vcpu->kvm, irq) && !vgic_valid_spi(vcpu->kvm, irq))
			return -EINVAL;
	} else if (kvm_arm_pmu_irq_initialized(vcpu)) {
		return -EINVAL;
	}

	return 0;
}

static int kvm_arm_pmu_v3_init(struct kvm_vcpu *vcpu)
{
	if (irqchip_in_kernel(vcpu->kvm)) {
		int ret;

		/*
		 * If using the PMU with an in-kernel virtual GIC
		 * implementation, we require the GIC to be already
		 * initialized when initializing the PMU.
		 */
		if (!vgic_initialized(vcpu->kvm))
			return -ENODEV;

		if (!kvm_arm_pmu_irq_initialized(vcpu)) {
			if (!vgic_is_v5(vcpu->kvm))
				return -ENXIO;

			/* Use the architected irq number for GICv5. */
			vcpu->arch.pmu.irq_num = KVM_ARMV8_PMU_GICV5_IRQ;
		}

		ret = kvm_vgic_set_owner(vcpu, vcpu->arch.pmu.irq_num,
					 &vcpu->arch.pmu);
		if (ret)
			return ret;
	}

	init_irq_work(&vcpu->arch.pmu.overflow_work,
		      kvm_pmu_perf_overflow_notify_vcpu);

	vcpu->arch.pmu.created = true;
	return 0;
}

/*
 * For one VM the interrupt type must be same for each vcpu.
 * As a PPI, the interrupt number is the same for all vcpus,
 * while as an SPI it must be a separate number per vcpu.
 */
static bool pmu_irq_is_valid(struct kvm *kvm, int irq)
{
	unsigned long i;
	struct kvm_vcpu *vcpu;

	/* On GICv5, the PMUIRQ is architecturally mandated to be PPI 23 */
	if (vgic_is_v5(kvm) && irq != KVM_ARMV8_PMU_GICV5_IRQ)
		return false;

	kvm_for_each_vcpu(i, vcpu, kvm) {
		if (!kvm_arm_pmu_irq_initialized(vcpu))
			continue;

		if (irq_is_ppi(kvm, irq)) {
			if (vcpu->arch.pmu.irq_num != irq)
				return false;
		} else {
			if (vcpu->arch.pmu.irq_num == irq)
				return false;
		}
	}

	return true;
}

/**
 * kvm_arm_pmu_get_max_counters - Return the max number of PMU counters.
 * @kvm: The kvm pointer
 */
u8 kvm_arm_pmu_get_max_counters(struct kvm *kvm)
{
	struct arm_pmu *arm_pmu = kvm->arch.arm_pmu;

	/*
	 * PMUv3 requires that all event counters are capable of counting any
	 * event, though the same may not be true of non-PMUv3 hardware.
	 */
	if (cpus_have_final_cap(ARM64_WORKAROUND_PMUV3_IMPDEF_TRAPS))
		return 1;

	/*
	 * If partitioned then we are limited by the max counters in
	 * the guest partition.
	 */
	if (pmu_is_partitioned(arm_pmu))
		return arm_pmu->max_guest_counters;

	/*
	 * The arm_pmu->cntr_mask considers the fixed counter(s) as well.
	 * Ignore those and return only the general-purpose counters.
	 */
	return bitmap_weight(arm_pmu->cntr_mask, ARMV8_PMU_MAX_GENERAL_COUNTERS);
}

static void kvm_arm_set_nr_counters(struct kvm *kvm, unsigned int nr)
{
	kvm->arch.nr_pmu_counters = nr;

	/* Reset MDCR_EL2.HPMN behind the vcpus' back... */
	if (test_bit(KVM_ARM_VCPU_HAS_EL2, kvm->arch.vcpu_features)) {
		struct kvm_vcpu *vcpu;
		unsigned long i;

		kvm_for_each_vcpu(i, vcpu, kvm) {
			u64 val = __vcpu_sys_reg(vcpu, MDCR_EL2);

			val &= ~MDCR_EL2_HPMN;
			val |= FIELD_PREP(MDCR_EL2_HPMN, kvm->arch.nr_pmu_counters);
			__vcpu_assign_sys_reg(vcpu, MDCR_EL2, val);
		}
	}
}

static void kvm_arm_set_pmu(struct kvm *kvm, struct arm_pmu *arm_pmu)
{
	lockdep_assert_held(&kvm->arch.config_lock);

	kvm->arch.arm_pmu = arm_pmu;
	kvm_arm_set_nr_counters(kvm, kvm_arm_pmu_get_max_counters(kvm));
}

/**
 * kvm_arm_set_default_pmu - No PMU set, get the default one.
 * @kvm: The kvm pointer
 *
 * The observant among you will notice that the supported_cpus
 * mask does not get updated for the default PMU even though it
 * is quite possible the selected instance supports only a
 * subset of cores in the system. This is intentional, and
 * upholds the preexisting behavior on heterogeneous systems
 * where vCPUs can be scheduled on any core but the guest
 * counters could stop working.
 */
int kvm_arm_set_default_pmu(struct kvm *kvm)
{
	struct arm_pmu *arm_pmu = kvm_pmu_probe_armpmu();

	if (!arm_pmu)
		return -ENODEV;

	kvm_arm_set_pmu(kvm, arm_pmu);
	return 0;
}

static int kvm_arm_pmu_v3_set_pmu(struct kvm_vcpu *vcpu, int pmu_id)
{
	struct kvm *kvm = vcpu->kvm;
	struct arm_pmu_entry *entry;
	struct arm_pmu *arm_pmu;
	int ret = -ENXIO;

	lockdep_assert_held(&kvm->arch.config_lock);
	mutex_lock(&arm_pmus_lock);

	list_for_each_entry(entry, &arm_pmus, entry) {
		arm_pmu = entry->arm_pmu;
		if (arm_pmu->pmu.type == pmu_id) {
			if (kvm_vm_has_ran_once(kvm) ||
			    (kvm->arch.pmu_filter && kvm->arch.arm_pmu != arm_pmu)) {
				ret = -EBUSY;
				break;
			}

			kvm_arm_set_pmu(kvm, arm_pmu);
			cpumask_copy(kvm->arch.supported_cpus, &arm_pmu->supported_cpus);
			ret = 0;
			break;
		}
	}

	mutex_unlock(&arm_pmus_lock);
	return ret;
}

static int kvm_arm_pmu_v3_set_nr_counters(struct kvm_vcpu *vcpu, unsigned int n)
{
	struct kvm *kvm = vcpu->kvm;

	if (!kvm->arch.arm_pmu)
		return -EINVAL;

	if (n > kvm_arm_pmu_get_max_counters(kvm))
		return -EINVAL;

	kvm_arm_set_nr_counters(kvm, n);
	return 0;
}

int kvm_arm_pmu_v3_set_attr(struct kvm_vcpu *vcpu, struct kvm_device_attr *attr)
{
	struct kvm *kvm = vcpu->kvm;

	lockdep_assert_held(&kvm->arch.config_lock);

	if (!kvm_vcpu_has_pmu(vcpu))
		return -ENODEV;

	if (vcpu->arch.pmu.created)
		return -EBUSY;

	switch (attr->attr) {
	case KVM_ARM_VCPU_PMU_V3_IRQ: {
		int __user *uaddr = (int __user *)(long)attr->addr;
		int irq;

		if (!irqchip_in_kernel(kvm))
			return -EINVAL;

		if (get_user(irq, uaddr))
			return -EFAULT;

		/* The PMU overflow interrupt can be a PPI or a valid SPI. */
		if (!(irq_is_ppi(kvm, irq) || irq_is_spi(kvm, irq)))
			return -EINVAL;

		if (!pmu_irq_is_valid(kvm, irq))
			return -EINVAL;

		if (kvm_arm_pmu_irq_initialized(vcpu))
			return -EBUSY;

		kvm_debug("Set kvm ARM PMU irq: %d\n", irq);
		vcpu->arch.pmu.irq_num = irq;
		return 0;
	}
	case KVM_ARM_VCPU_PMU_V3_FILTER: {
		u8 pmuver = kvm_arm_pmu_get_pmuver_limit();
		struct kvm_pmu_event_filter __user *uaddr;
		struct kvm_pmu_event_filter filter;
		int nr_events;

		/*
		 * Allow userspace to specify an event filter for the entire
		 * event range supported by PMUVer of the hardware, rather
		 * than the guest's PMUVer for KVM backward compatibility.
		 */
		nr_events = __kvm_pmu_event_mask(pmuver) + 1;

		uaddr = (struct kvm_pmu_event_filter __user *)(long)attr->addr;

		if (copy_from_user(&filter, uaddr, sizeof(filter)))
			return -EFAULT;

		if (((u32)filter.base_event + filter.nevents) > nr_events ||
		    (filter.action != KVM_PMU_EVENT_ALLOW &&
		     filter.action != KVM_PMU_EVENT_DENY))
			return -EINVAL;

		if (kvm_vm_has_ran_once(kvm))
			return -EBUSY;

		if (!kvm->arch.pmu_filter) {
			kvm->arch.pmu_filter = bitmap_alloc(nr_events, GFP_KERNEL_ACCOUNT);
			if (!kvm->arch.pmu_filter)
				return -ENOMEM;

			/*
			 * The default depends on the first applied filter.
			 * If it allows events, the default is to deny.
			 * Conversely, if the first filter denies a set of
			 * events, the default is to allow.
			 */
			if (filter.action == KVM_PMU_EVENT_ALLOW)
				bitmap_zero(kvm->arch.pmu_filter, nr_events);
			else
				bitmap_fill(kvm->arch.pmu_filter, nr_events);
		}

		if (filter.action == KVM_PMU_EVENT_ALLOW)
			bitmap_set(kvm->arch.pmu_filter, filter.base_event, filter.nevents);
		else
			bitmap_clear(kvm->arch.pmu_filter, filter.base_event, filter.nevents);

		return 0;
	}
	case KVM_ARM_VCPU_PMU_V3_SET_PMU: {
		int __user *uaddr = (int __user *)(long)attr->addr;
		int pmu_id;

		if (get_user(pmu_id, uaddr))
			return -EFAULT;

		return kvm_arm_pmu_v3_set_pmu(vcpu, pmu_id);
	}
	case KVM_ARM_VCPU_PMU_V3_SET_NR_COUNTERS: {
		unsigned int __user *uaddr = (unsigned int __user *)(long)attr->addr;
		unsigned int n;

		if (get_user(n, uaddr))
			return -EFAULT;

		return kvm_arm_pmu_v3_set_nr_counters(vcpu, n);
	}
	case KVM_ARM_VCPU_PMU_V3_INIT:
		return kvm_arm_pmu_v3_init(vcpu);
	}

	return -ENXIO;
}

int kvm_arm_pmu_v3_get_attr(struct kvm_vcpu *vcpu, struct kvm_device_attr *attr)
{
	switch (attr->attr) {
	case KVM_ARM_VCPU_PMU_V3_IRQ: {
		int __user *uaddr = (int __user *)(long)attr->addr;
		int irq;

		if (!irqchip_in_kernel(vcpu->kvm))
			return -EINVAL;

		if (!kvm_vcpu_has_pmu(vcpu))
			return -ENODEV;

		if (!kvm_arm_pmu_irq_initialized(vcpu))
			return -ENXIO;

		irq = vcpu->arch.pmu.irq_num;
		return put_user(irq, uaddr);
	}
	}

	return -ENXIO;
}

int kvm_arm_pmu_v3_has_attr(struct kvm_vcpu *vcpu, struct kvm_device_attr *attr)
{
	switch (attr->attr) {
	case KVM_ARM_VCPU_PMU_V3_IRQ:
	case KVM_ARM_VCPU_PMU_V3_INIT:
	case KVM_ARM_VCPU_PMU_V3_FILTER:
	case KVM_ARM_VCPU_PMU_V3_SET_PMU:
	case KVM_ARM_VCPU_PMU_V3_SET_NR_COUNTERS:
		if (kvm_vcpu_has_pmu(vcpu))
			return 0;
	}

	return -ENXIO;
}

u8 kvm_arm_pmu_get_pmuver_limit(void)
{
	unsigned int pmuver;

	pmuver = SYS_FIELD_GET(ID_AA64DFR0_EL1, PMUVer,
			       read_sanitised_ftr_reg(SYS_ID_AA64DFR0_EL1));

	/*
	 * Spoof a barebones PMUv3 implementation if the system supports IMPDEF
	 * traps of the PMUv3 sysregs
	 */
	if (cpus_have_final_cap(ARM64_WORKAROUND_PMUV3_IMPDEF_TRAPS))
		return ID_AA64DFR0_EL1_PMUVer_IMP;

	/*
	 * Otherwise, treat IMPLEMENTATION DEFINED functionality as
	 * unimplemented
	 */
	if (pmuver == ID_AA64DFR0_EL1_PMUVer_IMP_DEF)
		return 0;

	return min(pmuver, ID_AA64DFR0_EL1_PMUVer_V3P5);
}

u64 kvm_pmu_implemented_counter_mask(struct kvm_vcpu *vcpu)
{
	u64 val = FIELD_GET(ARMV8_PMU_PMCR_N, kvm_vcpu_read_pmcr(vcpu));

	if (val == 0)
		return BIT(ARMV8_PMU_CYCLE_IDX);
	else
		return GENMASK(val - 1, 0) | BIT(ARMV8_PMU_CYCLE_IDX);
}

u64 kvm_pmu_hyp_counter_mask(struct kvm_vcpu *vcpu)
{
	unsigned int hpmn, n;

	if (!vcpu_has_nv(vcpu))
		return 0;

	hpmn = SYS_FIELD_GET(MDCR_EL2, HPMN, __vcpu_sys_reg(vcpu, MDCR_EL2));
	n = vcpu->kvm->arch.nr_pmu_counters;

	/*
	 * Programming HPMN to a value greater than PMCR_EL0.N is
	 * CONSTRAINED UNPREDICTABLE. Make the implementation choice that an
	 * UNKNOWN number of counters (in our case, zero) are reserved for EL2.
	 */
	if (hpmn >= n)
		return 0;

	/*
	 * Programming HPMN=0 is CONSTRAINED UNPREDICTABLE if FEAT_HPMN0 isn't
	 * implemented. Since KVM's ability to emulate HPMN=0 does not directly
	 * depend on hardware (all PMU registers are trapped), make the
	 * implementation choice that all counters are included in the second
	 * range reserved for EL2/EL3.
	 */
	return GENMASK(n - 1, hpmn);
}

bool kvm_pmu_counter_is_hyp(struct kvm_vcpu *vcpu, unsigned int idx)
{
	return kvm_pmu_hyp_counter_mask(vcpu) & BIT(idx);
}

u64 kvm_pmu_accessible_counter_mask(struct kvm_vcpu *vcpu)
{
	u64 mask = kvm_pmu_implemented_counter_mask(vcpu);

	if (!vcpu_has_nv(vcpu) || vcpu_is_el2(vcpu))
		return mask;

	return mask & ~kvm_pmu_hyp_counter_mask(vcpu);
}

/**
 * kvm_vcpu_read_pmcr - Read PMCR_EL0 register for the vCPU
 * @vcpu: The vcpu pointer
 */
u64 kvm_vcpu_read_pmcr(struct kvm_vcpu *vcpu)
{
	u64 pmcr = __vcpu_sys_reg(vcpu, PMCR_EL0);
	u64 n = vcpu->kvm->arch.nr_pmu_counters;

	if (vcpu_has_nv(vcpu) && !vcpu_is_el2(vcpu))
		n = FIELD_GET(MDCR_EL2_HPMN, __vcpu_sys_reg(vcpu, MDCR_EL2));

	return u64_replace_bits(pmcr, n, ARMV8_PMU_PMCR_N);
}
