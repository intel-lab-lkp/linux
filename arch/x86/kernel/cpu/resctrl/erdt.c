// SPDX-License-Identifier: GPL-2.0-only
/*
 * Enhanced Resource Director Technology(ERDT)
 *
 * Copyright (C) 2026 Intel Corporation
 *
 */

#define pr_fmt(fmt)     "resctrl: " fmt

#include <linux/cleanup.h>
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/xarray.h>
#include <linux/resctrl.h>
#include <linux/acpi.h>
#include <asm/apic.h>
#include <asm/cpu_device_id.h>
#include "internal.h"

enum erdt_mmio_type {
	ERDT_MMIO_RMDD_CREG,
	ERDT_MMIO_CMRC_BASE,
	ERDT_MMIO_MAX
};

struct erdt_domain_info {
	void __iomem		*base[ERDT_MMIO_MAX];
	struct acpi_erdt_cmrc	*cmrc;
};

static bool erdt_enabled_flag;

static DEFINE_XARRAY(erdt_domain_xa);

#define ERDT_VALID_VERSION		1
#define RMDD_FLAG_CPU_DOMAIN		BIT(0)

static u32 valid_subtbl_mask;

bool erdt_enabled(void)
{
	return erdt_enabled_flag;
}

/**
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
 * Return: L3 cache ID for the first matching CPU, or -1 on failure.
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
		cpu = topo_lookup_cpuid(cacd->X2APICIDS[i]);
		if (cpu < 0) {
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

static void __iomem *erdt_ioremap_checked(phys_addr_t base, u32 size, const char *desc)
{
	void __iomem *addr = ioremap(base, size << 12);

	if (!addr) {
		pr_err("ERDT: Failed to map %s at phys addr %#llx (size: %u pages)\n",
		       desc, (unsigned long long)base, size);
	}
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
	kfree(d);
}

static __init bool cacd_init(struct erdt_domain_info *d, struct acpi_subtbl_hdr_16 *subtbl,
			     int *l3_cache_id)
{
	*l3_cache_id = get_l3_cache_id_from_cacd((struct acpi_erdt_cacd *)subtbl);

	return *l3_cache_id != -1;
}

static inline struct acpi_subtbl_hdr_16 *rmdd_subtbl(struct acpi_erdt_rmdd *rmdd)
{
	return (void *)rmdd + sizeof(*rmdd);
}

static inline struct acpi_subtbl_hdr_16 *next_subtbl(struct acpi_subtbl_hdr_16 *subtbl)
{
	return (void *)subtbl + subtbl->length;
}

static inline bool subtbl_valid(struct acpi_erdt_rmdd *rmdd, struct acpi_subtbl_hdr_16 *subtbl)
{
	void *rmdd_end = (void *)rmdd + rmdd->header.length;

	if (subtbl->length < sizeof(*subtbl))
		return false;

	if ((void *)subtbl + sizeof(*subtbl) > rmdd_end)
		return false;

	if ((void *)subtbl + subtbl->length > rmdd_end)
		return false;

	return true;
}

static __init bool parse_rmdd_entry(struct acpi_subtbl_hdr_16 *rmdd_hdr)
{
	struct erdt_domain_info *domain_info;
	struct acpi_subtbl_hdr_16 *subtbl;
	struct acpi_erdt_rmdd *rmdd;
	int l3_cache_id = -1;
	u32 subtbl_mask = 0;

	if (rmdd_hdr->length < sizeof(*rmdd)) {
		pr_info(FW_BUG "Invalid RMDD length %u\n", rmdd_hdr->length);
		return false;
	}

	rmdd = (struct acpi_erdt_rmdd *)rmdd_hdr;

	/* Quietly ignore non-CPU-based L3 domains */
	if (!(rmdd->flags & RMDD_FLAG_CPU_DOMAIN))
		return true;

	domain_info = kzalloc(sizeof(*domain_info), GFP_KERNEL);
	if (!domain_info)
		return false;

	domain_info->base[ERDT_MMIO_RMDD_CREG] =
		erdt_ioremap_checked(rmdd->creg_base, rmdd->creg_size, "RMDD ctrl base");
	if (!domain_info->base[ERDT_MMIO_RMDD_CREG])
		goto cleanup;

	for (subtbl = rmdd_subtbl(rmdd); subtbl_valid(rmdd, subtbl);
	     subtbl = next_subtbl(subtbl)) {
		switch (subtbl->type) {
		case ACPI_ERDT_TYPE_CACD:
			if (cacd_init(domain_info, subtbl, &l3_cache_id))
				subtbl_mask |= BIT(ACPI_ERDT_TYPE_CACD);
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

	erdt_enabled_flag = true;

	return 0;

cleanup:
	erdt_cleanup();
	return -EINVAL;
}

int __init erdt_init(void)
{
	return acpi_table_parse(ACPI_SIG_ERDT, enumerate_erdt_table);
}

void erdt_exit(void)
{
	erdt_cleanup();
}
