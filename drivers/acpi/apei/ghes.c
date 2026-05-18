// SPDX-License-Identifier: GPL-2.0-only
/*
 * APEI Generic Hardware Error Source support
 *
 * Generic Hardware Error Source provides a way to report platform
 * hardware errors (such as that from chipset). It works in so called
 * "Firmware First" mode, that is, hardware errors are reported to
 * firmware firstly, then reported to Linux by firmware. This way,
 * some non-standard hardware error registers or non-standard hardware
 * link can be checked by firmware to produce more hardware error
 * information for Linux.
 *
 * For more information about Generic Hardware Error Source, please
 * refer to ACPI Specification version 4.0, section 17.3.2.6
 *
 * Copyright 2010,2011 Intel Corp.
 *   Author: Huang Ying <ying.huang@intel.com>
 */

#include <linux/arm_sdei.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/cper.h>
#include <linux/cleanup.h>
#include <linux/platform_device.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/ratelimit.h>
#include <linux/vmalloc.h>
#include <linux/irq_work.h>
#include <linux/llist.h>
#include <linux/genalloc.h>
#include <linux/kfifo.h>
#include <linux/pci.h>
#include <linux/pfn.h>
#include <linux/aer.h>
#include <linux/nmi.h>
#include <linux/sched/clock.h>
#include <linux/uuid.h>
#include <linux/ras.h>
#include <linux/task_work.h>
#include <linux/vmcore_info.h>

#include <acpi/actbl1.h>
#include <acpi/ghes.h>
#include <acpi/ghes_cper.h>
#include <acpi/apei.h>
#include <asm/fixmap.h>
#include <asm/tlbflush.h>
#include <cxl/event.h>
#include <ras/ras_event.h>

#include "apei-internal.h"

/*
 *  NMI-like notifications vary by architecture, before the compiler can prune
 *  unused static functions it needs a value for these enums.
 */
#ifndef CONFIG_ARM_SDE_INTERFACE
#define FIX_APEI_GHES_SDEI_NORMAL	__end_of_fixed_addresses
#define FIX_APEI_GHES_SDEI_CRITICAL	__end_of_fixed_addresses
#endif

/*
 * This driver isn't really modular, however for the time being,
 * continuing to use module_param is the easiest way to remain
 * compatible with existing boot arg use cases.
 */
bool ghes_disable;
module_param_named(disable, ghes_disable, bool, 0);

/*
 * "ghes.edac_force_enable" forcibly enables ghes_edac and skips the platform
 * check.
 */
static bool ghes_edac_force_enable;
module_param_named(edac_force_enable, ghes_edac_force_enable, bool, 0);

/*
 * All error sources notified with HED (Hardware Error Device) share a
 * single notifier callback, so they need to be linked and checked one
 * by one. This holds true for NMI too.
 *
 * RCU is used for these lists, so ghes_list_mutex is only used for
 * list changing, not for traversing.
 */
static LIST_HEAD(ghes_hed);
static DEFINE_MUTEX(ghes_list_mutex);

/*
 * A list of GHES devices which are given to the corresponding EDAC driver
 * ghes_edac for further use.
 */
static LIST_HEAD(ghes_devs);
static DEFINE_MUTEX(ghes_devs_mutex);

/*
 * Because the memory area used to transfer hardware error information
 * from BIOS to Linux can be determined only in NMI, IRQ or timer
 * handler, but general ioremap can not be used in atomic context, so
 * the fixmap is used instead.
 *
 * This spinlock is used to prevent the fixmap entry from being used
 * simultaneously.
 */
static DEFINE_SPINLOCK(ghes_notify_lock_irq);

static void ghes_vendor_record_notifier_destroy(void *nb)
{
	ghes_unregister_vendor_record_notifier(nb);
}

int devm_ghes_register_vendor_record_notifier(struct device *dev,
					      struct notifier_block *nb)
{
	int ret;

