// SPDX-License-Identifier: GPL-2.0

#include <linux/init.h>
#include <linux/kpromoted.h>
#include <linux/percpu.h>
#include <linux/workqueue.h>
#include <linux/irq_work.h>

#include <asm/nmi.h>
#include <asm/perf_event.h> /* TODO: Move defns like IBS_OP_ENABLE into non-perf header */
#include <asm/apic.h>
#include <asm/ibs.h>

bool arch_hw_access_profiling;
static u64 ibs_config __read_mostly;
static u32 ibs_caps;

#define IBS_SAMPLE_PERIOD      10000
#define IBS_NR_SAMPLES	50

/*
 * Basic access info captured for each memory access.
 */
struct ibs_sample {
	unsigned long pfn;
	unsigned long time;	/* jiffies when accessed */
	int nid;		/* Accessing node ID, if known */
};

/*
 * Percpu buffer of access samples. Samples are accumulated here
 * before pushing them to kpromoted for further action.
 */
struct ibs_sample_pcpu {
	struct ibs_sample samples[IBS_NR_SAMPLES];
	int head, tail;
};

struct ibs_sample_pcpu __percpu *ibs_s;

/*
 * The workqueue for pushing the percpu access samples to kpromoted.
 */
static struct work_struct ibs_work;
static struct irq_work ibs_irq_work;

/*
 * Record the IBS-reported access sample in percpu buffer.
 * Called from IBS NMI handler.
 */
static int ibs_push_sample(unsigned long pfn, int nid, unsigned long time)
{
	struct ibs_sample_pcpu *ibs_pcpu = raw_cpu_ptr(ibs_s);
	int next = ibs_pcpu->head + 1;

	if (next >= IBS_NR_SAMPLES)
		next = 0;

	if (next == ibs_pcpu->tail)
		return 0;

	ibs_pcpu->samples[ibs_pcpu->head].pfn = pfn;
	ibs_pcpu->samples[ibs_pcpu->head].time = time;
	ibs_pcpu->head = next;
	return 1;
}

static int ibs_pop_sample(struct ibs_sample *s)
{
	struct ibs_sample_pcpu *ibs_pcpu = raw_cpu_ptr(ibs_s);

	int next = ibs_pcpu->tail + 1;

	if (ibs_pcpu->head == ibs_pcpu->tail)
		return 0;

	if (next >= IBS_NR_SAMPLES)
		next = 0;

	*s = ibs_pcpu->samples[ibs_pcpu->tail];
	ibs_pcpu->tail = next;
	return 1;
}

/*
 * Remove access samples from percpu buffer and send them
 * to kpromoted for further action.
 */
static void ibs_work_handler(struct work_struct *work)
{
	struct ibs_sample s;

	while (ibs_pop_sample(&s))
		kpromoted_record_access(s.pfn, s.nid, KPROMOTED_HW_HINTS,
					s.time);
}

static void ibs_irq_handler(struct irq_work *i)
{
	schedule_work_on(smp_processor_id(), &ibs_work);
}

void hw_access_profiling_stop(void)
{
	u64 ops_ctl;

	if (!arch_hw_access_profiling)
		return;

	rdmsrl(MSR_AMD64_IBSOPCTL, ops_ctl);
	wrmsrl(MSR_AMD64_IBSOPCTL, ops_ctl & ~IBS_OP_ENABLE);
}

void hw_access_profiling_start(void)
{
	u64 config = 0;
	unsigned int period = IBS_SAMPLE_PERIOD;

	if (!arch_hw_access_profiling)
		return;

	/* Disable IBS for kernel thread */
	if (!current->mm)
		goto out;

	config = (period >> 4)  & IBS_OP_MAX_CNT;
	config |= (period & IBS_OP_MAX_CNT_EXT_MASK);
	config |= ibs_config;
out:
	wrmsrl(MSR_AMD64_IBSOPCTL, config);
}

