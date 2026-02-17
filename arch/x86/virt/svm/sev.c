// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD SVM-SEV Host Support.
 *
 * Copyright (C) 2023 Advanced Micro Devices, Inc.
 *
 * Author: Ashish Kalra <ashish.kalra@amd.com>
 *
 */

#include <linux/cc_platform.h>
#include <linux/printk.h>
#include <linux/mm_types.h>
#include <linux/set_memory.h>
#include <linux/memblock.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/cpumask.h>
#include <linux/iommu.h>
#include <linux/amd-iommu.h>
#include <linux/nospec.h>
#include <linux/kthread.h>
#include <linux/configfs.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include <asm/sev.h>
#include <asm/processor.h>
#include <asm/setup.h>
#include <asm/svm.h>
#include <asm/smp.h>
#include <asm/cpu.h>
#include <asm/apic.h>
#include <asm/cpuid/api.h>
#include <asm/cmdline.h>
#include <asm/iommu.h>
#include <asm/msr.h>

/*
 * The RMP entry information as returned by the RMPREAD instruction.
 */
struct rmpentry {
	u64 gpa;
	u8  assigned		:1,
	    rsvd1		:7;
	u8  pagesize		:1,
	    hpage_region_status	:1,
	    rsvd2		:6;
	u8  immutable		:1,
	    rsvd3		:7;
	u8  rsvd4;
	u32 asid;
} __packed;

/*
 * The raw RMP entry format is not architectural. The format is defined in PPR
 * Family 19h Model 01h, Rev B1 processor. This format represents the actual
 * entry in the RMP table memory. The bitfield definitions are used for machines
 * without the RMPREAD instruction (Zen3 and Zen4), otherwise the "hi" and "lo"
 * fields are only used for dumping the raw data.
 */
struct rmpentry_raw {
	union {
		struct {
			u64 assigned	: 1,
			    pagesize	: 1,
			    immutable	: 1,
			    rsvd1	: 9,
			    gpa		: 39,
			    asid	: 10,
			    vmsa	: 1,
			    validated	: 1,
			    rsvd2	: 1;
		};
		u64 lo;
	};
	u64 hi;
} __packed;

/*
 * The first 16KB from the RMP_BASE is used by the processor for the
 * bookkeeping, the range needs to be added during the RMP entry lookup.
 */
#define RMPTABLE_CPU_BOOKKEEPING_SZ	0x4000

/*
 * For a non-segmented RMP table, use the maximum physical addressing as the
 * segment size in order to always arrive at index 0 in the table.
 */
#define RMPTABLE_NON_SEGMENTED_SHIFT	52

struct rmp_segment_desc {
	struct rmpentry_raw *rmp_entry;
	u64 max_index;
	u64 size;
};

/*
 * Segmented RMP Table support.
 *   - The segment size is used for two purposes:
 *     - Identify the amount of memory covered by an RMP segment
 *     - Quickly locate an RMP segment table entry for a physical address
 *
 *   - The RMP segment table contains pointers to an RMP table that covers
 *     a specific portion of memory. There can be up to 512 8-byte entries,
 *     one pages worth.
 */
#define RST_ENTRY_MAPPED_SIZE(x)	((x) & GENMASK_ULL(19, 0))
#define RST_ENTRY_SEGMENT_BASE(x)	((x) & GENMASK_ULL(51, 20))

#define RST_SIZE SZ_4K
static struct rmp_segment_desc **rmp_segment_table __ro_after_init;
static unsigned int rst_max_index __ro_after_init = 512;

static unsigned int rmp_segment_shift;
static u64 rmp_segment_size;
static u64 rmp_segment_mask;

#define RST_ENTRY_INDEX(x)	((x) >> rmp_segment_shift)
#define RMP_ENTRY_INDEX(x)	((u64)(PHYS_PFN((x) & rmp_segment_mask)))

static u64 rmp_cfg;

/* Mask to apply to a PFN to get the first PFN of a 2MB page */
#define PFN_PMD_MASK	GENMASK_ULL(63, PMD_SHIFT - PAGE_SHIFT)

static u64 probed_rmp_base, probed_rmp_size;

static LIST_HEAD(snp_leaked_pages_list);
static DEFINE_SPINLOCK(snp_leaked_pages_list_lock);

static unsigned long snp_nr_leaked_pages;

enum rmpopt_function {
	RMPOPT_FUNC_VERIFY_AND_REPORT_STATUS,
	RMPOPT_FUNC_REPORT_STATUS
};

#define RMPOPT_TABLE_MAX_LIMIT_IN_TB	2
#define NUM_TB(pfn_min, pfn_max)	\
	(((pfn_max) - (pfn_min)) / (1 << (40 - PAGE_SHIFT)))

static struct task_struct *rmpopt_task;

struct rmpopt_socket_config {
	unsigned long start_pfn, end_pfn;
	cpumask_var_t cpulist;
	int *node_id;
	int current_node_idx;
};

#define RMPOPT_CONFIGFS_NAME	"rmpopt"

static atomic_t rmpopt_in_progress = ATOMIC_INIT(0);

static cpumask_t rmpopt_cpumask;
static struct dentry *rmpopt_debugfs;

struct seq_paddr {
	phys_addr_t next_seq_paddr;
};

#undef pr_fmt
#define pr_fmt(fmt)	"SEV-SNP: " fmt

static int __mfd_enable(unsigned int cpu)
{
	u64 val;

	if (!cc_platform_has(CC_ATTR_HOST_SEV_SNP))
		return 0;

	rdmsrq(MSR_AMD64_SYSCFG, val);

	val |= MSR_AMD64_SYSCFG_MFDM;

	wrmsrq(MSR_AMD64_SYSCFG, val);

	return 0;
}

static __init void mfd_enable(void *arg)
{
	__mfd_enable(smp_processor_id());
}

static int __snp_enable(unsigned int cpu)
{
	u64 val;

	if (!cc_platform_has(CC_ATTR_HOST_SEV_SNP))
		return 0;

	rdmsrq(MSR_AMD64_SYSCFG, val);

	val |= MSR_AMD64_SYSCFG_SNP_EN;
	val |= MSR_AMD64_SYSCFG_SNP_VMPL_EN;

	wrmsrq(MSR_AMD64_SYSCFG, val);

	return 0;
}

static __init void snp_enable(void *arg)
{
	__snp_enable(smp_processor_id());
}

