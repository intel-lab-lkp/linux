// SPDX-License-Identifier: GPL-2.0
/*
 * Shared GHES helpers for firmware-first CPER error handling.
 *
 * This file holds the GHES helper code that is shared by the in-tree GHES
 * driver and by other firmware-first error sources that reuse the same CPER
 * handling flow.
 *
 * Derived from the ACPI APEI GHES driver.
 *
 * Copyright 2010,2011 Intel Corp.
 *   Author: Huang Ying <ying.huang@intel.com>
 */

#include <linux/err.h>
#include <linux/genalloc.h>
#include <linux/irq_work.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/notifier.h>
#include <linux/llist.h>
#include <linux/ratelimit.h>
#include <linux/rcupdate.h>
#include <linux/rculist.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>

#include <acpi/apei.h>
#include <acpi/ghes_cper.h>

#include <asm/fixmap.h>
#include <asm/tlbflush.h>

#include "apei-internal.h"

static struct ghes_estatus_cache __rcu *ghes_estatus_caches[GHES_ESTATUS_CACHES_SIZE];
static atomic_t ghes_estatus_cache_alloced;

static void __iomem *ghes_map(u64 pfn, enum fixed_addresses fixmap_idx)
{
	phys_addr_t paddr;
	pgprot_t prot;

	paddr = PFN_PHYS(pfn);
	prot = arch_apei_get_mem_attribute(paddr);
	__set_fixmap(fixmap_idx, paddr, prot);

	return (void __iomem *) __fix_to_virt(fixmap_idx);
}

static void ghes_unmap(void __iomem *vaddr, enum fixed_addresses fixmap_idx)
{
	int _idx = virt_to_fix((unsigned long)vaddr);

	WARN_ON_ONCE(fixmap_idx != _idx);
	clear_fixmap(fixmap_idx);
}

static void ghes_ack_error(struct acpi_hest_generic_v2 *gv2)
{
	int rc;
	u64 val = 0;

	rc = apei_read(&val, &gv2->read_ack_register);
	if (rc)
		return;

	val &= gv2->read_ack_preserve << gv2->read_ack_register.bit_offset;
	val |= gv2->read_ack_write    << gv2->read_ack_register.bit_offset;

	apei_write(val, &gv2->read_ack_register);
}

static int map_gen_v2(struct ghes *ghes)
{
	return apei_map_generic_address(&ghes->generic_v2->read_ack_register);
}

static void unmap_gen_v2(struct ghes *ghes)
{
	apei_unmap_generic_address(&ghes->generic_v2->read_ack_register);
}

struct ghes *ghes_new(struct acpi_hest_generic *generic)
{
	struct ghes *ghes;
	unsigned int error_block_length;
	int rc;

	ghes = kzalloc_obj(*ghes);
	if (!ghes)
		return ERR_PTR(-ENOMEM);

	ghes->generic = generic;
	if (is_hest_type_generic_v2(ghes)) {
		rc = map_gen_v2(ghes);
		if (rc)
			goto err_free;
	}

	rc = apei_map_generic_address(&generic->error_status_address);
	if (rc)
		goto err_unmap_read_ack_addr;
	error_block_length = generic->error_block_length;
	if (error_block_length > GHES_ESTATUS_MAX_SIZE) {
		pr_warn(FW_WARN GHES_PFX
			"Error status block length is too long: %u for "
			"generic hardware error source: %d.\n",
			error_block_length, generic->header.source_id);
		error_block_length = GHES_ESTATUS_MAX_SIZE;
	}
	ghes->estatus = kmalloc(error_block_length, GFP_KERNEL);
	ghes->estatus_length = error_block_length;
	if (!ghes->estatus) {
		rc = -ENOMEM;
		goto err_unmap_status_addr;
	}

	return ghes;

err_unmap_status_addr:
	apei_unmap_generic_address(&generic->error_status_address);
err_unmap_read_ack_addr:
	if (is_hest_type_generic_v2(ghes))
		unmap_gen_v2(ghes);
err_free:
	kfree(ghes);
	return ERR_PTR(rc);
}

void ghes_fini(struct ghes *ghes)
{
	kfree(ghes->estatus);
	apei_unmap_generic_address(&ghes->generic->error_status_address);
	if (is_hest_type_generic_v2(ghes))
		unmap_gen_v2(ghes);
}

static void ghes_copy_tofrom_phys(void *buffer, u64 paddr, u32 len,
				  int from_phys,
				  enum fixed_addresses fixmap_idx)
{
	void __iomem *vaddr;
	u64 offset;
	u32 trunk;

	while (len > 0) {
		offset = paddr - (paddr & PAGE_MASK);
		vaddr = ghes_map(PHYS_PFN(paddr), fixmap_idx);
		trunk = PAGE_SIZE - offset;
		trunk = min(trunk, len);
		if (from_phys)
			memcpy_fromio(buffer, vaddr + offset, trunk);
		else
			memcpy_toio(vaddr + offset, buffer, trunk);
		len -= trunk;
		paddr += trunk;
		buffer += trunk;
		ghes_unmap(vaddr, fixmap_idx);
	}
}