	ret = ghes_register_vendor_record_notifier(nb);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, ghes_vendor_record_notifier_destroy, nb);
}
EXPORT_SYMBOL_GPL(devm_ghes_register_vendor_record_notifier);

static void ghes_do_proc(struct ghes *ghes,
			 const struct acpi_hest_generic_status *estatus)
{
	ghes_cper_handle_status(ghes->dev, ghes->generic,
				estatus, is_hest_sync_notify(ghes));
}

static void __ghes_panic(struct ghes *ghes,
			 struct acpi_hest_generic_status *estatus,
			 u64 buf_paddr, enum fixed_addresses fixmap_idx)
{
	const char *msg = GHES_PFX "Fatal hardware error";

	__ghes_print_estatus(KERN_EMERG, ghes->generic, estatus);

	add_taint(TAINT_MACHINE_CHECK, LOCKDEP_STILL_OK);

	ghes_clear_estatus(ghes, estatus, buf_paddr, fixmap_idx);

	if (!panic_timeout)
		pr_emerg("%s but panic disabled\n", msg);

	panic(msg);
}

static int ghes_proc(struct ghes *ghes)
{
	struct acpi_hest_generic_status *estatus = ghes->estatus;
	u64 buf_paddr;
	int rc;

	rc = ghes_read_estatus(ghes, estatus, &buf_paddr, FIX_APEI_GHES_IRQ);
	if (rc)
		goto out;

	if (ghes_severity(estatus->error_severity) >= GHES_SEV_PANIC)
		__ghes_panic(ghes, estatus, buf_paddr, FIX_APEI_GHES_IRQ);

	if (!ghes_estatus_cached(estatus)) {
		if (ghes_print_estatus(NULL, ghes->generic, estatus))
			ghes_estatus_cache_add(ghes->generic, estatus);
	}
	ghes_do_proc(ghes, estatus);

out:
	ghes_clear_estatus(ghes, estatus, buf_paddr, FIX_APEI_GHES_IRQ);

	return rc;
}

static void ghes_add_timer(struct ghes *ghes)
{
	struct acpi_hest_generic *g = ghes->generic;
	unsigned long expire;

	if (!g->notify.poll_interval) {
		pr_warn(FW_WARN GHES_PFX "Poll interval is 0 for generic hardware error source: %d, disabled.\n",
			g->header.source_id);
		return;
	}
	expire = jiffies + msecs_to_jiffies(g->notify.poll_interval);
	ghes->timer.expires = round_jiffies_relative(expire);
	add_timer(&ghes->timer);
}

static void ghes_poll_func(struct timer_list *t)
{
	struct ghes *ghes = timer_container_of(ghes, t, timer);
	unsigned long flags;

	spin_lock_irqsave(&ghes_notify_lock_irq, flags);
	ghes_proc(ghes);
	spin_unlock_irqrestore(&ghes_notify_lock_irq, flags);
	if (!(ghes->flags & GHES_EXITING))
		ghes_add_timer(ghes);
}

static irqreturn_t ghes_irq_func(int irq, void *data)
{
	struct ghes *ghes = data;
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&ghes_notify_lock_irq, flags);
	rc = ghes_proc(ghes);
	spin_unlock_irqrestore(&ghes_notify_lock_irq, flags);
	if (rc)
		return IRQ_NONE;

	return IRQ_HANDLED;
}

static int ghes_notify_hed(struct notifier_block *this, unsigned long event,
			   void *data)
{
	struct ghes *ghes;
	unsigned long flags;
	int ret = NOTIFY_DONE;

	spin_lock_irqsave(&ghes_notify_lock_irq, flags);
	list_for_each_entry_rcu(ghes, &ghes_hed, list) {
		if (!ghes_proc(ghes))
			ret = NOTIFY_OK;
	}
	spin_unlock_irqrestore(&ghes_notify_lock_irq, flags);

	return ret;
}

static struct notifier_block ghes_notifier_hed = {
	.notifier_call = ghes_notify_hed,
};