static void __init __snp_fixup_e820_tables(u64 pa)
{
	if (IS_ALIGNED(pa, PMD_SIZE))
		return;

	/*
	 * Handle cases where the RMP table placement by the BIOS is not
	 * 2M aligned and the kexec kernel could try to allocate
	 * from within that chunk which then causes a fatal RMP fault.
	 *
	 * The e820_table needs to be updated as it is converted to
	 * kernel memory resources and used by KEXEC_FILE_LOAD syscall
	 * to load kexec segments.
	 *
	 * The e820_table_firmware needs to be updated as it is exposed
	 * to sysfs and used by the KEXEC_LOAD syscall to load kexec
	 * segments.
	 *
	 * The e820_table_kexec needs to be updated as it passed to
	 * the kexec-ed kernel.
	 */
	pa = ALIGN_DOWN(pa, PMD_SIZE);
	if (e820__mapped_any(pa, pa + PMD_SIZE, E820_TYPE_RAM)) {
		pr_info("Reserving start/end of RMP table on a 2MB boundary [0x%016llx]\n", pa);
		e820__range_update(pa, PMD_SIZE, E820_TYPE_RAM, E820_TYPE_RESERVED);
		e820__range_update_table(e820_table_kexec, pa, PMD_SIZE, E820_TYPE_RAM, E820_TYPE_RESERVED);
		if (!memblock_is_region_reserved(pa, PMD_SIZE))
			memblock_reserve(pa, PMD_SIZE);
	}
}

static void __init fixup_e820_tables_for_segmented_rmp(void)
{
	u64 pa, *rst, size, mapped_size;
	unsigned int i;

	__snp_fixup_e820_tables(probed_rmp_base);

	pa = probed_rmp_base + RMPTABLE_CPU_BOOKKEEPING_SZ;

	__snp_fixup_e820_tables(pa + RST_SIZE);

	rst = early_memremap(pa, RST_SIZE);
	if (!rst)
		return;

	for (i = 0; i < rst_max_index; i++) {
		pa = RST_ENTRY_SEGMENT_BASE(rst[i]);
		mapped_size = RST_ENTRY_MAPPED_SIZE(rst[i]);
		if (!mapped_size)
			continue;

		__snp_fixup_e820_tables(pa);

		/*
		 * Mapped size in GB. Mapped size is allowed to exceed
		 * the segment coverage size, but gets reduced to the
		 * segment coverage size.
		 */
		mapped_size <<= 30;
		if (mapped_size > rmp_segment_size)
			mapped_size = rmp_segment_size;

		/* Calculate the RMP segment size (16 bytes/page mapped) */
		size = PHYS_PFN(mapped_size) << 4;

		__snp_fixup_e820_tables(pa + size);
	}

	early_memunmap(rst, RST_SIZE);
}

static void __init fixup_e820_tables_for_contiguous_rmp(void)
{
	__snp_fixup_e820_tables(probed_rmp_base);
	__snp_fixup_e820_tables(probed_rmp_base + probed_rmp_size);
}

void __init snp_fixup_e820_tables(void)
{
	if (rmp_cfg & MSR_AMD64_SEG_RMP_ENABLED) {
		fixup_e820_tables_for_segmented_rmp();
	} else {
		fixup_e820_tables_for_contiguous_rmp();
	}
}

static bool __init clear_rmptable_bookkeeping(void)
{
	void *bk;

	bk = memremap(probed_rmp_base, RMPTABLE_CPU_BOOKKEEPING_SZ, MEMREMAP_WB);
	if (!bk) {
		pr_err("Failed to map RMP bookkeeping area\n");
		return false;
	}

	memset(bk, 0, RMPTABLE_CPU_BOOKKEEPING_SZ);

	memunmap(bk);

	return true;
}

static bool __init alloc_rmp_segment_desc(u64 segment_pa, u64 segment_size, u64 pa)
{
	u64 rst_index, rmp_segment_size_max;
	struct rmp_segment_desc *desc;
	void *rmp_segment;

	/* Calculate the maximum size an RMP can be (16 bytes/page mapped) */
	rmp_segment_size_max = PHYS_PFN(rmp_segment_size) << 4;

	/* Validate the RMP segment size */
	if (segment_size > rmp_segment_size_max) {
		pr_err("Invalid RMP size 0x%llx for configured segment size 0x%llx\n",
		       segment_size, rmp_segment_size_max);
		return false;
	}

	/* Validate the RMP segment table index */
	rst_index = RST_ENTRY_INDEX(pa);
	if (rst_index >= rst_max_index) {
		pr_err("Invalid RMP segment base address 0x%llx for configured segment size 0x%llx\n",
		       pa, rmp_segment_size);
		return false;
	}

	if (rmp_segment_table[rst_index]) {
		pr_err("RMP segment descriptor already exists at index %llu\n", rst_index);
		return false;
	}

	rmp_segment = memremap(segment_pa, segment_size, MEMREMAP_WB);
	if (!rmp_segment) {
		pr_err("Failed to map RMP segment addr 0x%llx size 0x%llx\n",
		       segment_pa, segment_size);
		return false;
	}

	desc = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (!desc) {
		memunmap(rmp_segment);
		return false;
	}

	desc->rmp_entry = rmp_segment;
	desc->max_index = segment_size / sizeof(*desc->rmp_entry);
	desc->size = segment_size;

	rmp_segment_table[rst_index] = desc;

	return true;
}

static void __init free_rmp_segment_table(void)
{
	unsigned int i;

	for (i = 0; i < rst_max_index; i++) {
		struct rmp_segment_desc *desc;

		desc = rmp_segment_table[i];
		if (!desc)
			continue;

		memunmap(desc->rmp_entry);

		kfree(desc);
	}

	free_page((unsigned long)rmp_segment_table);

	rmp_segment_table = NULL;
}

/* Allocate the table used to index into the RMP segments */
static bool __init alloc_rmp_segment_table(void)
{
	struct page *page;

	page = alloc_page(__GFP_ZERO);
	if (!page)
		return false;

	rmp_segment_table = page_address(page);

	return true;
}

