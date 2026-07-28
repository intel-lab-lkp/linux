// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt)	"amd_ibs_memprof: " fmt

#include <linux/init.h>
#include <linux/pghot.h>
#include <linux/percpu.h>
#include <linux/workqueue.h>
#include <linux/mm.h>
#include <linux/vm_event_item.h>
#include <linux/vmstat.h>
#include <linux/cpuhotplug.h>

#include <asm/ibs-mprof.h>
#include <asm/ibs-caps.h>
#include <asm/irq_vectors.h>
#include <asm/idtentry.h>
#include <asm/apic.h>
#include <asm/cpuid/api.h>

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
 * Per-CPU work for pushing the percpu access samples to pghot sub-system.
 *
 * @cpu records which CPU's sample ring this work item is responsible for
 * draining.
 */
struct mprof_worker {
	struct work_struct work;
	unsigned int cpu;
};
static DEFINE_PER_CPU(struct mprof_worker, mprof_work);

/*
 * Record the IBS-reported access sample in percpu buffer.
 * Called from IBS interrupt handler.
 */
static bool mprof_push_sample(unsigned long pfn, int nid, unsigned long time)
{
	struct mprof_sample_pcpu *pcpu = raw_cpu_ptr(mprof_s);
	int head = READ_ONCE(pcpu->head);
	int tail = READ_ONCE(pcpu->tail);
	int next = head + 1;

	if (next >= IBS_NR_SAMPLES)
		next = 0;

	if (next == tail) {
		count_vm_event(HWHINT_DROPPED_EVENTS);
		return false;
	}

	pcpu->samples[head].pfn = pfn;
	pcpu->samples[head].time = time;
	pcpu->samples[head].nid = nid;

	/*
	 * Publish the sample slot stores before advancing head; pairs
	 * with the smp_load_acquire() of head in mprof_pop_sample().
	 */
	smp_store_release(&pcpu->head, next);
	return true;
}

static bool mprof_pop_sample(struct mprof_sample_pcpu *pcpu, struct mprof_sample *s)
{
	int tail = READ_ONCE(pcpu->tail);
	/*
	 * Pairs with the smp_store_release() of head in mprof_push_sample();
	 * ensures the sample slot stores are visible before we read the slot.
	 */
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
	struct mprof_worker *mw = container_of(work, struct mprof_worker, work);
	unsigned int cpu = mw->cpu;
	struct mprof_sample_pcpu *pcpu = per_cpu_ptr(mprof_s, cpu);
	struct mprof_sample s;

	for (;;) {
		while (mprof_pop_sample(pcpu, &s))
			pghot_record_access(s.pfn, s.nid, PGHOT_HWHINTS,
					    s.time);

		per_cpu(mprof_work_pending, cpu) = false;

		/*
		 * Publish the cleared pending flag before re-checking the
		 * ring.
		 */
		smp_mb();

		if (READ_ONCE(pcpu->head) == READ_ONCE(pcpu->tail))
			return;

		/*
		 * A sample was pushed after we exited the drain loop; the
		 * producer may have skipped re-queuing because pending was
		 * still set. Keep draining until the ring is observably empty
		 * with pending cleared.
		 */
	}
}

/*
 * Empty a dying CPU's sample ring.
 *
 * This is called from hotplug teardown callback and IBS Memory Profiler
 * has been disabled at this point. There is no concurrent producer or
 * consumer for this CPU's ring here, hence the plain accesses.
 */
static inline void mprof_drain_cpu(unsigned int cpu)
{
	struct mprof_sample_pcpu *pcpu = per_cpu_ptr(mprof_s, cpu);

	pcpu->head = 0;
	pcpu->tail = 0;
}

/*
 * L3MissOnly + Exclude kernel RIP
 */
static void mprof_enable_profiling(void)
{
	u64 mprof_config = IBS_MPROF_CTL_CNT_CTL | IBS_MPROF_CTL_L3MISSONLY;
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
 * IBS interrupt handler: Process the memory access info reported by IBS.
 *
 * Reads the MSRs to collect all the information about the reported
 * memory access, validates the access, stores the valid sample and
 * schedules the work on this CPU to further process the sample.
 */
static void mprof_overflow_handler(void)
{
	u64 mem_ctl, mem_data3, mem_data2, paddr, data_src;
	struct work_struct *w = &this_cpu_ptr(&mprof_work)->work;
	unsigned long pfn;
	struct page *page;

	rdmsrq(MSR_AMD64_IBS_MPROF_CTL, mem_ctl);
	if (!(mem_ctl & IBS_MPROF_CTL_VAL))
		return;

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
		schedule_work_on(smp_processor_id(), w);
	}
	count_vm_event(HWHINT_USEFUL_EVENTS);

handled:
	mprof_enable_profiling();
}

DEFINE_IDTENTRY_SYSVEC(sysvec_ibs_memprof)
{
	inc_irq_stat(IBS_MEMPROF);
	mprof_overflow_handler();
	apic_eoi();
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

	if (setup_APIC_eilvt(offset, IBS_MEMPROF_VECTOR, APIC_DELIVERY_MODE_FIXED, 0)) {
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
		setup_APIC_eilvt(offset, IBS_MEMPROF_VECTOR, APIC_DELIVERY_MODE_FIXED, 1);

	rdmsrq(MSR_AMD64_IBS_MPROF_CTL, mem_ctl);
	mprof_disable_profiling(mem_ctl);

	/*
	 * The producer is now silenced and this CPU's worker is gone. Drop
	 * any unconsumed samples (see mprof_drain_cpu) and clear the pending
	 * flag so a subsequent re-online of this CPU starts from a clean
	 * state.
	 */
	mprof_drain_cpu(cpu);
	per_cpu(mprof_work_pending, cpu) = false;

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
		struct mprof_worker *mw = per_cpu_ptr(&mprof_work, cpu);

		INIT_WORK(&mw->work, mprof_work_handler);
		mw->cpu = cpu;
	}

	ret = cpuhp_setup_state(CPUHP_AP_MM_AMD_IBS_MEMPROF_STARTING,
				"x86/amd/ibs_mprof:starting",
				x86_amd_ibs_mprof_startup,
				x86_amd_ibs_mprof_teardown);

	if (ret) {
		free_percpu(mprof_s);
		pr_err("cpuhp_setup_state failed: %d\n", ret);
	} else {
		pr_info("IBS Memory Profiler is available for memory access profiling\n");
	}
	return 0;
}

device_initcall(mprof_access_profiling_init);
