// SPDX-License-Identifier: GPL-2.0
#include <linux/export.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/jump_label.h>
#include <linux/kvm_para.h>
#include <asm/paravirt.h>
#include <linux/static_call.h>

struct static_key paravirt_steal_enabled;
struct static_key paravirt_steal_rq_enabled;

static u64 native_steal_clock(int cpu)
{
	return 0;
}

DEFINE_STATIC_CALL(pv_steal_clock, native_steal_clock);

#ifdef CONFIG_SMP
static void pv_send_ipi_single(int cpu, unsigned int action)
{
	unsigned int min, old_action;
	unsigned long ipi_bitmap = 0;
	irq_cpustat_t *info = &per_cpu(irq_stat, cpu);

	action = 1UL << action;
	old_action = atomic_fetch_or(action, &info->messages);
	if (old_action == 0) {
		min = cpu_logical_map(cpu);
		ipi_bitmap = 1;
		kvm_hypercall2(KVM_HC_FUNC_IPI, ipi_bitmap, min);
	}
}

static void pv_send_ipi_mask(const struct cpumask *mask, unsigned int action)
{
	unsigned int cpu, i, min = 0, max = 0, old_action;
	u64 ipi_bitmap = 0;
	irq_cpustat_t *info;

	if (cpumask_empty(mask))
		return;

	action = 1UL << action;
	for_each_cpu(i, mask) {
		cpu = cpu_logical_map(i);
		if (!ipi_bitmap) {
			min = max = cpu;
		} else if (cpu < min && (max - cpu) < BITS_PER_LONG) {
			ipi_bitmap <<= min - cpu;
			min = cpu;
		} else if (cpu > min && cpu < min + BITS_PER_LONG) {
			max = cpu < max ? max : cpu;
		} else {
			kvm_hypercall2(KVM_HC_FUNC_IPI, ipi_bitmap, min);
			min = max = cpu;
			ipi_bitmap = 0;
		}
		info = &per_cpu(irq_stat, i);
		old_action = atomic_fetch_or(action, &info->messages);
		if (old_action == 0)
			__set_bit(cpu - min, (unsigned long *)&ipi_bitmap);
	}

	if (ipi_bitmap)
		kvm_hypercall2(KVM_HC_FUNC_IPI, ipi_bitmap, min);
}

static irqreturn_t loongson_do_swi(int irq, void *dev)
{
	irq_cpustat_t *info;
	long action;

	clear_csr_estat(1 << INT_SWI0);

	info = this_cpu_ptr(&irq_stat);
	do {
		action = atomic_xchg(&info->messages, 0);
		if (action & SMP_CALL_FUNCTION) {
			generic_smp_call_function_interrupt();
			info->ipi_irqs[IPI_CALL_FUNCTION]++;
		}

		if (action & SMP_RESCHEDULE) {
			scheduler_ipi();
			info->ipi_irqs[IPI_RESCHEDULE]++;
		}
	} while (action);

	return IRQ_HANDLED;
}

static void pv_ipi_init(void)
{
	int r, swi0;

	swi0 = get_percpu_irq(INT_SWI0);
	if (swi0 < 0)
		panic("SIP0 IRQ mapping failed\n");
	irq_set_percpu_devid(swi0);
	r = request_percpu_irq(swi0, loongson_do_swi, "SWI0", &irq_stat);
	if (r < 0)
		panic("SIP0 IRQ request failed\n");
}
#endif

static bool kvm_para_available(void)
{
	static int hypervisor_type;
	int config;

	if (!hypervisor_type) {
		config = read_cpucfg(CPUCFG_KVM_SIG);
		if (!memcmp(&config, KVM_SIGNATURE, 4))
			hypervisor_type = HYPERVISOR_KVM;
	}

	return hypervisor_type == HYPERVISOR_KVM;
}

int __init pv_guest_init(void)
{
	int feature;

	if (!cpu_has_hypervisor)
		return 0;
	if (!kvm_para_available())
		return 0;

	/*
	 * check whether KVM hypervisor supports pv_ipi or not
	 */
#ifdef CONFIG_SMP
	feature = read_cpucfg(CPUCFG_KVM_FEATURE);
	if (feature & KVM_FEATURE_PV_IPI) {
		smp_ops.call_func_single_ipi	= pv_send_ipi_single;
		smp_ops.call_func_ipi		= pv_send_ipi_mask;
		smp_ops.ipi_init		= pv_ipi_init;
	}
#endif

	return 1;
}
