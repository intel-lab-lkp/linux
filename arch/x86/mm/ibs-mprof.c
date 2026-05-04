// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt)	"amd_ibs_memprof: " fmt

#include <linux/init.h>
#include <linux/pghot.h>
#include <linux/percpu.h>
#include <linux/workqueue.h>
#include <linux/irq_work.h>
#include <linux/mm.h>
#include <linux/vm_event_item.h>
#include <linux/vmstat.h>
#include <linux/cpuhotplug.h>

#include <asm/ibs-mprof.h>
#include <asm/ibs-caps.h>
#include <asm/nmi.h>
#include <asm/apic.h>

#define IBS_NR_SAMPLES		150	/* Percpu sample buffer size */

static DEFINE_PER_CPU(bool, mprof_work_pending);

/*
 * Basic access info captured for each memory access.
 */
struct mprof_sample {
	unsigned long pfn;
	unsigned long time;	/* jiffies when accessed */
	int nid;		/* Accessing node ID, if known */
};

/*
 * Percpu buffer of access samples. Samples are accumulated here
 * before pushing them to pghot sub-system for further action.
 */
struct mprof_sample_pcpu {
	struct mprof_sample samples[IBS_NR_SAMPLES];
	int head, tail;
};

static struct mprof_sample_pcpu __percpu *mprof_s;

/*
 * The workqueue for pushing the percpu access samples to pghot sub-system.
 */
static DEFINE_PER_CPU(struct work_struct, mprof_work);
static DEFINE_PER_CPU(struct irq_work, mprof_irq_work);

/*
 * Record the IBS-reported access sample in percpu buffer.
 * Called from IBS NMI handler.
 */
static bool mprof_push_sample(unsigned long pfn, int nid, unsigned long time)
{
	struct mprof_sample_pcpu *pcpu = raw_cpu_ptr(mprof_s);
	int head = READ_ONCE(pcpu->head);
	int tail = READ_ONCE(pcpu->tail);
	int next = head + 1;

	if (next >= IBS_NR_SAMPLES)
		next = 0;

	if (next == tail)
		return false;

	pcpu->samples[head].pfn = pfn;
	pcpu->samples[head].time = time;
	pcpu->samples[head].nid = nid;

	smp_store_release(&pcpu->head, next);
	return true;
}

static bool mprof_pop_sample(struct mprof_sample *s)
{
	struct mprof_sample_pcpu *pcpu = raw_cpu_ptr(mprof_s);
	int tail = READ_ONCE(pcpu->tail);
	int head = smp_load_acquire(&pcpu->head);
	int next = tail + 1;

	if (head == tail)
		return false;

	if (next >= IBS_NR_SAMPLES)
		next = 0;

	*s = pcpu->samples[tail];

	WRITE_ONCE(pcpu->tail, next);
	return true;
}

/*
 * Remove access samples from percpu buffer and send them
 * to pghot sub-system for further action.
 */
static void mprof_work_handler(struct work_struct *work)
{
	struct mprof_sample s;

	while (mprof_pop_sample(&s))
		pghot_record_access(s.pfn, s.nid, PGHOT_HWHINTS, s.time);

	this_cpu_write(mprof_work_pending, false);
}

static void mprof_irq_handler(struct irq_work *i)
{
	struct work_struct *w = this_cpu_ptr(&mprof_work);

	/*
	 * FIXME: pending samples on a CPU that goes offline before the
	 * work runs may be lost or migrated to the wrong CPU's ring;
	 * needs a teardown-time drain.
	 */
	schedule_work_on(smp_processor_id(), w);
}

/*
 * L3MissOnly + Exclude kernel RIP
 */
static void mprof_enable_profiling(void)
{
	u64 mprof_config = IBS_MPROF_CTL_CNT_CTL | IBS_MPROF_CTL_ENABLE |
			   IBS_MPROF_CTL_L3MISSONLY;
	unsigned int period = IBS_MPROF_SAMPLE_PERIOD;
	u64 ctl, ctl2;

	/*
	 * Assemble bits 26:20 and 19:4 of periodic op counter in ctl.
	 * The lower 4 bits are always 0000b.
	 */
	ctl = (period >> 4) & IBS_MPROF_CTL_MAXCNT_MASK;
	ctl |= (period & IBS_MPROF_CTL_MAXCNT_EXT_MASK);
	ctl |= mprof_config;
	wrmsrq(MSR_AMD64_IBS_MPROF_CTL, ctl);

	/*
	 * Exclude samples that have bit 63 of their RIP set.
	 */
	ctl2 = IBS_MPROF_CTL2_EXCLUDE_KERNEL;
	wrmsrq(MSR_AMD64_IBS_MPROF_CTL2, ctl2);
}

static void mprof_disable_profiling(u64 mem_ctl)
{
	mem_ctl &= ~IBS_MPROF_CTL_ENABLE;
	mem_ctl &= ~IBS_MPROF_CTL_VAL;
	wrmsrq(MSR_AMD64_IBS_MPROF_CTL, mem_ctl);

	wrmsrq(MSR_AMD64_IBS_MPROF_CTL2, IBS_MPROF_CTL2_DISABLE);
}

/*
 * IBS NMI handler: Process the memory access info reported by IBS.
 *
 * Reads the MSRs to collect all the information about the reported
 * memory access, validates the access, stores the valid sample and
 * schedules the work on this CPU to further process the sample.
 */
