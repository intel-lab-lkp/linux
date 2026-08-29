// SPDX-License-Identifier: GPL-2.0-only
/*
 * Enhanced Resource Director Technology (ERDT)
 *
 * Copyright (C) 2026 Intel Corporation
 *
 */

#define pr_fmt(fmt)     "resctrl: " fmt

#include <linux/acpi.h>
#include <linux/overflow.h>
#include <linux/resctrl.h>
#include <linux/sizes.h>

#include <asm/apic.h>

#include "internal.h"

static LIST_HEAD(domain_info_list);

/* True when the ERDT ACPI table describes at least one domain with at least one CPU. */
static bool erdt_enabled;

#define ERDT_VALID_VERSION		1
#define CMRC_SUPPORTED_INDEX_FN		1
#define RMDD_FLAG_CPU_L3_DOMAIN		BIT(0)

/* Set in a monitoring counter when it holds no valid data to report. */
#define UNAVAILABLE_COUNTER		BIT_ULL(63)
#define CMRC_FLAG_UNAVAILABLE_BIT	BIT(0)

/* Bitmask of valid sub-tables found in the first RMDD, used to ensure all RMDDs match. */
static u32 valid_subtbl_mask;

/* Domain ID of the first RMDD that established @valid_subtbl_mask, for diagnostics. */
static u16 first_rmdd_domain_id;

/*
 * The minimal max-rmid of different domains. Using minimal is to avoid the domain with
 * small rmid accessing an invalid rmid.
 */
static unsigned int erdt_max_rmid;

/*
 * Used only by the limbo handler to round resctrl_rmid_realloc_threshold.
 * resctrl_rmid_realloc_threshold is a single global value, and
 * resctrl_arch_round_mon_val() takes no domain argument, so a single scale has
 * to be derived from the per-domain cmrc->up_scale. max() is chosen because the
 * rounding is a floor: a larger scale yields a slightly lower threshold, i.e. an
 * RMID has to drop to a slightly lower occupancy before it is reused.
 */
static unsigned int erdt_scale;

bool erdt_support(int flag)
{
	switch (flag) {
	case X86_FEATURE_CQM_OCCUP_LLC:
		return valid_subtbl_mask & BIT(ACPI_ERDT_TYPE_CMRC);
	default:
		return false;
	}
}

unsigned int erdt_get_max_rmid(void)
{
	return erdt_max_rmid;
}

unsigned int erdt_get_scale(void)
{
	/* Divided by snc_nodes_per_l3_cache, see erdt_read_l3_occupancy(). */
	return erdt_scale / snc_nodes_per_l3_cache;
}

static u32 cmrc_index_function_1(struct acpi_erdt_cmrc *cmrc, u32 rmid)
{
	/*
	 * MMIO_offset_for_RMID# =
	 *   (RMID / ClumpSize) * Stride +
	 *   (RMID % ClumpSize) * 8
	 */
	return (rmid / cmrc->clump_size) * cmrc->clump_stride +
	       (rmid % cmrc->clump_size) * 8;
}

static int erdt_read_l3_occupancy(const struct erdt_domain_info *d, u32 rmid, u64 *val)
{
	struct acpi_erdt_cmrc *cmrc;
	u64 l3_cmt_count;
	u32 offset;

	cmrc = d->cmrc;
	if (!cmrc)
		return -EIO;

	offset = cmrc_index_function_1(cmrc, rmid);
	/* Overflow of cmt_reg_size * SZ_4K already validated in erdt_ioremap(). */
	if (offset + sizeof(u64) > (u32)cmrc->cmt_reg_size * SZ_4K)
		return -EINVAL;

	l3_cmt_count = readq(d->base[ERDT_MMIO_CMRC_BASE] + offset);
	if ((cmrc->flags & CMRC_FLAG_UNAVAILABLE_BIT) &&
	    (l3_cmt_count & UNAVAILABLE_COUNTER))
		return -EINVAL;

	/*
	 * In legacy mode, scale is divided by snc_nodes_per_l3_cache to
	 * prevent over-calculation of aggregated monitor data.
	 * FIXME: This scaling factor needs to be revisited/tuned for future
	 * platforms that support both SNC and MMIO-based monitoring
	 * simultaneously.
	 */
	*val = l3_cmt_count * cmrc->up_scale / snc_nodes_per_l3_cache;

	return 0;
}

