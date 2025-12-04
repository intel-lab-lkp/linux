// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SMP support for pSeries machines.
 *
 * Dave Engebretsen, Peter Bergner, and
 * Mike Corrigan {engebret|bergner|mikec}@us.ibm.com
 *
 * Plus various changes from other IBM teams...
 */


#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/cache.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/cpu.h>
#include <linux/pgtable.h>
#if defined(CONFIG_DEBUG_FS) && defined(CONFIG_PPC_SPLPAR)
#include <linux/debugfs.h>
#endif

#include <asm/ptrace.h>
#include <linux/atomic.h>
#include <asm/irq.h>
#include <asm/page.h>
#include <asm/io.h>
#include <asm/smp.h>
#include <asm/paca.h>
#include <asm/machdep.h>
#include <asm/cputable.h>
#include <asm/firmware.h>
#include <asm/rtas.h>
#include <asm/vdso_datapage.h>
#include <asm/cputhreads.h>
#include <asm/xics.h>
#include <asm/xive.h>
#include <asm/dbell.h>
#include <asm/plpar_wrappers.h>
#include <asm/text-patching.h>
#include <asm/svm.h>
#include <asm/kvm_guest.h>
#include <asm/hvcall.h>

#include "pseries.h"

/*
 * The Primary thread of each non-boot processor was started from the OF client
 * interface by prom_hold_cpus and is spinning on secondary_hold_spinloop.
 */
static cpumask_var_t of_spin_mask;
#ifdef CONFIG_PPC_SPLPAR
static cpumask_var_t cpus;
#endif

/* Query where a cpu is now.  Return codes #defined in plpar_wrappers.h */
int smp_query_cpu_stopped(unsigned int pcpu)
{
	int cpu_status, status;
	int qcss_tok = rtas_function_token(RTAS_FN_QUERY_CPU_STOPPED_STATE);

	if (qcss_tok == RTAS_UNKNOWN_SERVICE) {
		printk_once(KERN_INFO
			"Firmware doesn't support query-cpu-stopped-state\n");
		return QCSS_HARDWARE_ERROR;
	}

	status = rtas_call(qcss_tok, 1, 2, &cpu_status, pcpu);
	if (status != 0) {
		printk(KERN_ERR
		       "RTAS query-cpu-stopped-state failed: %i\n", status);
		return status;
	}

	return cpu_status;
}

/**
 * smp_startup_cpu() - start the given cpu
 *
 * At boot time, there is nothing to do for primary threads which were
 * started from Open Firmware.  For anything else, call RTAS with the
 * appropriate start location.
 *
 * Returns:
 *	0	- failure
 *	1	- success
 */
static inline int smp_startup_cpu(unsigned int lcpu)
{
	int status;
	unsigned long start_here =
			__pa(ppc_function_entry(generic_secondary_smp_init));
	unsigned int pcpu;
	int start_cpu;

	if (cpumask_test_cpu(lcpu, of_spin_mask))
		/* Already started by OF and sitting in spin loop */
		return 1;

	pcpu = get_hard_smp_processor_id(lcpu);

	/* Check to see if the CPU out of FW already for kexec */
	if (smp_query_cpu_stopped(pcpu) == QCSS_NOT_STOPPED){
		cpumask_set_cpu(lcpu, of_spin_mask);
		return 1;
	}

	/* 
	 * If the RTAS start-cpu token does not exist then presume the
	 * cpu is already spinning.
	 */
	start_cpu = rtas_function_token(RTAS_FN_START_CPU);
	if (start_cpu == RTAS_UNKNOWN_SERVICE)
		return 1;

	status = rtas_call(start_cpu, 3, 1, NULL, pcpu, start_here, pcpu);
	if (status != 0) {
		printk(KERN_ERR "start-cpu failed: %i\n", status);
		return 0;
	}

	return 1;
}

#ifdef CONFIG_PPC_SPLPAR
struct offline_worker {
	struct work_struct work;
	int offline;
	int cpu;
};

static DEFINE_PER_CPU(struct offline_worker, offline_workers);

static void softoffline_work_fn(struct work_struct *work)
{
	struct offline_worker *worker = this_cpu_ptr(&offline_workers);

	set_cpu_softoffline(worker->cpu, worker->offline);
}

static void softoffline_work_init(void)
{
	int cpu;

	if (!is_shared_processor() || is_kvm_guest())
		return;

	for_each_possible_cpu(cpu) {
		struct offline_worker *worker = &per_cpu(offline_workers, cpu);

		INIT_WORK(&worker->work, softoffline_work_fn);
		worker->cpu = cpu;
	}
}
#else
static void softoffline_work_init(void)
{
}
#endif

