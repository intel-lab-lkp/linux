// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2020 Intel Corporation. All rights reserved. */
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/moduleparam.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/sizes.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/aer.h>
#include <linux/io.h>
#include "cxlmbox.h"
#include "cxlmem.h"
#include "cxlpci.h"
#include "cxl.h"
#include "pmu.h"

/**
 * DOC: cxl pci
 *
 * This implements the PCI exclusive functionality for a CXL device as it is
 * defined by the Compute Express Link specification. CXL devices may surface
 * certain functionality even if it isn't CXL enabled. While this driver is
 * focused around the PCI specific aspects of a CXL device, it binds to the
 * specific CXL memory device class code, and therefore the implementation of
 * cxl_pci is focused around CXL memory devices.
 *
 * The driver has several responsibilities, mainly:
 *  - Create the memX device and register on the CXL bus.
 *  - Enumerate device's register interface and map them.
 *  - Registers nvdimm bridge device with cxl_core.
 *  - Registers a CXL mailbox with cxl_core.
 */

/*
 * CXL 2.0 ECN "Add Mailbox Ready Time" defines a capability field to
 * dictate how long to wait for the mailbox to become ready. The new
 * field allows the device to tell software the amount of time to wait
 * before mailbox ready. This field per the spec theoretically allows
 * for up to 255 seconds. 255 seconds is unreasonably long, its longer
 * than the maximum SATA port link recovery wait. Default to 60 seconds
 * until someone builds a CXL device that needs more time in practice.
 */
static unsigned short mbox_ready_timeout = 60;
module_param(mbox_ready_timeout, ushort, 0644);
MODULE_PARM_DESC(mbox_ready_timeout, "seconds to wait for mailbox ready");

struct cxl_dev_id {
	struct cxl_memdev_state *mds;
};

static int cxl_request_irq(struct device *dev, struct cxl_memdev_state *mds,
			   int irq, irq_handler_t handler,
			   irq_handler_t thread_fn)
{
	struct cxl_dev_id *dev_id;

	/* dev_id must be globally unique and must contain the cxlds */
	dev_id = devm_kzalloc(dev, sizeof(*dev_id), GFP_KERNEL);
	if (!dev_id)
		return -ENOMEM;
	dev_id->mds = mds;

	return devm_request_threaded_irq(dev, irq, handler, thread_fn,
					 IRQF_SHARED | IRQF_ONESHOT,
					 NULL, dev_id);
}

static bool cxl_pci_mbox_special_irq(struct cxl_mbox *mbox, u16 opcode)
{
	if (opcode == CXL_MBOX_OP_SANITIZE) {
		struct cxl_memdev_state *mds =
			container_of(mbox, struct cxl_memdev_state, mbox);

		if (mds->security.sanitize_node)
			sysfs_notify_dirent(mds->security.sanitize_node);
		dev_dbg(mbox->dev, "Sanitization operation ended\n");
		return true;
	}

	return false;
}

static bool cxl_pci_mbox_special_bg(struct cxl_mbox *mbox, u16 opcode)
{
	if (opcode == CXL_MBOX_OP_SANITIZE) {
		struct cxl_memdev_state *mds =
			container_of(mbox, struct cxl_memdev_state, mbox);

		if (mds->security.poll) {
			/* give first timeout a second */
			int timeout = 1;
			/* hold the device throughout */
			get_device(mds->cxlds.dev);

			mds->security.poll_tmo_secs = timeout;
			queue_delayed_work(system_wq,
					&mds->security.poll_dwork,
					timeout * HZ);
		}
		dev_dbg(mbox->dev, "Sanitization operation started\n");

		return true;
	}

	return false;
}

static irqreturn_t cxl_pci_mbox_irq(int irq, void *id)
{
	struct cxl_dev_id *dev_id = id;
	struct cxl_memdev_state *mds = dev_id->mds;

	return cxl_mbox_irq(irq, &mds->mbox);
}

/*
 * Sanitization operation polling mode.
 */
