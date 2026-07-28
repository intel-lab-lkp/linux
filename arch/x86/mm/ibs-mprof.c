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
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/smp.h>
#include <linux/uaccess.h>

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
 * Runtime-configurable profiler parameters, exposed via debugfs. The
 * interrupt handler consumes the published snapshot on every re-arm, so
 * configuration changes need no locking on the fast path. Writers are
 * serialized by mprof_cfg_lock and publish an immutable snapshot; the hot
 * path only does an smp_load_acquire() of the pointer.
 */
struct mprof_config {
	bool enabled;		/* profiling enabled (IbsMemDis inverted) */
	bool l3miss_only;	/* IbsMemL3MissOnly */
	bool lat_filter;	/* IbsMemLatFltEn */
	u8   lat_thresh;	/* IbsMemLatThrsh, 0x0 .. 0xf */
	u32  period;		/* op sample period (IbsMemMaxCnt) */
	/* Precomputed register values for the interrupt fast path. */
	u64  ctl;
	u64  ctl2;
};

static struct mprof_config mprof_cfg_slots[2];
static struct mprof_config *mprof_cfg;
static DEFINE_MUTEX(mprof_cfg_lock);

/*
 * ibs-memprof: kernel cmdline parameter to arm IBS Memory
 * Profiler right from boot time.
 */
bool ibs_mprof_enabled __read_mostly;

static int __init setup_ibs_mprof(char *str)
{
	return (kstrtobool(str, &ibs_mprof_enabled) == 0);
}

__setup("ibs-memprof=", setup_ibs_mprof);

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
 * Translate the software config into IbsMemCtl / IbsMemCtl2 register values.
 * Kernel RIP is always excluded: the profiler only samples user memory
 * accesses.
 */
static void mprof_compose(struct mprof_config *cfg)
{
	u64 ctl;

	if (!cfg->enabled) {
		ctl = 0;
		cfg->ctl = ctl;
		cfg->ctl2 = IBS_MPROF_CTL2_DISABLE;
		return;
	}

	/*
	 * Assemble bits 26:20 and 19:4 of the periodic op counter in ctl.
	 * The lower 4 bits are always 0000b.
	 */
	ctl = (cfg->period >> 4) & IBS_MPROF_CTL_MAXCNT_MASK;
	ctl |= cfg->period & IBS_MPROF_CTL_MAXCNT_EXT_MASK;
	ctl |= IBS_MPROF_CTL_CNT_CTL | IBS_MPROF_CTL_ENABLE;

	if (cfg->l3miss_only)
		ctl |= IBS_MPROF_CTL_L3MISSONLY;

	if (cfg->lat_filter) {
		ctl |= IBS_MPROF_CTL_LATFLTEN;
		ctl |= ((u64)cfg->lat_thresh << IBS_MPROF_CTL_LATTHRSH_SHIFT) &
			IBS_MPROF_CTL_LATTHRSH_MASK;
	}

	cfg->ctl = ctl;
	/* Exclude samples that have bit 63 of their RIP set (kernel). */
	cfg->ctl2 = IBS_MPROF_CTL2_EXCLUDE_KERNEL;
}

/*
 * Program the profiler from the currently published config snapshot. Called
 * at CPU startup, from the interrupt handler on every re-arm, and via
 * on_each_cpu() when the config changes.
 */
static void mprof_enable_profiling(void)
{
	/* Acquire the snapshot; pairs with smp_store_release() in the writers. */
	struct mprof_config *cfg = smp_load_acquire(&mprof_cfg);

	wrmsrq(MSR_AMD64_IBS_MPROF_CTL, cfg->ctl);
	wrmsrq(MSR_AMD64_IBS_MPROF_CTL2, cfg->ctl2);
}

static void mprof_disable_profiling(u64 mem_ctl)
{
	mem_ctl &= ~IBS_MPROF_CTL_ENABLE;
	mem_ctl &= ~IBS_MPROF_CTL_VAL;
	wrmsrq(MSR_AMD64_IBS_MPROF_CTL, mem_ctl);

	wrmsrq(MSR_AMD64_IBS_MPROF_CTL2, IBS_MPROF_CTL2_DISABLE);
}

static void mprof_reprogram_this_cpu(void *info)
{
	mprof_enable_profiling();
}

/*
 * Publish a new config snapshot and push it to every online CPU
 * immediately. Must be called with mprof_cfg_lock held.
 */