static bool __init setup_contiguous_rmptable(void)
{
	u64 max_rmp_pfn, calc_rmp_sz, rmptable_segment, rmptable_size, rmp_end;

	if (!probed_rmp_size)
		return false;

	rmp_end = probed_rmp_base + probed_rmp_size - 1;

	/*
	 * Calculate the amount of memory that must be reserved by the BIOS to
	 * address the whole RAM, including the bookkeeping area. The RMP itself
	 * must also be covered.
	 */
	max_rmp_pfn = max_pfn;
	if (PFN_UP(rmp_end) > max_pfn)
		max_rmp_pfn = PFN_UP(rmp_end);

	calc_rmp_sz = (max_rmp_pfn << 4) + RMPTABLE_CPU_BOOKKEEPING_SZ;
	if (calc_rmp_sz > probed_rmp_size) {
		pr_err("Memory reserved for the RMP table does not cover full system RAM (expected 0x%llx got 0x%llx)\n",
		       calc_rmp_sz, probed_rmp_size);
		return false;
	}

	if (!alloc_rmp_segment_table())
		return false;

	/* Map only the RMP entries */
	rmptable_segment = probed_rmp_base + RMPTABLE_CPU_BOOKKEEPING_SZ;
	rmptable_size    = probed_rmp_size - RMPTABLE_CPU_BOOKKEEPING_SZ;

	if (!alloc_rmp_segment_desc(rmptable_segment, rmptable_size, 0)) {
		free_rmp_segment_table();
		return false;
	}

	return true;
}

static bool __init setup_segmented_rmptable(void)
{
	u64 rst_pa, *rst, pa, ram_pa_end, ram_pa_max;
	unsigned int i, max_index;

	if (!probed_rmp_base)
		return false;

	if (!alloc_rmp_segment_table())
		return false;

	rst_pa = probed_rmp_base + RMPTABLE_CPU_BOOKKEEPING_SZ;
	rst = memremap(rst_pa, RST_SIZE, MEMREMAP_WB);
	if (!rst) {
		pr_err("Failed to map RMP segment table addr 0x%llx\n", rst_pa);
		goto e_free;
	}

	pr_info("Segmented RMP using %lluGB segments\n", rmp_segment_size >> 30);

	ram_pa_max = max_pfn << PAGE_SHIFT;

	max_index = 0;
	ram_pa_end = 0;
	for (i = 0; i < rst_max_index; i++) {
		u64 rmp_segment, rmp_size, mapped_size;

		mapped_size = RST_ENTRY_MAPPED_SIZE(rst[i]);
		if (!mapped_size)
			continue;

		max_index = i;

		/*
		 * Mapped size in GB. Mapped size is allowed to exceed the
		 * segment coverage size, but gets reduced to the segment
		 * coverage size.
		 */
		mapped_size <<= 30;
		if (mapped_size > rmp_segment_size) {
			pr_info("RMP segment %u mapped size (0x%llx) reduced to 0x%llx\n",
				i, mapped_size, rmp_segment_size);
			mapped_size = rmp_segment_size;
		}

		rmp_segment = RST_ENTRY_SEGMENT_BASE(rst[i]);

		/* Calculate the RMP segment size (16 bytes/page mapped) */
		rmp_size = PHYS_PFN(mapped_size) << 4;

		pa = (u64)i << rmp_segment_shift;

		/*
		 * Some segments may be for MMIO mapped above system RAM. These
		 * segments are used for Trusted I/O.
		 */
		if (pa < ram_pa_max)
			ram_pa_end = pa + mapped_size;

		if (!alloc_rmp_segment_desc(rmp_segment, rmp_size, pa))
			goto e_unmap;

		pr_info("RMP segment %u physical address [0x%llx - 0x%llx] covering [0x%llx - 0x%llx]\n",
			i, rmp_segment, rmp_segment + rmp_size - 1, pa, pa + mapped_size - 1);
	}

	if (ram_pa_max > ram_pa_end) {
		pr_err("Segmented RMP does not cover full system RAM (expected 0x%llx got 0x%llx)\n",
		       ram_pa_max, ram_pa_end);
		goto e_unmap;
	}

	/* Adjust the maximum index based on the found segments */
	rst_max_index = max_index + 1;

	memunmap(rst);

	return true;

e_unmap:
	memunmap(rst);

e_free:
	free_rmp_segment_table();

	return false;
}

static bool __init setup_rmptable(void)
{
	if (rmp_cfg & MSR_AMD64_SEG_RMP_ENABLED) {
		return setup_segmented_rmptable();
	} else {
		return setup_contiguous_rmptable();
	}
}

/*
 * Build a cpumask of online primary threads, accounting for primary threads
 * that have been offlined while their secondary threads are still online.
 */
static void get_cpumask_of_primary_threads(cpumask_var_t cpulist)
{
	cpumask_t cpus;
	int cpu;

	cpumask_copy(&cpus, cpu_online_mask);
	for_each_cpu(cpu, &cpus) {
		cpumask_set_cpu(cpu, cpulist);
		cpumask_andnot(&cpus, &cpus, cpu_smt_mask(cpu));
	}
}

/*
 * 'val' is a system physical address aligned to 1GB OR'ed with
 * a function selection. Currently supported functions are 0
 * (verify and report status) and 1 (report status).
 */
static void rmpopt(void *val)
{
	bool optimized;

	asm volatile(".byte 0xf2, 0x0f, 0x01, 0xfc\n\t"
		     : "=@ccc" (optimized)
		     : "a" ((u64)val & PUD_MASK), "c" ((u64)val & 0x1)
		     : "memory", "cc");

	assign_cpu(smp_processor_id(), &rmpopt_cpumask, optimized);
}

static int rmpopt_kthread(void *__unused)
{
	phys_addr_t pa_start, pa_end;
	cpumask_var_t cpus;

	if (!zalloc_cpumask_var(&cpus, GFP_KERNEL))
		return -ENOMEM;

	pa_start = ALIGN_DOWN(PFN_PHYS(min_low_pfn), PUD_SIZE);
	pa_end = ALIGN(PFN_PHYS(max_pfn), PUD_SIZE);

	while (!kthread_should_stop()) {
		phys_addr_t pa;

		pr_info("RMP optimizations enabled on physical address range @1GB alignment [0x%016llx - 0x%016llx]\n",
			pa_start, pa_end);

		/* Only one thread per core needs to issue RMPOPT instruction */
		get_cpumask_of_primary_threads(cpus);

		/*
		 * RMPOPT optimizations skip RMP checks at 1GB granularity if this range of
		 * memory does not contain any SNP guest memory.
		 */
		for (pa = pa_start; pa < pa_end; pa += PUD_SIZE) {
			/* Bit zero passes the function to the RMPOPT instruction. */
			on_each_cpu_mask(cpus, rmpopt,
					 (void *)(pa | RMPOPT_FUNC_VERIFY_AND_REPORT_STATUS),
					 true);

			 /* Give a chance for other threads to run */
			cond_resched();
		}

		/* Clear in_progress flag before going to sleep */
		atomic_set(&rmpopt_in_progress, 0);

		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	}

	free_cpumask_var(cpus);
	return 0;
}