static void cxl_mbox_sanitize_work(struct work_struct *work)
{
	struct cxl_memdev_state *mds =
		container_of(work, typeof(*mds), security.poll_dwork.work);
	struct cxl_dev_state *cxlds = &mds->cxlds;

	mutex_lock(&mds->mbox.mbox_mutex);
	if (cxl_mbox_background_complete(&mds->mbox)) {
		mds->security.poll_tmo_secs = 0;
		put_device(cxlds->dev);

		if (mds->security.sanitize_node)
			sysfs_notify_dirent(mds->security.sanitize_node);

		dev_dbg(cxlds->dev, "Sanitization operation ended\n");
	} else {
		int timeout = mds->security.poll_tmo_secs + 10;

		mds->security.poll_tmo_secs = min(15 * 60, timeout);
		queue_delayed_work(system_wq, &mds->security.poll_dwork,
				   timeout * HZ);
	}
	mutex_unlock(&mds->mbox.mbox_mutex);
}

static u64 cxl_pci_mbox_get_status(struct cxl_mbox *mbox)
{
	struct cxl_memdev_state *mds = container_of(mbox, struct cxl_memdev_state, mbox);

	return readq(mds->cxlds.regs.memdev + CXLMDEV_STATUS_OFFSET);
}

static bool cxl_pci_mbox_can_run(struct cxl_mbox *mbox, u16 opcode)
{
	struct cxl_memdev_state *mds = container_of(mbox, struct cxl_memdev_state, mbox);

	if (mds->security.poll_tmo_secs > 0) {
		if (opcode != CXL_MBOX_OP_GET_HEALTH_INFO)
			return false;
	}

	return true;
}

static void cxl_pci_mbox_init_poll(struct cxl_mbox *mbox)
{
	struct cxl_memdev_state *mds = container_of(mbox, struct cxl_memdev_state, mbox);

	mds->security.poll = true;
	INIT_DELAYED_WORK(&mds->security.poll_dwork, cxl_mbox_sanitize_work);
}

static bool cxl_pci_mbox_extra_cmds(struct cxl_mbox *mbox, u16 opcode)
{
	struct cxl_memdev_state *mds = container_of(mbox, struct cxl_memdev_state, mbox);
	bool found = false;
	if (cxl_is_poison_command(opcode)) {
		cxl_set_poison_cmd_enabled(&mds->poison, opcode);
		found = true;
	}
	if (cxl_is_security_command(opcode)) {
		cxl_set_security_cmd_enabled(&mds->security, opcode);
		found = true;
	}

	return found;
}

