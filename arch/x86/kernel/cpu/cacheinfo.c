// SPDX-License-Identifier: GPL-2.0
/*
 * x86 CPU caches detection and configuration
 *
 * Previous changes
 * - Venkatesh Pallipadi:		Cache identification through CPUID(0x4)
 * - Ashok Raj <ashok.raj@intel.com>:	Work with CPU hotplug infrastructure
 * - Andi Kleen / Andreas Herrmann:	CPUID(0x4) emulation on AMD
 */

#include <linux/cacheinfo.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/stop_machine.h>

#include <asm/amd/nb.h>
#include <asm/cacheinfo.h>
#include <asm/cpufeature.h>
#include <asm/cpuid/api.h>
#include <asm/mtrr.h>
#include <asm/smp.h>
#include <asm/tlbflush.h>

#include "cpu.h"

/* Shared last level cache maps */
DEFINE_PER_CPU_READ_MOSTLY(cpumask_var_t, cpu_llc_shared_map);

/* Shared L2 cache maps */
DEFINE_PER_CPU_READ_MOSTLY(cpumask_var_t, cpu_l2c_shared_map);

static cpumask_var_t cpu_cacheinfo_mask;

/* Kernel controls MTRR and/or PAT MSRs. */
unsigned int memory_caching_control __ro_after_init;

enum _cache_type {
	CTYPE_NULL	= 0,
	CTYPE_DATA	= 1,
	CTYPE_INST	= 2,
	CTYPE_UNIFIED	= 3
};

struct _cpuid4_info {
	struct leaf_0x4_n regs;
	unsigned int id;
	unsigned long size;
};

/* Map CPUID(0x4) EAX.cache_type to <linux/cacheinfo.h> types */
static const enum cache_type cache_type_map[] = {
	[CTYPE_NULL]	= CACHE_TYPE_NOCACHE,
	[CTYPE_DATA]	= CACHE_TYPE_DATA,
	[CTYPE_INST]	= CACHE_TYPE_INST,
	[CTYPE_UNIFIED] = CACHE_TYPE_UNIFIED,
};

/*
 * Fallback AMD CPUID(0x4) emulation:
 * AMD CPUs with TOPOEXT can just use CPUID(0x8000001d)
 */

/*
 * @AMD_L2_L3_INVALID_ASSOC: cache info for the respective L2/L3 cache should
 * be determined from CPUID(0x8000001d) instead of CPUID(0x80000006).
 */
#define AMD_CPUID4_FULLY_ASSOCIATIVE	0xffff
#define AMD_L2_L3_INVALID_ASSOC		0x9

/* L2/L3 associativity mapping */
static const unsigned short assocs[] = {
	[1]		= 1,
	[2]		= 2,
	[3]		= 3,
	[4]		= 4,
	[5]		= 6,
	[6]		= 8,
	[8]		= 16,
	[0xa]		= 32,
	[0xb]		= 48,
	[0xc]		= 64,
	[0xd]		= 96,
	[0xe]		= 128,
	[0xf]		= AMD_CPUID4_FULLY_ASSOCIATIVE
};

static const unsigned char levels[] = { 1, 1, 2, 3 };
static const unsigned char types[]  = { 1, 2, 3, 3 };