static void rmpopt_all_physmem(void)
{
	if (rmpopt_task)
		wake_up_process(rmpopt_task);
}

static ssize_t rmpopt_action_show(struct config_item *item, char *page)
{
	return sprintf(page, "RMP optimization in progress: %s\n",
		       atomic_read(&rmpopt_in_progress) == 1 ? "Yes" : "No");
}

static ssize_t rmpopt_action_store(struct config_item *item,
				   const char *page, size_t count)
{
	int in_progress_flag, ret;
	unsigned int action;

	ret = kstrtouint(page, 10, &action);
	if (ret)
		return ret;

	if (action == 1) {
		/* perform RMP re-optimizations */
		in_progress_flag = atomic_cmpxchg(&rmpopt_in_progress, 0, 1);
		if (!in_progress_flag)
			rmpopt_all_physmem();
	} else {
		return -EINVAL;
	}

	return count;
}

static ssize_t rmpopt_description_show(struct config_item *item, char *page)
{
	return sprintf(page, "[RMPOPT]\n\necho 1 > action to perform RMP optimization.\n");
}

CONFIGFS_ATTR(rmpopt_, action);
CONFIGFS_ATTR_RO(rmpopt_, description);

static struct configfs_attribute *rmpopt_attrs[] = {
	&rmpopt_attr_action,
	&rmpopt_attr_description,
	NULL,
};

static const struct config_item_type rmpopt_config_type = {
	.ct_attrs       = rmpopt_attrs,
	.ct_owner       = THIS_MODULE,
};

static struct configfs_subsystem rmpopt_configfs = {
	.su_group = {
		.cg_item = {
		.ci_namebuf = RMPOPT_CONFIGFS_NAME,
		.ci_type = &rmpopt_config_type,
		},
	},
	.su_mutex = __MUTEX_INITIALIZER(rmpopt_configfs.su_mutex),
};

static int rmpopt_configfs_setup(void)
{
	int ret;

	config_group_init(&rmpopt_configfs.su_group);
	ret = configfs_register_subsystem(&rmpopt_configfs);
	if (ret)
		pr_err("Error %d while registering subsystem %s\n", ret, RMPOPT_CONFIGFS_NAME);

	return ret;
}

/*
 * start() can be called multiple times if allocated buffer has overflowed
 * and bigger buffer is allocated.
 */
static void *rmpopt_table_seq_start(struct seq_file *seq, loff_t *pos)
{
	phys_addr_t end_paddr = ALIGN(PFN_PHYS(max_pfn), PUD_SIZE);
	struct seq_paddr *p = seq->private;

	if (*pos == 0) {
		p->next_seq_paddr = ALIGN_DOWN(PFN_PHYS(min_low_pfn), PUD_SIZE);
		return &p->next_seq_paddr;
	}

	if (p->next_seq_paddr == end_paddr)
		return NULL;

	return &p->next_seq_paddr;
}

static void *rmpopt_table_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	phys_addr_t end_paddr = ALIGN(PFN_PHYS(max_pfn), PUD_SIZE);
	phys_addr_t *curr_paddr = v;

	(*pos)++;
	if (*curr_paddr == end_paddr)
		return NULL;
	*curr_paddr += PUD_SIZE;

	return curr_paddr;
}

static void rmpopt_table_seq_stop(struct seq_file *seq, void *v)
{
}

static int rmpopt_table_seq_show(struct seq_file *seq, void *v)
{
	phys_addr_t *curr_paddr = v;

	seq_printf(seq, "Memory @%3lluGB: ", *curr_paddr >> PUD_SHIFT);

	cpumask_clear(&rmpopt_cpumask);
	on_each_cpu_mask(cpu_online_mask, rmpopt,
			 (void *)(*curr_paddr | RMPOPT_FUNC_REPORT_STATUS),
			 true);

	if (cpumask_empty(&rmpopt_cpumask))
		seq_puts(seq, "CPU(s): none\n");
	else
		seq_printf(seq, "CPU(s): %*pbl\n", cpumask_pr_args(&rmpopt_cpumask));

	return 0;
}

static const struct seq_operations rmpopt_table_seq_ops = {
	.start = rmpopt_table_seq_start,
	.next = rmpopt_table_seq_next,
	.stop = rmpopt_table_seq_stop,
	.show = rmpopt_table_seq_show
};

static int rmpopt_table_open(struct inode *inode, struct file *file)
{
	return seq_open_private(file, &rmpopt_table_seq_ops, sizeof(struct seq_paddr));
}

static const struct file_operations rmpopt_table_fops = {
	.open = rmpopt_table_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release_private,
};

static void rmpopt_debugfs_setup(void)
{
	rmpopt_debugfs = debugfs_create_dir("rmpopt", NULL);

	debugfs_create_file("rmpopt-table", 0444, rmpopt_debugfs,
			    NULL, &rmpopt_table_fops);
}

static void __configure_rmpopt(void *val)
{
	u64 rmpopt_base = ((u64)val & PUD_MASK) | MSR_AMD64_RMPOPT_ENABLE;

	wrmsrq(MSR_AMD64_RMPOPT_BASE, rmpopt_base);
}

static void configure_rmpopt_non_numa(cpumask_var_t primary_threads_cpulist)
{
	on_each_cpu_mask(primary_threads_cpulist, __configure_rmpopt, (void *)0, true);
}

static void free_rmpopt_socket_config(struct rmpopt_socket_config *socket)
{
	int i;

	if (!socket)
		return;

	for (i = 0; i < topology_max_packages(); i++) {
		free_cpumask_var(socket[i].cpulist);
		kfree(socket[i].node_id);
	}

	kfree(socket);
}
DEFINE_FREE(free_rmpopt_socket_config, struct rmpopt_socket_config *, free_rmpopt_socket_config(_T))

