// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Service driver for PCIe Flit Logging Extended Capability
 *
 * Copyright (c) 2026, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Authors:	Avadhut Naik <Avadhut.Naik@amd.com>
 *		Yazen Ghannam <Yazen.Ghannam@amd.com>
 */

#define pr_fmt(fmt) "Flit: " fmt
#define dev_fmt pr_fmt

#include <linux/bitfield.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <ras/ras_event.h>
#include "portdrv.h"
#include "../pci.h"

/*
 * Report only a link accumulating errors far faster than the counter drains.
 * 0xff never fires, since an event needs the counter to exceed the trigger.
 */
#define FLIT_DEFAULT_TRIGGER	0xfe

/*
 * Per PCIe r6.4, sec 7.7.8.2, table 7-93, More Entries can be set again
 * mid-drain, so bound the loop.
 */
#define FLIT_MAX_LOG_ENTRIES	0xff

/*
 * Per PCIe r6.4, sec 7.7.8.2, table 7-92, an entry with none of these set
 * and Flit Error Log 2 zero is Reserved.
 */
#define FLIT_ERR_LOG1_ERROR	(PCI_FLIT_ERR_LOG1_UNRECOG | \
				 PCI_FLIT_ERR_LOG1_FEC_UNCOR | \
				 PCI_FLIT_ERR_LOG1_SYND_PARITY0 | \
				 PCI_FLIT_ERR_LOG1_SYND_CHECK0)

/* Only the port types pcie_portdrv_probe() claims get a Flit service. */
static bool flit_is_port(struct pci_dev *pdev)
{
	if (!pci_is_pcie(pdev))
		return false;

	switch (pci_pcie_type(pdev)) {
	case PCI_EXP_TYPE_ROOT_PORT:
	case PCI_EXP_TYPE_UPSTREAM:
	case PCI_EXP_TYPE_DOWNSTREAM:
	case PCI_EXP_TYPE_RC_EC:
		return true;
	default:
		return false;
	}
}

/*
 * Without _OSC control the OS must not touch the capability. Flit Logging
 * control is only meaningful alongside AER control, so require both.
 */
static bool flit_is_native(struct pci_dev *pdev)
{
	struct pci_host_bridge *host = pci_find_host_bridge(pdev->bus);

	if (pcie_ports_native)
		return true;

	return host->native_flit && host->native_aer;
}

struct flit_info {
	spinlock_t lock;	/* serializes Counter Control read-modify-write */
	struct ratelimit_state ratelimit;
	bool cntr_enabled;	/* Counter Enable as the driver intends it */
};

void pci_flit_init(struct pci_dev *pdev)
{
	if (!flit_is_port(pdev) || !flit_is_native(pdev))
		return;

	pdev->flit_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_FLIT);
	if (!pdev->flit_cap)
		return;

	pdev->flit_info = kzalloc_obj(*pdev->flit_info);
	if (!pdev->flit_info) {
		pdev->flit_cap = 0;
		return;
	}

	spin_lock_init(&pdev->flit_info->lock);

	ratelimit_state_init(&pdev->flit_info->ratelimit,
			     DEFAULT_RATELIMIT_INTERVAL,
			     DEFAULT_RATELIMIT_BURST);

	pci_add_ext_cap_save_buffer(pdev, PCI_EXT_CAP_ID_FLIT, sizeof(u16));

	pci_dbg(pdev, "Flit Logging Extended Capability present.\n");
}

void pci_flit_exit(struct pci_dev *pdev)
{
	kfree(pdev->flit_info);
	pdev->flit_info = NULL;
	pdev->flit_cap = 0;
}

void pci_save_flit_state(struct pci_dev *pdev)
{
	struct pci_cap_saved_state *save_state;
	u16 *cap;

	if (!pdev->flit_cap)
		return;

	save_state = pci_find_saved_ext_cap(pdev, PCI_EXT_CAP_ID_FLIT);
	if (!save_state)
		return;

	/*
	 * Take Counter Enable from the driver rather than the register.
	 * flit_isr() clears it and only flit_ist() sets it again once the log
	 * is drained, and a save landing in between would capture a zero that
	 * the next restore writes back for good.
	 */
	cap = (u16 *)&save_state->cap.data[0];
	pci_read_config_word(pdev, pdev->flit_cap + PCI_FLIT_ERR_CNTR_CTRL, cap);
	if (pdev->flit_info->cntr_enabled)
		*cap |= PCI_FLIT_ERR_CNTR_CTRL_EN;
	else
		*cap &= ~PCI_FLIT_ERR_CNTR_CTRL_EN;
}