static void mprof_publish(const struct mprof_config *newcfg)
{
	struct mprof_config *slot;

	lockdep_assert_held(&mprof_cfg_lock);

	/* Fill the slot that is not currently published, then flip to it. */
	slot = (mprof_cfg == &mprof_cfg_slots[0]) ?
		&mprof_cfg_slots[1] : &mprof_cfg_slots[0];
	*slot = *newcfg;
	mprof_compose(slot);
	/* Publish the fully composed slot; pairs with smp_load_acquire() in readers. */
	smp_store_release(&mprof_cfg, slot);

	/*
	 * on_each_cpu() with wait serializes against any in-flight interrupt
	 * handler on each CPU, so the previously published slot has no readers
	 * once this returns and can be safely reused by the next writer.
	 */
	on_each_cpu(mprof_reprogram_this_cpu, NULL, 1);
}

static void mprof_config_init(void)
{
	struct mprof_config *cfg = &mprof_cfg_slots[0];

	if (ibs_mprof_enabled)
		cfg->enabled = true;
	cfg->l3miss_only = true;
	cfg->lat_filter = false;
	cfg->lat_thresh = 0;
	cfg->period = IBS_MPROF_SAMPLE_PERIOD;
	mprof_compose(cfg);
	/* Publish the initial snapshot; pairs with smp_load_acquire() in readers. */
	smp_store_release(&mprof_cfg, cfg);
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

	/*
	 * If L3 miss filtering is turned off, non load/store
	 * samples may get reported. Ignore them.
	 */
	if (!(mem_data3 & (IBS_MPROF_DATA3_LDOP | IBS_MPROF_DATA3_STOP)))
		goto handled;

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

/*
 * debugfs interface. Each parameter is a separate file under
 * <debugfs>/ibs-mprof/. A write validates the value, updates the current
 * config and actively propagates it to all CPUs via mprof_publish().
 */
enum mprof_field {
	MPROF_L3MISS_ONLY,
	MPROF_LAT_FILTER,
	MPROF_LAT_THRESH,
	MPROF_PERIOD,
};

static int mprof_parse_uint(const char __user *ubuf, size_t cnt, unsigned int *val)
{
	char buf[16];

	if (cnt > sizeof(buf) - 1)
		cnt = sizeof(buf) - 1;
	if (copy_from_user(buf, ubuf, cnt))
		return -EFAULT;
	buf[cnt] = '\0';
	if (kstrtouint(buf, 0, val))
		return -EINVAL;
	return 0;
}

static void mprof_store(enum mprof_field field, unsigned int val)
{
	struct mprof_config new;

	mutex_lock(&mprof_cfg_lock);
	new = *mprof_cfg;
	switch (field) {
	case MPROF_L3MISS_ONLY:
		new.l3miss_only = val;
		break;
	case MPROF_LAT_FILTER:
		new.lat_filter = val;
		break;
	case MPROF_LAT_THRESH:
		new.lat_thresh = val;
		break;
	case MPROF_PERIOD:
		new.period = val;
		break;
	}
	mprof_publish(&new);
	mutex_unlock(&mprof_cfg_lock);
}

#define MPROF_SHOW(name, field)						\
static int mprof_##name##_show(struct seq_file *m, void *v)		\
{									\
	/* Acquire the snapshot; pairs with smp_store_release() in writers. */	\
	struct mprof_config *cfg = smp_load_acquire(&mprof_cfg);	\
									\
	seq_printf(m, "%u\n", cfg->field);				\
	return 0;							\
}									\
static int mprof_##name##_open(struct inode *inode, struct file *filp)	\
{									\
	return single_open(filp, mprof_##name##_show, NULL);		\
}

MPROF_SHOW(l3miss_only, l3miss_only)
MPROF_SHOW(lat_filter, lat_filter)
MPROF_SHOW(lat_thresh, lat_thresh)
MPROF_SHOW(period, period)

static ssize_t mprof_l3miss_only_write(struct file *filp, const char __user *ubuf,
				       size_t cnt, loff_t *ppos)
{
	unsigned int val;
	int ret;

	ret = mprof_parse_uint(ubuf, cnt, &val);
	if (ret)
		return ret;
	if (val > 1)
		return -EINVAL;

	mprof_store(MPROF_L3MISS_ONLY, val);
	*ppos += cnt;
	return cnt;
}

static ssize_t mprof_lat_filter_write(struct file *filp, const char __user *ubuf,
				      size_t cnt, loff_t *ppos)
{
	unsigned int val;
	int ret;

	ret = mprof_parse_uint(ubuf, cnt, &val);
	if (ret)
		return ret;
	if (val > 1)
		return -EINVAL;

	mprof_store(MPROF_LAT_FILTER, val);
	*ppos += cnt;
	return cnt;
}

static ssize_t mprof_lat_thresh_write(struct file *filp, const char __user *ubuf,
				      size_t cnt, loff_t *ppos)
{
	unsigned int val;
	int ret;

	ret = mprof_parse_uint(ubuf, cnt, &val);
	if (ret)
		return ret;
	if (val > IBS_MPROF_LATTHRSH_MAX)
		return -EINVAL;

	mprof_store(MPROF_LAT_THRESH, val);
	*ppos += cnt;
	return cnt;
}

static ssize_t mprof_period_write(struct file *filp, const char __user *ubuf,
				  size_t cnt, loff_t *ppos)
{
	unsigned int val;
	int ret;

	ret = mprof_parse_uint(ubuf, cnt, &val);
	if (ret)
		return ret;
	if (val < IBS_MPROF_MAXCNT_MIN || val > IBS_MPROF_MAXCNT_MAX)
		return -EINVAL;

	mprof_store(MPROF_PERIOD, val);
	*ppos += cnt;
	return cnt;
}

#define MPROF_FOPS(name)						\
static const struct file_operations mprof_##name##_fops = {		\
	.open		= mprof_##name##_open,				\
	.read		= seq_read,					\
	.write		= mprof_##name##_write,				\
	.llseek		= seq_lseek,					\
	.release	= single_release,				\
}

MPROF_FOPS(l3miss_only);
MPROF_FOPS(lat_filter);
MPROF_FOPS(lat_thresh);
MPROF_FOPS(period);

static void mprof_debugfs_init(void)
{
	struct dentry *dir = debugfs_create_dir("ibs-mprof", NULL);

	debugfs_create_file("l3miss-only", 0644, dir, NULL, &mprof_l3miss_only_fops);
	debugfs_create_file("lat-filter", 0644, dir, NULL, &mprof_lat_filter_fops);
	debugfs_create_file("lat-thresh", 0644, dir, NULL, &mprof_lat_thresh_fops);
	debugfs_create_file("period", 0644, dir, NULL, &mprof_period_fops);
}

static ssize_t enabled_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	/* Acquire the snapshot; pairs with smp_store_release() in the writers. */
	struct mprof_config *cfg = smp_load_acquire(&mprof_cfg);

	return sysfs_emit(buf, "%s\n", str_enabled_disabled(cfg->enabled));
}