/*
 * IBS NMI handler: Process the memory access info reported by IBS.
 *
 * Reads the MSRs to collect all the information about the reported
 * memory access, validates the access, stores the valid sample and
 * schedules the work on this CPU to further process the sample.
 */
static int ibs_overflow_handler(unsigned int cmd, struct pt_regs *regs)
{
	struct mm_struct *mm = current->mm;
	u64 ops_ctl, ops_data3, ops_data2;
	u64 laddr = -1, paddr = -1;
	u64 data_src, rmt_node;
	struct page *page;
	unsigned long pfn;

	rdmsrl(MSR_AMD64_IBSOPCTL, ops_ctl);

	/*
	 * When IBS sampling period is reprogrammed via read-modify-update
	 * of MSR_AMD64_IBSOPCTL, overflow NMIs could be generated with
	 * IBS_OP_ENABLE not set. For such cases, return as HANDLED.
	 *
	 * With this, the handler will say "handled" for all NMIs that
	 * aren't related to this NMI.  This stems from the limitation of
	 * having both status and control bits in one MSR.
	 */
	if (!(ops_ctl & IBS_OP_VAL))
		goto handled;

	wrmsrl(MSR_AMD64_IBSOPCTL, ops_ctl & ~IBS_OP_VAL);

	count_vm_event(HWHINT_NR_EVENTS);

	if (!user_mode(regs)) {
		count_vm_event(HWHINT_KERNEL);
		goto handled;
	}

	if (!mm) {
		count_vm_event(HWHINT_KTHREAD);
		goto handled;
	}

	rdmsrl(MSR_AMD64_IBSOPDATA3, ops_data3);

	/* Load/Store ops only */
	/* TODO: DataSrc isn't valid for stores, so filter out stores? */
	if (!(ops_data3 & (MSR_AMD64_IBSOPDATA3_LDOP |
			   MSR_AMD64_IBSOPDATA3_STOP))) {
		count_vm_event(HWHINT_NON_LOAD_STORES);
		goto handled;
	}

	/* Discard the sample if it was L1 or L2 hit */
	if (!(ops_data3 & (MSR_AMD64_IBSOPDATA3_DCMISS |
			   MSR_AMD64_IBSOPDATA3_L2MISS))) {
		count_vm_event(HWHINT_DC_L2_HITS);
		goto handled;
	}

	rdmsrl(MSR_AMD64_IBSOPDATA2, ops_data2);
	data_src = ops_data2 & MSR_AMD64_IBSOPDATA2_DATASRC;
	if (ibs_caps & IBS_CAPS_ZEN4)
		data_src |= ((ops_data2 & 0xC0) >> 3);

	switch (data_src) {
	case MSR_AMD64_IBSOPDATA2_DATASRC_LCL_CACHE:
		count_vm_event(HWHINT_LOCAL_L3L1L2);
		break;
	case MSR_AMD64_IBSOPDATA2_DATASRC_PEER_CACHE_NEAR:
		count_vm_event(HWHINT_LOCAL_PEER_CACHE_NEAR);
		break;
	case MSR_AMD64_IBSOPDATA2_DATASRC_DRAM:
		count_vm_event(HWHINT_DRAM_ACCESSES);
		break;
	case MSR_AMD64_IBSOPDATA2_DATASRC_EXT_MEM:
		count_vm_event(HWHINT_CXL_ACCESSES);
		break;
	case MSR_AMD64_IBSOPDATA2_DATASRC_FAR_CCX_CACHE:
		count_vm_event(HWHINT_FAR_CACHE_HITS);
		break;
	}

	rmt_node = ops_data2 & MSR_AMD64_IBSOPDATA2_RMTNODE;
	if (rmt_node)
		count_vm_event(HWHINT_REMOTE_NODE);

	/* Is linear addr valid? */
	if (ops_data3 & MSR_AMD64_IBSOPDATA3_LADDR_VALID)
		rdmsrl(MSR_AMD64_IBSDCLINAD, laddr);
	else {
		count_vm_event(HWHINT_LADDR_INVALID);
		goto handled;
	}

	/* Discard kernel address accesses */
	if (laddr & (1UL << 63)) {
		count_vm_event(HWHINT_KERNEL_ADDR);
		goto handled;
	}

	/* Is phys addr valid? */
	if (ops_data3 & MSR_AMD64_IBSOPDATA3_PADDR_VALID)
		rdmsrl(MSR_AMD64_IBSDCPHYSAD, paddr);
	else {
		count_vm_event(HWHINT_PADDR_INVALID);
		goto handled;
	}

	pfn = PHYS_PFN(paddr);
	page = pfn_to_online_page(pfn);
	if (!page)
		goto handled;

	if (!PageLRU(page)) {
		count_vm_event(HWHINT_NON_LRU);
		goto handled;
	}

	if (!ibs_push_sample(pfn, numa_node_id(), jiffies)) {
		count_vm_event(HWHINT_BUFFER_FULL);
		goto handled;
	}

	irq_work_queue(&ibs_irq_work);
	count_vm_event(HWHINT_USEFUL_SAMPLES);

handled:
	return NMI_HANDLED;
}