static void smp_setup_cpu(int cpu)
{
	if (xive_enabled())
		xive_smp_setup_cpu();
	else if (cpu != boot_cpuid)
		xics_setup_cpu();

	if (firmware_has_feature(FW_FEATURE_SPLPAR))
		vpa_init(cpu);

	cpumask_clear_cpu(cpu, of_spin_mask);
}

static int smp_pSeries_kick_cpu(int nr)
{
	if (nr < 0 || nr >= nr_cpu_ids)
		return -EINVAL;

	if (!smp_startup_cpu(nr))
		return -ENOENT;

	/*
	 * The processor is currently spinning, waiting for the
	 * cpu_start field to become non-zero After we set cpu_start,
	 * the processor will continue on to secondary_start
	 */
	paca_ptrs[nr]->cpu_start = 1;

	return 0;
}

static int pseries_smp_prepare_cpu(int cpu)
{
	if (xive_enabled())
		return xive_smp_prepare_cpu(cpu);
	return 0;
}

/* Cause IPI as setup by the interrupt controller (xics or xive) */
static void (*ic_cause_ipi)(int cpu) __ro_after_init;

/* Use msgsndp doorbells target is a sibling, else use interrupt controller */
static void dbell_or_ic_cause_ipi(int cpu)
{
	if (doorbell_try_core_ipi(cpu))
		return;

	ic_cause_ipi(cpu);
}

static int pseries_cause_nmi_ipi(int cpu)
{
	int hwcpu;

	if (cpu == NMI_IPI_ALL_OTHERS) {
		hwcpu = H_SIGNAL_SYS_RESET_ALL_OTHERS;
	} else {
		if (cpu < 0) {
			WARN_ONCE(true, "incorrect cpu parameter %d", cpu);
			return 0;
		}

		hwcpu = get_hard_smp_processor_id(cpu);
	}

	if (plpar_signal_sys_reset(hwcpu) == H_SUCCESS)
		return 1;

	return 0;
}

static __init void pSeries_smp_probe(void)
{
	if (xive_enabled())
		xive_smp_probe();
	else
		xics_smp_probe();

	/* No doorbell facility, must use the interrupt controller for IPIs */
	if (!cpu_has_feature(CPU_FTR_DBELL))
		return;

	/* Doorbells can only be used for IPIs between SMT siblings */
	if (!cpu_has_feature(CPU_FTR_SMT))
		return;

	check_kvm_guest();

	if (is_kvm_guest()) {
		/*
		 * KVM emulates doorbells by disabling FSCR[MSGP] so msgsndp
		 * faults to the hypervisor which then reads the instruction
		 * from guest memory, which tends to be slower than using XIVE.
		 */
		if (xive_enabled())
			return;

		/*
		 * XICS hcalls aren't as fast, so we can use msgsndp (which
		 * also helps exercise KVM emulation), however KVM can't
		 * emulate secure guests because it can't read the instruction
		 * out of their memory.
		 */
		if (is_secure_guest())
			return;
	}

	/*
	 * Under PowerVM, FSCR[MSGP] is enabled as guest vCPU siblings are
	 * gang scheduled on the same physical core, so doorbells are always
	 * faster than the interrupt controller, and they can be used by
	 * secure guests.
	 */

	ic_cause_ipi = smp_ops->cause_ipi;
	smp_ops->cause_ipi = dbell_or_ic_cause_ipi;
}

#ifdef CONFIG_PPC_SPLPAR
/*
 * Set higher threshold values to which steal has to be limited. Also set
 * lower threshold values below which allow work to spread out to more
 * cores.
 */
static unsigned int max_virtual_cores __read_mostly;
static unsigned int entitled_cores __read_mostly;
static unsigned int available_cores;

/* Get pseries soft entitlement limit */
unsigned int pseries_num_available_cores(void)
{
	unsigned int present_cores = num_present_cpus() / threads_per_core;
	unsigned long retbuf[PLPAR_HCALL9_BUFSIZE];

	if (!is_shared_processor() || is_kvm_guest())
		return present_cores;

	if (entitled_cores && max_virtual_cores == present_cores)
		return available_cores;

	if (plpar_hcall9(H_GET_PPP, retbuf))
		return num_present_cpus() / threads_per_core;

	if (!entitled_cores)
		softoffline_work_init();

	entitled_cores = retbuf[0] / 100;
	max_virtual_cores = present_cores;

	if (!available_cores)
		available_cores = max_virtual_cores;
	else if (available_cores < entitled_cores)
		available_cores = entitled_cores;
	else if (available_cores > max_virtual_cores)
		available_cores = max_virtual_cores;

	return available_cores;
}

static u8 steal_ratio_high = 10;
static u8 steal_ratio_low = 5;