static void legacy_amd_cpuid4(struct cpuinfo_x86 *c, int index, struct leaf_0x4_n *regs)
{
	const struct leaf_0x80000005_0 *el5 = cpuid_leaf(c, 0x80000005);
	const struct leaf_0x80000006_0 *el6 = cpuid_leaf(c, 0x80000006);
	const struct cpuid_regs *el5_raw = cpuid_leaf_raw(c, 0x80000005);
	unsigned int line_size, lines_per_tag, assoc, size_in_kb;

	*regs = (struct leaf_0x4_n){ };

	switch (index) {
	case 0:
		if (!el5 || !el5_raw->ecx)
			return;

		assoc		= el5->l1_dcache_assoc;
		line_size	= el5->l1_dcache_line_size;
		lines_per_tag	= el5->l1_dcache_nlines;
		size_in_kb	= el5->l1_dcache_size_kb;
		break;
	case 1:
		if (!el5 || !el5_raw->edx)
			return;

		assoc		= el5->l1_icache_assoc;
		line_size	= el5->l1_icache_line_size;
		lines_per_tag	= el5->l1_icache_nlines;
		size_in_kb	= el5->l1_icache_size_kb;
		break;
	case 2:
		if (!el6 || !el6->l2_assoc || el6->l2_assoc == AMD_L2_L3_INVALID_ASSOC)
			return;

		/* Use x86_cache_size as it might have K7 errata fixes */
		assoc		= assocs[el6->l2_assoc];
		line_size	= el6->l2_line_size;
		lines_per_tag	= el6->l2_nlines;
		size_in_kb	= __this_cpu_read(cpu_info.x86_cache_size);
		break;
	case 3:
		if (!el6 || !el6->l3_assoc || el6->l3_assoc == AMD_L2_L3_INVALID_ASSOC)
			return;

		assoc		= assocs[el6->l3_assoc];
		line_size	= el6->l3_line_size;
		lines_per_tag	= el6->l3_nlines;
		size_in_kb	= el6->l3_size_range * 512;
		if (boot_cpu_has(X86_FEATURE_AMD_DCM)) {
			size_in_kb	= size_in_kb >> 1;
			assoc		= assoc >> 1;
		}
		break;
	default:
		return;
	}

	/* For L1d and L1i caches, 0xff is the full associativity marker */
	if ((index == 0 || index == 1) && assoc == 0xff)
		assoc = AMD_CPUID4_FULLY_ASSOCIATIVE;

	regs->cache_self_init		= 1;
	regs->cache_type		= types[index];
	regs->cache_level		= levels[index];
	regs->num_threads_sharing	= 0;
	regs->num_cores_on_die		= topology_num_cores_per_package();

	if (assoc == AMD_CPUID4_FULLY_ASSOCIATIVE)
		regs->fully_associative	= 1;

	regs->cache_linesize		= line_size - 1;
	regs->cache_nways		= assoc - 1;
	regs->cache_npartitions		= lines_per_tag - 1;
	regs->cache_nsets		= (size_in_kb * 1024) / line_size /
		(regs->cache_nways + 1) - 1;
}

static int cpuid4_info_fill_done(struct _cpuid4_info *id4, const struct leaf_0x4_n *regs)
{
	if (regs->cache_type == CTYPE_NULL)
		return -EIO;

	id4->regs = *regs;
	id4->size = (regs->cache_nsets	     + 1) *
		    (regs->cache_linesize    + 1) *
		    (regs->cache_npartitions + 1) *
		    (regs->cache_nways	     + 1);

	return 0;
}

static int amd_fill_cpuid4_info(struct cpuinfo_x86 *c, int index, struct _cpuid4_info *id4)
{
	struct leaf_0x4_n l_0x4_regs;

	if (boot_cpu_has(X86_FEATURE_TOPOEXT) || boot_cpu_data.x86_vendor == X86_VENDOR_HYGON) {
		const struct leaf_0x8000001d_n *regs = cpuid_subleaf_n(c, 0x8000001d, index);

		if (!regs)
			return -EIO;

		/* CPUID(0x8000001d) and CPUID(0x4) have the same bitfields */
		l_0x4_regs = *(struct leaf_0x4_n *)regs;
	} else
		legacy_amd_cpuid4(c, index, &l_0x4_regs);

	return cpuid4_info_fill_done(id4, &l_0x4_regs);
}

static int intel_fill_cpuid4_info(struct cpuinfo_x86 *c, int index, struct _cpuid4_info *id4)
{
	const struct leaf_0x4_n *regs = cpuid_subleaf_n(c, 0x4, index);

	if (!regs)
		return -EIO;

	return cpuid4_info_fill_done(id4, regs);
}

static int fill_cpuid4_info(struct cpuinfo_x86 *c, int index, struct _cpuid4_info *id4)
{
	u8 cpu_vendor = boot_cpu_data.x86_vendor;

	return (cpu_vendor == X86_VENDOR_AMD || cpu_vendor == X86_VENDOR_HYGON) ?
		amd_fill_cpuid4_info(c, index, id4) :
		intel_fill_cpuid4_info(c, index, id4);
}

/*
 * The max shared threads number comes from CPUID(0x4) EAX[25-14] with input
 * ECX as cache index. Then right shift apicid by the number's order to get
 * cache id for this cache node.
 */
static unsigned int get_cache_id(u32 apicid, const struct _cpuid4_info *id4)
{
	unsigned long num_threads_sharing;
	int index_msb;

	num_threads_sharing = 1 + id4->regs.num_threads_sharing;
	index_msb = get_count_order(num_threads_sharing);

	return apicid >> index_msb;
}

