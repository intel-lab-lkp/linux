// SPDX-License-Identifier: GPL-2.0-only
/*
 * Enhanced Resource Director Technology(ERDT)
 *
 * Copyright (C) 2026 Intel Corporation
 *
 */

#define pr_fmt(fmt)     "resctrl: " fmt

#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/xarray.h>
#include <linux/resctrl.h>
#include <linux/acpi.h>
#include <asm/cpu.h>
#include <asm/apic.h>
#include <asm/cpu_device_id.h>
#include "internal.h"

enum erdt_mmio_type {
	ERDT_MMIO_RMDD_CREG,
	ERDT_MMIO_CMRC_BASE,
	ERDT_MMIO_MAX
};

struct erdt_domain_info {
	struct acpi_erdt_cacd *cacd;
	struct acpi_erdt_cmrc *cmrc;
	/* MMIO  address */
	void __iomem *base[ERDT_MMIO_MAX];
};

/* true if ERDT table is present and valid */
static bool erdt_available;

/* Global variable to hold ERDT ACPI table information for later processing */
static DEFINE_XARRAY(erdt_domain_xa); /* Indexed by L3 cache ID */

#define ERDT_VALID_VERSION 1
#define CMRC_VALID_INDEX_FUNC_VERSION 1
#define UNAVAILABLE_COUNTER    BIT_ULL(63)

static u32 valid_subtbl_mask;

/*
 * erdt_enabled - Check if the ERDT table is present and enabled
 */
bool erdt_enabled(void)
{
	return erdt_available;
}

/*
 * lookup_logical_cpu_by_x2apicid - Map x2APIC ID to logical CPU number
 */
static __init int lookup_logical_cpu_by_x2apicid(u32 x2apicid)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (cpu_physical_id(cpu) == x2apicid)
			return cpu;
	}

	return -1;
}

static void __iomem *cmrc_index_function_1(struct erdt_domain_info *d,
					   struct acpi_erdt_cmrc *cmrc, int rmid)
{
	u16 clump_size, stride_size;
	void __iomem *vaddr;

	clump_size = cmrc->clump_size;
	stride_size = cmrc->clump_stride;

	/*
	 * MMIO_ADDRESS_for_RMID# = CMRC Base +
	 *   (RMID / ClumpSize) * Stride +
	 *   (RMID % ClumpSize) * 8
	 */
	vaddr = d->base[ERDT_MMIO_CMRC_BASE] +
		(rmid / clump_size) * stride_size +
		(rmid % clump_size) * 8;

	return vaddr;
}

/*
 * erdt_read_l3_occupancy - Read L3 occupancy count for a given RMID
 * @d:    Pointer to the ERDT domain info
 * @rmid: Resource Monitoring ID to read occupancy for
 *
 * Calculates the MMIO address using clump and stride information
 * from the CMRC ACPI structure and reads the L3 cache occupancy
 * count for the given RMID. The raw value is scaled using the
 * up_scale factor provided by firmware.
 *
 * Return: 0 for success, error code for other cases.
 */
static int erdt_read_l3_occupancy(struct erdt_domain_info *d, int rmid,
				  u64 *val)
{
	struct acpi_erdt_cmrc *cmrc;
	void __iomem *vaddr;
	u64 l3_cmt_count;
	u32 offset;

	cmrc = d->cmrc;
	if (!cmrc)
		return -EIO;

	offset = (rmid / cmrc->clump_size) * cmrc->clump_stride +
		 (rmid % cmrc->clump_size) * 8;
	if (offset + sizeof(u64) > (u32)cmrc->cmt_reg_size << 12)
		return -EINVAL;

	vaddr = cmrc_index_function_1(d, cmrc, rmid);

	l3_cmt_count = readq(vaddr);
	if (l3_cmt_count & UNAVAILABLE_COUNTER)
		return -EINVAL;

	*val = l3_cmt_count * cmrc->up_scale;

	return 0;
}

/*
 * erdt_mon_read - Read monitoring data for a given domain and RMID
 * @hdr:    Domain header
 * @ev_id:  Monitoring event ID (e.g. QOS_L3_OCCUP_EVENT_ID)
 * @rmid:   Resource Monitoring ID for which to read the data
 * @val:    Store the read data
 *
 * Looks up the domain by domid and dispatches the read request
 * to the appropriate helper based on the event type.
 * Currently supports only L3 occupancy monitoring.
 *
 * Return 0 on succeed, error code otherwise.
 */
int erdt_mon_read(struct rdt_domain_hdr *hdr, int ev_id, int rmid,
		  u64 *val)
{
	struct erdt_domain_info *d;

	d = xa_load(&erdt_domain_xa, hdr->id);
	if (!d)
		return -EIO;

	if (ev_id == QOS_L3_OCCUP_EVENT_ID)
		return erdt_read_l3_occupancy(d, rmid, val);

	return -EINVAL;
}

