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
#include <acpi/apei.h>
#include <asm/fixmap.h>
#include <asm/tlbflush.h>
#include <cxl/event.h>
#include <ras/ras_event.h>
#include <linux/estatus.h>

#include "apei-internal.h"

#define GHES_PFX	"GHES: "

#define GHES_ESTATUS_MAX_SIZE		65536

/*
 *  NMI-like notifications vary by architecture, before the compiler can prune
 *  unused static functions it needs a value for these enums.
 */
#ifndef CONFIG_ARM_SDE_INTERFACE
#define FIX_APEI_GHES_SDEI_NORMAL	__end_of_fixed_addresses
#define FIX_APEI_GHES_SDEI_CRITICAL	__end_of_fixed_addresses
#endif

static inline bool is_hest_type_generic_v2(struct ghes *ghes)
{
	return ghes->generic->header.type == ACPI_HEST_TYPE_GENERIC_ERROR_V2;
}

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

int ghes_estatus_pool_init(unsigned int num_ghes)
{
	return estatus_pool_init(num_ghes);
}
EXPORT_SYMBOL_GPL(ghes_estatus_pool_init);

/**
 * ghes_estatus_pool_region_free - free previously allocated memory
 *				   from the ghes_estatus_pool.
 * @addr: address of memory to free.
 * @size: size of memory to free.
 *
 * Returns none.
 */
void ghes_estatus_pool_region_free(unsigned long addr, u32 size)
{
	estatus_pool_region_free(addr, size);
}
EXPORT_SYMBOL_GPL(ghes_estatus_pool_region_free);

static int map_gen_v2(struct ghes *ghes)
{
	return apei_map_generic_address(&ghes->generic_v2->read_ack_register);
}

static void unmap_gen_v2(struct ghes *ghes)
{
	apei_unmap_generic_address(&ghes->generic_v2->read_ack_register);
}

static const struct estatus_ops ghes_estatus_ops;

static struct ghes *ghes_new(struct acpi_hest_generic *generic)
{
	struct ghes *ghes;
	unsigned int error_block_length;
	int rc;

	ghes = kzalloc(sizeof(*ghes), GFP_KERNEL);
	if (!ghes)
		return ERR_PTR(-ENOMEM);

	ghes->generic = generic;
	scnprintf(ghes->name, sizeof(ghes->name), "GHES source %d",
		  generic->header.source_id);
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
	if (!ghes->estatus) {
		rc = -ENOMEM;
		goto err_unmap_status_addr;
	}

	ghes->estatus_src.ops = &ghes_estatus_ops;
	ghes->estatus_src.priv = ghes;
	ghes->estatus_src.estatus = ghes->estatus;
	ghes->estatus_src.fixmap_idx = FIX_APEI_GHES_IRQ;
	INIT_LIST_HEAD(&ghes->elist);

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

static void ghes_fini(struct ghes *ghes)
{
	kfree(ghes->estatus);
	apei_unmap_generic_address(&ghes->generic->error_status_address);
	if (is_hest_type_generic_v2(ghes))
		unmap_gen_v2(ghes);
}
int ghes_register_vendor_record_notifier(struct notifier_block *nb)
{
	return estatus_register_vendor_record_notifier(nb);
}
EXPORT_SYMBOL_GPL(ghes_register_vendor_record_notifier);

void ghes_unregister_vendor_record_notifier(struct notifier_block *nb)
{
	estatus_unregister_vendor_record_notifier(nb);
}
EXPORT_SYMBOL_GPL(ghes_unregister_vendor_record_notifier);

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
	estatus_proc(&ghes->estatus_src);
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
	rc = estatus_proc(&ghes->estatus_src);
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
		if (!estatus_proc(&ghes->estatus_src))
			ret = NOTIFY_OK;
	}
	spin_unlock_irqrestore(&ghes_notify_lock_irq, flags);

	return ret;
}

static struct notifier_block ghes_notifier_hed = {
	.notifier_call = ghes_notify_hed,
};

static struct irq_work ghes_proc_irq_work;

static int ghes_in_nmi_spool_from_list(struct list_head *rcu_list,
				       enum fixed_addresses fixmap_idx)
{
	int ret = -ENOENT;
	struct ghes *ghes;