/*
 * AMD/Hygon CPUs may have multiple LLCs if L3 caches exist.
 */

void cacheinfo_amd_init_llc_id(struct cpuinfo_x86 *c, u16 die_id)
{
	if (!cpuid_amd_hygon_has_l3_cache())
		return;

	if (c->x86 < 0x17) {
		/* Pre-Zen: LLC is at the node level */
		c->topo.llc_id = die_id;
	} else if (c->x86 == 0x17 && c->x86_model <= 0x1F) {
		/*
		 * Family 17h up to 1F models: LLC is at the core
		 * complex level.  Core complex ID is ApicId[3].
		 */
		c->topo.llc_id = c->topo.apicid >> 3;
	} else {
		/*
		 * Newer families: LLC ID is calculated from the number
		 * of threads sharing the L3 cache.
		 */
		u32 llc_index = cpuid_subleaf_count(c, 0x8000001d) - 1;
		struct _cpuid4_info id4 = {};

		if (!amd_fill_cpuid4_info(c, llc_index, &id4))
			c->topo.llc_id = get_cache_id(c->topo.apicid, &id4);
	}
}

void cacheinfo_hygon_init_llc_id(struct cpuinfo_x86 *c)
{
	if (!cpuid_amd_hygon_has_l3_cache())
		return;

	/*
	 * Hygons are similar to AMD Family 17h up to 1F models: LLC is
	 * at the core complex level.  Core complex ID is ApicId[3].
	 */
	c->topo.llc_id = c->topo.apicid >> 3;
}

void init_amd_cacheinfo(struct cpuinfo_x86 *c)
{
	const struct leaf_0x80000006_0 *el6 = cpuid_leaf(c, 0x80000006);
	struct cpu_cacheinfo *ci = get_cpu_cacheinfo(c->cpu_index);

	if (boot_cpu_has(X86_FEATURE_TOPOEXT))
		ci->num_leaves = cpuid_subleaf_count(c, 0x8000001d);
	else if (el6)
		ci->num_leaves = (el6->l3_assoc) ? 4 : 3;
}

void init_hygon_cacheinfo(struct cpuinfo_x86 *c)
{
	struct cpu_cacheinfo *ci = get_cpu_cacheinfo(c->cpu_index);

	ci->num_leaves = cpuid_subleaf_count(c, 0x8000001d);
}

static void intel_cacheinfo_done(struct cpuinfo_x86 *c, unsigned int l3,
				 unsigned int l2, unsigned int l1i, unsigned int l1d)
{
	/*
	 * If llc_id is still unset, then cpuid_level < 4, which implies
	 * that the only possibility left is SMT.  Since CPUID(0x2) doesn't
	 * specify any shared caches and SMT shares all caches, we can
	 * unconditionally set LLC ID to the package ID so that all
	 * threads share it.
	 */
	if (c->topo.llc_id == BAD_APICID)
		c->topo.llc_id = c->topo.pkg_id;

	c->x86_cache_size = l3 ? l3 : (l2 ? l2 : l1i + l1d);

	if (!l2)
		cpu_detect_cache_sizes(c);
}

/*
 * Legacy Intel CPUID(0x2) path if CPUID(0x4) is not available.
 */
static void intel_cacheinfo_0x2(struct cpuinfo_x86 *c)
{
	unsigned int l1i = 0, l1d = 0, l2 = 0, l3 = 0;
	const struct leaf_0x2_table *desc;
	const struct cpuid_regs *regs;
	const u8 *ptr;

	regs = cpuid_leaf_raw(c, 0x2);
	if (!regs)
		return;

	for_each_cpuid_0x2_desc(regs, ptr, desc) {
		switch (desc->c_type) {
		case CACHE_L1_INST:	l1i += desc->c_size; break;
		case CACHE_L1_DATA:	l1d += desc->c_size; break;
		case CACHE_L2:		l2  += desc->c_size; break;
		case CACHE_L3:		l3  += desc->c_size; break;
		}
	}

	intel_cacheinfo_done(c, l3, l2, l1i, l1d);
}

static unsigned int calc_cache_topo_id(struct cpuinfo_x86 *c, const struct _cpuid4_info *id4)
{
	unsigned int num_threads_sharing;
	int index_msb;

	num_threads_sharing = 1 + id4->regs.num_threads_sharing;
	index_msb = get_count_order(num_threads_sharing);
	return c->topo.apicid & ~((1 << index_msb) - 1);
}