static void configure_rmpopt_large_physmem(cpumask_var_t primary_threads_cpulist)
{
	struct rmpopt_socket_config *socket __free(free_rmpopt_socket_config) = NULL;
	int max_packages = topology_max_packages();
	struct rmpopt_socket_config *sc;
	int cpu, i;

	socket = kcalloc(max_packages, sizeof(struct rmpopt_socket_config), GFP_KERNEL);
	if (!socket)
		return;

	for (i = 0; i < max_packages; i++) {
		sc = &socket[i];
		if (!zalloc_cpumask_var(&sc->cpulist, GFP_KERNEL))
			return;
		sc->node_id = kcalloc(nr_node_ids, sizeof(int), GFP_KERNEL);
		if (!sc->node_id)
			return;
		sc->current_node_idx = -1;
	}

	/*
	 * Handle case of virtualized NUMA software domains, such as AMD Nodes Per Socket(NPS)
	 * configurations. The kernel does not have an abstraction for physical sockets,
	 * therefore, enumerate the physical sockets and Nodes Per Socket(NPS) information by
	 * walking the online CPU list.
	 */
	for_each_cpu(cpu, primary_threads_cpulist) {
		int socket_id, nid;

		socket_id = topology_logical_package_id(cpu);
		nid = cpu_to_node(cpu);
		sc = &socket[socket_id];

		/*
		 * For each socket, determine the corresponding nodes and the socket's start
		 * and end PFNs.
		 * Record the node and the start and end PFNs of the first node found on the
		 * socket, then record each subsequent node and update the end PFN for that
		 * socket as additional nodes are found.
		 */
		if (sc->current_node_idx == -1) {
			sc->current_node_idx = 0;
			sc->node_id[sc->current_node_idx] = nid;
			sc->start_pfn = node_start_pfn(nid);
			sc->end_pfn = node_end_pfn(nid);
		} else if (sc->node_id[sc->current_node_idx] != nid) {
			sc->current_node_idx++;
			sc->node_id[sc->current_node_idx] = nid;
			sc->end_pfn = node_end_pfn(nid);
		}

		cpumask_set_cpu(cpu, sc->cpulist);
	}

	/*
	 * If the "physical" socket has up to 2TB of memory, the per-CPU RMPOPT tables are
	 * configured to the starting physical address of the socket, otherwise the tables
	 * are configured per-node.
	 */
	for (i = 0; i < max_packages; i++) {
		int num_tb_socket;
		phys_addr_t pa;
		int j;

		sc = &socket[i];
		num_tb_socket = NUM_TB(sc->start_pfn, sc->end_pfn) + 1;

		pr_debug("socket start_pfn 0x%lx, end_pfn 0x%lx, socket cpu mask %*pbl\n",
			 sc->start_pfn, sc->end_pfn, cpumask_pr_args(sc->cpulist));

		if (num_tb_socket <= RMPOPT_TABLE_MAX_LIMIT_IN_TB) {
			pa = PFN_PHYS(sc->start_pfn);
			on_each_cpu_mask(sc->cpulist, __configure_rmpopt, (void *)pa, true);
			continue;
		}

		for (j = 0; j <= sc->current_node_idx; j++) {
			int nid = sc->node_id[j];
			struct cpumask node_mask;

			cpumask_and(&node_mask, cpumask_of_node(nid), sc->cpulist);
			pa = PFN_PHYS(node_start_pfn(nid));

			pr_debug("RMPOPT_BASE MSR on nodeid %d cpu mask %*pbl set to 0x%llx\n",
				 nid, cpumask_pr_args(&node_mask), pa);
			on_each_cpu_mask(&node_mask, __configure_rmpopt, (void *)pa, true);
		}
	}
}

static __init void configure_and_enable_rmpopt(void)
{
	cpumask_var_t primary_threads_cpulist;
	int num_tb;

	if (!cpu_feature_enabled(X86_FEATURE_RMPOPT)) {
		pr_debug("RMPOPT not supported on this platform\n");
		return;
	}

	if (!cc_platform_has(CC_ATTR_HOST_SEV_SNP)) {
		pr_debug("RMPOPT optimizations not enabled as SNP support is not enabled\n");
		return;
	}

	if (!(rmp_cfg & MSR_AMD64_SEG_RMP_ENABLED)) {
		pr_info("RMPOPT optimizations not enabled, segmented RMP required\n");
		return;
	}

	if (!zalloc_cpumask_var(&primary_threads_cpulist, GFP_KERNEL))
		return;

	num_tb = NUM_TB(min_low_pfn, max_pfn) + 1;
	pr_debug("NUM_TB pages in system %d\n", num_tb);

	/* Only one thread per core needs to set RMPOPT_BASE MSR as it is per-core */
	get_cpumask_of_primary_threads(primary_threads_cpulist);

	/*
	 * Per-CPU RMPOPT tables support at most 2 TB of addressable memory for RMP optimizations.
	 *
	 * Fastpath RMPOPT configuration and setup:
	 * For systems with <= 2 TB of RAM, configure each per-core RMPOPT base to 0,
	 * ensuring all system RAM is RMP-optimized on all CPUs.
	 */
	if (num_tb <= RMPOPT_TABLE_MAX_LIMIT_IN_TB)
		configure_rmpopt_non_numa(primary_threads_cpulist);
	else
		configure_rmpopt_large_physmem(primary_threads_cpulist);

	rmpopt_task = kthread_create(rmpopt_kthread, NULL, "rmpopt_kthread");
	if (IS_ERR(rmpopt_task)) {
		pr_warn("Unable to start RMPOPT kernel thread\n");
		rmpopt_task = NULL;
		goto free_cpumask;
	}

	pr_info("RMPOPT worker thread created with PID %d\n", task_pid_nr(rmpopt_task));

	/*
	 * Once all per-CPU RMPOPT tables have been configured, enable RMPOPT
	 * optimizations on all physical memory.
	 */
	rmpopt_all_physmem();

	rmpopt_configfs_setup();

	rmpopt_debugfs_setup();

free_cpumask:
	free_cpumask_var(primary_threads_cpulist);
}

/*
 * Do the necessary preparations which are verified by the firmware as
 * described in the SNP_INIT_EX firmware command description in the SNP
 * firmware ABI spec.
 */