/*
 * Handlers for CPER records may not be NMI safe. For example,
 * memory_failure_queue() takes spinlocks and calls schedule_work_on().
 * In any NMI-like handler, memory from ghes_estatus_pool is used to save
 * estatus, and added to the ghes_estatus_llist. irq_work_queue() causes
 * ghes_proc_in_irq() to run in IRQ context where each estatus in
 * ghes_estatus_llist is processed.
 *
 * Memory from the ghes_estatus_pool is also used with the ghes_estatus_cache
 * to suppress frequent messages.
 */
static struct llist_head ghes_estatus_llist;
static struct irq_work ghes_proc_irq_work;

static void ghes_proc_in_irq(struct irq_work *irq_work)
{
	struct llist_node *llnode, *next;
	struct ghes_estatus_node *estatus_node;
	struct acpi_hest_generic *generic;
	struct acpi_hest_generic_status *estatus;
	u32 len, node_len;

	llnode = llist_del_all(&ghes_estatus_llist);
	/*
	 * Because the time order of estatus in list is reversed,
	 * revert it back to proper order.
	 */
	llnode = llist_reverse_order(llnode);
	while (llnode) {
		next = llnode->next;
		estatus_node = llist_entry(llnode, struct ghes_estatus_node,
					   llnode);
		estatus = GHES_ESTATUS_FROM_NODE(estatus_node);
		len = cper_estatus_len(estatus);
		node_len = GHES_ESTATUS_NODE_LEN(len);

		ghes_do_proc(estatus_node->ghes, estatus);

		if (!ghes_estatus_cached(estatus)) {
			generic = estatus_node->generic;
			if (ghes_print_estatus(NULL, generic, estatus))
				ghes_estatus_cache_add(generic, estatus);
		}
		gen_pool_free(ghes_estatus_pool, (unsigned long)estatus_node,
			      node_len);

		llnode = next;
	}
}

static void ghes_print_queued_estatus(void)
{
	struct llist_node *llnode;
	struct ghes_estatus_node *estatus_node;
	struct acpi_hest_generic *generic;
	struct acpi_hest_generic_status *estatus;

	llnode = llist_del_all(&ghes_estatus_llist);
	/*
	 * Because the time order of estatus in list is reversed,
	 * revert it back to proper order.
	 */
	llnode = llist_reverse_order(llnode);
	while (llnode) {
		estatus_node = llist_entry(llnode, struct ghes_estatus_node,
					   llnode);
		estatus = GHES_ESTATUS_FROM_NODE(estatus_node);
		generic = estatus_node->generic;
		ghes_print_estatus(NULL, generic, estatus);
		llnode = llnode->next;
	}
}

static int ghes_in_nmi_queue_one_entry(struct ghes *ghes,
				       enum fixed_addresses fixmap_idx)
{
	struct acpi_hest_generic_status *estatus, tmp_header;
	struct ghes_estatus_node *estatus_node;
	u32 len, node_len;
	u64 buf_paddr;
	int sev, rc;

	if (!IS_ENABLED(CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG))
		return -EOPNOTSUPP;

	rc = __ghes_peek_estatus(ghes, &tmp_header, &buf_paddr, fixmap_idx);
	if (rc) {
		ghes_clear_estatus(ghes, &tmp_header, buf_paddr, fixmap_idx);
		return rc;
	}

	rc = __ghes_check_estatus(ghes, &tmp_header);
	if (rc) {
		ghes_clear_estatus(ghes, &tmp_header, buf_paddr, fixmap_idx);
		return rc;
	}

	len = cper_estatus_len(&tmp_header);
	node_len = GHES_ESTATUS_NODE_LEN(len);
	estatus_node = (void *)gen_pool_alloc(ghes_estatus_pool, node_len);
	if (!estatus_node)
		return -ENOMEM;

