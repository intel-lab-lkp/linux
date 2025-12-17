/* SPDX-License-Identifier: GPL-2.0 */
#ifndef GHES_H
#define GHES_H

#include <acpi/apei.h>
#include <acpi/hed.h>
#include <linux/estatus.h>

/*
 * One struct ghes is created for each generic hardware error source.
 * It provides the context for APEI hardware error timer/IRQ/SCI/NMI
 * handler.
 *
 * estatus: memory buffer for error status block, allocated during
 * HEST parsing.
 */
#define GHES_EXITING		0x0002

struct ghes {
	union {
		struct acpi_hest_generic *generic;
		struct acpi_hest_generic_v2 *generic_v2;
	};
	struct acpi_hest_generic_status *estatus;
	unsigned long flags;
	union {
		struct list_head list;
		struct timer_list timer;
		unsigned int irq;
	};
	struct device *dev;
	struct list_head elist;
};

struct ghes_estatus_node {
	struct llist_node llnode;
	struct acpi_hest_generic *generic;
	struct ghes *ghes;
};

struct ghes_estatus_cache {
	u32 estatus_len;
	atomic_t count;
	struct acpi_hest_generic *generic;
	unsigned long long time_in;
	struct rcu_head rcu;
};

enum {
	GHES_SEV_NO = 0x0,
	GHES_SEV_CORRECTED = 0x1,
	GHES_SEV_RECOVERABLE = 0x2,
	GHES_SEV_PANIC = 0x3,
};

#ifdef CONFIG_ACPI_APEI_GHES
/**
 * ghes_register_vendor_record_notifier - register a notifier for vendor
 * records that the kernel would otherwise ignore.
 * @nb: pointer to the notifier_block structure of the event handler.
 *
 * return 0 : SUCCESS, non-zero : FAIL
 */
int ghes_register_vendor_record_notifier(struct notifier_block *nb);

/**
 * ghes_unregister_vendor_record_notifier - unregister the previously
 * registered vendor record notifier.
 * @nb: pointer to the notifier_block structure of the vendor record handler.
 */
void ghes_unregister_vendor_record_notifier(struct notifier_block *nb);

struct list_head *ghes_get_devices(void);

void ghes_estatus_pool_region_free(unsigned long addr, u32 size);
#else
static inline struct list_head *ghes_get_devices(void) { return NULL; }

static inline void ghes_estatus_pool_region_free(unsigned long addr, u32 size) { return; }
#endif

int ghes_estatus_pool_init(unsigned int num_ghes);

#define apei_estatus_for_each_section(estatus, section)			\
	estatus_for_each_section(estatus, section)

#ifdef CONFIG_ACPI_APEI_SEA
int ghes_notify_sea(void);
#else
static inline int ghes_notify_sea(void) { return -ENOENT; }
#endif

struct notifier_block;
extern void ghes_register_report_chain(struct notifier_block *nb);
extern void ghes_unregister_report_chain(struct notifier_block *nb);
#endif /* GHES_H */