int __init snp_rmptable_init(void)
{
	unsigned int i;
	u64 val;

	if (WARN_ON_ONCE(!cc_platform_has(CC_ATTR_HOST_SEV_SNP)))
		return -ENOSYS;

	if (WARN_ON_ONCE(!amd_iommu_snp_en))
		return -ENOSYS;

	if (!setup_rmptable())
		return -ENOSYS;

	/*
	 * Check if SEV-SNP is already enabled, this can happen in case of
	 * kexec boot.
	 */
	rdmsrq(MSR_AMD64_SYSCFG, val);
	if (val & MSR_AMD64_SYSCFG_SNP_EN)
		goto skip_enable;

	/* Zero out the RMP bookkeeping area */
	if (!clear_rmptable_bookkeeping()) {
		free_rmp_segment_table();
		return -ENOSYS;
	}

	/* Zero out the RMP entries */
	for (i = 0; i < rst_max_index; i++) {
		struct rmp_segment_desc *desc;

		desc = rmp_segment_table[i];
		if (!desc)
			continue;

		memset(desc->rmp_entry, 0, desc->size);
	}

	/* Flush the caches to ensure that data is written before SNP is enabled. */
	wbinvd_on_all_cpus();

	/* MtrrFixDramModEn must be enabled on all the CPUs prior to enabling SNP. */
	on_each_cpu(mfd_enable, NULL, 1);

	on_each_cpu(snp_enable, NULL, 1);

skip_enable:
	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "x86/rmptable_init:online", __snp_enable, NULL);

	configure_and_enable_rmpopt();

	/*
	 * Setting crash_kexec_post_notifiers to 'true' to ensure that SNP panic
	 * notifier is invoked to do SNP IOMMU shutdown before kdump.
	 */
	crash_kexec_post_notifiers = true;

	return 0;
}

static void set_rmp_segment_info(unsigned int segment_shift)
{
	rmp_segment_shift = segment_shift;
	rmp_segment_size  = 1ULL << rmp_segment_shift;
	rmp_segment_mask  = rmp_segment_size - 1;
}

#define RMP_ADDR_MASK GENMASK_ULL(51, 13)

static bool probe_contiguous_rmptable_info(void)
{
	u64 rmp_sz, rmp_base, rmp_end;

	rdmsrq(MSR_AMD64_RMP_BASE, rmp_base);
	rdmsrq(MSR_AMD64_RMP_END, rmp_end);

	if (!(rmp_base & RMP_ADDR_MASK) || !(rmp_end & RMP_ADDR_MASK)) {
		pr_err("Memory for the RMP table has not been reserved by BIOS\n");
		return false;
	}

	if (rmp_base > rmp_end) {
		pr_err("RMP configuration not valid: base=%#llx, end=%#llx\n", rmp_base, rmp_end);
		return false;
	}

	rmp_sz = rmp_end - rmp_base + 1;

	/* Treat the contiguous RMP table as a single segment */
	rst_max_index = 1;

	set_rmp_segment_info(RMPTABLE_NON_SEGMENTED_SHIFT);

	probed_rmp_base = rmp_base;
	probed_rmp_size = rmp_sz;

	pr_info("RMP table physical range [0x%016llx - 0x%016llx]\n",
		rmp_base, rmp_end);

	return true;
}

static bool probe_segmented_rmptable_info(void)
{
	unsigned int eax, ebx, segment_shift, segment_shift_min, segment_shift_max;
	u64 rmp_base, rmp_end;

	rdmsrq(MSR_AMD64_RMP_BASE, rmp_base);
	if (!(rmp_base & RMP_ADDR_MASK)) {
		pr_err("Memory for the RMP table has not been reserved by BIOS\n");
		return false;
	}

	rdmsrq(MSR_AMD64_RMP_END, rmp_end);
	WARN_ONCE(rmp_end & RMP_ADDR_MASK,
		  "Segmented RMP enabled but RMP_END MSR is non-zero\n");

	/* Obtain the min and max supported RMP segment size */
	eax = cpuid_eax(0x80000025);
	segment_shift_min = eax & GENMASK(5, 0);
	segment_shift_max = (eax & GENMASK(11, 6)) >> 6;

	/* Verify the segment size is within the supported limits */
	segment_shift = MSR_AMD64_RMP_SEGMENT_SHIFT(rmp_cfg);
	if (segment_shift > segment_shift_max || segment_shift < segment_shift_min) {
		pr_err("RMP segment size (%u) is not within advertised bounds (min=%u, max=%u)\n",
		       segment_shift, segment_shift_min, segment_shift_max);
		return false;
	}

	/* Override the max supported RST index if a hardware limit exists */
	ebx = cpuid_ebx(0x80000025);
	if (ebx & BIT(10))
		rst_max_index = ebx & GENMASK(9, 0);

	set_rmp_segment_info(segment_shift);

	probed_rmp_base = rmp_base;
	probed_rmp_size = 0;

	pr_info("Segmented RMP base table physical range [0x%016llx - 0x%016llx]\n",
		rmp_base, rmp_base + RMPTABLE_CPU_BOOKKEEPING_SZ + RST_SIZE);

	return true;
}

bool snp_probe_rmptable_info(void)
{
	if (cpu_feature_enabled(X86_FEATURE_SEGMENTED_RMP))
		rdmsrq(MSR_AMD64_RMP_CFG, rmp_cfg);

	if (rmp_cfg & MSR_AMD64_SEG_RMP_ENABLED)
		return probe_segmented_rmptable_info();
	else
		return probe_contiguous_rmptable_info();
}

/*
 * About the array_index_nospec() usage below:
 *
 * This function can get called by exported functions like
 * snp_lookup_rmpentry(), which is used by the KVM #PF handler, among
 * others, and since the @pfn passed in cannot always be trusted,
 * speculation should be stopped as a protective measure.
 */
static struct rmpentry_raw *get_raw_rmpentry(u64 pfn)
{
	u64 paddr, rst_index, segment_index;
	struct rmp_segment_desc *desc;

	if (!rmp_segment_table)
		return ERR_PTR(-ENODEV);

	paddr = pfn << PAGE_SHIFT;

	rst_index = RST_ENTRY_INDEX(paddr);
	if (unlikely(rst_index >= rst_max_index))
		return ERR_PTR(-EFAULT);

	rst_index = array_index_nospec(rst_index, rst_max_index);

	desc = rmp_segment_table[rst_index];
	if (unlikely(!desc))
		return ERR_PTR(-EFAULT);

	segment_index = RMP_ENTRY_INDEX(paddr);
	if (unlikely(segment_index >= desc->max_index))
		return ERR_PTR(-EFAULT);

	segment_index = array_index_nospec(segment_index, desc->max_index);

	return desc->rmp_entry + segment_index;
}