static int cxl_pci_setup_mailbox(struct cxl_memdev_state *mds)
{
	struct cxl_mbox *mbox = &mds->mbox;
	const int cap = readl(mbox->mbox + CXLDEV_MBOX_CAPS_OFFSET);
	unsigned long timeout;
	u64 md_status;

	timeout = jiffies + mbox_ready_timeout * HZ;
	do {
		md_status = readq(mds->cxlds.regs.memdev + CXLMDEV_STATUS_OFFSET);
		if (md_status & CXLMDEV_MBOX_IF_READY)
			break;
		if (msleep_interruptible(100))
			break;
	} while (!time_after(jiffies, timeout));

	if (!(md_status & CXLMDEV_MBOX_IF_READY)) {
		cxl_err(mbox->dev, md_status, "timeout awaiting mailbox ready");
		return -ETIMEDOUT;
	}

	/*
	 * A command may be in flight from a previous driver instance,
	 * think kexec, do one doorbell wait so that
	 * __cxl_pci_mbox_send_cmd() can assume that it is the only
	 * source for future doorbell busy events.
	 */
	if (cxl_pci_mbox_wait_for_doorbell(mbox) != 0) {
		md_status = readq(mds->cxlds.regs.memdev + CXLMDEV_STATUS_OFFSET);
		cxl_err(mbox->dev, md_status, "timeout awaiting mailbox idle");

		return -ETIMEDOUT;
	}

	mbox->payload_size =
		1 << FIELD_GET(CXLDEV_MBOX_CAP_PAYLOAD_SIZE_MASK, cap);

	/*
	 * CXL 2.0 8.2.8.4.3 Mailbox Capabilities Register
	 *
	 * If the size is too small, mandatory commands will not work and so
	 * there's no point in going forward. If the size is too large, there's
	 * no harm is soft limiting it.
	 */
	mbox->payload_size = min_t(size_t, mbox->payload_size, SZ_1M);
	if (mbox->payload_size < 256) {
		dev_err(mbox->dev, "Mailbox is too small (%zub)",
			mbox->payload_size);
		return -ENXIO;
	}

	dev_dbg(mbox->dev, "Mailbox payload sized %zu", mbox->payload_size);

	rcuwait_init(&mbox->mbox_wait);

	if (cap & CXLDEV_MBOX_CAP_BG_CMD_IRQ) {
		u32 ctrl;
		int irq, msgnum;
		struct pci_dev *pdev = to_pci_dev(mbox->dev);

		msgnum = FIELD_GET(CXLDEV_MBOX_CAP_IRQ_MSGNUM_MASK, cap);
		irq = pci_irq_vector(pdev, msgnum);
		if (irq < 0)
			goto mbox_poll;

		if (cxl_request_irq(mbox->dev, mds, irq, cxl_pci_mbox_irq, NULL))
			goto mbox_poll;

		/* enable background command mbox irq support */
		ctrl = readl(mbox->mbox + CXLDEV_MBOX_CTRL_OFFSET);
		ctrl |= CXLDEV_MBOX_CTRL_BG_CMD_IRQ;
		writel(ctrl, mbox->mbox + CXLDEV_MBOX_CTRL_OFFSET);

		return 0;
	}

mbox_poll:

	cxl_pci_mbox_init_poll(mbox);

	dev_dbg(mbox->dev, "Mailbox interrupts are unsupported");
	return 0;
}

/*
 * Assume that any RCIEP that emits the CXL memory expander class code
 * is an RCD
 */
static bool is_cxl_restricted(struct pci_dev *pdev)
{
	return pci_pcie_type(pdev) == PCI_EXP_TYPE_RC_END;
}

static int cxl_rcrb_get_comp_regs(struct pci_dev *pdev,
				  struct cxl_register_map *map)
{
	struct cxl_port *port;
	struct cxl_dport *dport;
	resource_size_t component_reg_phys;

	*map = (struct cxl_register_map) {
		.dev = &pdev->dev,
		.resource = CXL_RESOURCE_NONE,
	};

	port = cxl_pci_find_port(pdev, &dport);
	if (!port)
		return -EPROBE_DEFER;

	component_reg_phys = cxl_rcd_component_reg_phys(&pdev->dev, dport);

	put_device(&port->dev);

	if (component_reg_phys == CXL_RESOURCE_NONE)
		return -ENXIO;

	map->resource = component_reg_phys;
	map->reg_type = CXL_REGLOC_RBI_COMPONENT;
	map->max_size = CXL_COMPONENT_REG_BLOCK_SIZE;

	return 0;
}

static int cxl_pci_setup_regs(struct pci_dev *pdev, enum cxl_regloc_type type,
			      struct cxl_register_map *map)
{
	int rc;

	rc = cxl_find_regblock(pdev, type, map);

	/*
	 * If the Register Locator DVSEC does not exist, check if it
	 * is an RCH and try to extract the Component Registers from
	 * an RCRB.
	 */
	if (rc && type == CXL_REGLOC_RBI_COMPONENT && is_cxl_restricted(pdev))
		rc = cxl_rcrb_get_comp_regs(pdev, map);

	if (rc)
		return rc;

	return cxl_setup_regs(map);
}