	rcu_read_lock();
	list_for_each_entry_rcu(ghes, rcu_list, list) {
		if (!estatus_in_nmi_queue_one_entry(&ghes->estatus_src, fixmap_idx))
			ret = 0;
	}
	rcu_read_unlock();

	if (IS_ENABLED(CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG) && !ret)
		irq_work_queue(&ghes_proc_irq_work);

	return ret;
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

	raw_spin_lock(&ghes_notify_lock_sea);
	rv = ghes_in_nmi_spool_from_list(&ghes_sea, FIX_APEI_GHES_SEA);
	raw_spin_unlock(&ghes_notify_lock_sea);

	return rv;
}

static void ghes_sea_add(struct ghes *ghes)
{
	mutex_lock(&ghes_list_mutex);
	list_add_rcu(&ghes->list, &ghes_sea);
	mutex_unlock(&ghes_list_mutex);
}

static void ghes_sea_remove(struct ghes *ghes)
{
	mutex_lock(&ghes_list_mutex);
	list_del_rcu(&ghes->list);
	mutex_unlock(&ghes_list_mutex);
	synchronize_rcu();
}
#else /* CONFIG_ACPI_APEI_SEA */
static inline void ghes_sea_add(struct ghes *ghes) { }
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

	if (!atomic_add_unless(&ghes_in_nmi, 1, 1))
		return ret;

	raw_spin_lock(&ghes_notify_lock_nmi);
	if (!ghes_in_nmi_spool_from_list(&ghes_nmi, FIX_APEI_GHES_NMI))
		ret = NMI_HANDLED;
	raw_spin_unlock(&ghes_notify_lock_nmi);

	atomic_dec(&ghes_in_nmi);
	return ret;
}

static void ghes_nmi_add(struct ghes *ghes)
{
	mutex_lock(&ghes_list_mutex);
	if (list_empty(&ghes_nmi))
		register_nmi_handler(NMI_LOCAL, ghes_notify_nmi, 0, "ghes");
	list_add_rcu(&ghes->list, &ghes_nmi);
	mutex_unlock(&ghes_list_mutex);
}

static void ghes_nmi_remove(struct ghes *ghes)
{
	mutex_lock(&ghes_list_mutex);
	list_del_rcu(&ghes->list);
	if (list_empty(&ghes_nmi))
		unregister_nmi_handler(NMI_LOCAL, "ghes");
	mutex_unlock(&ghes_list_mutex);
	/*
	 * To synchronize with NMI handler, ghes can only be
	 * freed after NMI handler finishes.
	 */
	synchronize_rcu();
}
#else /* CONFIG_HAVE_ACPI_APEI_NMI */
static inline void ghes_nmi_add(struct ghes *ghes) { }
static inline void ghes_nmi_remove(struct ghes *ghes) { }
#endif /* CONFIG_HAVE_ACPI_APEI_NMI */

static void ghes_nmi_init_cxt(void)
{
	init_irq_work(&ghes_proc_irq_work, estatus_proc_in_irq);
}

static int __ghes_sdei_callback(struct ghes *ghes,
				enum fixed_addresses fixmap_idx)
{
	if (!estatus_in_nmi_queue_one_entry(&ghes->estatus_src, fixmap_idx)) {
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
		ghes_sea_add(ghes);
		break;
	case ACPI_HEST_NOTIFY_NMI:
		ghes_nmi_add(ghes);
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
	estatus_proc(&ghes->estatus_src);
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
	estatus_register_report_chain(nb);
}
EXPORT_SYMBOL_GPL(ghes_register_report_chain);

void ghes_unregister_report_chain(struct notifier_block *nb)
{
	estatus_unregister_report_chain(nb);
}
EXPORT_SYMBOL_GPL(ghes_unregister_report_chain);

#define GHES_ESTATUS_RW_FROM_PHYS	1
#define GHES_ESTATUS_RW_TO_PHYS		0

static void __iomem *ghes_estatus_map(struct ghes *ghes, u64 pfn,
				      enum fixed_addresses fixmap_idx)
{
	phys_addr_t paddr = PFN_PHYS(pfn);
	pgprot_t prot = arch_apei_get_mem_attribute(paddr);

	__set_fixmap(fixmap_idx, paddr, prot);