static bool intel_cacheinfo_0x4(struct cpuinfo_x86 *c)
{
	struct cpu_cacheinfo *ci = get_cpu_cacheinfo(c->cpu_index);
	unsigned int l2_id = BAD_APICID, l3_id = BAD_APICID;
	unsigned int l1d = 0, l1i = 0, l2 = 0, l3 = 0;

	/* Non-zero means that it has been previously initialized */
	if (!ci->num_leaves)
		ci->num_leaves = cpuid_subleaf_count(c, 0x4);

	if (!ci->num_leaves)
		return false;

	for (int i = 0; i < ci->num_leaves; i++) {
		struct _cpuid4_info id4 = {};
		int ret;

		ret = intel_fill_cpuid4_info(c, i, &id4);
		if (ret < 0)
			continue;

		switch (id4.regs.cache_level) {
		case 1:
			if (id4.regs.cache_type == CTYPE_DATA)
				l1d = id4.size / 1024;
			else if (id4.regs.cache_type == CTYPE_INST)
				l1i = id4.size / 1024;
			break;
		case 2:
			l2 = id4.size / 1024;
			l2_id = calc_cache_topo_id(c, &id4);
			break;
		case 3:
			l3 = id4.size / 1024;
			l3_id = calc_cache_topo_id(c, &id4);
			break;
		default:
			break;
		}
	}

	c->topo.l2c_id = l2_id;
	c->topo.llc_id = (l3_id == BAD_APICID) ? l2_id : l3_id;
	intel_cacheinfo_done(c, l3, l2, l1i, l1d);
	return true;
}

void init_intel_cacheinfo(struct cpuinfo_x86 *c)
{
	/* Don't use CPUID(0x2) if CPUID(0x4) is supported. */
	if (intel_cacheinfo_0x4(c))
		return;

	intel_cacheinfo_0x2(c);
}

/*
 * <linux/cacheinfo.h> shared_cpu_map setup, AMD/Hygon
 */
static int __cache_amd_cpumap_setup(unsigned int cpu, int index,
				    const struct _cpuid4_info *id4)
{
	struct cpu_cacheinfo *this_cpu_ci;
	struct cacheinfo *ci;
	int i, sibling;

	/*
	 * For L3, always use the pre-calculated cpu_llc_shared_mask
	 * to derive shared_cpu_map.
	 */
	if (index == 3) {
		for_each_cpu(i, cpu_llc_shared_mask(cpu)) {
			this_cpu_ci = get_cpu_cacheinfo(i);
			if (!this_cpu_ci->info_list)
				continue;

			ci = this_cpu_ci->info_list + index;
			for_each_cpu(sibling, cpu_llc_shared_mask(cpu)) {
				if (!cpu_online(sibling))
					continue;
				cpumask_set_cpu(sibling, &ci->shared_cpu_map);
			}
		}
	} else if (boot_cpu_has(X86_FEATURE_TOPOEXT)) {
		unsigned int apicid, nshared, first, last;

		nshared = id4->regs.num_threads_sharing + 1;
		apicid = cpu_data(cpu).topo.apicid;
		first = apicid - (apicid % nshared);
		last = first + nshared - 1;

		for_each_online_cpu(i) {
			this_cpu_ci = get_cpu_cacheinfo(i);
			if (!this_cpu_ci->info_list)
				continue;

			apicid = cpu_data(i).topo.apicid;
			if ((apicid < first) || (apicid > last))
				continue;

			ci = this_cpu_ci->info_list + index;

			for_each_online_cpu(sibling) {
				apicid = cpu_data(sibling).topo.apicid;
				if ((apicid < first) || (apicid > last))
					continue;
				cpumask_set_cpu(sibling, &ci->shared_cpu_map);
			}
		}
	} else
		return 0;

	return 1;
}

/*
 * <linux/cacheinfo.h> shared_cpu_map setup, Intel + fallback AMD/Hygon
 */
