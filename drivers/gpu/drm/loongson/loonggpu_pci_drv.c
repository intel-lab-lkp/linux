// SPDX-License-Identifier: GPL-2.0+

/*
 * Authors:
 *      Sui Jingfeng <sui.jingfeng@linux.dev>
 */

#include <linux/component.h>
#include <linux/pci.h>

#include <drm/drm_device.h>
#include <drm/drm_print.h>

#include "loongson_drv.h"
#include "loongson_module.h"
#include "loonggpu_pci_drv.h"

static int loonggpu_get_version(struct loonggpu_device *gpu)
{
	u32 hw_info = loong_rreg32(gpu, 0x8C);
	u8 host_id;
	u8 revision;

	/* LoongGPU hardware info */
	gpu->ver_major = (hw_info >> 8) & 0x0F;
	gpu->ver_minor = (hw_info & 0xF0) >> 4;
	revision = hw_info & 0x0F;
	host_id = (hw_info >> 16) & 0xFF;

	drm_info(gpu->drm, "LoongGPU(TM): LG%x%x0, revision: %x, Host: %s\n",
		 gpu->ver_major, gpu->ver_minor, revision,
		 host_id ? "LS2K2000" : "LS7A2000");

	return 0;
}

static irqreturn_t loonggpu_irq_handler(int irq, void *arg)
{
	struct loonggpu_device *gpu = arg;

	drm_dbg(gpu->drm, "LoongGPU interrupted\n");

	return IRQ_HANDLED;
}

static int loonggpu_component_bind(struct device *dev,
				   struct device *master,
				   void *data)
{
	struct loonggpu_device *gpu = dev_get_drvdata(dev);
	struct drm_device *drm = data;
	struct loongson_drm *ldrm = to_loongson_drm(drm);
	struct pci_dev *pdev = to_pci_dev(dev);
	int ret;

	gpu->drm = drm;
	ldrm->loonggpu = gpu;

	loonggpu_get_version(gpu);

	ret = devm_request_irq(dev,
			       pdev->irq,
			       loonggpu_irq_handler,
			       IRQF_SHARED,
			       dev_name(dev),
			       gpu);
	if (ret)
		return ret;

	drm_info(gpu->drm, "LoongGPU irq: %d\n", pdev->irq);

	return 0;
}

static void loonggpu_component_unbind(struct device *dev,
				      struct device *master,
				      void *data)
{
	dev_dbg(dev, "LoongGPU unbind\n");
}

static const struct component_ops loonggpu_component_ops = {
	.bind = loonggpu_component_bind,
	.unbind = loonggpu_component_unbind,
};

static int loonggpu_pci_probe(struct pci_dev *pdev,
			      const struct pci_device_id *ent)
{
	struct loonggpu_device *gpu;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	pci_set_master(pdev);

	gpu = devm_kzalloc(&pdev->dev, sizeof(*gpu), GFP_KERNEL);
	if (!gpu)
		return -ENOMEM;

	gpu->pdev = pdev;

	gpu->reg_base = pcim_iomap(pdev, 0, 0);
	if (!gpu->reg_base)
		return -ENOMEM;

	pci_set_drvdata(pdev, gpu);

	dev_info(&pdev->dev, "LoongGPU(TM) PCI driver probed\n");

	return component_add(&pdev->dev, &loonggpu_component_ops);
}

static void loonggpu_pci_remove(struct pci_dev *pdev)
{
	component_del(&pdev->dev, &loonggpu_component_ops);
}

static int loonggpu_pm_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	pci_save_state(pdev);
	pci_disable_device(pdev);
	pci_set_power_state(pdev, PCI_D3hot);

	return 0;
}

static int loonggpu_pm_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	pci_set_power_state(pdev, PCI_D0);
	pci_restore_state(pdev);

	if (pcim_enable_device(pdev))
		return -EIO;

	return 0;
}

static const struct dev_pm_ops loonggpu_pm_ops = {
	.suspend = loonggpu_pm_suspend,
	.resume = loonggpu_pm_resume,
};

static const struct pci_device_id loonggpu_pci_id_list[] = {
	{PCI_VDEVICE(LOONGSON, 0x7a25), CHIP_LS7A2000},
	{ },
};

struct pci_driver loonggpu_pci_driver = {
	.name = "loongson.loonggpu",
	.id_table = loonggpu_pci_id_list,
	.probe = loonggpu_pci_probe,
	.remove = loonggpu_pci_remove,
	.driver.pm = &loonggpu_pm_ops,
};

MODULE_DEVICE_TABLE(pci, loonggpu_pci_id_list);