/* Check the top-level record header has an appropriate size. */
int __ghes_check_estatus(struct ghes *ghes,
			 struct acpi_hest_generic_status *estatus)
{
	u32 len = cper_estatus_len(estatus);
	u32 max_len = min(ghes->generic->error_block_length,
			  ghes->estatus_length);

	if (len < sizeof(*estatus)) {
		pr_warn_ratelimited(FW_WARN GHES_PFX "Truncated error status block!\n");
		return -EIO;
	}

	if (!len || len > max_len) {
		pr_warn_ratelimited(FW_WARN GHES_PFX "Invalid error status block length!\n");
		return -EIO;
	}

	if (cper_estatus_check_header(estatus)) {
		pr_warn_ratelimited(FW_WARN GHES_PFX "Invalid CPER header!\n");
		return -EIO;
	}

	return 0;
}

/* Read the CPER block, returning its address, and header in estatus. */
int __ghes_peek_estatus(struct ghes *ghes,
			struct acpi_hest_generic_status *estatus,
			u64 *buf_paddr, enum fixed_addresses fixmap_idx)
{
	struct acpi_hest_generic *g = ghes->generic;
	int rc;

	rc = apei_read(buf_paddr, &g->error_status_address);
	if (rc) {
		*buf_paddr = 0;
		pr_warn_ratelimited(FW_WARN GHES_PFX
				    "Failed to read error status block address for hardware error source: %d.\n",
				   g->header.source_id);
		return -EIO;
	}
	if (!*buf_paddr)
		return -ENOENT;

	ghes_copy_tofrom_phys(estatus, *buf_paddr, sizeof(*estatus), 1,
			      fixmap_idx);
	if (!estatus->block_status) {
		*buf_paddr = 0;
		return -ENOENT;
	}

	return 0;
}

int __ghes_read_estatus(struct acpi_hest_generic_status *estatus,
			u64 buf_paddr, enum fixed_addresses fixmap_idx,
			size_t buf_len)
{
	ghes_copy_tofrom_phys(estatus, buf_paddr, buf_len, 1, fixmap_idx);
	if (cper_estatus_check(estatus)) {
		pr_warn_ratelimited(FW_WARN GHES_PFX
				    "Failed to read error status block!\n");
		return -EIO;
	}

	return 0;
}

int ghes_read_estatus(struct ghes *ghes,
		      struct acpi_hest_generic_status *estatus,
		      u64 *buf_paddr, enum fixed_addresses fixmap_idx)
{
	int rc;

	rc = __ghes_peek_estatus(ghes, estatus, buf_paddr, fixmap_idx);
	if (rc)
		return rc;

	rc = __ghes_check_estatus(ghes, estatus);
	if (rc)
		return rc;

	return __ghes_read_estatus(estatus, *buf_paddr, fixmap_idx,
				   cper_estatus_len(estatus));
}

void ghes_clear_estatus(struct ghes *ghes,
			struct acpi_hest_generic_status *estatus,
			u64 buf_paddr, enum fixed_addresses fixmap_idx)
{
	estatus->block_status = 0;

	if (!buf_paddr)
		return;

	ghes_copy_tofrom_phys(estatus, buf_paddr,
			      sizeof(estatus->block_status), 0,
			      fixmap_idx);

	/*
	 * GHESv2 type HEST entries introduce support for error acknowledgment,
	 * so only acknowledge the error if this support is present.
	 */
	if (is_hest_type_generic_v2(ghes))
		ghes_ack_error(ghes->generic_v2);
}

static BLOCKING_NOTIFIER_HEAD(vendor_record_notify_list);

int ghes_register_vendor_record_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&vendor_record_notify_list, nb);
}
EXPORT_SYMBOL_GPL(ghes_register_vendor_record_notifier);

void ghes_unregister_vendor_record_notifier(struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&vendor_record_notify_list, nb);
}
EXPORT_SYMBOL_GPL(ghes_unregister_vendor_record_notifier);

static void ghes_vendor_record_work_func(struct work_struct *work)
{
	struct ghes_vendor_record_entry *entry;
	struct acpi_hest_generic_data *gdata;
	u32 len;

	entry = container_of(work, struct ghes_vendor_record_entry, work);
	gdata = GHES_GDATA_FROM_VENDOR_ENTRY(entry);

	blocking_notifier_call_chain(&vendor_record_notify_list,
				     entry->error_severity, gdata);

	len = GHES_VENDOR_ENTRY_LEN(acpi_hest_get_record_size(gdata));
	gen_pool_free(ghes_estatus_pool, (unsigned long)entry, len);
}