int erdt_mon_read(struct rdt_domain_hdr *hdr, enum resctrl_event_id evtid, u32 rmid, u64 *val)
{
	struct rdt_hw_l3_mon_domain *hw_dom;
	const struct erdt_domain_info *d;

	hw_dom = resctrl_to_arch_mon_dom(container_of(hdr, struct rdt_l3_mon_domain, hdr));
	d = hw_dom->d_info;
	if (!d)
		return -EIO;

	if (evtid == QOS_L3_OCCUP_EVENT_ID)
		return erdt_read_l3_occupancy(d, rmid, val);

	return -EIO;
}

static void __iomem *erdt_ioremap(resource_size_t base, u32 num_pages, const char *desc)
{
	void __iomem *addr;
	unsigned long size;

	if (check_mul_overflow(num_pages, SZ_4K, &size))
		return NULL;

	addr = ioremap(base, size);
	if (!addr)
		pr_warn(FW_BUG "ERDT: Failed to map %s at phys addr %pa (size: %u pages)\n",
			desc, &base, num_pages);

	return addr;
}

static void erdt_iounmap_domain(struct erdt_domain_info *domain)
{
	for (int i = 0; i < ERDT_MMIO_NUM_TYPES; i++) {
		if (domain->base[i]) {
			iounmap(domain->base[i]);
			domain->base[i] = NULL;
		}
	}
}

static void cleanup_one_domain(struct erdt_domain_info *d)
{
	erdt_iounmap_domain(d);
	kfree(d->cmrc);
	kfree(d);
}

/*
 * Save CACD information for this RMDD:
 * convert the X2APIC to CPU and save them in a mask.
 */
static __init int cacd_init(struct acpi_subtbl_hdr_16 *subtbl,
			    struct erdt_domain_info *domain_info)
{
	struct acpi_erdt_cacd *cacd = (struct acpi_erdt_cacd *)subtbl;
	unsigned int num_ids;
	int cpu;

	if (cacd->header.length < struct_size(cacd, X2APICIDS, 1)) {
		pr_warn(FW_BUG "Invalid x2apicid CACD table\n");
		return -EIO;
	}

	num_ids = (cacd->header.length - sizeof(*cacd)) / sizeof(cacd->X2APICIDS[0]);

	for (unsigned int i = 0; i < num_ids; i++) {
		cpu = topo_lookup_cpuid(cacd->X2APICIDS[i]);
		if (cpu < 0) {
			pr_warn(FW_BUG "Unknown x2apicid 0x%x\n", cacd->X2APICIDS[i]);
			return -EIO;
		}

		cpumask_set_cpu(cpu, &domain_info->cpu_mask);
	}

	return 0;
}

static __init int cmrc_init(struct acpi_subtbl_hdr_16 *subtbl,
			    struct erdt_domain_info *domain_info)
{
	struct acpi_erdt_cmrc *cmrc = (struct acpi_erdt_cmrc *)subtbl;

	if (cmrc->header.length < sizeof(*cmrc)) {
		pr_warn(FW_BUG "Truncated CMRC sub-table\n");
		return -EIO;
	}

	if (cmrc->index_fn != CMRC_SUPPORTED_INDEX_FN) {
		pr_info("Unsupported CMRC index function %u\n", cmrc->index_fn);
		return -EIO;
	}

	if (!cmrc->clump_size) {
		pr_warn(FW_BUG "CMRC clump_size is zero\n");
		return -EIO;
	}

	/* resctrl scales monitoring values with an unsigned int. */
	if (cmrc->up_scale > UINT_MAX) {
		pr_warn(FW_BUG "Insane CMRC up_scale value 0x%llx\n", cmrc->up_scale);
		return -EIO;
	}

	domain_info->base[ERDT_MMIO_CMRC_BASE] =
		erdt_ioremap(cmrc->cmt_reg_base, cmrc->cmt_reg_size, "CMRC base");
	if (!domain_info->base[ERDT_MMIO_CMRC_BASE])
		return -EIO;

	domain_info->cmrc = kmemdup(cmrc, cmrc->header.length, GFP_KERNEL);
	if (!domain_info->cmrc) {
		iounmap(domain_info->base[ERDT_MMIO_CMRC_BASE]);
		domain_info->base[ERDT_MMIO_CMRC_BASE] = NULL;
		return -ENOMEM;
	}

	erdt_scale = max(erdt_scale, cmrc->up_scale);

	return 0;
}

static inline struct acpi_subtbl_hdr_16 *rmdd_subtbl(struct acpi_erdt_rmdd *rmdd)
{
	return (void *)rmdd + sizeof(*rmdd);
}

static inline struct acpi_subtbl_hdr_16 *next_subtbl(struct acpi_subtbl_hdr_16 *subtbl)
{
	return (void *)subtbl + subtbl->length;
}