	estatus_node->ghes = ghes;
	estatus_node->generic = ghes->generic;
	estatus = GHES_ESTATUS_FROM_NODE(estatus_node);

	if (__ghes_read_estatus(estatus, buf_paddr, fixmap_idx, len)) {
		ghes_clear_estatus(ghes, estatus, buf_paddr, fixmap_idx);
		rc = -ENOENT;
		goto no_work;
	}

	sev = ghes_severity(estatus->error_severity);
	if (sev >= GHES_SEV_PANIC) {
		ghes_print_queued_estatus();
		__ghes_panic(ghes, estatus, buf_paddr, fixmap_idx);
	}

	ghes_clear_estatus(ghes, &tmp_header, buf_paddr, fixmap_idx);

	/* This error has been reported before, don't process it again. */
	if (ghes_estatus_cached(estatus))
		goto no_work;

	llist_add(&estatus_node->llnode, &ghes_estatus_llist);

	return rc;

no_work:
	gen_pool_free(ghes_estatus_pool, (unsigned long)estatus_node,
		      node_len);

	return rc;
}

static int ghes_in_nmi_spool_from_list(struct list_head *rcu_list,
				       enum fixed_addresses fixmap_idx)
{
	int ret = -ENOENT;
	struct ghes *ghes;

	rcu_read_lock();
	list_for_each_entry_rcu(ghes, rcu_list, list) {
		if (!ghes_in_nmi_queue_one_entry(ghes, fixmap_idx))
			ret = 0;
	}
	rcu_read_unlock();

	if (IS_ENABLED(CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG) && !ret)
		irq_work_queue(&ghes_proc_irq_work);

	return ret;
}

/**
 * ghes_has_active_errors - Check if there are active errors in error sources
 * @ghes_list: List of GHES entries to check for active errors
 *
 * This function iterates through all GHES entries in the given list and
 * checks if any of them has active error status by reading the error
 * status register.
 *
 * Return: true if at least one source has active error, false otherwise.
 */
static bool __maybe_unused ghes_has_active_errors(struct list_head *ghes_list)
{
	struct ghes *ghes;

	guard(rcu)();
	list_for_each_entry_rcu(ghes, ghes_list, list) {
		if (ghes->error_status_vaddr &&
		    readl(ghes->error_status_vaddr))
			return true;
	}

	return false;
}

/**
 * ghes_map_error_status - Map error status address to virtual address
 * @ghes: pointer to GHES structure
 *
 * Reads the error status address from ACPI HEST table and maps it to a virtual
 * address that can be accessed by the kernel.
 *
 * Return: 0 on success, error code on failure.
 */
static int __maybe_unused ghes_map_error_status(struct ghes *ghes)
{
	struct acpi_hest_generic *g = ghes->generic;
	u64 paddr;
	int rc;

	rc = apei_read(&paddr, &g->error_status_address);
	if (rc)
		return rc;

	ghes->error_status_vaddr =
		acpi_os_ioremap(paddr, sizeof(ghes->estatus->block_status));
	if (!ghes->error_status_vaddr)
		return -EINVAL;

	return 0;
}

/**
 * ghes_unmap_error_status - Unmap error status virtual address
 * @ghes: pointer to GHES structure
 *
 * Unmaps the error status address if it was previously mapped.
 */
static void __maybe_unused ghes_unmap_error_status(struct ghes *ghes)
{
	if (ghes->error_status_vaddr) {
		iounmap(ghes->error_status_vaddr);
		ghes->error_status_vaddr = NULL;
	}
}

#ifdef CONFIG_ACPI_APEI_SEA
static LIST_HEAD(ghes_sea);

/*
 * Return 0 only if one of the SEA error sources successfully reported an error
 * record sent from the firmware.
 */