static int cxl_pci_ras_unmask(struct pci_dev *pdev)
{
	struct cxl_dev_state *cxlds = pci_get_drvdata(pdev);
	void __iomem *addr;
	u32 orig_val, val, mask;
	u16 cap;
	int rc;

	if (!cxlds->regs.ras) {
		dev_dbg(&pdev->dev, "No RAS registers.\n");
		return 0;
	}

	/* BIOS has PCIe AER error control */
	if (!pcie_aer_is_native(pdev))
		return 0;

	rc = pcie_capability_read_word(pdev, PCI_EXP_DEVCTL, &cap);
	if (rc)
		return rc;

	if (cap & PCI_EXP_DEVCTL_URRE) {
		addr = cxlds->regs.ras + CXL_RAS_UNCORRECTABLE_MASK_OFFSET;
		orig_val = readl(addr);

		mask = CXL_RAS_UNCORRECTABLE_MASK_MASK |
		       CXL_RAS_UNCORRECTABLE_MASK_F256B_MASK;
		val = orig_val & ~mask;
		writel(val, addr);
		dev_dbg(&pdev->dev,
			"Uncorrectable RAS Errors Mask: %#x -> %#x\n",
			orig_val, val);
	}

	if (cap & PCI_EXP_DEVCTL_CERE) {
		addr = cxlds->regs.ras + CXL_RAS_CORRECTABLE_MASK_OFFSET;
		orig_val = readl(addr);
		val = orig_val & ~CXL_RAS_CORRECTABLE_MASK_MASK;
		writel(val, addr);
		dev_dbg(&pdev->dev, "Correctable RAS Errors Mask: %#x -> %#x\n",
			orig_val, val);
	}

	return 0;
}

static void free_event_buf(void *buf)
{
	kvfree(buf);
}

/*
 * There is a single buffer for reading event logs from the mailbox.  All logs
 * share this buffer protected by the mds->event_log_lock.
 */