static int get_rmpentry(u64 pfn, struct rmpentry *e)
{
	struct rmpentry_raw *e_raw;

	if (cpu_feature_enabled(X86_FEATURE_RMPREAD)) {
		int ret;

		/* Binutils version 2.44 supports the RMPREAD mnemonic. */
		asm volatile(".byte 0xf2, 0x0f, 0x01, 0xfd"
			     : "=a" (ret)
			     : "a" (pfn << PAGE_SHIFT), "c" (e)
			     : "memory", "cc");

		return ret;
	}

	e_raw = get_raw_rmpentry(pfn);
	if (IS_ERR(e_raw))
		return PTR_ERR(e_raw);

	/*
	 * Map the raw RMP table entry onto the RMPREAD output format.
	 * The 2MB region status indicator (hpage_region_status field) is not
	 * calculated, since the overhead could be significant and the field
	 * is not used.
	 */
	memset(e, 0, sizeof(*e));
	e->gpa       = e_raw->gpa << PAGE_SHIFT;
	e->asid      = e_raw->asid;
	e->assigned  = e_raw->assigned;
	e->pagesize  = e_raw->pagesize;
	e->immutable = e_raw->immutable;

	return 0;
}

static int __snp_lookup_rmpentry(u64 pfn, struct rmpentry *e, int *level)
{
	struct rmpentry e_large;
	int ret;

	if (!cc_platform_has(CC_ATTR_HOST_SEV_SNP))
		return -ENODEV;

	ret = get_rmpentry(pfn, e);
	if (ret)
		return ret;

	/*
	 * Find the authoritative RMP entry for a PFN. This can be either a 4K
	 * RMP entry or a special large RMP entry that is authoritative for a
	 * whole 2M area.
	 */
	ret = get_rmpentry(pfn & PFN_PMD_MASK, &e_large);
	if (ret)
		return ret;

	*level = RMP_TO_PG_LEVEL(e_large.pagesize);

	return 0;
}

int snp_lookup_rmpentry(u64 pfn, bool *assigned, int *level)
{
	struct rmpentry e;
	int ret;

	ret = __snp_lookup_rmpentry(pfn, &e, level);
	if (ret)
		return ret;

	*assigned = !!e.assigned;
	return 0;
}
EXPORT_SYMBOL_GPL(snp_lookup_rmpentry);

/*
 * Dump the raw RMP entry for a particular PFN. These bits are documented in the
 * PPR for a particular CPU model and provide useful information about how a
 * particular PFN is being utilized by the kernel/firmware at the time certain
 * unexpected events occur, such as RMP faults.
 */
static void dump_rmpentry(u64 pfn)
{
	struct rmpentry_raw *e_raw;
	u64 pfn_i, pfn_end;
	struct rmpentry e;
	int level, ret;

	ret = __snp_lookup_rmpentry(pfn, &e, &level);
	if (ret) {
		pr_err("Failed to read RMP entry for PFN 0x%llx, error %d\n",
		       pfn, ret);
		return;
	}

	if (e.assigned) {
		e_raw = get_raw_rmpentry(pfn);
		if (IS_ERR(e_raw)) {
			pr_err("Failed to read RMP contents for PFN 0x%llx, error %ld\n",
			       pfn, PTR_ERR(e_raw));
			return;
		}

		pr_info("PFN 0x%llx, RMP entry: [0x%016llx - 0x%016llx]\n",
			pfn, e_raw->lo, e_raw->hi);
		return;
	}

	/*
	 * If the RMP entry for a particular PFN is not in an assigned state,
	 * then it is sometimes useful to get an idea of whether or not any RMP
	 * entries for other PFNs within the same 2MB region are assigned, since
	 * those too can affect the ability to access a particular PFN in
	 * certain situations, such as when the PFN is being accessed via a 2MB
	 * mapping in the host page table.
	 */
	pfn_i = ALIGN_DOWN(pfn, PTRS_PER_PMD);
	pfn_end = pfn_i + PTRS_PER_PMD;

	pr_info("PFN 0x%llx unassigned, dumping non-zero entries in 2M PFN region: [0x%llx - 0x%llx]\n",
		pfn, pfn_i, pfn_end);

	while (pfn_i < pfn_end) {
		e_raw = get_raw_rmpentry(pfn_i);
		if (IS_ERR(e_raw)) {
			pr_err("Error %ld reading RMP contents for PFN 0x%llx\n",
			       PTR_ERR(e_raw), pfn_i);
			pfn_i++;
			continue;
		}

		if (e_raw->lo || e_raw->hi)
			pr_info("PFN: 0x%llx, [0x%016llx - 0x%016llx]\n", pfn_i, e_raw->lo, e_raw->hi);
		pfn_i++;
	}
}

void snp_dump_hva_rmpentry(unsigned long hva)
{
	unsigned long paddr;
	unsigned int level;
	pgd_t *pgd;
	pte_t *pte;

	pgd = __va(read_cr3_pa());
	pgd += pgd_index(hva);
	pte = lookup_address_in_pgd(pgd, hva, &level);

	if (!pte) {
		pr_err("Can't dump RMP entry for HVA %lx: no PTE/PFN found\n", hva);
		return;
	}

	paddr = PFN_PHYS(pte_pfn(*pte)) | (hva & ~page_level_mask(level));
	dump_rmpentry(PHYS_PFN(paddr));
}

/*
 * PSMASH a 2MB aligned page into 4K pages in the RMP table while preserving the
 * Validated bit.
 */
int psmash(u64 pfn)
{
	unsigned long paddr = pfn << PAGE_SHIFT;
	int ret;

	if (!cc_platform_has(CC_ATTR_HOST_SEV_SNP))
		return -ENODEV;

	if (!pfn_valid(pfn))
		return -EINVAL;

	/* Binutils version 2.36 supports the PSMASH mnemonic. */
	asm volatile(".byte 0xF3, 0x0F, 0x01, 0xFF"
		      : "=a" (ret)
		      : "a" (paddr)
		      : "memory", "cc");

	return ret;
}
EXPORT_SYMBOL_GPL(psmash);

/*
 * If the kernel uses a 2MB or larger directmap mapping to write to an address,
 * and that mapping contains any 4KB pages that are set to private in the RMP
 * table, an RMP #PF will trigger and cause a host crash. Hypervisor code that
 * owns the PFNs being transitioned will never attempt such a write, but other
 * kernel tasks writing to other PFNs in the range may trigger these checks
 * inadvertently due a large directmap mapping that happens to overlap such a
 * PFN.
 *
 * Prevent this by splitting any 2MB+ mappings that might end up containing a
 * mix of private/shared PFNs as a result of a subsequent RMPUPDATE for the
 * PFN/rmp_level passed in.
 *
 * Note that there is no attempt here to scan all the RMP entries for the 2MB
 * physical range, since it would only be worthwhile in determining if a
 * subsequent RMPUPDATE for a 4KB PFN would result in all the entries being of
 * the same shared/private state, thus avoiding the need to split the mapping.
 * But that would mean the entries are currently in a mixed state, and so the
 * mapping would have already been split as a result of prior transitions.
 * And since the 4K split is only done if the mapping is 2MB+, and there isn't
 * currently a mechanism in place to restore 2MB+ mappings, such a check would
 * not provide any usable benefit.
 *
 * More specifics on how these checks are carried out can be found in APM
 * Volume 2, "RMP and VMPL Access Checks".
 */