static void __cache_cpumap_setup(unsigned int cpu, int index,
				 const struct _cpuid4_info *id4)
{
	struct cpu_cacheinfo *this_cpu_ci = get_cpu_cacheinfo(cpu);
	struct cpuinfo_x86 *c = &cpu_data(cpu);
	struct cacheinfo *ci, *sibling_ci;
	unsigned long num_threads_sharing;
	int index_msb, i;

	if (c->x86_vendor == X86_VENDOR_AMD || c->x86_vendor == X86_VENDOR_HYGON) {
		if (__cache_amd_cpumap_setup(cpu, index, id4))
			return;
	}

	ci = this_cpu_ci->info_list + index;
	num_threads_sharing = 1 + id4->regs.num_threads_sharing;

	cpumask_set_cpu(cpu, &ci->shared_cpu_map);
	if (num_threads_sharing == 1)
		return;

	index_msb = get_count_order(num_threads_sharing);

	for_each_online_cpu(i)
		if (cpu_data(i).topo.apicid >> index_msb == c->topo.apicid >> index_msb) {
			struct cpu_cacheinfo *sib_cpu_ci = get_cpu_cacheinfo(i);

			/* Skip if itself or no cacheinfo */
			if (i == cpu || !sib_cpu_ci->info_list)
				continue;

			sibling_ci = sib_cpu_ci->info_list + index;
			cpumask_set_cpu(i, &ci->shared_cpu_map);
			cpumask_set_cpu(cpu, &sibling_ci->shared_cpu_map);
		}
}

static void ci_info_init(struct cacheinfo *ci, const struct _cpuid4_info *id4,
			 struct amd_northbridge *nb)
{
	ci->id				= id4->id;
	ci->attributes			= CACHE_ID;
	ci->level			= id4->regs.cache_level;
	ci->type			= cache_type_map[id4->regs.cache_type];
	ci->coherency_line_size		= id4->regs.cache_linesize + 1;
	ci->ways_of_associativity	= id4->regs.cache_nways + 1;
	ci->size			= id4->size;
	ci->number_of_sets		= id4->regs.cache_nsets + 1;
	ci->physical_line_partition	= id4->regs.cache_npartitions + 1;
	ci->priv			= nb;
}

int init_cache_level(unsigned int cpu)
{
	struct cpu_cacheinfo *ci = get_cpu_cacheinfo(cpu);

	/* There should be at least one leaf. */
	if (!ci->num_leaves)
		return -ENOENT;

	return 0;
}

int populate_cache_leaves(unsigned int cpu)
{
	struct cpu_cacheinfo *this_cpu_ci = get_cpu_cacheinfo(cpu);
	struct cacheinfo *ci = this_cpu_ci->info_list;
	u8 cpu_vendor = boot_cpu_data.x86_vendor;
	struct cpuinfo_x86 *c = &cpu_data(cpu);
	struct amd_northbridge *nb = NULL;
	struct _cpuid4_info id4 = {};
	int idx, ret;

	for (idx = 0; idx < this_cpu_ci->num_leaves; idx++) {
		ret = fill_cpuid4_info(c, idx, &id4);
		if (ret)
			return ret;

		id4.id = get_cache_id(c->topo.apicid, &id4);

		if (cpu_vendor == X86_VENDOR_AMD || cpu_vendor == X86_VENDOR_HYGON)
			nb = amd_init_l3_cache(idx);

		ci_info_init(ci++, &id4, nb);
		__cache_cpumap_setup(cpu, idx, &id4);
	}

	this_cpu_ci->cpu_map_populated = true;
	return 0;
}

/*
 * Disable and enable caches. Needed for changing MTRRs and the PAT MSR.
 *
 * Since we are disabling the cache don't allow any interrupts,
 * they would run extremely slow and would only increase the pain.
 *
 * The caller must ensure that local interrupts are disabled and
 * are reenabled after cache_enable() has been called.
 */
static unsigned long saved_cr4;
static DEFINE_RAW_SPINLOCK(cache_disable_lock);

/*
 * Cache flushing is the most time-consuming step when programming the
 * MTRRs.  On many Intel CPUs without known erratas, it can be skipped
 * if the CPU declares cache self-snooping support.
 */
static void maybe_flush_caches(void)
{
	if (!static_cpu_has(X86_FEATURE_SELFSNOOP))
		wbinvd();
}