int ghes_notify_sea(void)
{
	static DEFINE_RAW_SPINLOCK(ghes_notify_lock_sea);
	int rv;

	if (!ghes_has_active_errors(&ghes_sea))
		return -ENOENT;

	raw_spin_lock(&ghes_notify_lock_sea);
	rv = ghes_in_nmi_spool_from_list(&ghes_sea, FIX_APEI_GHES_SEA);
	raw_spin_unlock(&ghes_notify_lock_sea);

	return rv;
}

static int ghes_sea_add(struct ghes *ghes)
{
	int rc;

	rc = ghes_map_error_status(ghes);
	if (rc)
		return rc;

	mutex_lock(&ghes_list_mutex);
	list_add_rcu(&ghes->list, &ghes_sea);
	mutex_unlock(&ghes_list_mutex);

	return 0;
}

static void ghes_sea_remove(struct ghes *ghes)
{
	mutex_lock(&ghes_list_mutex);
	list_del_rcu(&ghes->list);
	mutex_unlock(&ghes_list_mutex);
	ghes_unmap_error_status(ghes);
	synchronize_rcu();
}
#else /* CONFIG_ACPI_APEI_SEA */
static inline int ghes_sea_add(struct ghes *ghes) { return -EINVAL; }
static inline void ghes_sea_remove(struct ghes *ghes) { }
#endif /* CONFIG_ACPI_APEI_SEA */

#ifdef CONFIG_HAVE_ACPI_APEI_NMI
/*
 * NMI may be triggered on any CPU, so ghes_in_nmi is used for
 * having only one concurrent reader.
 */
static atomic_t ghes_in_nmi = ATOMIC_INIT(0);

static LIST_HEAD(ghes_nmi);

static int ghes_notify_nmi(unsigned int cmd, struct pt_regs *regs)
{
	static DEFINE_RAW_SPINLOCK(ghes_notify_lock_nmi);
	int ret = NMI_DONE;

	if (!ghes_has_active_errors(&ghes_nmi))
		return ret;

	if (!atomic_add_unless(&ghes_in_nmi, 1, 1))
		return ret;

	raw_spin_lock(&ghes_notify_lock_nmi);
	if (!ghes_in_nmi_spool_from_list(&ghes_nmi, FIX_APEI_GHES_NMI))
		ret = NMI_HANDLED;
	raw_spin_unlock(&ghes_notify_lock_nmi);

	atomic_dec(&ghes_in_nmi);
	return ret;
}

static int ghes_nmi_add(struct ghes *ghes)
{
	int rc;

	rc = ghes_map_error_status(ghes);
	if (rc)
		return rc;

	mutex_lock(&ghes_list_mutex);
	if (list_empty(&ghes_nmi))
		register_nmi_handler(NMI_LOCAL, ghes_notify_nmi, 0, "ghes");
	list_add_rcu(&ghes->list, &ghes_nmi);
	mutex_unlock(&ghes_list_mutex);

	return 0;
}

static void ghes_nmi_remove(struct ghes *ghes)
{
	mutex_lock(&ghes_list_mutex);
	list_del_rcu(&ghes->list);
	if (list_empty(&ghes_nmi))
		unregister_nmi_handler(NMI_LOCAL, "ghes");
	mutex_unlock(&ghes_list_mutex);

	ghes_unmap_error_status(ghes);

	/*
	 * To synchronize with NMI handler, ghes can only be
	 * freed after NMI handler finishes.
	 */
	synchronize_rcu();
}
#else /* CONFIG_HAVE_ACPI_APEI_NMI */
static inline int ghes_nmi_add(struct ghes *ghes) { return -EINVAL; }
static inline void ghes_nmi_remove(struct ghes *ghes) { }
#endif /* CONFIG_HAVE_ACPI_APEI_NMI */

static void ghes_nmi_init_cxt(void)
{
	init_irq_work(&ghes_proc_irq_work, ghes_proc_in_irq);
}

static int __ghes_sdei_callback(struct ghes *ghes,
				enum fixed_addresses fixmap_idx)
{
	if (!ghes_in_nmi_queue_one_entry(ghes, fixmap_idx)) {
		irq_work_queue(&ghes_proc_irq_work);

		return 0;
	}