static int adjust_direct_map(u64 pfn, int rmp_level)
{
	unsigned long vaddr;
	unsigned int level;
	int npages, ret;
	pte_t *pte;

	/*
	 * pfn_to_kaddr() will return a vaddr only within the direct
	 * map range.
	 */
	vaddr = (unsigned long)pfn_to_kaddr(pfn);

	/* Only 4KB/2MB RMP entries are supported by current hardware. */
	if (WARN_ON_ONCE(rmp_level > PG_LEVEL_2M))
		return -EINVAL;

	if (!pfn_valid(pfn))
		return -EINVAL;

	if (rmp_level == PG_LEVEL_2M &&
	    (!IS_ALIGNED(pfn, PTRS_PER_PMD) || !pfn_valid(pfn + PTRS_PER_PMD - 1)))
		return -EINVAL;

	/*
	 * If an entire 2MB physical range is being transitioned, then there is
	 * no risk of RMP #PFs due to write accesses from overlapping mappings,
	 * since even accesses from 1GB mappings will be treated as 2MB accesses
	 * as far as RMP table checks are concerned.
	 */
	if (rmp_level == PG_LEVEL_2M)
		return 0;

	pte = lookup_address(vaddr, &level);
	if (!pte || pte_none(*pte))
		return 0;

	if (level == PG_LEVEL_4K)
		return 0;

	npages = page_level_size(rmp_level) / PAGE_SIZE;
	ret = set_memory_4k(vaddr, npages);
	if (ret)
		pr_warn("Failed to split direct map for PFN 0x%llx, ret: %d\n",
			pfn, ret);

	return ret;
}

/*
 * It is expected that those operations are seldom enough so that no mutual
 * exclusion of updaters is needed and thus the overlap error condition below
 * should happen very rarely and would get resolved relatively quickly by
 * the firmware.
 *
 * If not, one could consider introducing a mutex or so here to sync concurrent
 * RMP updates and thus diminish the amount of cases where firmware needs to
 * lock 2M ranges to protect against concurrent updates.
 *
 * The optimal solution would be range locking to avoid locking disjoint
 * regions unnecessarily but there's no support for that yet.
 */
static int rmpupdate(u64 pfn, struct rmp_state *state)
{
	unsigned long paddr = pfn << PAGE_SHIFT;
	int ret, level;

	if (!cc_platform_has(CC_ATTR_HOST_SEV_SNP))
		return -ENODEV;

	level = RMP_TO_PG_LEVEL(state->pagesize);

	if (adjust_direct_map(pfn, level))
		return -EFAULT;

	do {
		/* Binutils version 2.36 supports the RMPUPDATE mnemonic. */
		asm volatile(".byte 0xF2, 0x0F, 0x01, 0xFE"
			     : "=a" (ret)
			     : "a" (paddr), "c" ((unsigned long)state)
			     : "memory", "cc");
	} while (ret == RMPUPDATE_FAIL_OVERLAP);

	if (ret) {
		pr_err("RMPUPDATE failed for PFN %llx, pg_level: %d, ret: %d\n",
		       pfn, level, ret);
		dump_rmpentry(pfn);
		dump_stack();
		return -EFAULT;
	}

	return 0;
}

/* Transition a page to guest-owned/private state in the RMP table. */
int rmp_make_private(u64 pfn, u64 gpa, enum pg_level level, u32 asid, bool immutable)
{
	struct rmp_state state;

	memset(&state, 0, sizeof(state));
	state.assigned = 1;
	state.asid = asid;
	state.immutable = immutable;
	state.gpa = gpa;
	state.pagesize = PG_LEVEL_TO_RMP(level);

	return rmpupdate(pfn, &state);
}
EXPORT_SYMBOL_GPL(rmp_make_private);

/* Transition a page to hypervisor-owned/shared state in the RMP table. */
int rmp_make_shared(u64 pfn, enum pg_level level)
{
	struct rmp_state state;

	memset(&state, 0, sizeof(state));
	state.pagesize = PG_LEVEL_TO_RMP(level);

	return rmpupdate(pfn, &state);
}
EXPORT_SYMBOL_GPL(rmp_make_shared);

int snp_perform_rmp_optimization(void)
{
	if (!cpu_feature_enabled(X86_FEATURE_RMPOPT))
		return -EINVAL;

	if (!cc_platform_has(CC_ATTR_HOST_SEV_SNP))
		return -EINVAL;

	if (!(rmp_cfg & MSR_AMD64_SEG_RMP_ENABLED))
		return -EINVAL;

	rmpopt_all_physmem();

	return 0;
}
EXPORT_SYMBOL_GPL(snp_perform_rmp_optimization);

void __snp_leak_pages(u64 pfn, unsigned int npages, bool dump_rmp)
{
	struct page *page = pfn_to_page(pfn);

	pr_warn("Leaking PFN range 0x%llx-0x%llx\n", pfn, pfn + npages);

	spin_lock(&snp_leaked_pages_list_lock);
	while (npages--) {

		/*
		 * Reuse the page's buddy list for chaining into the leaked
		 * pages list. This page should not be on a free list currently
		 * and is also unsafe to be added to a free list.
		 */
		if (likely(!PageCompound(page)) ||

			/*
			 * Skip inserting tail pages of compound page as
			 * page->buddy_list of tail pages is not usable.
			 */
		    (PageHead(page) && compound_nr(page) <= npages))
			list_add_tail(&page->buddy_list, &snp_leaked_pages_list);

		if (dump_rmp)
			dump_rmpentry(pfn);
		snp_nr_leaked_pages++;
		pfn++;
		page++;
	}
	spin_unlock(&snp_leaked_pages_list_lock);
}
EXPORT_SYMBOL_GPL(__snp_leak_pages);

void kdump_sev_callback(void)
{
	/*
	 * Do wbinvd() on remote CPUs when SNP is enabled in order to
	 * safely do SNP_SHUTDOWN on the local CPU.
	 */
	if (cc_platform_has(CC_ATTR_HOST_SEV_SNP))
		wbinvd();
}
