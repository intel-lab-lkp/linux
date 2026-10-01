// SPDX-License-Identifier: GPL-2.0-only
/*
 * TLBI domains (FEAT_TLBID) support
 *
 * Copyright (C) 2026 Arm Ltd.
 * Author: Kristina Martsenko <kristina.martsenko@arm.com>
 */

#include <linux/acpi.h>
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/bitmap.h>
#include <linux/efi.h>
#include <linux/jump_label.h>
#include <linux/mm_types.h>
#include <linux/slab.h>
#include <linux/sort.h>

#include <asm/mmu.h>

DEFINE_STATIC_KEY_FALSE(tlbid_enabled);

static u16 *domain_ids;
static unsigned long **cpu_domains;
static int domain_count;

void update_tlbid_domains(struct mm_struct *mm, unsigned int cpu)
{
	if (!system_supports_tlbid())
		return;

	for (int i = 0; i < BITS_TO_U64(domain_count); i++)
		atomic64_and(cpu_domains[cpu][i], &mm->context.tlbid_domains[i]);
}

int init_tlbid_context(struct mm_struct *mm)
{
	bool is_efi = mm_is_efi(mm);
	atomic64_t *domains;
	int ndomains, nwords;

	/* TLBI domain support not yet known at EFI init time */
	if (!system_supports_tlbid() && !is_efi)
		return 0;

	ndomains = is_efi ? MAX_TLBI_DOMAINS : domain_count;
	nwords = BITS_TO_U64(ndomains);
	domains = kcalloc(nwords, sizeof(*domains), GFP_KERNEL);
	if (!domains)
		return -ENOMEM;

	if (is_efi) {
		/*
		 * Use a safe domain since updates to domains would be lost if
		 * efi_mm were installed before TLBID is enabled.
		 */
		atomic64_set(domains, 1);
	} else {
		for (int i = 0; i < nwords; i++)
			atomic64_set(&domains[i], -1);
	}

	mm->context.tlbid_domains = domains;

	return 0;
}

void destroy_tlbid_context(struct mm_struct *mm)
{
	if (system_supports_tlbid())
		kfree(mm->context.tlbid_domains);
}

static int __init init_cpu_domain_bitmaps(struct tlbi_domain *domains)
{
	int cpu;

	cpu_domains = kcalloc(nr_cpu_ids, sizeof(*cpu_domains), GFP_KERNEL);
	if (!cpu_domains)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		cpu_domains[cpu] = bitmap_zalloc(domain_count, GFP_KERNEL);
		if (!cpu_domains[cpu])
			return -ENOMEM;

		for (int i = 0; i < domain_count; i++) {
			if (cpumask_test_cpu(cpu, domains[i].cpus))
				set_bit(i, cpu_domains[cpu]);
		}
	}

	return 0;
}

static int __init store_domain_ids(struct tlbi_domain *domains)
{
	domain_ids = kmalloc_array(domain_count, sizeof(*domain_ids),
				   GFP_KERNEL);
	if (!domain_ids)
		return -ENOMEM;

	for (int i = 0; i < domain_count; i++)
		domain_ids[i] = domains[i].id;

	return 0;
}

static int __init tlbi_domain_cmp(const void *a, const void *b)
{
	const struct tlbi_domain *d1 = a;
	const struct tlbi_domain *d2 = b;

	/* Sort domains from largest to smallest */
	return cmp_int(cpumask_weight(d2->cpus), cpumask_weight(d1->cpus));
}

static struct tlbi_domain *__init sort_domains_by_size(struct tlbi_domain *domains)
{
	struct tlbi_domain *domains_sorted;

	domains_sorted = kmemdup(domains, domain_count * sizeof(*domains),
				 GFP_KERNEL);
	if (!domains_sorted)
		return NULL;

	sort(domains_sorted, domain_count, sizeof(*domains_sorted),
	     tlbi_domain_cmp, NULL);

	return domains_sorted;
}

int __init arm64_tlbid_init(struct tlbi_domain *domains_in, int count)
{
	struct tlbi_domain *domains;
	int ret;

	domain_count = count;

	domains = sort_domains_by_size(domains_in);
	if (!domains)
		return -ENOMEM;

	ret = store_domain_ids(domains);
	if (ret)
		return ret;

	ret = init_cpu_domain_bitmaps(domains);
	if (ret)
		return ret;

	static_branch_enable(&tlbid_enabled);

	kfree(domains);
	return 0;
}