static ssize_t enabled_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct mprof_config new;
	bool enabled;
	int ret;

	ret = kstrtobool(buf, &enabled);
	if (ret)
		return ret;

	guard(mutex)(&mprof_cfg_lock);
	new = *mprof_cfg;
	new.enabled = enabled;
	mprof_publish(&new);

	return count;
}

static DEVICE_ATTR_RW(enabled);

static struct attribute *ibs_mprof_attributes[] = {
	&dev_attr_enabled.attr,
	NULL
};

static const struct attribute_group ibs_mprof_attr_group = {
	.name = "ibs-mprof",
	.attrs = ibs_mprof_attributes,
};

static void mprof_tunables_init(void)
{
	struct device *dev_root;
	int ret;

	dev_root = bus_get_dev_root(&cpu_subsys);
	if (dev_root) {
		ret = sysfs_create_group(&dev_root->kobj, &ibs_mprof_attr_group);
		put_device(dev_root);
		if (ret)
			return;
	}

	mprof_debugfs_init();
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

	/* Publish the default config before the startup callback consumes it. */
	mprof_config_init();

	ret = cpuhp_setup_state(CPUHP_AP_MM_AMD_IBS_MEMPROF_STARTING,
				"x86/amd/ibs_mprof:starting",
				x86_amd_ibs_mprof_startup,
				x86_amd_ibs_mprof_teardown);

	if (ret) {
		free_percpu(mprof_s);
		pr_err("cpuhp_setup_state failed: %d\n", ret);
	} else {
		pr_info("IBS Memory Profiler is available for memory access profiling\n");
		mprof_tunables_init();
	}
	return 0;
}

device_initcall(mprof_access_profiling_init);
