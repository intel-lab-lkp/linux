/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * APEI Generic Hardware Error Source: CPER Helper
 *
 * Copyright (C) 2026 ARM Ltd.
 * Author: Ahmed Tiba <ahmed.tiba@arm.com>
 * Based on ACPI APEI GHES driver.
 *
 */

#ifndef ACPI_APEI_GHES_CPER_H
#define ACPI_APEI_GHES_CPER_H

#include <linux/workqueue.h>

#include <acpi/ghes.h>

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

static struct ghes *ghes_new(struct acpi_hest_generic *generic);
static void ghes_fini(struct ghes *ghes);

static int ghes_read_estatus(struct ghes *ghes,
		      struct acpi_hest_generic_status *estatus,
		      u64 *buf_paddr, enum fixed_addresses fixmap_idx);
static void ghes_clear_estatus(struct ghes *ghes,
			struct acpi_hest_generic_status *estatus,
			u64 buf_paddr, enum fixed_addresses fixmap_idx);
static int __ghes_peek_estatus(struct ghes *ghes,
			struct acpi_hest_generic_status *estatus,
			u64 *buf_paddr, enum fixed_addresses fixmap_idx);
static int __ghes_check_estatus(struct ghes *ghes,
			 struct acpi_hest_generic_status *estatus);
static int __ghes_read_estatus(struct acpi_hest_generic_status *estatus,
			u64 buf_paddr, enum fixed_addresses fixmap_idx,
			size_t buf_len);

#endif /* ACPI_APEI_GHES_CPER_H */