void cache_disable(void) __acquires(cache_disable_lock)
{
	unsigned long cr0;

	/*
	 * This is not ideal since the cache is only flushed/disabled
	 * for this CPU while the MTRRs are changed, but changing this
	 * requires more invasive changes to the way the kernel boots.
	 */
	raw_spin_lock(&cache_disable_lock);

	/* Enter the no-fill (CD=1, NW=0) cache mode and flush caches. */
	cr0 = read_cr0() | X86_CR0_CD;
	write_cr0(cr0);

	maybe_flush_caches();

	/* Save value of CR4 and clear Page Global Enable (bit 7) */
	if (cpu_feature_enabled(X86_FEATURE_PGE)) {
		saved_cr4 = __read_cr4();
		__write_cr4(saved_cr4 & ~X86_CR4_PGE);
	}

	/* Flush all TLBs via a mov %cr3, %reg; mov %reg, %cr3 */
	count_vm_tlb_event(NR_TLB_LOCAL_FLUSH_ALL);
	flush_tlb_local();

	if (cpu_feature_enabled(X86_FEATURE_MTRR))
		mtrr_disable();

	maybe_flush_caches();
}

void cache_enable(void) __releases(cache_disable_lock)
{
	/* Flush TLBs (no need to flush caches - they are disabled) */
	count_vm_tlb_event(NR_TLB_LOCAL_FLUSH_ALL);
	flush_tlb_local();

	if (cpu_feature_enabled(X86_FEATURE_MTRR))
		mtrr_enable();

	/* Enable caches */
	write_cr0(read_cr0() & ~X86_CR0_CD);

	/* Restore value of CR4 */
	if (cpu_feature_enabled(X86_FEATURE_PGE))
		__write_cr4(saved_cr4);

	raw_spin_unlock(&cache_disable_lock);
}

static void cache_cpu_init(void)
{
	unsigned long flags;

	local_irq_save(flags);

	if (memory_caching_control & CACHE_MTRR) {
		cache_disable();
		mtrr_generic_set_state();
		cache_enable();
	}

	if (memory_caching_control & CACHE_PAT)
		pat_cpu_init();

	local_irq_restore(flags);
}

static bool cache_aps_delayed_init = true;

void set_cache_aps_delayed_init(bool val)
{
	cache_aps_delayed_init = val;
}

bool get_cache_aps_delayed_init(void)
{
	return cache_aps_delayed_init;
}

static int cache_rendezvous_handler(void *unused)
{
	if (get_cache_aps_delayed_init() || !cpu_online(smp_processor_id()))
		cache_cpu_init();

	return 0;
}

void __init cache_bp_init(void)
{
	mtrr_bp_init();
	pat_bp_init();

	if (memory_caching_control)
		cache_cpu_init();
}

void cache_bp_restore(void)
{
	if (memory_caching_control)
		cache_cpu_init();
}

static int cache_ap_online(unsigned int cpu)
{
	cpumask_set_cpu(cpu, cpu_cacheinfo_mask);

	if (!memory_caching_control || get_cache_aps_delayed_init())
		return 0;

	/*
	 * Ideally we should hold mtrr_mutex here to avoid MTRR entries
	 * changed, but this routine will be called in CPU boot time,
	 * holding the lock breaks it.
	 *
	 * This routine is called in two cases:
	 *
	 *   1. very early time of software resume, when there absolutely
	 *      isn't MTRR entry changes;
	 *
	 *   2. CPU hotadd time. We let mtrr_add/del_page hold cpuhotplug
	 *      lock to prevent MTRR entry changes
	 */
	stop_machine_from_inactive_cpu(cache_rendezvous_handler, NULL,
				       cpu_cacheinfo_mask);

	return 0;
}

static int cache_ap_offline(unsigned int cpu)
{
	cpumask_clear_cpu(cpu, cpu_cacheinfo_mask);
	return 0;
}

/*
 * Delayed cache initialization for all AP's
 */
void cache_aps_init(void)
{
	if (!memory_caching_control || !get_cache_aps_delayed_init())
		return;

	stop_machine(cache_rendezvous_handler, NULL, cpu_online_mask);
	set_cache_aps_delayed_init(false);
}

static int __init cache_ap_register(void)
{
	zalloc_cpumask_var(&cpu_cacheinfo_mask, GFP_KERNEL);
	cpumask_set_cpu(smp_processor_id(), cpu_cacheinfo_mask);

	cpuhp_setup_state_nocalls(CPUHP_AP_CACHECTRL_STARTING,
				  "x86/cachectrl:starting",
				  cache_ap_online, cache_ap_offline);
	return 0;
}
early_initcall(cache_ap_register);