static int cxl_mem_alloc_event_buf(struct cxl_memdev_state *mds)
{
	struct cxl_get_event_payload *buf;

	buf = kvmalloc(mds->mbox.payload_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	mds->event.buf = buf;

	return devm_add_action_or_reset(mds->cxlds.dev, free_event_buf, buf);
}

static int cxl_alloc_irq_vectors(struct pci_dev *pdev)
{
	int nvecs;

	/*
	 * Per CXL 3.0 3.1.1 CXL.io Endpoint a function on a CXL device must
	 * not generate INTx messages if that function participates in
	 * CXL.cache or CXL.mem.
	 *
	 * Additionally pci_alloc_irq_vectors() handles calling
	 * pci_free_irq_vectors() automatically despite not being called
	 * pcim_*.  See pci_setup_msi_context().
	 */
	nvecs = pci_alloc_irq_vectors(pdev, 1, CXL_PCI_DEFAULT_MAX_VECTORS,
				      PCI_IRQ_MSIX | PCI_IRQ_MSI);
	if (nvecs < 1) {
		dev_dbg(&pdev->dev, "Failed to alloc irq vectors: %d\n", nvecs);
		return -ENXIO;
	}
	return 0;
}

static irqreturn_t cxl_event_thread(int irq, void *id)
{
	struct cxl_dev_id *dev_id = id;
	struct cxl_memdev_state *mds = dev_id->mds;
	u32 status;

	do {
		/*
		 * CXL 3.0 8.2.8.3.1: The lower 32 bits are the status;
		 * ignore the reserved upper 32 bits
		 */
		status = readl(mds->cxlds.regs.status + CXLDEV_DEV_EVENT_STATUS_OFFSET);
		/* Ignore logs unknown to the driver */
		status &= CXLDEV_EVENT_STATUS_ALL;
		if (!status)
			break;
		cxl_mem_get_event_records(mds, status);
		cond_resched();
	} while (status);

	return IRQ_HANDLED;
}

static int cxl_event_req_irq(struct cxl_memdev_state *mds, u8 setting)
{
	struct pci_dev *pdev = to_pci_dev(mds->cxlds.dev);
	int irq;

	if (FIELD_GET(CXLDEV_EVENT_INT_MODE_MASK, setting) != CXL_INT_MSI_MSIX)
		return -ENXIO;

	irq =  pci_irq_vector(pdev,
			      FIELD_GET(CXLDEV_EVENT_INT_MSGNUM_MASK, setting));
	if (irq < 0)
		return irq;

	return cxl_request_irq(mds->cxlds.dev, mds, irq, NULL, cxl_event_thread);
}

static int cxl_event_get_int_policy(struct cxl_memdev_state *mds,
				    struct cxl_event_interrupt_policy *policy)
{
	struct cxl_mbox_cmd mbox_cmd = {
		.opcode = CXL_MBOX_OP_GET_EVT_INT_POLICY,
		.payload_out = policy,
		.size_out = sizeof(*policy),
	};
	int rc;

	rc = cxl_internal_send_cmd(&mds->mbox, &mbox_cmd);
	if (rc < 0)
		dev_err(mds->cxlds.dev,
			"Failed to get event interrupt policy : %d", rc);

	return rc;
}

static int cxl_event_config_msgnums(struct cxl_memdev_state *mds,
				    struct cxl_event_interrupt_policy *policy)
{
	struct cxl_mbox_cmd mbox_cmd;
	int rc;

	*policy = (struct cxl_event_interrupt_policy) {
		.info_settings = CXL_INT_MSI_MSIX,
		.warn_settings = CXL_INT_MSI_MSIX,
		.failure_settings = CXL_INT_MSI_MSIX,
		.fatal_settings = CXL_INT_MSI_MSIX,
	};

	mbox_cmd = (struct cxl_mbox_cmd) {
		.opcode = CXL_MBOX_OP_SET_EVT_INT_POLICY,
		.payload_in = policy,
		.size_in = sizeof(*policy),
	};

	rc = cxl_internal_send_cmd(&mds->mbox, &mbox_cmd);
	if (rc < 0) {
		dev_err(mds->cxlds.dev, "Failed to set event interrupt policy : %d",
			rc);
		return rc;
	}

	/* Retrieve final interrupt settings */
	return cxl_event_get_int_policy(mds, policy);
}

static int cxl_event_irqsetup(struct cxl_memdev_state *mds)
{
	struct device *dev = mds->cxlds.dev;
	struct cxl_event_interrupt_policy policy;
	int rc;

	rc = cxl_event_config_msgnums(mds, &policy);
	if (rc)
		return rc;

	rc = cxl_event_req_irq(mds, policy.info_settings);
	if (rc) {
		dev_err(dev, "Failed to get interrupt for event Info log\n");
		return rc;
	}

	rc = cxl_event_req_irq(mds, policy.warn_settings);
	if (rc) {
		dev_err(dev, "Failed to get interrupt for event Warn log\n");
		return rc;
	}

	rc = cxl_event_req_irq(mds, policy.failure_settings);
	if (rc) {
		dev_err(dev, "Failed to get interrupt for event Failure log\n");
		return rc;
	}

	rc = cxl_event_req_irq(mds, policy.fatal_settings);
	if (rc) {
		dev_err(dev, "Failed to get interrupt for event Fatal log\n");
		return rc;
	}

	return 0;
}

static bool cxl_event_int_is_fw(u8 setting)
{
	u8 mode = FIELD_GET(CXLDEV_EVENT_INT_MODE_MASK, setting);

	return mode == CXL_INT_FW;
}

static int cxl_event_config(struct pci_host_bridge *host_bridge,
			    struct cxl_memdev_state *mds)
{
	struct cxl_event_interrupt_policy policy;
	int rc;

	/*
	 * When BIOS maintains CXL error reporting control, it will process
	 * event records.  Only one agent can do so.
	 */
	if (!host_bridge->native_cxl_error)
		return 0;

	rc = cxl_mem_alloc_event_buf(mds);
	if (rc)
		return rc;

	rc = cxl_event_get_int_policy(mds, &policy);
	if (rc)
		return rc;

	if (cxl_event_int_is_fw(policy.info_settings) ||
	    cxl_event_int_is_fw(policy.warn_settings) ||
	    cxl_event_int_is_fw(policy.failure_settings) ||
	    cxl_event_int_is_fw(policy.fatal_settings)) {
		dev_err(mds->cxlds.dev,
			"FW still in control of Event Logs despite _OSC settings\n");
		return -EBUSY;
	}

	rc = cxl_event_irqsetup(mds);
	if (rc)
		return rc;

	cxl_mem_get_event_records(mds, CXLDEV_EVENT_STATUS_ALL);

	return 0;
}

static int cxl_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct pci_host_bridge *host_bridge = pci_find_host_bridge(pdev->bus);
	struct cxl_memdev_state *mds;
	struct cxl_dev_state *cxlds;
	struct cxl_register_map map;
	struct cxl_memdev *cxlmd;
	int i, rc, pmu_count;

	/*
	 * Double check the anonymous union trickery in struct cxl_regs
	 * FIXME switch to struct_group()
	 */
	BUILD_BUG_ON(offsetof(struct cxl_regs, memdev) !=
		     offsetof(struct cxl_regs, device_regs.memdev));

	rc = pcim_enable_device(pdev);
	if (rc)
		return rc;
	pci_set_master(pdev);

	mds = cxl_memdev_state_create(&pdev->dev);
	if (IS_ERR(mds))
		return PTR_ERR(mds);
	cxlds = &mds->cxlds;
	pci_set_drvdata(pdev, cxlds);

	cxlds->rcd = is_cxl_restricted(pdev);
	cxlds->serial = pci_get_dsn(pdev);
	cxlds->cxl_dvsec = pci_find_dvsec_capability(
		pdev, PCI_DVSEC_VENDOR_ID_CXL, CXL_DVSEC_PCIE_DEVICE);
	if (!cxlds->cxl_dvsec)
		dev_warn(&pdev->dev,
			 "Device DVSEC not present, skip CXL.mem init\n");

	rc = cxl_pci_setup_regs(pdev, CXL_REGLOC_RBI_MEMDEV, &map);
	if (rc)
		return rc;

	rc = cxl_map_device_regs(&map, &cxlds->regs.device_regs);
	if (rc)
		return rc;

	/* Status register already mapped */
	rc = cxl_map_mbox_regs(&map, &mds->mbox.mbox, NULL);
	if (rc)
		return rc;

	/* Connect up the status register access for the mbox */
	mds->mbox.status = cxlds->regs.status;

	/*
	 * If the component registers can't be found, the cxl_pci driver may
	 * still be useful for management functions so don't return an error.
	 */
	cxlds->component_reg_phys = CXL_RESOURCE_NONE;
	rc = cxl_pci_setup_regs(pdev, CXL_REGLOC_RBI_COMPONENT, &map);
	if (rc)
		dev_warn(&pdev->dev, "No component registers (%d)\n", rc);
	else if (!map.component_map.ras.valid)
		dev_dbg(&pdev->dev, "RAS registers not found\n");

	cxlds->component_reg_phys = map.resource;

	rc = cxl_map_component_regs(&map, &cxlds->regs.component,
				    BIT(CXL_CM_CAP_CAP_ID_RAS));
	if (rc)
		dev_dbg(&pdev->dev, "Failed to map RAS capability.\n");

	rc = cxl_await_media_ready(cxlds);
	if (rc == 0)
		cxlds->media_ready = true;
	else
		dev_warn(&pdev->dev, "Media not active (%d)\n", rc);

	rc = cxl_alloc_irq_vectors(pdev);
	if (rc)
		return rc;

	mds->mbox.status = cxlds->regs.status;
	mds->mbox.dev = &pdev->dev;
	mds->mbox.special_irq = cxl_pci_mbox_special_irq;
	mds->mbox.special_bg = cxl_pci_mbox_special_bg;
	mds->mbox.get_status = cxl_pci_mbox_get_status;
	mds->mbox.can_run = cxl_pci_mbox_can_run;
	mds->mbox.extra_cmds = cxl_pci_mbox_extra_cmds;

	rc = cxl_pci_setup_mailbox(mds);
	if (rc)
		return rc;

	rc = cxl_enumerate_cmds(&mds->mbox);
	if (rc)
		return rc;

	rc = cxl_set_timestamp(mds);
	if (rc)
		return rc;

	rc = cxl_poison_state_init(mds);
	if (rc)
		return rc;

	rc = cxl_dev_state_identify(mds);
	if (rc)
		return rc;

	rc = cxl_mem_create_range_info(mds);
	if (rc)
		return rc;

	cxlmd = devm_cxl_add_memdev(cxlds);
	if (IS_ERR(cxlmd))
		return PTR_ERR(cxlmd);

	rc = cxl_memdev_setup_fw_upload(mds);
	if (rc)
		return rc;

	pmu_count = cxl_count_regblock(pdev, CXL_REGLOC_RBI_PMU);
	for (i = 0; i < pmu_count; i++) {
		struct cxl_pmu_regs pmu_regs;

		rc = cxl_find_regblock_instance(pdev, CXL_REGLOC_RBI_PMU, &map, i);
		if (rc) {
			dev_dbg(&pdev->dev, "Could not find PMU regblock\n");
			break;
		}

		rc = cxl_map_pmu_regs(pdev, &pmu_regs, &map);
		if (rc) {
			dev_dbg(&pdev->dev, "Could not map PMU regs\n");
			break;
		}

		rc = devm_cxl_pmu_add(cxlds->dev, &pmu_regs, cxlmd->id, i, CXL_PMU_MEMDEV);
		if (rc) {
			dev_dbg(&pdev->dev, "Could not add PMU instance\n");
			break;
		}
	}

	rc = cxl_event_config(host_bridge, mds);
	if (rc)
		return rc;

	rc = cxl_pci_ras_unmask(pdev);
	if (rc)
		dev_dbg(&pdev->dev, "No RAS reporting unmasked\n");

	pci_save_state(pdev);

	return rc;
}