void pci_restore_flit_state(struct pci_dev *pdev)
{
	struct pci_cap_saved_state *save_state;
	u16 *cap;

	if (!pdev->flit_cap)
		return;

	save_state = pci_find_saved_ext_cap(pdev, PCI_EXT_CAP_ID_FLIT);
	if (!save_state)
		return;

	cap = (u16 *)&save_state->cap.data[0];
	pci_write_config_word(pdev, pdev->flit_cap + PCI_FLIT_ERR_CNTR_CTRL, *cap);
}

/*
 * Serializes the Counter Control read-modify-write against flit_isr(), which
 * runs in hard irq context on the vector this port shares with PME, hotplug
 * and bandwidth notification. A lost update here leaves Trigger Event on Error
 * Count at zero, and per PCIe r6.4, sec 7.7.8.4, table 7-95, a zero trigger
 * never generates an event.
 */
static void flit_cntr_ctrl_update(struct pci_dev *pdev, u16 clear, u16 set)
{
	unsigned long flags;
	u16 ctrl;

	spin_lock_irqsave(&pdev->flit_info->lock, flags);
	pci_read_config_word(pdev, pdev->flit_cap + PCI_FLIT_ERR_CNTR_CTRL, &ctrl);
	ctrl &= ~clear;
	ctrl |= set;
	pci_write_config_word(pdev, pdev->flit_cap + PCI_FLIT_ERR_CNTR_CTRL, ctrl);
	spin_unlock_irqrestore(&pdev->flit_info->lock, flags);
}

/*
 * pci_dev_attr_groups[] in pci-sysfs.c is the only consumer of the group
 * below, and the ratelimit accessors it is built from live there too.
 */
#ifdef CONFIG_SYSFS

PCI_RATELIMIT_INTERVAL_ATTR(ratelimit_interval_ms, flit_info->ratelimit);
PCI_RATELIMIT_BURST_ATTR(ratelimit_burst, flit_info->ratelimit);

static struct attribute *flit_attrs[] = {
	&dev_attr_ratelimit_interval_ms.attr,
	&dev_attr_ratelimit_burst.attr,
	NULL
};

static bool flit_group_visible(struct kobject *kobj)
{
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj));

	return !!pdev->flit_info;
}

DEFINE_SIMPLE_SYSFS_GROUP_VISIBLE(flit);

const struct attribute_group flit_attr_group = {
	.name		= "flit",
	.attrs		= flit_attrs,
	.is_visible	= SYSFS_GROUP_VISIBLE(flit),
};

#endif /* CONFIG_SYSFS */

static void flit_cntr_enable(struct pci_dev *pdev)
{
	u16 flit = pdev->flit_cap;
	unsigned long flags;
	u16 reg;

	pci_read_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_STA, &reg);
	pci_write_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_STA, reg);

	pdev->flit_info->cntr_enabled = true;

	spin_lock_irqsave(&pdev->flit_info->lock, flags);
	pci_read_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_CTRL, &reg);

	/* Set default trigger count if not set by platform, since zero never fires */
	if (!(reg & PCI_FLIT_ERR_CNTR_CTRL_TRIGGER))
		reg |= FIELD_PREP(PCI_FLIT_ERR_CNTR_CTRL_TRIGGER, FLIT_DEFAULT_TRIGGER);

	reg |= PCI_FLIT_ERR_CNTR_CTRL_EN | PCI_FLIT_ERR_CNTR_CTRL_INTR_EN;
	pci_write_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_CTRL, reg);
	spin_unlock_irqrestore(&pdev->flit_info->lock, flags);
}

static void flit_cntr_disable(struct pci_dev *pdev)
{
	pdev->flit_info->cntr_enabled = false;
	flit_cntr_ctrl_update(pdev, PCI_FLIT_ERR_CNTR_CTRL_EN | PCI_FLIT_ERR_CNTR_CTRL_INTR_EN, 0);
}

static void flit_report(struct pci_dev *pdev, u32 err_log1, u32 err_log2)
{
	/* Software should silently discard a Reserved entry */
	if (!(err_log1 & FLIT_ERR_LOG1_ERROR) && !err_log2)
		return;

	if (__ratelimit(&pdev->flit_info->ratelimit))
		pci_warn(pdev, "ErrLog1: 0x%08x ErrLog2: 0x%08x\n", err_log1, err_log2);

	trace_flit_event(pci_name(pdev), err_log1, err_log2);
}