static int mprof_overflow_handler(unsigned int cmd, struct pt_regs *regs)
{
	u64 mem_ctl, mem_data3, mem_data2, paddr, data_src;
	unsigned long pfn;
	struct page *page;

	rdmsrq(MSR_AMD64_IBS_MPROF_CTL, mem_ctl);
	if (!(mem_ctl & IBS_MPROF_CTL_VAL))
		return NMI_DONE;

	mprof_disable_profiling(mem_ctl);
	count_vm_event(HWHINT_TOTAL_EVENTS);

	rdmsrq(MSR_AMD64_IBS_MPROF_DATA3, mem_data3);
	rdmsrq(MSR_AMD64_IBS_MPROF_DATA2, mem_data2);

	data_src = mem_data2 & IBS_MPROF_DATA2_DATASRC_MASK;
	data_src |= ((mem_data2 & IBS_MPROF_DATA2_DATASRC_MASK_HIGH) >>
			IBS_MPROF_DATA2_DATASRC_MASK_HIGH_SHIFT);

	switch (data_src) {
	case IBS_MPROF_DATA2_DATASRC_DRAM:
		count_vm_event(HWHINT_DRAM_ACCESSES);
		break;
	case IBS_MPROF_DATA2_DATASRC_EXT_MEM:
		count_vm_event(HWHINT_EXTMEM_ACCESSES);
		break;
	}

	/* Is linear addr valid? */
	if (!(mem_data3 & IBS_MPROF_DATA3_LADDR_VALID))
		goto handled;

	/* Is phys addr valid? */
	if (!(mem_data3 & IBS_MPROF_DATA3_PADDR_VALID))
		goto handled;
	rdmsrq(MSR_AMD64_IBS_MPROF_PHYADDR, paddr);

	pfn = PHYS_PFN(paddr);
	page = pfn_to_online_page(pfn);
	if (!page)
		goto handled;

	/*
	 * Use the accessing CPU's node as the migration target. On
	 * topologies where all CPUs reside on toptier nodes (the common
	 * case), this is the desired behaviour. Topologies that place
	 * CPUs on lower-tier nodes are rejected later by
	 * pghot_record_access() via the src_nid == nid early return.
	 */
	if (!mprof_push_sample(pfn, numa_node_id(), jiffies))
		goto handled;

	if (!this_cpu_read(mprof_work_pending)) {
		this_cpu_write(mprof_work_pending, true);
		irq_work_queue(this_cpu_ptr(&mprof_irq_work));
	}
	count_vm_event(HWHINT_USEFUL_EVENTS);

handled:
	mprof_enable_profiling();
	return NMI_HANDLED;
}

static int get_mprof_lvt_offset(void)
{
	u64 val;

	rdmsrq(MSR_AMD64_IBSCTL, val);
	if (!(val & IBSCTL_MPROF_LVT_OFFSET_VALID))
		return -EINVAL;

	return (val & IBSCTL_MPROF_LVT_OFFSET_MASK) >>
		IBSCTL_MPROF_LVT_OFFSET_SHIFT;
}

static int x86_amd_ibs_mprof_startup(unsigned int cpu)
{
	int offset = get_mprof_lvt_offset();

	if (offset < 0) {
		pr_warn("offset not valid on cpu #%d\n", cpu);
		return 0;
	}

	if (setup_APIC_eilvt(offset, 0, APIC_DELIVERY_MODE_NMI, 0)) {
		pr_warn("APIC setup failed on cpu #%d\n", cpu);
		return 0;
	}

	mprof_enable_profiling();
	return 0;
}

static int x86_amd_ibs_mprof_teardown(unsigned int cpu)
{
	int offset = get_mprof_lvt_offset();
	u64 mem_ctl;

	if (offset >= 0)
		setup_APIC_eilvt(offset, 0, APIC_DELIVERY_MODE_FIXED, 1);

	rdmsrq(MSR_AMD64_IBS_MPROF_CTL, mem_ctl);
	mprof_disable_profiling(mem_ctl);

	return 0;
}

static int __init mprof_access_profiling_init(void)
{
	u32 mprof_caps = cpuid_eax(IBS_CPUID_FEATURES);
	int cpu, ret;

	if (!(mprof_caps & IBS_CAPS_MEM_PROFILER)) {
		pr_info("capability is unavailable for access profiling\n");
		return 0;
	}

	mprof_s = alloc_percpu_gfp(struct mprof_sample_pcpu, GFP_KERNEL | __GFP_ZERO);
	if (!mprof_s) {
		pr_err("alloc_percpu_gfp failed\n");
		return 0;
	}

	for_each_possible_cpu(cpu) {
		INIT_WORK(per_cpu_ptr(&mprof_work, cpu), mprof_work_handler);
		init_irq_work(per_cpu_ptr(&mprof_irq_work, cpu), mprof_irq_handler);
	}

	register_nmi_handler(NMI_LOCAL, mprof_overflow_handler, 0, "ibs-memprof");

	ret = cpuhp_setup_state(CPUHP_AP_MM_AMD_IBS_MEMPROF_STARTING,
				"x86/amd/ibs_mprof:starting",
				x86_amd_ibs_mprof_startup,
				x86_amd_ibs_mprof_teardown);

	if (ret) {
		unregister_nmi_handler(NMI_LOCAL, "ibs-memprof");
		free_percpu(mprof_s);
		pr_err("cpuhp_setup_state failed: %d\n", ret);
	} else {
		pr_info("IBS Memory Profiler setup for memory access profiling\n");
	}
	return 0;
}

device_initcall(mprof_access_profiling_init);