	return (void __iomem *)__fix_to_virt(fixmap_idx);
}

static void ghes_estatus_unmap(void __iomem *vaddr,
			       enum fixed_addresses fixmap_idx)
{
	int _idx = virt_to_fix((unsigned long)vaddr);

	WARN_ON_ONCE(fixmap_idx != _idx);
	clear_fixmap(fixmap_idx);
}

static void ghes_estatus_copy_buffer(struct ghes *ghes, void *buffer,
				     phys_addr_t paddr, size_t len,
				     int from_phys,
				     enum fixed_addresses fixmap_idx)
{
	void __iomem *vaddr;
	u64 offset;
	u32 chunk;

	while (len > 0) {
		offset = paddr - (paddr & PAGE_MASK);
		vaddr = ghes_estatus_map(ghes, PHYS_PFN(paddr), fixmap_idx);
		chunk = PAGE_SIZE - offset;
		chunk = min_t(u32, chunk, len);
		if (from_phys == GHES_ESTATUS_RW_FROM_PHYS)
			memcpy_fromio(buffer, vaddr + offset, chunk);
		else
			memcpy_toio(vaddr + offset, buffer, chunk);

		len -= chunk;
		paddr += chunk;
		buffer += chunk;
		ghes_estatus_unmap(vaddr, fixmap_idx);
	}
}

static int ghes_estatus_get_phys(struct estatus_source *source,
				 phys_addr_t *addr)
{
	struct ghes *ghes = source->priv;
	u64 value = 0;
	int rc;

	rc = apei_read(&value, &ghes->generic->error_status_address);
	if (rc)
		return rc;

	*addr = value;
	return 0;
}

static int ghes_estatus_read(struct estatus_source *source, phys_addr_t addr,
			     void *buf, size_t len,
			     enum fixed_addresses fixmap_idx)
{
	struct ghes *ghes = source->priv;

	ghes_estatus_copy_buffer(ghes, buf, addr, len,
				 GHES_ESTATUS_RW_FROM_PHYS, fixmap_idx);
	return 0;
}

static int ghes_estatus_write(struct estatus_source *source, phys_addr_t addr,
			      const void *buf, size_t len,
			      enum fixed_addresses fixmap_idx)
{
	struct ghes *ghes = source->priv;

	ghes_estatus_copy_buffer(ghes, (void *)buf, addr, len,
				 GHES_ESTATUS_RW_TO_PHYS, fixmap_idx);
	return 0;
}

static void ghes_estatus_ack(struct estatus_source *source)
{
	struct ghes *ghes = source->priv;
	struct acpi_hest_generic_v2 *gv2;
	u64 val = 0;
	int rc;

	if (ghes->generic->header.type != ACPI_HEST_TYPE_GENERIC_ERROR_V2)
		return;

	gv2 = ghes->generic_v2;
	rc = apei_read(&val, &gv2->read_ack_register);
	if (rc)
		return;

	val &= gv2->read_ack_preserve << gv2->read_ack_register.bit_offset;
	val |= gv2->read_ack_write << gv2->read_ack_register.bit_offset;

	apei_write(val, &gv2->read_ack_register);
}

static size_t ghes_estatus_get_max_len(struct estatus_source *source)
{
	struct ghes *ghes = source->priv;

	return ghes->generic->error_block_length;
}

static enum estatus_notify_mode
ghes_estatus_get_notify_mode(struct estatus_source *source)
{
	struct ghes *ghes = source->priv;
	u8 notify_type = ghes->generic->notify.type;

	if (notify_type == ACPI_HEST_NOTIFY_SEA)
		return ESTATUS_NOTIFY_SEA;

	return ESTATUS_NOTIFY_ASYNC;
}

static const char *ghes_estatus_get_name(struct estatus_source *source)
{
	struct ghes *ghes = source->priv;

	if (ghes->dev)
		return dev_name(ghes->dev);

	return ghes->name;
}

static const struct estatus_ops ghes_estatus_ops = {
	.get_phys	= ghes_estatus_get_phys,
	.read		= ghes_estatus_read,
	.write		= ghes_estatus_write,
	.ack		= ghes_estatus_ack,
	.get_max_len	= ghes_estatus_get_max_len,
	.get_notify_mode = ghes_estatus_get_notify_mode,
	.get_name	= ghes_estatus_get_name,
};