	return -ENOENT;
}

static int ghes_sdei_normal_callback(u32 event_num, struct pt_regs *regs,
				      void *arg)
{
	static DEFINE_RAW_SPINLOCK(ghes_notify_lock_sdei_normal);
	struct ghes *ghes = arg;
	int err;

	raw_spin_lock(&ghes_notify_lock_sdei_normal);
	err = __ghes_sdei_callback(ghes, FIX_APEI_GHES_SDEI_NORMAL);
	raw_spin_unlock(&ghes_notify_lock_sdei_normal);

	return err;
}

static int ghes_sdei_critical_callback(u32 event_num, struct pt_regs *regs,
				       void *arg)
{
	static DEFINE_RAW_SPINLOCK(ghes_notify_lock_sdei_critical);
	struct ghes *ghes = arg;
	int err;

	raw_spin_lock(&ghes_notify_lock_sdei_critical);
	err = __ghes_sdei_callback(ghes, FIX_APEI_GHES_SDEI_CRITICAL);
	raw_spin_unlock(&ghes_notify_lock_sdei_critical);

	return err;
}

static int apei_sdei_register_ghes(struct ghes *ghes)
{
	if (!IS_ENABLED(CONFIG_ARM_SDE_INTERFACE))
		return -EOPNOTSUPP;

	return sdei_register_ghes(ghes, ghes_sdei_normal_callback,
				 ghes_sdei_critical_callback);
}

static int apei_sdei_unregister_ghes(struct ghes *ghes)
{
	if (!IS_ENABLED(CONFIG_ARM_SDE_INTERFACE))
		return -EOPNOTSUPP;

	return sdei_unregister_ghes(ghes);
}