void ghes_defer_non_standard_event(struct acpi_hest_generic_data *gdata,
				   int sev)
{
	struct acpi_hest_generic_data *copied_gdata;
	struct ghes_vendor_record_entry *entry;
	u32 len;

	len = GHES_VENDOR_ENTRY_LEN(acpi_hest_get_record_size(gdata));
	entry = (void *)gen_pool_alloc(ghes_estatus_pool, len);
	if (!entry)
		return;

	copied_gdata = GHES_GDATA_FROM_VENDOR_ENTRY(entry);
	memcpy(copied_gdata, gdata, acpi_hest_get_record_size(gdata));
	entry->error_severity = sev;

	INIT_WORK(&entry->work, ghes_vendor_record_work_func);
	schedule_work(&entry->work);
}

/*
 * GHES error status reporting throttle, to report more kinds of
 * errors, instead of just most frequently occurred errors.
 */
int ghes_estatus_cached(struct acpi_hest_generic_status *estatus)
{
	u32 len;
	int i, cached = 0;
	unsigned long long now;
	struct ghes_estatus_cache *cache;
	struct acpi_hest_generic_status *cache_estatus;

	len = cper_estatus_len(estatus);
	rcu_read_lock();
	for (i = 0; i < GHES_ESTATUS_CACHES_SIZE; i++) {
		cache = rcu_dereference(ghes_estatus_caches[i]);
		if (cache == NULL)
			continue;
		if (len != cache->estatus_len)
			continue;
		cache_estatus = GHES_ESTATUS_FROM_CACHE(cache);
		if (memcmp(estatus, cache_estatus, len))
			continue;
		atomic_inc(&cache->count);
		now = sched_clock();
		if (now - cache->time_in < GHES_ESTATUS_IN_CACHE_MAX_NSEC)
			cached = 1;
		break;
	}
	rcu_read_unlock();
	return cached;
}

static struct ghes_estatus_cache *ghes_estatus_cache_alloc(
	struct acpi_hest_generic *generic,
	struct acpi_hest_generic_status *estatus)
{
	int alloced;
	u32 len, cache_len;
	struct ghes_estatus_cache *cache;
	struct acpi_hest_generic_status *cache_estatus;

	alloced = atomic_add_return(1, &ghes_estatus_cache_alloced);
	if (alloced > GHES_ESTATUS_CACHE_ALLOCED_MAX) {
		atomic_dec(&ghes_estatus_cache_alloced);
		return NULL;
	}
	len = cper_estatus_len(estatus);
	cache_len = GHES_ESTATUS_CACHE_LEN(len);
	cache = (void *)gen_pool_alloc(ghes_estatus_pool, cache_len);
	if (cache == NULL) {
		atomic_dec(&ghes_estatus_cache_alloced);
		return NULL;
	}
	cache_estatus = GHES_ESTATUS_FROM_CACHE(cache);
	memcpy(cache_estatus, estatus, len);
	cache->estatus_len = len;
	atomic_set(&cache->count, 0);
	cache->generic = generic;
	cache->time_in = sched_clock();
	return cache;
}

static void ghes_estatus_cache_rcu_free(struct rcu_head *head)
{
	struct ghes_estatus_cache *cache;
	u32 len;

	cache = container_of(head, struct ghes_estatus_cache, rcu);
	len = cper_estatus_len(GHES_ESTATUS_FROM_CACHE(cache));
	len = GHES_ESTATUS_CACHE_LEN(len);
	gen_pool_free(ghes_estatus_pool, (unsigned long)cache, len);
	atomic_dec(&ghes_estatus_cache_alloced);
}

void
ghes_estatus_cache_add(struct acpi_hest_generic *generic,
		       struct acpi_hest_generic_status *estatus)
{
	unsigned long long now, duration, period, max_period = 0;
	struct ghes_estatus_cache *cache, *new_cache;
	struct ghes_estatus_cache __rcu *victim;
	int i, slot = -1, count;

	new_cache = ghes_estatus_cache_alloc(generic, estatus);
	if (!new_cache)
		return;

	rcu_read_lock();
	now = sched_clock();
	for (i = 0; i < GHES_ESTATUS_CACHES_SIZE; i++) {
		cache = rcu_dereference(ghes_estatus_caches[i]);
		if (cache == NULL) {
			slot = i;
			break;
		}
		duration = now - cache->time_in;
		if (duration >= GHES_ESTATUS_IN_CACHE_MAX_NSEC) {
			slot = i;
			break;
		}
		count = atomic_read(&cache->count);
		period = duration;
		do_div(period, (count + 1));
		if (period > max_period) {
			max_period = period;
			slot = i;
		}
	}
	rcu_read_unlock();

	if (slot != -1) {
		/*
		 * Use release semantics to ensure that ghes_estatus_cached()
		 * running on another CPU will see the updated cache fields if
		 * it can see the new value of the pointer.
		 */
		victim = xchg_release(&ghes_estatus_caches[slot],
				      RCU_INITIALIZER(new_cache));

		/*
		 * At this point, victim may point to a cached item different
		 * from the one based on which we selected the slot. Instead of
		 * going to the loop again to pick another slot, let's just
		 * drop the other item anyway: this may cause a false cache
		 * miss later on, but that won't cause any problems.
		 */
		if (victim)
			call_rcu(&unrcu_pointer(victim)->rcu,
				 ghes_estatus_cache_rcu_free);
	}
}