void trigger_softoffline(unsigned long steal_ratio)
{
	int currcpu = smp_processor_id();
	static int prev_direction;
	int success = 0;
	int cpu, i;

	/*
	 * Compare delta runtime versus delta steal time.
	 *  [0]<----------->[EC]--------->[VP]
	 *  [0]<------------------>{AC}-->[VP]
	 *  EC == Entitled Cores
	 *  VP == Virtual Processors
	 *  AC == Available Cores Varies between 0 to EC/VP.
	 * If Steal time is high, then reduce Available Cores.
	 * If steal time is low, increase Available Cores
	 */
	if (steal_ratio >= STEAL_RATIO * steal_ratio_high && prev_direction > 0) {
		/*
		 * System entitlement was reduced earlier but we continue to
		 * see steal time. Reduce entitlement further if possible.
		 */
		if (available_cores <= entitled_cores)
			return;

		cpu = cpumask_last(cpu_active_mask);
		for_each_cpu_andnot(i, cpu_sibling_mask(cpu), cpu_sibling_mask(currcpu)) {
			struct offline_worker *worker = &per_cpu(offline_workers, i);

			worker->offline = 1;
			schedule_work_on(i, &worker->work);
			success = 1;
		}
		if (success)
			available_cores--;
	} else if (steal_ratio <= STEAL_RATIO * steal_ratio_low && prev_direction < 0) {
		/*
		 * System entitlement was increased but we continue to see
		 * less steal time. Increase entitlement further if possible.
		 */
		if (available_cores >= max_virtual_cores)
			return;

		cpumask_andnot(cpus, cpu_online_mask, cpu_active_mask);
		if (cpumask_empty(cpus))
			return;

		cpu = cpumask_first(cpus);
		for_each_cpu_andnot(i, cpu_sibling_mask(cpu), cpu_sibling_mask(currcpu)) {
			struct offline_worker *worker = &per_cpu(offline_workers, i);

			worker->offline = 0;
			schedule_work_on(i, &worker->work);
			success = 1;
		}
		if (success)
			available_cores++;
	}
	if (steal_ratio >= STEAL_RATIO * steal_ratio_high)
		prev_direction = 1;
	else if (steal_ratio <= STEAL_RATIO * steal_ratio_low)
		prev_direction = -1;
	else
		prev_direction = 0;
}
#endif

static struct smp_ops_t pseries_smp_ops = {
	.message_pass	= NULL,	/* Use smp_muxed_ipi_message_pass */
	.cause_ipi	= NULL,	/* Filled at runtime by pSeries_smp_probe() */
	.cause_nmi_ipi	= pseries_cause_nmi_ipi,
	.probe		= pSeries_smp_probe,
	.prepare_cpu	= pseries_smp_prepare_cpu,
	.kick_cpu	= smp_pSeries_kick_cpu,
	.setup_cpu	= smp_setup_cpu,
	.cpu_bootable	= smp_generic_cpu_bootable,
#ifdef CONFIG_PPC_SPLPAR
	.num_available_cores = pseries_num_available_cores,
#endif
};

/* This is called very early */
void __init smp_init_pseries(void)
{
	int i;

	pr_debug(" -> smp_init_pSeries()\n");
	smp_ops = &pseries_smp_ops;

	alloc_bootmem_cpumask_var(&of_spin_mask);
#ifdef CONFIG_PPC_SPLPAR
	alloc_bootmem_cpumask_var(&cpus);
#endif

	/*
	 * Mark threads which are still spinning in hold loops
	 *
	 * We know prom_init will not have started them if RTAS supports
	 * query-cpu-stopped-state.
	 */
	if (rtas_function_token(RTAS_FN_QUERY_CPU_STOPPED_STATE) == RTAS_UNKNOWN_SERVICE) {
		if (cpu_has_feature(CPU_FTR_SMT)) {
			for_each_present_cpu(i) {
				if (cpu_thread_in_core(i) == 0)
					cpumask_set_cpu(i, of_spin_mask);
			}
		} else
			cpumask_copy(of_spin_mask, cpu_present_mask);

		cpumask_clear_cpu(boot_cpuid, of_spin_mask);
	}

	pr_debug(" <- smp_init_pSeries()\n");
}

#if defined(CONFIG_DEBUG_FS) && defined(CONFIG_PPC_SPLPAR)
static int __init steal_ratio_debugfs_init(void)
{
	if (!firmware_has_feature(FW_FEATURE_SPLPAR))
		return 0;

	debugfs_create_u8("steal_high", 0600, arch_debugfs_dir, &steal_ratio_high);
	debugfs_create_u8("steal_low", 0600, arch_debugfs_dir, &steal_ratio_low);
	return 0;
}
machine_arch_initcall(pseries, steal_ratio_debugfs_init);
#endif /* CONFIG_DEBUG_FS && CONFIG_PPC_SPLPAR*/
