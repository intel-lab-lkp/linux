// SPDX-License-Identifier: GPL-2.0+

#include <linux/component.h>
#include <linux/pci.h>

#include <drm/drm_device.h>
#include <drm/drm_print.h>

#include "loongson_module.h"
#include "loong_gpu_pci_drv.h"

static int loong_gpu_bind(struct device *dev, struct device *master, void *data)
{
	struct drm_device *drm = data;
	struct loong_gpu_device *gpu;
	u32 hw_info;
	u8 host_id;
	u8 revision;

	gpu = devm_kzalloc(dev, sizeof(*gpu), GFP_KERNEL);
	if (!gpu)
		return -ENOMEM;

	gpu->reg_base = pcim_iomap(to_pci_dev(dev), 0, 0);
	if (!gpu->reg_base)
		return -ENOMEM;

	hw_info = loong_rreg32(gpu, 0x8C);

	gpu->ver_major = (hw_info >> 8) * 0x0F;
	gpu->ver_minor = (hw_info & 0xF0) >> 4;
	revision = hw_info & 0x0F;
	host_id = (hw_info >> 16) & 0xFF;

	drm_info(drm, "Found LoongGPU: LG%x%x0, revision: %x, Host: %s\n",
		 gpu->ver_major, gpu->ver_minor, revision,
		 host_id ? "LS2K2000" : "LS7A2000");

	dev_set_drvdata(dev, gpu);

	return 0;
}

static void loong_gpu_unbind(struct device *dev, struct device *master, void *data)
{
	struct loong_gpu_device *gpu = dev_get_drvdata(dev);

	if (gpu) {
		pcim_iounmap(to_pci_dev(dev), gpu->reg_base);
		devm_kfree(dev, gpu);
	}
}

static const struct component_ops loong_gpu_component_ops = {
	.bind = loong_gpu_bind,
	.unbind = loong_gpu_unbind,
};

static int loong_gpu_pci_probe(struct pci_dev *pdev,
			       const struct pci_device_id *ent)
{
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	pci_set_master(pdev);

	return component_add(&pdev->dev, &loong_gpu_component_ops);
}

static void loong_gpu_pci_remove(struct pci_dev *pdev)
{
	component_del(&pdev->dev, &loong_gpu_component_ops);
}

static const struct pci_device_id loong_gpu_pci_id_list[] = {
	{PCI_VDEVICE(LOONGSON, 0x7a25), CHIP_LS7A2000},
	{ },
};

struct pci_driver loong_gpu_pci_driver = {
	.name = "loong",
	.id_table = loong_gpu_pci_id_list,
	.probe = loong_gpu_pci_probe,
	.remove = loong_gpu_pci_remove,
};

MODULE_DEVICE_TABLE(pci, loong_gpu_pci_id_list);
