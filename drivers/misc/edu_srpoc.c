// SPDX-License-Identifier: GPL-2.0
/*
 * edu_srpoc.c — Surprise Removal POC driver for the QEMU edu device
 *
 * In remove(), schedules a delayed interrupt on the edu device and
 * blocks waiting for it to complete. This simulates del_gendisk()
 * blocked in blk_mq_freeze_queue_wait() on slow in-flight I/O.
 *
 * Surprise-remove the device during this window to reproduce the hang.
 *
 * edu BAR 0 registers used:
 *   0x08  Factorial: write N to compute N! asynchronously
 *   0x20  Status: write EDU_STATUS_IRQFACT to enable IRQ on completion
 *   0x24  IRQ status: bit 0 = FACT_IRQ, bit 9 = DELAY_IRQ
 *   0x30  Delayed IRQ: write N (ms). Hacked in this functionality(not upstream).
 *   0x64  IRQ lower: write bitmask to ack
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/delay.h>

#define PCI_VENDOR_ID_EDU	0x1234
#define PCI_DEVICE_ID_EDU	0x11e8

#define EDU_REG_FACT		0x08
#define EDU_REG_STATUS		0x20
#define EDU_REG_DELAYED_IRQ	0x30
#define EDU_REG_IRQ_STATUS	0x24
#define EDU_REG_IRQ_LOWER	0x64

#define EDU_STATUS_IRQFACT	0x80
#define EDU_FACT_IRQ		BIT(0)
#define EDU_DELAY_IRQ		BIT(9)

/* Large enough to take several seconds in the QEMU thread */
#define EDU_SLOW_FACTORIAL	0xffffffff

struct edu_dev {
	struct pci_dev		*pdev;
	void __iomem		*regs;
	struct completion	irq_done;
};

static irqreturn_t edu_irq_handler(int irq, void *data)
{
	struct edu_dev *edu = data;
	u32 status;

	status = ioread32(edu->regs + EDU_REG_IRQ_STATUS);
	if (!status)
		return IRQ_NONE;

	iowrite32(status, edu->regs + EDU_REG_IRQ_LOWER);

	if (status & (EDU_FACT_IRQ | EDU_DELAY_IRQ))
	{
		pr_info("complete(&edu->irq_done)\n");
		complete(&edu->irq_done);
	}

	return IRQ_HANDLED;
}

static void edu_disconnect(struct work_struct *work)
{
	struct pci_dev *pdev = container_of(work, struct pci_dev,
					    disconnect_work);
	struct edu_dev *edu = pci_get_drvdata(pdev);

	dev_info(&pdev->dev, "edu_disconnect()\n");
	if (!pci_test_and_clear_disconnect_enable(pdev))
		return;

	if (!edu)
		return;

	dev_info(&pdev->dev, "disconnect_work fired — unblocking remove()\n");
	complete(&edu->irq_done);
}

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct edu_dev *edu;
	int err;

	dev_info(&pdev->dev, "edu_probe(): starting\n");

	edu = devm_kzalloc(&pdev->dev, sizeof(*edu), GFP_KERNEL);
	if (!edu)
		return -ENOMEM;

	edu->pdev = pdev;
	init_completion(&edu->irq_done);

	err = pci_enable_device(pdev);
	if (err)
		return err;

	err = pci_request_regions(pdev, "edu_srpoc");
	if (err)
		goto err_disable;

	edu->regs = pci_iomap(pdev, 0, 0);
	if (!edu->regs) {
		err = -ENOMEM;
		goto err_release;
	}

	pci_set_master(pdev);

	err = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_INTX);
	if (err < 0)
		goto err_iounmap;

	err = request_irq(pci_irq_vector(pdev, 0), edu_irq_handler,
			  IRQF_SHARED, "edu_srpoc", edu);
	if (err)
		goto err_free_vectors;

	pci_set_drvdata(pdev, edu);

	INIT_WORK(&pdev->disconnect_work, edu_disconnect);
	pci_set_disconnect_work(pdev);

	dev_info(&pdev->dev, "edu_srpoc probed\n");
	return 0;

err_free_vectors:
	pci_free_irq_vectors(pdev);
err_iounmap:
	pci_iounmap(pdev, edu->regs);
err_release:
	pci_release_regions(pdev);
err_disable:
	pci_disable_device(pdev);
	return err;
}

static void edu_remove(struct pci_dev *pdev)
{
	struct edu_dev *edu = pci_get_drvdata(pdev);

	dev_info(&pdev->dev, "remove(): starting factorial — blocking for IRQ\n");

	iowrite32(EDU_STATUS_IRQFACT, edu->regs + EDU_REG_STATUS);

	iowrite32(600000, edu->regs + EDU_REG_DELAYED_IRQ);

	wait_for_completion(&edu->irq_done);

	dev_info(&pdev->dev, "remove(): unblocked, cleaning up\n");

	pci_clear_disconnect_work(pdev);
	free_irq(pci_irq_vector(pdev, 0), edu);
	pci_free_irq_vectors(pdev);
	pci_iounmap(pdev, edu->regs);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_EDU, PCI_DEVICE_ID_EDU) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

static struct pci_driver edu_driver = {
	.name		= "edu_srpoc",
	.id_table	= edu_ids,
	.probe		= edu_probe,
	.remove		= edu_remove,
};

module_pci_driver(edu_driver);
MODULE_AUTHOR("Abhin Parekadan Jose");
MODULE_DESCRIPTION("edu surprise removal POC driver");
MODULE_LICENSE("GPL");
