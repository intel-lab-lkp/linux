/*
 * This file is subject to the terms and conditions of the GNU General
 * Public License.  See the file "COPYING" in the main directory of this
 * archive for more details.
 *
 * Copyright (C) 2000 - 2001 by Kanoj Sarcar (kanoj@sgi.com)
 * Copyright (C) 2000 - 2001 by Silicon Graphics, Inc.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/mm.h>
#include <linux/export.h>
#include <linux/cpumask.h>
#include <asm/bootinfo.h>
#include <asm/cpu.h>
#include <asm/io.h>
#include <asm/sgialib.h>
#include <asm/time.h>
#include <asm/sn/agent.h>
#include <asm/sn/types.h>
#include <asm/sn/klconfig.h>
#include <asm/sn/ioc3.h>
#include <asm/mipsregs.h>
#include <asm/sn/gda.h>
#include <asm/sn/intr.h>
#include <asm/current.h>
#include <asm/processor.h>
#include <asm/mmu_context.h>
#include <asm/thread_info.h>
#include <asm/sn/launch.h>
#include <asm/sn/mapped_kernel.h>
#include <asm/xtalk/xwidget.h>

#include "ip27-common.h"

#define CPU_NONE		(cpuid_t)-1

/* A Bedrock hub has two PI register blocks; a Hub has one. */
#define PIS_PER_HUB		2

static DECLARE_BITMAP(hub_init_mask, MAX_NUMNODES);
static DECLARE_BITMAP(pi_init_mask, MAX_NUMNODES * PIS_PER_HUB);
nasid_t master_nasid = INVALID_NASID;
bool system_is_ip35;

struct cpuinfo_ip27 sn_cpu_info[NR_CPUS];
EXPORT_SYMBOL_GPL(sn_cpu_info);

static void per_hub_init(nasid_t nasid)
{
	int cpu = smp_processor_id();
	struct hub_data *hub = hub_data(nasid);
	int subnode = cputosubnode(cpu);

	cpumask_set_cpu(cpu, &hub->h_cpus);

	/* A second pass would zero the counter under a CPU already ticking. */
	if (!test_and_set_bit(nasid * PIS_PER_HUB + subnode, pi_init_mask))
		hub_rtc_init(nasid);

	if (test_and_set_bit(nasid, hub_init_mask))
		return;
	/*
	 * Set CRB timeout at 5ms, (< PI timeout of 10ms)
	 */
	REMOTE_HUB_S(nasid, IIO_ICTP, 0x800);
	REMOTE_HUB_S(nasid, IIO_ICTO, 0xff);

	if (nasid) {
		/* copy exception handlers from first node to current node */
		memcpy((void *)NODE_OFFSET_TO_K0(nasid, 0),
		       (void *)CKSEG0, 0x200);
		__flush_cache_all();
		/* switch to node local exception handlers */
		REMOTE_HUB_S(nasid, PI_CALIAS_SIZE, PI_CALIAS_SIZE_8K);
	}
}

void per_cpu_init(void)
{
	int cpu = smp_processor_id();
	nasid_t nasid = cputonasid(cpu);

	clear_c0_status(ST0_IM);

	per_hub_init(nasid);

	pr_info("CPU %d clock is %dMHz.\n", cpu, sn_cpu_info[cpu].p_speed);

	install_ipi();

	/* Install our NMI handler if symmon hasn't installed one. */
	install_cpu_nmi_handler(cputoslice(cpu));

	enable_percpu_irq(IP27_HUB_PEND0_IRQ, IRQ_TYPE_NONE);
	enable_percpu_irq(IP27_HUB_PEND1_IRQ, IRQ_TYPE_NONE);
}

void __init plat_mem_setup(void)
{
	u64 p, e, n_mode;
	nasid_t nid;

	register_smp_ops(&ip27_smp_ops);

	ip27_reboot_setup();

	/*
	 * hub_rtc init and cpu clock intr enabled for later calibrate_delay.
	 */
	nid = get_nasid();
	pr_info("%s: Running on node %d.\n",
		system_is_ip35 ? "IP35" : "IP27", nid);

	p = LOCAL_HUB_L(PI_CPU_PRESENT_A) & 1;
	e = LOCAL_HUB_L(PI_CPU_ENABLE_A) & 1;
	printk("Node %d has %s primary CPU%s.\n", nid,
	       p ? "a" : "no",
	       e ? ", CPU is running" : "");

	p = LOCAL_HUB_L(PI_CPU_PRESENT_B) & 1;
	e = LOCAL_HUB_L(PI_CPU_ENABLE_B) & 1;
	printk("Node %d has %s secondary CPU%s.\n", nid,
	       p ? "a" : "no",
	       e ? ", CPU is running" : "");

	/*
	 * Try to catch kernel missconfigurations and give user an
	 * indication what option to select.
	 */
	if (system_is_ip35)
		n_mode = LRI_SYSTEM_SIZE_IP35(LOCAL_HUB_L(NI_STATUS_REV_ID)) ==
			 LRI_SYSTEM_SIZE_NMODE_IP35;
	else
		n_mode = LOCAL_HUB_L(NI_STATUS_REV_ID) & NSRI_MORENODES_MASK;
	printk("Machine is in %c mode.\n", n_mode ? 'N' : 'M');
#ifdef CONFIG_SGI_SN_N_MODE
	if (!n_mode)
		panic("Kernel compiled for M mode.");
#else
	if (n_mode)
		panic("Kernel compiled for N mode.");
#endif

	ioport_resource.start = 0;
	ioport_resource.end = ~0UL;
	set_io_port_base(IO_BASE);
}

const char *get_system_type(void)
{
	return system_is_ip35 ? "SGI IP35" : "SGI Origin";
}

static inline bool hub_is_bedrock(void)
{
	return XWIDGET_PART_NUM(LOCAL_HUB_L(IIO_WID)) == WIDGET_BDRCK_PART_NUM;
}

void __init prom_init(void)
{
	system_is_ip35 = hub_is_bedrock();

	prom_init_cmdline(fw_arg0, (LONG *)fw_arg1);
	prom_meminit();
}