static inline int get_ibs_lvt_offset(void)
{
	u64 val;

	rdmsrl(MSR_AMD64_IBSCTL, val);
	if (!(val & IBSCTL_LVT_OFFSET_VALID))
		return -EINVAL;

	return val & IBSCTL_LVT_OFFSET_MASK;
}

static void setup_APIC_ibs(void)
{
	int offset;

	offset = get_ibs_lvt_offset();
	if (offset < 0)
		goto failed;

	if (!setup_APIC_eilvt(offset, 0, APIC_EILVT_MSG_NMI, 0))
		return;
failed:
	pr_warn("IBS APIC setup failed on cpu #%d\n",
		smp_processor_id());
}

static void clear_APIC_ibs(void)
{
	int offset;

	offset = get_ibs_lvt_offset();
	if (offset >= 0)
		setup_APIC_eilvt(offset, 0, APIC_EILVT_MSG_FIX, 1);
}

static int x86_amd_ibs_access_profile_startup(unsigned int cpu)
{
	setup_APIC_ibs();
	return 0;
}

static int x86_amd_ibs_access_profile_teardown(unsigned int cpu)
{
	clear_APIC_ibs();
	return 0;
}

static int __init ibs_access_profiling_init(void)
{
	if (!boot_cpu_has(X86_FEATURE_IBS)) {
		pr_info("IBS capability is unavailable for access profiling\n");
		return 0;
	}

	ibs_s = alloc_percpu_gfp(struct ibs_sample_pcpu, __GFP_ZERO);
	if (!ibs_s)
		return 0;

	INIT_WORK(&ibs_work, ibs_work_handler);
	init_irq_work(&ibs_irq_work, ibs_irq_handler);

	/* Uses IBS Op sampling */
	ibs_config = IBS_OP_CNT_CTL | IBS_OP_ENABLE;
	ibs_caps = cpuid_eax(IBS_CPUID_FEATURES);
	if (ibs_caps & IBS_CAPS_ZEN4)
		ibs_config |= IBS_OP_L3MISSONLY;

	register_nmi_handler(NMI_LOCAL, ibs_overflow_handler, 0, "ibs");

	cpuhp_setup_state(CPUHP_AP_PERF_X86_AMD_IBS_STARTING,
			  "x86/amd/ibs_access_profile:starting",
			  x86_amd_ibs_access_profile_startup,
			  x86_amd_ibs_access_profile_teardown);

	arch_hw_access_profiling = true;
	pr_info("IBS setup for memory access profiling\n");
	return 0;
}

arch_initcall(ibs_access_profiling_init);
