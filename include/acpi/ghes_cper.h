/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * GHES declarations used by both the ACPI APEI GHES driver
 * and the firmware-first CPER provider.
 *
 * These declarations lets GHES and other firmware-first error sources use
 * the same helper so the non-ACPI path follows the same
 * behavior as GHES instead of carrying a separate copy.
 *
 * Derived from the ACPI APEI GHES driver.
 *
 * Copyright 2010,2011 Intel Corp.
 *   Author: Huang Ying <ying.huang@intel.com>
 */

#ifndef ACPI_APEI_GHES_CPER_H
#define ACPI_APEI_GHES_CPER_H

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>

#include <acpi/ghes.h>
#include <cxl/event.h>

#define GHES_PFX	"GHES: "

#define GHES_ESTATUS_MAX_SIZE		65536
#define GHES_ESOURCE_PREALLOC_MAX_SIZE	65536

#define GHES_ESTATUS_POOL_MIN_ALLOC_ORDER 3

/* This is just an estimation for memory pool allocation */
#define GHES_ESTATUS_CACHE_AVG_SIZE	512

#define GHES_ESTATUS_CACHES_SIZE	4

#define GHES_ESTATUS_IN_CACHE_MAX_NSEC	10000000000ULL
/* Prevent too many caches are allocated because of RCU */
#define GHES_ESTATUS_CACHE_ALLOCED_MAX	(GHES_ESTATUS_CACHES_SIZE * 3 / 2)

#define GHES_ESTATUS_CACHE_LEN(estatus_len)			\
	(sizeof(struct ghes_estatus_cache) + (estatus_len))
#define GHES_ESTATUS_FROM_CACHE(estatus_cache)			\
	((struct acpi_hest_generic_status *)				\
	 ((struct ghes_estatus_cache *)(estatus_cache) + 1))

#define GHES_ESTATUS_NODE_LEN(estatus_len)			\
	(sizeof(struct ghes_estatus_node) + (estatus_len))
#define GHES_ESTATUS_FROM_NODE(estatus_node)			\
	((struct acpi_hest_generic_status *)				\
	 ((struct ghes_estatus_node *)(estatus_node) + 1))

#define GHES_VENDOR_ENTRY_LEN(gdata_len)                               \
	(sizeof(struct ghes_vendor_record_entry) + (gdata_len))
#define GHES_GDATA_FROM_VENDOR_ENTRY(vendor_entry)                     \
	((struct acpi_hest_generic_data *)                              \
	((struct ghes_vendor_record_entry *)(vendor_entry) + 1))

extern struct gen_pool *ghes_estatus_pool;
extern struct atomic_notifier_head ghes_report_chain;

static inline bool is_hest_type_generic_v2(struct ghes *ghes)
{
	return ghes->generic->header.type == ACPI_HEST_TYPE_GENERIC_ERROR_V2;
}

/*
 * A platform may describe one error source for the handling of synchronous
 * errors (e.g. MCE or SEA), or for handling asynchronous errors (e.g. SCI
 * or External Interrupt). On x86, the HEST notifications are always
 * asynchronous, so only SEA on ARM is delivered as a synchronous
 * notification.
 */
static inline bool is_hest_sync_notify(struct ghes *ghes)
{
	u8 notify_type = ghes->generic->notify.type;

	return notify_type == ACPI_HEST_NOTIFY_SEA;
}

struct ghes_vendor_record_entry {
	struct work_struct work;
	int error_severity;
	char vendor_record[];
};

#ifdef CONFIG_ACPI_APEI
struct ghes *ghes_new(struct acpi_hest_generic *generic);
void ghes_fini(struct ghes *ghes);

int ghes_read_estatus(struct ghes *ghes,
		      struct acpi_hest_generic_status *estatus,
		      u64 *buf_paddr, enum fixed_addresses fixmap_idx);
void ghes_clear_estatus(struct ghes *ghes,
			struct acpi_hest_generic_status *estatus,
			u64 buf_paddr, enum fixed_addresses fixmap_idx);
int __ghes_peek_estatus(struct ghes *ghes,
			struct acpi_hest_generic_status *estatus,
			u64 *buf_paddr, enum fixed_addresses fixmap_idx);
int __ghes_check_estatus(struct ghes *ghes,
			 struct acpi_hest_generic_status *estatus);
int __ghes_read_estatus(struct acpi_hest_generic_status *estatus,
			u64 buf_paddr, enum fixed_addresses fixmap_idx,
			size_t buf_len);
#endif
int ghes_estatus_cached(struct acpi_hest_generic_status *estatus);
void ghes_estatus_cache_add(struct acpi_hest_generic *generic,
			    struct acpi_hest_generic_status *estatus);
void ghes_defer_non_standard_event(struct acpi_hest_generic_data *gdata,
				   int sev);
int ghes_severity(int severity);
bool ghes_handle_memory_failure(struct acpi_hest_generic_data *gdata,
				int sev, bool sync);
bool ghes_handle_arm_hw_error(struct acpi_hest_generic_data *gdata,
			      int sev, bool sync);
void ghes_handle_aer(struct acpi_hest_generic_data *gdata);
void ghes_log_hwerr(int sev, guid_t *sec_type);
void __ghes_print_estatus(const char *pfx,
			  const struct acpi_hest_generic *generic,
			  const struct acpi_hest_generic_status *estatus);
int ghes_print_estatus(const char *pfx,
		       const struct acpi_hest_generic *generic,
		       const struct acpi_hest_generic_status *estatus);
void ghes_cper_handle_status(struct device *dev,
			     const struct acpi_hest_generic *generic,
			     const struct acpi_hest_generic_status *estatus,
			     bool sync);
void cxl_cper_post_prot_err(struct cxl_cper_sec_prot_err *prot_err,
			    int severity);
int cxl_cper_register_prot_err_work(struct work_struct *work);
int cxl_cper_unregister_prot_err_work(struct work_struct *work);
int cxl_cper_prot_err_kfifo_get(struct cxl_cper_prot_err_work_data *wd);
void cxl_cper_post_event(enum cxl_event_type event_type,
			 struct cxl_cper_event_rec *rec);
int cxl_cper_register_work(struct work_struct *work);
int cxl_cper_unregister_work(struct work_struct *work);
int cxl_cper_kfifo_get(struct cxl_cper_work_data *wd);

#endif /* ACPI_APEI_GHES_CPER_H */