/*
 * get_l3_cache_id_from_cacd - Resolve L3 cache ID from CACD subtable
 * @cacd: Pointer to the ACPI ERDT CACD structure
 *
 * Parses the X2APIC ID list in the given CACD subtable to
 * identify an online logical CPU and uses it to query the associated
 * L3 cache ID. The first valid CPU found is used for this lookup.
 *
 * The L3 cache ID is used as a unique domain key for ERDT domain
 * registration and lookup.
 *
 * Return:
 *   L3 cache ID for the first matching CPU, or
 *  -1 if no valid CPU or L3 cache ID could be determined.
 */
static __init int get_l3_cache_id_from_cacd(struct acpi_erdt_cacd *cacd)
{
	int num_ids, cpu, online_cpu = -1, cache_id = -1, tmp;
	struct cacheinfo *ci;

	if (cacd->header.length < sizeof(*cacd) + sizeof(cacd->X2APICIDS[0])) {
		pr_warn(FW_BUG "Invalid x2apicid CACD table\n");
		return -1;
	}

	num_ids = (cacd->header.length - sizeof(*cacd)) / sizeof(cacd->X2APICIDS[0]);

	guard(cpus_read_lock)();

	for (int i = 0; i < num_ids; i++) {
		cpu = lookup_logical_cpu_by_x2apicid(cacd->X2APICIDS[i]);
		if (cpu == -1) {
			pr_warn(FW_BUG "Unknown x2apicid 0x%x\n", cacd->X2APICIDS[i]);

			return -1;
		}

		if (!cpu_online(cpu))
			continue;

		tmp = get_cpu_cacheinfo_id(cpu, RESCTRL_L3_CACHE);
		if (tmp == -1) {
			pr_warn(FW_BUG "Can not find L3 cache id for CPU%d\n", cpu);
			return -1;
		}

		if (cache_id == -1)
			cache_id = tmp;

		if (tmp != cache_id) {
			pr_warn(FW_BUG "CACD references multiple L3 cache instances\n");
			return -1;
		}
		online_cpu = cpu;
	}

	if (online_cpu == -1)
		return -1;

	/*
	 * Check if CACD lists all CPUs in the LLC domain.
	 */
	ci = get_cpu_cacheinfo_level(online_cpu, RESCTRL_L3_CACHE);
	if (!ci || num_ids != cpumask_weight(&ci->shared_cpu_map)) {
		pr_warn(FW_BUG "CACD does not list all the CPUs in L3 domain\n");
		return -1;
	}

	return cache_id;
}

static void __iomem *erdt_ioremap_checked(phys_addr_t base, u32 size,
					  const char *desc)
{
	void __iomem *addr = ioremap(base, size << 12);

	if (!addr)
		pr_err("ERDT: Failed to map %s at phys addr %#llx (size: %u pages)\n",
		       desc, (unsigned long long)base, size);
	return addr;
}

