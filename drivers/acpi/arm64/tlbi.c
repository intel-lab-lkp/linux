// SPDX-License-Identifier: GPL-2.0-only
/*
 * Arm TLBI Domain Table (TLBI) support
 *
 * Copyright (C) 2026 Arm Ltd.
 * Author: Kristina Martsenko <kristina.martsenko@arm.com>
 */

#define pr_fmt(fmt)	"ACPI: TLBI: " fmt

#include <linux/acpi.h>
#include <linux/cpufeature.h>
#include <linux/limits.h>
#include <linux/slab.h>

#include "init.h"

#define MAX_HIERARCHY_DEPTH 10

struct acpi_table_tlbi *tlbi;
static struct tlbi_domain *domains;
static int domain_count;

static int __init validate_tlbi_domains(void)
{
	u64 tlbididr = read_sanitised_ftr_reg(SYS_TLBIDIDR_EL1);
	int domain_bits = FIELD_GET(TLBIDIDR_EL1_NIS, tlbididr);
	int domain0_idx = -1;
	int cpu;

	if (domain_bits > 16)
		return -EINVAL;

	for (int i = 0; i < domain_count; i++) {
		u16 max_domain_id = (1 << domain_bits) - 1;

		if (domains[i].id > max_domain_id &&
		    !cpumask_empty(domains[i].cpus))
			return -EINVAL;

		if (!domains[i].id)
			domain0_idx = i;
	}

	if (domain0_idx == -1)
		return -EINVAL;

	for_each_possible_cpu(cpu) {
		if (!cpumask_test_cpu(cpu, domains[domain0_idx].cpus))
			return -EINVAL;
	}

	return 0;
}

static struct acpi_tlbi_descendant *__init tlbi_get_descendant(u32 offset)
{
	struct acpi_tlbi_descendant *descendant;

	descendant = ACPI_ADD_PTR(struct acpi_tlbi_descendant, tlbi, offset);
	if (descendant->length < sizeof(*descendant) ||
	    offset + descendant->length > tlbi->header.length)
		return NULL;

	return descendant;
}

static struct acpi_tlbi_domain *__init tlbi_get_domain(u32 offset)
{
	struct acpi_tlbi_domain *domain;

	domain = ACPI_ADD_PTR(struct acpi_tlbi_domain, tlbi, offset);
	if (domain->length < sizeof(*domain) ||
	    offset + domain->length > tlbi->header.length)
		return NULL;

	return domain;
}

static int __init parse_tlbi_domain(struct acpi_tlbi_domain *domain,
				    struct cpumask *cpus, int depth)
{
	u32 offset = domain->descendants_offset;

	if (depth > MAX_HIERARCHY_DEPTH)
		return -EINVAL;

	for (int i = 0; i < domain->descendant_count; i++) {
		struct acpi_tlbi_descendant *desc = tlbi_get_descendant(offset);

		if (!desc)
			return -EINVAL;

		switch (desc->type) {
		case ACPI_TLBI_DESCENDANT_DOMAIN: {
			struct acpi_tlbi_domain *child;
			int ret;

			child = tlbi_get_domain(desc->identifier);
			if (!child)
				return -EINVAL;

			ret = parse_tlbi_domain(child, cpus, depth + 1);
			if (ret)
				return ret;
			break;
		}
		case ACPI_TLBI_DESCENDANT_CPU: {
			int cpu = get_cpu_for_acpi_id(desc->identifier);

			if (cpu >= 0 && cpu_possible(cpu))
				cpumask_set_cpu(cpu, cpus);
			break;
		}
		case ACPI_TLBI_DESCENDANT_SMMU:
			break;
		default:
			return -EINVAL;
		}

		offset += desc->length;
	}

	return 0;
}

static int __init parse_tlbi_table(void)
{
	struct acpi_table_header *table __free(acpi_put_table) =
		acpi_get_table_pointer(ACPI_SIG_TLBI, 0);
	u32 offset;
	int ret;

	if (IS_ERR(table))
		return PTR_ERR(table);

	tlbi = (struct acpi_table_tlbi *)table;
	if (tlbi->header.length < sizeof(*tlbi) ||
	    tlbi->domain_count > MAX_TLBI_DOMAINS)
		return -EINVAL;
	if (!tlbi->domain_count)
		return -ENOENT;

	domain_count = tlbi->domain_count;
	domains = kcalloc(domain_count, sizeof(*domains), GFP_KERNEL);
	if (!domains)
		return -ENOMEM;

	offset = tlbi->domains_offset;

	for (int i = 0; i < domain_count; i++) {
		struct acpi_tlbi_domain *domain = tlbi_get_domain(offset);

		if (!domain || domain->id > U16_MAX)
			return -EINVAL;

		domains[i].id = domain->id;
		if (!zalloc_cpumask_var(&domains[i].cpus, GFP_KERNEL))
			return -ENOMEM;

		ret = parse_tlbi_domain(domain, domains[i].cpus, 0);
		if (ret)
			return ret;

		offset += domain->length;
	}

	ret = validate_tlbi_domains();
	if (ret)
		return ret;

	return 0;
}

void __init acpi_tlbi_init(void)
{
	int ret;

	if (!cpus_have_final_cap(ARM64_HAS_TLBID))
		return;

	ret = parse_tlbi_table();
	if (ret) {
		if (ret != -ENOENT)
			pr_err("Failed to parse ACPI table\n");
		return;
	}
}
