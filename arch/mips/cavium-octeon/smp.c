/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2004-2008, 2009, 2010 Cavium Networks
 */
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/sched.h>
#include <linux/sched/hotplug.h>
#include <linux/sched/task_stack.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/kexec.h>

#include <asm/ipi.h>
#include <asm/mmu_context.h>
#include <asm/time.h>
#include <asm/setup.h>
#include <asm/smp.h>

#include <asm/octeon/octeon.h>

#include "octeon_boot.h"

volatile unsigned long octeon_processor_boot = 0xff;
volatile unsigned long octeon_processor_sp;
volatile unsigned long octeon_processor_gp;
#ifdef CONFIG_RELOCATABLE
volatile unsigned long octeon_processor_relocated_kernel_entry;
#endif /* CONFIG_RELOCATABLE */

#ifdef CONFIG_HOTPLUG_CPU
uint64_t octeon_bootloader_entry_addr;
EXPORT_SYMBOL(octeon_bootloader_entry_addr);
#endif

extern void kernel_entry(unsigned long arg1, ...);

static irqreturn_t octeon_icache_flush(int irq, void *dev_id)
{
	asm volatile ("synci 0($0)\n");
	return IRQ_HANDLED;
}

static irqreturn_t mailbox_interrupt(int irq, void *dev_id)
{
	u64 mbox_clrx = CVMX_CIU_MBOX_CLRX(cvmx_get_core_num());
	unsigned long action;
	int op;

	BUILD_BUG_ON(IPI_MAX > 8);

	/*
	 * Load the mailbox register to figure out what we're supposed
	 * to do.
	 */
	action = cvmx_read_csr(mbox_clrx);

	if (OCTEON_IS_MODEL(OCTEON_CN68XX))
		action &= 0xff;
	else
		action &= 0xffff;

	/* Clear the mailbox to clear the interrupt */
	cvmx_write_csr(mbox_clrx, action);

	for_each_set_bit(op, &action, IPI_MAX) {
		ipi_handlers[op](0, NULL);
	}

	return IRQ_HANDLED;
}

/*
 * Cause the function described by call_data to be executed on the passed
 * cpu.	 When the function has finished, increment the finished field of
 * call_data.
 */
void octeon_send_ipi_single(int cpu, enum ipi_message_type op)
{
	int coreid = cpu_logical_map(cpu);
	/*
	pr_info("SMP: Mailbox send cpu=%d, coreid=%d, action=%u\n", cpu,
	       coreid, action);
	*/
	cvmx_write_csr(CVMX_CIU_MBOX_SETX(coreid), op);
}

static void octeon_send_ipi_mask(const struct cpumask *mask,
				 enum ipi_message_type op)
{
	unsigned int i;

	for_each_cpu(i, mask)
		octeon_send_ipi_single(i, op);
}

/*
 * Detect available CPUs, populate cpu_possible_mask
 */
static void octeon_smp_hotplug_setup(void)
{
#ifdef CONFIG_HOTPLUG_CPU
	struct linux_app_boot_info *labi;

	if (!setup_max_cpus)
		return;

	labi = (struct linux_app_boot_info *)PHYS_TO_XKSEG_CACHED(LABI_ADDR_IN_BOOTLOADER);
	if (labi->labi_signature != LABI_SIGNATURE) {
		pr_info("The bootloader on this board does not support HOTPLUG_CPU.");
		return;
	}

	octeon_bootloader_entry_addr = labi->InitTLBStart_addr;
#endif
}