static const struct pci_device_id cxl_mem_pci_tbl[] = {
	/* PCI class code for CXL.mem Type-3 Devices */
	{ PCI_DEVICE_CLASS((PCI_CLASS_MEMORY_CXL << 8 | CXL_MEMORY_PROGIF), ~0)},
	{ /* terminate list */ },
};
MODULE_DEVICE_TABLE(pci, cxl_mem_pci_tbl);

static pci_ers_result_t cxl_slot_reset(struct pci_dev *pdev)
{
	struct cxl_dev_state *cxlds = pci_get_drvdata(pdev);
	struct cxl_memdev *cxlmd = cxlds->cxlmd;
	struct device *dev = &cxlmd->dev;

	dev_info(&pdev->dev, "%s: restart CXL.mem after slot reset\n",
		 dev_name(dev));
	pci_restore_state(pdev);
	if (device_attach(dev) <= 0)
		return PCI_ERS_RESULT_DISCONNECT;
	return PCI_ERS_RESULT_RECOVERED;
}

static void cxl_error_resume(struct pci_dev *pdev)
{
	struct cxl_dev_state *cxlds = pci_get_drvdata(pdev);
	struct cxl_memdev *cxlmd = cxlds->cxlmd;
	struct device *dev = &cxlmd->dev;

	dev_info(&pdev->dev, "%s: error resume %s\n", dev_name(dev),
		 dev->driver ? "successful" : "failed");
}

static const struct pci_error_handlers cxl_error_handlers = {
	.error_detected	= cxl_error_detected,
	.slot_reset	= cxl_slot_reset,
	.resume		= cxl_error_resume,
	.cor_error_detected	= cxl_cor_error_detected,
};

static struct pci_driver cxl_pci_driver = {
	.name			= KBUILD_MODNAME,
	.id_table		= cxl_mem_pci_tbl,
	.probe			= cxl_pci_probe,
	.err_handler		= &cxl_error_handlers,
	.driver	= {
		.probe_type	= PROBE_PREFER_ASYNCHRONOUS,
	},
};

MODULE_LICENSE("GPL v2");
module_pci_driver(cxl_pci_driver);
MODULE_IMPORT_NS(CXL);
