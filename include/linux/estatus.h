/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Firmware-first RAS: Generic Error Status Core
 *
 * Copyright (C) 2025 ARM Ltd.
 * Author: Ahmed Tiba <ahmed.tiba@arm.com>
 */

#ifndef __LINUX_ESTATUS_H
#define __LINUX_ESTATUS_H

/* "estatus" abbreviates "error status" (CPER status blocks). */

/*
 * "estatus" is a contraction of "error status".  The naming mirrors the ACPI
 * Generic Error Status Block (HEST/CPER) terminology while staying agnostic of
 * the transport (ACPI, DeviceTree, etc.).
 */

#include <linux/irq_work.h>
#include <linux/kconfig.h>
#include <linux/cper.h>
#include <asm/fixmap.h>

#if IS_ENABLED(CONFIG_ACPI)
#include <linux/acpi.h>
#include <acpi/actbl1.h>
#define estatus_generic_status struct acpi_hest_generic_status
#define estatus_generic_data struct acpi_hest_generic_data
#define estatus_generic_data_v300 struct acpi_hest_generic_data_v300
#else
struct estatus_generic_status {
	u32 block_status;
	u32 raw_data_offset;
	u32 raw_data_length;
	u32 data_length;
	u32 error_severity;
} __packed;

struct estatus_generic_data {
	u8 section_type[16];
	u32 error_severity;
	u16 revision;
	u8 validation_bits;
	u8 flags;
	u32 error_data_length;
	u8 fru_id[16];
	u8 fru_text[20];
} __packed;

struct estatus_generic_data_v300 {
	u8 section_type[16];
	u32 error_severity;
	u16 revision;
	u8 validation_bits;
	u8 flags;
	u32 error_data_length;
	u8 fru_id[16];
	u8 fru_text[20];
	u64 time_stamp;
} __packed;

#define estatus_generic_status struct estatus_generic_status
#define estatus_generic_data struct estatus_generic_data
#define estatus_generic_data_v300 struct estatus_generic_data_v300

#define acpi_hest_generic_status estatus_generic_status
#define acpi_hest_generic_data estatus_generic_data
#define acpi_hest_generic_data_v300 estatus_generic_data_v300
#endif

struct estatus_source;

#if IS_ENABLED(CONFIG_ACPI_APEI_GHES)
#include <acpi/apei.h>
#endif

void estatus_report_mem_error(int sev, struct cper_sec_mem_err *mem_err);

enum estatus_notify_mode {
	ESTATUS_NOTIFY_ASYNC,
	ESTATUS_NOTIFY_SEA,
};

struct estatus_ops {
	int (*get_phys)(struct estatus_source *source, phys_addr_t *addr);
	int (*read)(struct estatus_source *source, phys_addr_t addr,
		    void *buf, size_t len, enum fixed_addresses fixmap_idx);
	int (*write)(struct estatus_source *source, phys_addr_t addr,
		     const void *buf, size_t len, enum fixed_addresses fixmap_idx);
	void (*ack)(struct estatus_source *source);
	size_t (*get_max_len)(struct estatus_source *source);
	enum estatus_notify_mode (*get_notify_mode)(struct estatus_source *source);
	const char *(*get_name)(struct estatus_source *source);
};

struct estatus_source {
	const struct estatus_ops *ops;
	void *priv;
	estatus_generic_status *estatus;
	enum fixed_addresses fixmap_idx;
};

struct estatus_node {
	struct llist_node llnode;
	struct estatus_source *source;
};

struct estatus_cache {
	u32 estatus_len;
	atomic_t count;
	struct estatus_source *source;
	unsigned long long time_in;
	struct rcu_head rcu;
};

enum {
	ESTATUS_SEV_NO = 0x0,
	ESTATUS_SEV_CORRECTED = 0x1,
	ESTATUS_SEV_RECOVERABLE = 0x2,
	ESTATUS_SEV_PANIC = 0x3,
};

int estatus_proc(struct estatus_source *ghes);
int estatus_in_nmi_queue_one_entry(struct estatus_source *ghes, enum fixed_addresses fixmap_idx);
void estatus_proc_in_irq(struct irq_work *irq_work);