static void __init octeon_smp_setup(void)
{
	const int coreid = cvmx_get_core_num();
	int cpus;
	int id;
	struct cvmx_sysinfo *sysinfo = cvmx_sysinfo_get();

#ifdef CONFIG_HOTPLUG_CPU
	int core_mask = octeon_get_boot_coremask();
	unsigned int num_cores = cvmx_octeon_num_cores();
#endif

	ipi_handlers[IPI_ICACHE_FLUSH] = octeon_icache_flush;
	ipi_names[IPI_ICACHE_FLUSH] = "Octeon ICache Flush";

	/* The present CPUs are initially just the boot cpu (CPU 0). */
	for (id = 0; id < NR_CPUS; id++) {
		set_cpu_possible(id, id == 0);
		set_cpu_present(id, id == 0);
	}

	__cpu_number_map[coreid] = 0;
	__cpu_logical_map[0] = coreid;

	/* The present CPUs get the lowest CPU numbers. */
	cpus = 1;
	for (id = 0; id < NR_CPUS; id++) {
		if ((id != coreid) && cvmx_coremask_is_core_set(&sysinfo->core_mask, id)) {
			set_cpu_possible(cpus, true);
			set_cpu_present(cpus, true);
			__cpu_number_map[id] = cpus;
			__cpu_logical_map[cpus] = id;
			cpus++;
		}
	}

#ifdef CONFIG_HOTPLUG_CPU
	/*
	 * The possible CPUs are all those present on the chip.	 We
	 * will assign CPU numbers for possible cores as well.	Cores
	 * are always consecutively numberd from 0.
	 */
	for (id = 0; setup_max_cpus && octeon_bootloader_entry_addr &&
		     id < num_cores && id < NR_CPUS; id++) {
		if (!(core_mask & (1 << id))) {
			set_cpu_possible(cpus, true);
			__cpu_number_map[id] = cpus;
			__cpu_logical_map[cpus] = id;
			cpus++;
		}
	}
#endif

	octeon_smp_hotplug_setup();
}


#ifdef CONFIG_RELOCATABLE
int plat_post_relocation(long offset)
{
	unsigned long entry = (unsigned long)kernel_entry;

	/* Send secondaries into relocated kernel */
	octeon_processor_relocated_kernel_entry = entry + offset;

	return 0;
}
#endif /* CONFIG_RELOCATABLE */

/*
 * Firmware CPU startup hook
 */
static int octeon_boot_secondary(int cpu, struct task_struct *idle)
{
	int count;

	pr_info("SMP: Booting CPU%02d (CoreId %2d)...\n", cpu,
		cpu_logical_map(cpu));

	octeon_processor_sp = __KSTK_TOS(idle);
	octeon_processor_gp = (unsigned long)(task_thread_info(idle));
	octeon_processor_boot = cpu_logical_map(cpu);
	mb();

	count = 10000;
	while (octeon_processor_sp && count) {
		/* Waiting for processor to get the SP and GP */
		udelay(1);
		count--;
	}
	if (count == 0) {
		pr_err("Secondary boot timeout\n");
		return -ETIMEDOUT;
	}

	return 0;
}

/*
 * After we've done initial boot, this function is called to allow the
 * board code to clean up state, if needed
 */
static void octeon_init_secondary(void)
{
	unsigned int sr;

	sr = set_c0_status(ST0_BEV);
	write_c0_ebase((u32)ebase);
	write_c0_status(sr);

	octeon_check_cpu_bist();
	octeon_init_cvmcount();

	octeon_irq_setup_secondary();
}

/*
 * Callout to firmware before smp_init
 */
static void __init octeon_prepare_cpus(unsigned int max_cpus)
{
	/*
	 * Only the low order mailbox bits are used for IPIs, leave
	 * the other bits alone.
	 */
	cvmx_write_csr(CVMX_CIU_MBOX_CLRX(cvmx_get_core_num()), 0xffff);
	if (request_irq(OCTEON_IRQ_MBOX0, mailbox_interrupt,
			IRQF_PERCPU | IRQF_NO_THREAD, "SMP-IPI",
			mailbox_interrupt)) {
		panic("Cannot request_irq(OCTEON_IRQ_MBOX0)");
	}
}

/*
 * Last chance for the board code to finish SMP initialization before
 * the CPU is "online".
 */