static inline bool subtbl_valid(void *end, struct acpi_subtbl_hdr_16 *subtbl)
{
	/* Ensure the header is within bounds before dereferencing it. */
	if ((void *)subtbl + sizeof(*subtbl) > end)
		return false;

	/* A sub-table must be at least as large as its header. */
	if (subtbl->length < sizeof(*subtbl))
		return false;

	/* The entire sub-table (including body) must fit within the parent. */
	if ((void *)subtbl + subtbl->length > end)
		return false;

	return true;
}

static __init bool parse_rmdd_table(struct acpi_subtbl_hdr_16 *rmdd_hdr)
{
	struct acpi_erdt_rmdd *rmdd = (struct acpi_erdt_rmdd *)rmdd_hdr;
	struct erdt_domain_info *domain_info;
	struct acpi_subtbl_hdr_16 *subtbl;
	u32 subtbl_mask = 0;

	if (rmdd->header.length < sizeof(*rmdd)) {
		pr_warn(FW_BUG "Invalid RMDD length %u bytes\n", rmdd->header.length);
		return false;
	}

	/* Quietly ignore non-CPU-based L3 domains */
	if (!(rmdd->flags & RMDD_FLAG_CPU_L3_DOMAIN))
		return true;

	domain_info = kzalloc_obj(*domain_info, GFP_KERNEL);
	if (!domain_info)
		return false;

	domain_info->dom_id = -1;

	domain_info->base[ERDT_MMIO_RMDD_CREG] =
		erdt_ioremap(rmdd->creg_base, rmdd->creg_size, "RMDD ctrl base");
	if (!domain_info->base[ERDT_MMIO_RMDD_CREG])
		goto cleanup;

	for (subtbl = rmdd_subtbl(rmdd);
	     subtbl_valid((void *)rmdd + rmdd->header.length, subtbl);
	     subtbl = next_subtbl(subtbl)) {
		switch (subtbl->type) {
		/* An RMDD table has one or more CACD sub-table(s) */
		case ACPI_ERDT_TYPE_CACD:
			if (cacd_init(subtbl, domain_info))
				goto cleanup;

			subtbl_mask |= BIT(ACPI_ERDT_TYPE_CACD);
			break;
		case ACPI_ERDT_TYPE_CMRC:
			/*
			 * Only one CMRC is supported per domain as there is no
			 * method to distinguish different CMRCs within a domain.
			 */
			if (subtbl_mask & BIT(ACPI_ERDT_TYPE_CMRC))
				break;

			if (cmrc_init(subtbl, domain_info))
				goto cleanup;

			subtbl_mask |= BIT(ACPI_ERDT_TYPE_CMRC);
			break;
		default:
			break;
		}
	}

	if (!subtbl_mask)
		goto cleanup;

	/*
	 * Require all RMDDs to support same set of sub-tables
	 */
	if (!valid_subtbl_mask) {
		valid_subtbl_mask = subtbl_mask;
		first_rmdd_domain_id = rmdd->domain_id;
	} else if (subtbl_mask != valid_subtbl_mask) {
		pr_warn(FW_BUG "RMDD %u sub-table set does not match the first RMDD %u\n",
			rmdd->domain_id, first_rmdd_domain_id);
		goto cleanup;
	}

	if (!rmdd->max_rmid) {
		pr_warn(FW_BUG "Unreasonable RMDD max_rmid %u\n", rmdd->max_rmid);
		goto cleanup;
	}
	domain_info->max_rmid = rmdd->max_rmid;

	if (!erdt_max_rmid)
		erdt_max_rmid = rmdd->max_rmid;
	else
		erdt_max_rmid = min(erdt_max_rmid, rmdd->max_rmid);

	list_add(&domain_info->entry, &domain_info_list);

	return true;

cleanup:
	cleanup_one_domain(domain_info);
	return false;
}