static int ghes_probe(struct platform_device *ghes_dev)
{
	struct acpi_hest_generic *generic;
	struct ghes *ghes = NULL;
	unsigned long flags;

	int rc = -EINVAL;

	generic = *(struct acpi_hest_generic **)ghes_dev->dev.platform_data;
	if (!generic->enabled)
		return -ENODEV;

	switch (generic->notify.type) {
	case ACPI_HEST_NOTIFY_POLLED:
	case ACPI_HEST_NOTIFY_EXTERNAL:
	case ACPI_HEST_NOTIFY_SCI:
	case ACPI_HEST_NOTIFY_GSIV:
	case ACPI_HEST_NOTIFY_GPIO:
		break;

	case ACPI_HEST_NOTIFY_SEA:
		if (!IS_ENABLED(CONFIG_ACPI_APEI_SEA)) {
			pr_warn(GHES_PFX "Generic hardware error source: %d notified via SEA is not supported\n",
				generic->header.source_id);
			rc = -ENOTSUPP;
			goto err;
		}
		break;
	case ACPI_HEST_NOTIFY_NMI:
		if (!IS_ENABLED(CONFIG_HAVE_ACPI_APEI_NMI)) {
			pr_warn(GHES_PFX "Generic hardware error source: %d notified via NMI interrupt is not supported!\n",
				generic->header.source_id);
			goto err;
		}
		break;
	case ACPI_HEST_NOTIFY_SOFTWARE_DELEGATED:
		if (!IS_ENABLED(CONFIG_ARM_SDE_INTERFACE)) {
			pr_warn(GHES_PFX "Generic hardware error source: %d notified via SDE Interface is not supported!\n",
				generic->header.source_id);
			goto err;
		}
		break;
	case ACPI_HEST_NOTIFY_LOCAL:
		pr_warn(GHES_PFX "Generic hardware error source: %d notified via local interrupt is not supported!\n",
			generic->header.source_id);
		goto err;
	default:
		pr_warn(FW_WARN GHES_PFX "Unknown notification type: %u for generic hardware error source: %d\n",
			generic->notify.type, generic->header.source_id);
		goto err;
	}

	rc = -EIO;
	if (generic->error_block_length <
	    sizeof(struct acpi_hest_generic_status)) {
		pr_warn(FW_BUG GHES_PFX "Invalid error block length: %u for generic hardware error source: %d\n",
			generic->error_block_length, generic->header.source_id);
		goto err;
	}
	ghes = ghes_new(generic);
	if (IS_ERR(ghes)) {
		rc = PTR_ERR(ghes);
		ghes = NULL;
		goto err;
	}

	switch (generic->notify.type) {
	case ACPI_HEST_NOTIFY_POLLED:
		timer_setup(&ghes->timer, ghes_poll_func, 0);
		ghes_add_timer(ghes);
		break;
	case ACPI_HEST_NOTIFY_EXTERNAL:
		/* External interrupt vector is GSI */
		rc = acpi_gsi_to_irq(generic->notify.vector, &ghes->irq);
		if (rc) {
			pr_err(GHES_PFX "Failed to map GSI to IRQ for generic hardware error source: %d\n",
			       generic->header.source_id);
			goto err;
		}
		rc = request_irq(ghes->irq, ghes_irq_func, IRQF_SHARED,
				 "GHES IRQ", ghes);
		if (rc) {
			pr_err(GHES_PFX "Failed to register IRQ for generic hardware error source: %d\n",
			       generic->header.source_id);
			goto err;
		}
		break;

	case ACPI_HEST_NOTIFY_SCI:
	case ACPI_HEST_NOTIFY_GSIV:
	case ACPI_HEST_NOTIFY_GPIO:
		mutex_lock(&ghes_list_mutex);
		if (list_empty(&ghes_hed))
			register_acpi_hed_notifier(&ghes_notifier_hed);
		list_add_rcu(&ghes->list, &ghes_hed);
		mutex_unlock(&ghes_list_mutex);
		break;

	case ACPI_HEST_NOTIFY_SEA:
		rc = ghes_sea_add(ghes);
		if (rc)
			goto err;
		break;
	case ACPI_HEST_NOTIFY_NMI:
		rc = ghes_nmi_add(ghes);
		if (rc)
			goto err;
		break;
	case ACPI_HEST_NOTIFY_SOFTWARE_DELEGATED:
		rc = apei_sdei_register_ghes(ghes);
		if (rc)
			goto err;
		break;
	default:
		BUG();
	}

	platform_set_drvdata(ghes_dev, ghes);

	ghes->dev = &ghes_dev->dev;

	mutex_lock(&ghes_devs_mutex);
	list_add_tail(&ghes->elist, &ghes_devs);
	mutex_unlock(&ghes_devs_mutex);

	/* Handle any pending errors right away */
	spin_lock_irqsave(&ghes_notify_lock_irq, flags);
	ghes_proc(ghes);
	spin_unlock_irqrestore(&ghes_notify_lock_irq, flags);

	return 0;

err:
	if (ghes) {
		ghes_fini(ghes);
		kfree(ghes);
	}
	return rc;
}

