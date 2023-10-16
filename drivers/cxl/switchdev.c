// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright(c) Huawei Technologies
 * Based on cxl/pci.c Copyright(c) 2020 Intel Corporation. All rights reserved.
 */

#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/moduleparam.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/sizes.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/io.h>
#include "cxlpci.h"
#include "switch.h"
#include "cxl.h"

static irqreturn_t cxl_swmb_mbox_irq(int irq, void *d)
{
	return cxl_mbox_irq(irq, d);
}

static int cxl_swmb_setup_mailbox(struct cxl_mbox *mbox)
{
	const int cap = readl(mbox->mbox + CXLDEV_MBOX_CAPS_OFFSET);

	/*
	 * A command may be in flight from a previous driver instance,
	 * think kexec, do one doorbell wait so that
	 * __cxl_pci_mbox_send_cmd() can assume that it is the only
	 * source for future doorbell busy events.
	 */
	if (cxl_pci_mbox_wait_for_doorbell(mbox) != 0) {
		dev_err(mbox->dev, "timeout awaiting mailbox idle");

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
		int irq, msgnum, rc;
		struct pci_dev *pdev = to_pci_dev(mbox->dev);

		msgnum = FIELD_GET(CXLDEV_MBOX_CAP_IRQ_MSGNUM_MASK, cap);
		irq = pci_irq_vector(pdev, msgnum);
		if (irq < 0)
			goto mbox_poll;

		rc = devm_request_threaded_irq(mbox->dev, irq, cxl_swmb_mbox_irq,
					       NULL, IRQF_SHARED | IRQF_ONESHOT,
					       NULL, mbox);
		if (rc)
			goto mbox_poll;

		/* enable background command mbox irq support */
		ctrl = readl(mbox->mbox + CXLDEV_MBOX_CTRL_OFFSET);
		ctrl |= CXLDEV_MBOX_CTRL_BG_CMD_IRQ;
		writel(ctrl, mbox->mbox + CXLDEV_MBOX_CTRL_OFFSET);

		return 0;
	}

mbox_poll:

	dev_dbg(mbox->dev, "Mailbox interrupts are unsupported");
	return 0;
}


static int cxl_swmb_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct cxl_register_map map;
	struct cxl_swdev *cxlswd;
	int rc;

	rc = pcim_enable_device(pdev);
	if (rc)
		return rc;

	cxlswd = cxl_swdev_alloc(&pdev->dev);
	if (IS_ERR(cxlswd))
		return PTR_ERR(cxlswd);

	mutex_init(&cxlswd->mbox.mbox_mutex);
	rc = cxl_find_regblock(pdev, CXL_REGLOC_RBI_MEMDEV, &map);
	if (rc)
		return rc;
	rc = cxl_setup_regs(&map);
	if (rc)
		return rc;

	rc = cxl_map_mbox_regs(&map, &cxlswd->mbox.mbox, &cxlswd->mbox.status);
	if (rc)
		return rc;

	cxlswd->mbox.dev = &pdev->dev;

	rc = cxl_swmb_setup_mailbox(&cxlswd->mbox);
	if (rc)
		return rc;

	pci_set_drvdata(pdev, cxlswd);

	rc = cxl_enumerate_cmds(&cxlswd->mbox);
	if (rc)
		goto error_put_device;

	rc = cdev_device_add(&cxlswd->cdev, &cxlswd->dev);
	if (rc)
		goto error_put_device;

	return 0;

error_put_device:
	cxl_swdev_shutdown(cxlswd);
	put_device(&cxlswd->dev);
	return rc;
}

static void cxl_swbm_remove(struct pci_dev *pdev)
{
	struct cxl_swdev *cxlswd = pci_get_drvdata(pdev);
	struct device *dev = &cxlswd->dev;

	cxl_swdev_shutdown(cxlswd);
	cdev_device_del(&cxlswd->cdev, dev);
	put_device(&cxlswd->dev);
}

static const struct pci_device_id cxl_swmb_pci_tbl[] = {
	{ PCI_DEVICE_CLASS(PCI_CLASS_SERIAL_CXL_SWITCH_CCI, ~0) },
	{}
};
MODULE_DEVICE_TABLE(pci, cxl_swmb_pci_tbl);

static struct pci_driver cxl_swmb_driver = {
	.name = KBUILD_MODNAME,
	.id_table = cxl_swmb_pci_tbl,
	.probe = cxl_swmb_probe,
	.remove = cxl_swbm_remove,
};

module_pci_driver(cxl_swmb_driver);
MODULE_DESCRIPTION("CXL Switch CCI mailbox access driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(CXL);