static irqreturn_t flit_ist(int irq, void *context)
{
	struct pcie_device *dev = (struct pcie_device *)context;
	struct pci_dev *pdev = dev->port;
	u16 flit = pdev->flit_cap;
	u32 err_log1, err_log2;
	unsigned int i;

	for (i = 0; i < FLIT_MAX_LOG_ENTRIES; i++) {
		pci_read_config_dword(pdev, flit + PCI_FLIT_ERR_LOG1, &err_log1);

		if (PCI_POSSIBLE_ERROR(err_log1) || !(err_log1 & PCI_FLIT_ERR_LOG1_VALID))
			break;

		pci_read_config_dword(pdev, flit + PCI_FLIT_ERR_LOG2, &err_log2);

		flit_report(pdev, err_log1, err_log2);

		pci_write_config_dword(pdev, flit + PCI_FLIT_ERR_LOG1, err_log1);

		if (!(err_log1 & PCI_FLIT_ERR_LOG1_MORE))
			break;
	}

	/*
	 * Re-enable the counter. Per PCIe r6.4, sec 7.7.8.5, table 7-96, the
	 * status bit is "Cleared on 0b to 1b transition of Flit Error Counter
	 * Enable", which also clears the counter and re-arms interrupt
	 * generation.
	 */
	flit_cntr_ctrl_update(pdev, 0, PCI_FLIT_ERR_CNTR_CTRL_EN);
	return IRQ_HANDLED;
}

static irqreturn_t flit_isr(int irq, void *context)
{
	struct pcie_device *dev = (struct pcie_device *)context;
	struct pci_dev *pdev = dev->port;
	u16 flit = pdev->flit_cap;
	u16 cntr_sta;

	pci_read_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_STA, &cntr_sta);
	if (PCI_POSSIBLE_ERROR(cntr_sta) || !(cntr_sta & PCI_FLIT_ERR_CNTR_STA_INTR_GEN))
		return IRQ_NONE;

	/*
	 * Clear Flit Error Counter Enable first, so the counter cannot reach
	 * the trigger again while the thread runs, then clear the status bit.
	 * A message-signaled vector needs no more than that, but this service
	 * can land on INTx, where the line stays asserted until the status is
	 * cleared. flit_ist() transitions Counter Enable back 0->1.
	 */
	flit_cntr_ctrl_update(pdev, PCI_FLIT_ERR_CNTR_CTRL_EN, 0);
	pci_write_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_STA,
			      PCI_FLIT_ERR_CNTR_STA_INTR_GEN);
	return IRQ_WAKE_THREAD;
}

static int flit_probe(struct pcie_device *dev)
{
	struct pci_dev *pdev = dev->port;
	int status;

	/*
	 * Not devm_request_threaded_irq(): devres would free the IRQ only
	 * after .remove() runs, too late to stop flit_ist() re-enabling the
	 * counter.
	 */
	status = request_threaded_irq(dev->irq, flit_isr, flit_ist,
				      IRQF_SHARED, "pcie-flit", dev);
	if (status) {
		pci_warn(pdev, "request Flit IRQ %d failed: %d\n", dev->irq, status);
		return status;
	}

	flit_cntr_enable(pdev);
	pci_info(pdev, "enabled with IRQ %d\n", dev->irq);

	return 0;
}

/*
 * No .suspend or .runtime_suspend counterpart. pci_save_state() runs after
 * both, so disabling the counter there would put a transient value in the
 * save buffer, and pcie_portdrv_slot_reset() restores that buffer after
 * calling .slot_reset.
 */
static int flit_cntr_restore(struct pcie_device *dev)
{
	flit_cntr_enable(dev->port);
	return 0;
}

static void flit_cntr_remove(struct pcie_device *dev)
{
	/* Drop the IRQ first: free_irq() waits for the threaded handler. */
	free_irq(dev->irq, dev);
	flit_cntr_disable(dev->port);
}

static struct pcie_port_service_driver flitdriver = {
	.name		= "flit",
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_FLIT,
	.probe		= flit_probe,
	.resume		= flit_cntr_restore,
	.runtime_resume	= flit_cntr_restore,
	.slot_reset	= flit_cntr_restore,
	.remove		= flit_cntr_remove,
};

int __init pcie_flit_init(void)
{
	return pcie_port_service_register(&flitdriver);
}