static void ghes_remove(struct platform_device *ghes_dev)
{
	int rc;
	struct ghes *ghes;
	struct acpi_hest_generic *generic;

	ghes = platform_get_drvdata(ghes_dev);
	generic = ghes->generic;

	ghes->flags |= GHES_EXITING;
	switch (generic->notify.type) {
	case ACPI_HEST_NOTIFY_POLLED:
		timer_shutdown_sync(&ghes->timer);
		break;
	case ACPI_HEST_NOTIFY_EXTERNAL:
		free_irq(ghes->irq, ghes);
		break;

	case ACPI_HEST_NOTIFY_SCI:
	case ACPI_HEST_NOTIFY_GSIV:
	case ACPI_HEST_NOTIFY_GPIO:
		mutex_lock(&ghes_list_mutex);
		list_del_rcu(&ghes->list);
		if (list_empty(&ghes_hed))
			unregister_acpi_hed_notifier(&ghes_notifier_hed);
		mutex_unlock(&ghes_list_mutex);
		synchronize_rcu();
		break;

	case ACPI_HEST_NOTIFY_SEA:
		ghes_sea_remove(ghes);
		break;
	case ACPI_HEST_NOTIFY_NMI:
		ghes_nmi_remove(ghes);
		break;
	case ACPI_HEST_NOTIFY_SOFTWARE_DELEGATED:
		rc = apei_sdei_unregister_ghes(ghes);
		if (rc) {
			/*
			 * Returning early results in a resource leak, but we're
			 * only here if stopping the hardware failed.
			 */
			dev_err(&ghes_dev->dev, "Failed to unregister ghes (%pe)\n",
				ERR_PTR(rc));
			return;
		}
		break;
	default:
		BUG();
		break;
	}

	ghes_fini(ghes);

	mutex_lock(&ghes_devs_mutex);
	list_del(&ghes->elist);
	mutex_unlock(&ghes_devs_mutex);

	kfree(ghes);
}

static struct platform_driver ghes_platform_driver = {
	.driver		= {
		.name	= "GHES",
	},
	.probe		= ghes_probe,
	.remove		= ghes_remove,
};

void __init acpi_ghes_init(void)
{
	int rc;

	acpi_sdei_init();

	if (acpi_disabled)
		return;

	switch (hest_disable) {
	case HEST_NOT_FOUND:
		return;
	case HEST_DISABLED:
		pr_info(GHES_PFX "HEST is not enabled!\n");
		return;
	default:
		break;
	}

	if (ghes_disable) {
		pr_info(GHES_PFX "GHES is not enabled!\n");
		return;
	}

	ghes_nmi_init_cxt();

	rc = platform_driver_register(&ghes_platform_driver);
	if (rc)
		return;

	rc = apei_osc_setup();
	if (rc == 0 && osc_sb_apei_support_acked)
		pr_info(GHES_PFX "APEI firmware first mode is enabled by APEI bit and WHEA _OSC.\n");
	else if (rc == 0 && !osc_sb_apei_support_acked)
		pr_info(GHES_PFX "APEI firmware first mode is enabled by WHEA _OSC.\n");
	else if (rc && osc_sb_apei_support_acked)
		pr_info(GHES_PFX "APEI firmware first mode is enabled by APEI bit.\n");
	else
		pr_info(GHES_PFX "Failed to enable APEI firmware first mode.\n");
}

/*
 * Known x86 systems that prefer GHES error reporting:
 */
static struct acpi_platform_list plat_list[] = {
	{"HPE   ", "Server  ", 0, ACPI_SIG_FADT, all_versions},
	{"__ZX__", "EDK2    ", 3, ACPI_SIG_FADT, greater_than_or_equal},
	{"_BYO_ ", "BYOSOFT ", 3, ACPI_SIG_FADT, greater_than_or_equal},
	{ } /* End */
};

struct list_head *ghes_get_devices(void)
{
	int idx = -1;

	if (IS_ENABLED(CONFIG_X86)) {
		idx = acpi_match_platform_list(plat_list);
		if (idx < 0) {
			if (!ghes_edac_force_enable)
				return NULL;

			pr_warn_once("Force-loading ghes_edac on an unsupported platform. You're on your own!\n");
		}
	} else if (list_empty(&ghes_devs)) {
		return NULL;
	}

	return &ghes_devs;
}
EXPORT_SYMBOL_GPL(ghes_get_devices);

void ghes_register_report_chain(struct notifier_block *nb)
{
	atomic_notifier_chain_register(&ghes_report_chain, nb);
}
EXPORT_SYMBOL_GPL(ghes_register_report_chain);

void ghes_unregister_report_chain(struct notifier_block *nb)
{
	atomic_notifier_chain_unregister(&ghes_report_chain, nb);
}
EXPORT_SYMBOL_GPL(ghes_unregister_report_chain);