static void octeon_smp_finish(void)
{
	octeon_user_io_init();

	/* to generate the first CPU timer interrupt */
	write_c0_compare(read_c0_count() + mips_hpt_frequency / HZ);
	local_irq_enable();
}

#ifdef CONFIG_HOTPLUG_CPU

/* State of each CPU. */
static DEFINE_PER_CPU(int, cpu_state);

static int octeon_cpu_disable(void)
{
	unsigned int cpu = smp_processor_id();

	if (!octeon_bootloader_entry_addr)
		return -ENOTSUPP;

	set_cpu_online(cpu, false);
	calculate_cpu_foreign_map();
	octeon_fixup_irqs();

	__flush_cache_all();
	local_flush_tlb_all();

	return 0;
}

static void octeon_cpu_die(unsigned int cpu)
{
	int coreid = cpu_logical_map(cpu);
	uint32_t mask, new_mask;
	const struct cvmx_bootmem_named_block_desc *block_desc;

	while (per_cpu(cpu_state, cpu) != CPU_DEAD)
		cpu_relax();

	/*
	 * This is a bit complicated strategics of getting/settig available
	 * cores mask, copied from bootloader
	 */

	mask = 1 << coreid;
	/* LINUX_APP_BOOT_BLOCK is initialized in bootoct binary */
	block_desc = cvmx_bootmem_find_named_block(LINUX_APP_BOOT_BLOCK_NAME);

	if (!block_desc) {
		struct linux_app_boot_info *labi;

		labi = (struct linux_app_boot_info *)PHYS_TO_XKSEG_CACHED(LABI_ADDR_IN_BOOTLOADER);

		labi->avail_coremask |= mask;
		new_mask = labi->avail_coremask;
	} else {		       /* alternative, already initialized */
		uint32_t *p = (uint32_t *)PHYS_TO_XKSEG_CACHED(block_desc->base_addr +
							       AVAIL_COREMASK_OFFSET_IN_LINUX_APP_BOOT_BLOCK);
		*p |= mask;
		new_mask = *p;
	}

	pr_info("Reset core %d. Available Coremask = 0x%x \n", coreid, new_mask);
	mb();
	cvmx_write_csr(CVMX_CIU_PP_RST, 1 << coreid);
	cvmx_write_csr(CVMX_CIU_PP_RST, 0);
}

void play_dead(void)
{
	int cpu = cpu_number_map(cvmx_get_core_num());

	idle_task_exit();
	cpuhp_ap_report_dead();
	octeon_processor_boot = 0xff;
	per_cpu(cpu_state, cpu) = CPU_DEAD;

	mb();

	while (1)	/* core will be reset here */
		;
}

static void start_after_reset(void)
{
	kernel_entry(0, 0, 0);	/* set a2 = 0 for secondary core */
}

static int octeon_update_boot_vector(unsigned int cpu)
{

	int coreid = cpu_logical_map(cpu);
	uint32_t avail_coremask;
	const struct cvmx_bootmem_named_block_desc *block_desc;
	struct boot_init_vector *boot_vect =
		(struct boot_init_vector *)PHYS_TO_XKSEG_CACHED(BOOTLOADER_BOOT_VECTOR);

	block_desc = cvmx_bootmem_find_named_block(LINUX_APP_BOOT_BLOCK_NAME);

	if (!block_desc) {
		struct linux_app_boot_info *labi;

		labi = (struct linux_app_boot_info *)PHYS_TO_XKSEG_CACHED(LABI_ADDR_IN_BOOTLOADER);

		avail_coremask = labi->avail_coremask;
		labi->avail_coremask &= ~(1 << coreid);
	} else {		       /* alternative, already initialized */
		avail_coremask = *(uint32_t *)PHYS_TO_XKSEG_CACHED(
			block_desc->base_addr + AVAIL_COREMASK_OFFSET_IN_LINUX_APP_BOOT_BLOCK);
	}

	if (!(avail_coremask & (1 << coreid))) {
		/* core not available, assume, that caught by simple-executive */
		cvmx_write_csr(CVMX_CIU_PP_RST, 1 << coreid);
		cvmx_write_csr(CVMX_CIU_PP_RST, 0);
	}

	boot_vect[coreid].app_start_func_addr =
		(uint32_t) (unsigned long) start_after_reset;
	boot_vect[coreid].code_addr = octeon_bootloader_entry_addr;

	mb();

	cvmx_write_csr(CVMX_CIU_NMI, (1 << coreid) & avail_coremask);

	return 0;
}