static void erdt_iounmap_domain(struct erdt_domain_info *domain)
{
	for (int i = 0; i < ERDT_MMIO_MAX; i++) {
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

static __init bool cacd_init(struct erdt_domain_info *d,
			     struct acpi_subtbl_hdr_16 *subtbl,
			     int *l3_cache_id)
{
	*l3_cache_id = get_l3_cache_id_from_cacd((struct acpi_erdt_cacd *)subtbl);

	return *l3_cache_id != -1;
}

static __init bool cmrc_init(struct erdt_domain_info *d, struct acpi_subtbl_hdr_16 *subtbl)
{
	struct acpi_erdt_cmrc *cmrc = (struct acpi_erdt_cmrc *)subtbl;

	if (subtbl->length < sizeof(*cmrc)) {
		pr_warn(FW_BUG "Truncated CMRC subtable\n");
		return false;
	}

	if (cmrc->index_fn != CMRC_VALID_INDEX_FUNC_VERSION) {
		pr_info("Unknown CMRC index function %d\n", cmrc->index_fn);
		return false;
	}

	if (!cmrc->clump_size) {
		pr_warn(FW_BUG "CMRC clump_size is zero\n");
		return false;
	}

	d->base[ERDT_MMIO_CMRC_BASE] = erdt_ioremap_checked(cmrc->cmt_reg_base,
							    cmrc->cmt_reg_size, "CMRC base");
	if (!d->base[ERDT_MMIO_CMRC_BASE])
		return false;

	d->cmrc = kmemdup(cmrc, subtbl->length, GFP_KERNEL);
	if (!d->cmrc) {
		iounmap(d->base[ERDT_MMIO_CMRC_BASE]);
		d->base[ERDT_MMIO_CMRC_BASE] = NULL;
		return false;
	}

	return true;
}

static __init bool parse_rmdd_entry(struct acpi_subtbl_hdr_16 *rmdd_hdr)
{
	struct acpi_erdt_rmdd *rmdd;
	struct erdt_domain_info *domain_info;
	struct acpi_subtbl_hdr_16 *subtbl;
	int l3_cache_id = -1;
	u32 subtbl_mask = 0;
	void *rmdd_end;

	if (rmdd_hdr->length < sizeof(*rmdd)) {
		pr_info(FW_BUG "Invalid RMDD length %u\n", rmdd_hdr->length);
		return false;
	}

	rmdd = (struct acpi_erdt_rmdd *)rmdd_hdr;

	/* Quietly ignore non-CPU-based L3 domains (bit 0 set) */
	if (!(rmdd->flags & 0x1))
		return true;

	domain_info = kzalloc(sizeof(*domain_info), GFP_KERNEL);
	if (!domain_info)
		return false;

	domain_info->base[ERDT_MMIO_RMDD_CREG] = erdt_ioremap_checked(rmdd->creg_base, rmdd->creg_size,
								      "RMDD ctrl base");
	if (!domain_info->base[ERDT_MMIO_RMDD_CREG])
		goto cleanup;

	rmdd_end = (void *)rmdd + rmdd->header.length;

	/* Iterate through all sub-structures inside this RMDD block */
	for (subtbl = (void *)rmdd + sizeof(*rmdd);
	     (void *)subtbl + sizeof(*subtbl) <= rmdd_end;
	     subtbl = (void *)subtbl + subtbl->length) {
		if (subtbl->length < sizeof(*subtbl) ||
		    (void *)subtbl + subtbl->length > rmdd_end) {
			pr_info("ERDT: Invalid subtable length in RMDD domain %d\n",
				rmdd->domain_id);

			goto cleanup;
		}

		switch (subtbl->type) {
		case ACPI_ERDT_TYPE_CACD:
			if (cacd_init(domain_info, subtbl, &l3_cache_id))
				subtbl_mask |= BIT(ACPI_ERDT_TYPE_CACD);
			break;
		case ACPI_ERDT_TYPE_CMRC:
			if (cmrc_init(domain_info, subtbl))
				subtbl_mask |= BIT(ACPI_ERDT_TYPE_CMRC);
			break;
		default:
			break;
		}
	}

	if (l3_cache_id == -1) {
		pr_info("ERDT: Failed to resolve L3 cache ID for RMDD domain %d\n",
			rmdd->domain_id);

		goto cleanup;
	}

	/* Require all RMDDs to support same set of sub-tables */
	if (!valid_subtbl_mask) {
		valid_subtbl_mask = subtbl_mask;
	} else if (subtbl_mask != valid_subtbl_mask) {
		pr_info(FW_BUG "Mismatch domain\n");
		goto cleanup;
	}

	if (xa_insert(&erdt_domain_xa, l3_cache_id, domain_info, GFP_KERNEL)) {
		pr_info("ERDT: Failed to store domain info for RMDD domain %d\n",
			rmdd->domain_id);
		goto cleanup;
	}

	return true;

cleanup:
	cleanup_one_domain(domain_info);
	return false;
}

static void erdt_cleanup(void)
{
	struct erdt_domain_info *d;
	unsigned long index;

	xa_for_each(&erdt_domain_xa, index, d)
		cleanup_one_domain(d);
	xa_destroy(&erdt_domain_xa);
}

/*
 * enumerate_erdt_table - Store pointer to ERDT and begin domain parsing
 */
static __init int enumerate_erdt_table(struct acpi_table_header *table_hdr)
{
	struct acpi_table_erdt *erdt = (struct acpi_table_erdt *)table_hdr;
	struct acpi_subtbl_hdr_16 *subtbl;
	void *table_end;

	if (erdt->header.revision != ERDT_VALID_VERSION) {
		pr_info("Unknown ERDT table revision %d\n", erdt->header.revision);
		return -EINVAL;
	}

	if (erdt->header.length < sizeof(*erdt)) {
		pr_info(FW_BUG "ERDT: Invalid table length %u\n", erdt->header.length);
		return -EINVAL;
	}

	subtbl = (void *)erdt + sizeof(struct acpi_table_erdt);
	table_end = (void *)erdt + erdt->header.length;

	while ((void *)subtbl + sizeof(*subtbl) <= table_end) {
		if (subtbl->length < sizeof(*subtbl) ||
		    (void *)subtbl + subtbl->length > table_end) {
			pr_info("ERDT: Invalid subtable length\n");
			goto cleanup;
		}

		if (subtbl->type == ACPI_ERDT_TYPE_RMDD)
			if (!parse_rmdd_entry(subtbl))
				goto cleanup;

		subtbl = (void *)subtbl + subtbl->length;
	}

	erdt_available = true;

	return 0;

cleanup:
	erdt_cleanup();
	return -EINVAL;
}

/*
 * erdt_init - ACPI ERDT table initialization entry point
 */
int __init erdt_init(void)
{
	return acpi_table_parse(ACPI_SIG_ERDT, enumerate_erdt_table);
}

void __exit erdt_exit(void)
{
	erdt_cleanup();
}