bool erdt_cpu_valid(int cpu)
{
	struct erdt_domain_info *d, *cpu_dom = NULL;
	int dom_id;

	/* Without ERDT there is no firmware topology to disagree with. */
	if (!erdt_enabled)
		return true;

	dom_id = get_cpu_cacheinfo_id(cpu, RESCTRL_L3_CACHE);
	if (dom_id < 0) {
		pr_warn(FW_BUG "Can't find l3 id for CPU:%d\n", cpu);
		return false;
	}

	/*
	 * Find the erdt_domain_info that contains this CPU, then bind that ERDT
	 * domain to this CPU's L3 id. A CPU whose L3 id does not match the binding
	 * of its ERDT domain cannot be covered by resctrl.
	 *
	 * For example, the CACD sub-tables report:
	 * domain0: CPU0, CPU2, domain1: CPU1, CPU3
	 * while CPUID/cacheinfo reports the L3 cache is shared by:
	 * id0: CPU0, CPU1, id1: CPU2, CPU3
	 * With the CPUs coming online in order, CPU0 binds domain0 to L3 id0 and
	 * CPU3 binds domain1 to L3 id1, so CPU1 and CPU2 are not covered by
	 * resctrl.
	 */
	list_for_each_entry(d, &domain_info_list, entry) {
		if (cpumask_test_cpu(cpu, &d->cpu_mask)) {
			cpu_dom = d;
			break;
		}
	}

	if (!cpu_dom) {
		pr_warn(FW_BUG "Cannot find the ERDT domain which has CPU%d\n", cpu);
		return false;
	}

	/* This ERDT domain is already bound to this CPU's L3 domain. */
	if (cpu_dom->dom_id == dom_id)
		return true;

	/*
	 * This ERDT domain is already bound to a different L3 domain. Rebinding it
	 * would leave two L3 domains reading the counters of one ERDT domain, so
	 * skip this CPU instead.
	 */
	if (cpu_dom->dom_id != -1) {
		pr_warn(FW_BUG "CPU%d's id=%d not equal to CACD domain(%*pbl) id=%d, skip this CPU\n",
			cpu, dom_id, cpumask_pr_args(&cpu_dom->cpu_mask), cpu_dom->dom_id);

		return false;
	}

	/*
	 * A possible new binding. Check if another ERDT domain shares the same
	 * l3 id. If yes, this is a conflict and this CPU should not be considered
	 * by resctrl.
	 */
	list_for_each_entry(d, &domain_info_list, entry) {
		if (d == cpu_dom)
			continue;

		if (d->dom_id == dom_id) {
			pr_warn(FW_BUG "CPU%d's id=%d is already used by CACD domain(%*pbl), skip this CPU\n",
				cpu, dom_id, cpumask_pr_args(&d->cpu_mask));

			return false;
		}
	}

	/* Eligible new binding, assign the l3 id. */
	cpu_dom->dom_id = dom_id;

	return true;
}

/*
 * Associate ERDT table information with this domain.
 */
void erdt_l3_mon_domain_setup(int id, struct rdt_domain_hdr *hdr)
{
	struct rdt_hw_l3_mon_domain *hw_dom;
	struct erdt_domain_info *d;

	if (!erdt_enabled)
		return;

	hw_dom = resctrl_to_arch_mon_dom(container_of(hdr, struct rdt_l3_mon_domain, hdr));

	list_for_each_entry(d, &domain_info_list, entry) {
		if (d->dom_id == id) {
			/* Assign the ERDT information to hw_dom */
			if (hw_dom->d_info) {
				pr_warn(FW_BUG "Duplicated ERDT domains are mapped to an existing l3 domain\n");
				return;
			}
			hw_dom->d_info = d;
			return;
		}
	}
}

void erdt_exit(void)
{
	struct erdt_domain_info *d, *tmp;

	list_for_each_entry_safe(d, tmp, &domain_info_list, entry) {
		list_del(&d->entry);
		cleanup_one_domain(d);
	}
	erdt_enabled = false;
	valid_subtbl_mask = 0;
	first_rmdd_domain_id = 0;
	erdt_max_rmid = 0;
}

static __init int enumerate_erdt_table(struct acpi_table_header *table_hdr)
{
	struct acpi_table_erdt *erdt = (struct acpi_table_erdt *)table_hdr;
	struct acpi_subtbl_hdr_16 *subtbl;

	if (erdt->header.revision != ERDT_VALID_VERSION) {
		pr_info("Unsupported ERDT table revision %u (expected %u)\n",
			erdt->header.revision, ERDT_VALID_VERSION);
		return -EINVAL;
	}

	if (erdt->header.length < sizeof(*erdt)) {
		pr_warn(FW_BUG "ERDT: Invalid table length %u bytes\n", erdt->header.length);
		return -EINVAL;
	}

	for (subtbl = (void *)erdt + sizeof(*erdt);
	     subtbl_valid((void *)erdt + erdt->header.length, subtbl);
	     subtbl = next_subtbl(subtbl)) {
		if (subtbl->type == ACPI_ERDT_TYPE_RMDD &&
		    !parse_rmdd_table(subtbl))
			goto cleanup;
	}

	if (list_empty(&domain_info_list))
		goto cleanup;

	erdt_enabled = true;

	return 0;

cleanup:
	erdt_exit();
	return -EINVAL;
}

int __init erdt_init(void)
{
	return acpi_table_parse(ACPI_SIG_ERDT, enumerate_erdt_table);
}
