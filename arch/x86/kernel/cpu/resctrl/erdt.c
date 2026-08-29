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
#define RMDD_FLAG_CPU_L3_DOMAIN		BIT(0)

/* Bitmask of valid sub-tables found in the first RMDD, used to ensure all RMDDs match. */
static u32 valid_subtbl_mask;

/* Domain ID of the first RMDD that established @valid_subtbl_mask, for diagnostics. */
static u16 first_rmdd_domain_id;

/*
 * The minimal max-rmid of different domains. Using minimal is to avoid the domain with
 * small rmid accessing an invalid rmid.
 */
static unsigned int erdt_max_rmid;

unsigned int erdt_get_max_rmid(void)
{
	return erdt_max_rmid;
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