/**
 * estatus_register_vendor_record_notifier - register a notifier for vendor
 * records that the kernel would otherwise ignore.
 * @nb: pointer to the notifier_block structure of the event handler.
 *
 * return 0 : SUCCESS, non-zero : FAIL
 */
int estatus_register_vendor_record_notifier(struct notifier_block *nb);

/**
 * estatus_unregister_vendor_record_notifier - unregister the previously
 * registered vendor record notifier.
 * @nb: pointer to the notifier_block structure of the vendor record handler.
 */
void estatus_unregister_vendor_record_notifier(struct notifier_block *nb);

int estatus_pool_init(unsigned int num_ghes);

struct notifier_block;
void estatus_register_report_chain(struct notifier_block *nb);
void estatus_unregister_report_chain(struct notifier_block *nb);

static inline int estatus_get_version(estatus_generic_data *gdata)
{
	return gdata->revision >> 8;
}

static inline void *estatus_get_payload(estatus_generic_data *gdata)
{
	if (estatus_get_version(gdata) >= 3)
		return (void *)(((estatus_generic_data_v300 *)(gdata)) + 1);

	return gdata + 1;
}

static inline int estatus_get_error_length(estatus_generic_data *gdata)
{
	return gdata->error_data_length;
}

static inline int estatus_get_size(estatus_generic_data *gdata)
{
	if (estatus_get_version(gdata) >= 3)
		return sizeof(estatus_generic_data_v300);

	return sizeof(estatus_generic_data);
}

static inline int estatus_get_record_size(estatus_generic_data *gdata)
{
	return (estatus_get_size(gdata) + estatus_get_error_length(gdata));
}

static inline void *estatus_get_next(estatus_generic_data *gdata)
{
	return (void *)(gdata) + estatus_get_record_size(gdata);
}

static inline estatus_generic_data *
estatus_first_section(estatus_generic_status *estatus)
{
	return (estatus_generic_data *)(estatus + 1);
}

static inline bool
estatus_section_valid(estatus_generic_status *estatus,
		      estatus_generic_data *section)
{
	return (void *)section - (void *)(estatus + 1) < estatus->data_length;
}

struct estatus_section_iter {
	estatus_generic_status *estatus;
	estatus_generic_data *section;
	bool started;
};

static inline estatus_generic_data *
estatus_section_iter_next(struct estatus_section_iter *iter,
			  estatus_generic_status *estatus)
{
	if (!iter->started) {
		iter->estatus = estatus;
		iter->section = estatus_first_section(estatus);
		iter->started = true;
	} else if (iter->section) {
		iter->section = estatus_get_next(iter->section);
	}

	if (!iter->section)
		return NULL;

	if (!estatus_section_valid(iter->estatus, iter->section)) {
		iter->section = NULL;
		return NULL;
	}

	return iter->section;
}

#define estatus_for_each_section(_estatus, _section)				\
	for (struct estatus_section_iter __estatus_iter = {0};		\
	     ((_section) = estatus_section_iter_next(&__estatus_iter,		\
				    (estatus_generic_status *)(_estatus)));	\
	     )

static inline int acpi_hest_get_version(struct acpi_hest_generic_data *gdata)
{
	return estatus_get_version(gdata);
}

static inline void *acpi_hest_get_payload(struct acpi_hest_generic_data *gdata)
{
	return estatus_get_payload(gdata);
}

static inline int acpi_hest_get_error_length(struct acpi_hest_generic_data *gdata)
{
	return estatus_get_error_length(gdata);
}

static inline int acpi_hest_get_size(struct acpi_hest_generic_data *gdata)
{
	return estatus_get_size(gdata);
}

static inline int acpi_hest_get_record_size(struct acpi_hest_generic_data *gdata)
{
	return estatus_get_record_size(gdata);
}

static inline void *acpi_hest_get_next(struct acpi_hest_generic_data *gdata)
{
	return estatus_get_next(gdata);
}

#define apei_estatus_for_each_section(estatus, section)			\
	estatus_for_each_section(estatus, section)

#endif