static int register_cavium_notifier(void)
{
	return cpuhp_setup_state_nocalls(CPUHP_MIPS_SOC_PREPARE,
					 "mips/cavium:prepare",
					 octeon_update_boot_vector, NULL);
}
late_initcall(register_cavium_notifier);

#endif	/* CONFIG_HOTPLUG_CPU */

static const struct plat_smp_ops octeon_smp_ops = {
	.send_ipi_single	= octeon_send_ipi_single,
	.send_ipi_mask		= octeon_send_ipi_mask,
	.init_secondary		= octeon_init_secondary,
	.smp_finish		= octeon_smp_finish,
	.boot_secondary		= octeon_boot_secondary,
	.smp_setup		= octeon_smp_setup,
	.prepare_cpus		= octeon_prepare_cpus,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_disable		= octeon_cpu_disable,
	.cpu_die		= octeon_cpu_die,
#endif
#ifdef CONFIG_KEXEC_CORE
	.kexec_nonboot_cpu	= kexec_nonboot_cpu_jump,
#endif
};

/*
 * Callout to firmware before smp_init
 */
static void octeon_78xx_prepare_cpus(unsigned int max_cpus)
{
	int i;

	/*
	 * FIXME: Hardware have 10 MBOX but only 4 virqs are reserved
	 * for CIU3 MBOX.
	 */
	BUILD_BUG_ON(IPI_MAX > 4);

	for (i = 0; i < IPI_MAX; i++) {
		if (request_irq(OCTEON_IRQ_MBOX0 + i,
				ipi_handlers[i],
				IRQF_PERCPU | IRQF_NO_THREAD, "IPI",
				ipi_handlers[i])) {
			panic("Cannot request_irq for %s", ipi_names[i]);
		}
	}
}

static void octeon_78xx_send_ipi_single(int cpu, enum ipi_message_type op)
{
	octeon_ciu3_mbox_send(cpu, op);
}

static void octeon_78xx_send_ipi_mask(const struct cpumask *mask,
				      enum ipi_message_type op)
{
	unsigned int cpu;

	for_each_cpu(cpu, mask)
		octeon_78xx_send_ipi_single(cpu, op);
}

static const struct plat_smp_ops octeon_78xx_smp_ops = {
	.send_ipi_single	= octeon_78xx_send_ipi_single,
	.send_ipi_mask		= octeon_78xx_send_ipi_mask,
	.init_secondary		= octeon_init_secondary,
	.smp_finish		= octeon_smp_finish,
	.boot_secondary		= octeon_boot_secondary,
	.smp_setup		= octeon_smp_setup,
	.prepare_cpus		= octeon_78xx_prepare_cpus,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_disable		= octeon_cpu_disable,
	.cpu_die		= octeon_cpu_die,
#endif
#ifdef CONFIG_KEXEC_CORE
	.kexec_nonboot_cpu	= kexec_nonboot_cpu_jump,
#endif
};

void __init octeon_setup_smp(void)
{
	const struct plat_smp_ops *ops;

	if (octeon_has_feature(OCTEON_FEATURE_CIU3))
		ops = &octeon_78xx_smp_ops;
	else
		ops = &octeon_smp_ops;

	register_smp_ops(ops);
}
