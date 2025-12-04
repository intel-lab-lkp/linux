// SPDX-License-Identifier: GPL-2.0

#include <drm/clients/drm_client_setup.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_dumb_buffers.h>
#include <drm/drm_fbdev_ttm.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_module.h>
#include <drm/drm_vblank.h>

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/aperture.h>

#include "yhgch_drm_drv.h"
#include "yhgch_drm_regs.h"

#define MEM_SIZE_RESERVE4KVM 0x200000

DEFINE_DRM_GEM_FOPS(yhgch_fops);

static int yhgch_dumb_create(struct drm_file *file, struct drm_device *dev,
			     struct drm_mode_create_dumb *args)
{
	int ret;

	ret = drm_mode_size_dumb(dev, args, SZ_16, 0);
	if (ret)
		return ret;

	return drm_gem_shmem_dumb_create(file, dev,  args);
}

static struct drm_driver yhgch_driver = {
	.driver_features	= DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,
	.fops = &yhgch_fops,
	.name = "yhgch",
	.desc = "yhgch drm driver",
	.major = 3,
	.minor = 1,
	.dumb_create = yhgch_dumb_create,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
};

static int __maybe_unused yhgch_pm_suspend(struct device *dev)
{
	struct drm_device *drm_dev = dev_get_drvdata(dev);

	return drm_mode_config_helper_suspend(drm_dev);
}

static int __maybe_unused yhgch_pm_resume(struct device *dev)
{
	struct drm_device *drm_dev = dev_get_drvdata(dev);

	return drm_mode_config_helper_resume(drm_dev);
}

static const struct dev_pm_ops yhgch_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(yhgch_pm_suspend,
				yhgch_pm_resume)
};

static const struct drm_mode_config_funcs yhgch_mode_funcs = {
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
	.fb_create = drm_gem_fb_create_with_dirty,
};

static int yhgch_kms_init(struct yhgch_drm_private *priv)
{
	struct drm_device *dev = &priv->dev;
	int ret;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;

	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;
	dev->mode_config.max_width = 1920;
	dev->mode_config.max_height = 1200;
	dev->mode_config.preferred_depth = 24;
	dev->mode_config.funcs = &yhgch_mode_funcs;

	ret = yhgch_de_init(priv);
	if (ret) {
		drm_err(dev, "failed to init de: %d\n", ret);
		return ret;
	}

	ret = yhgch_vdac_init(priv);
	if (ret) {
		drm_err(dev, "failed to init vdac: %d\n", ret);
		return ret;
	}
	drm_kms_helper_poll_init(dev);

	return 0;
}

/*
 * It can operate in one of three modes: power mode 0, power mode 1 or Sleep
 * mode.
 */
void yhgch_set_power_mode(struct yhgch_drm_private *priv, u32 power_mode)
{
	unsigned int control_value = 0;
	void __iomem *mmio = priv->mmio;
	u32 input = 1;

	if (power_mode > YHGCH_PW_MODE_CTL_MODE_SLEEP)
		return;

	if (power_mode == YHGCH_PW_MODE_CTL_MODE_SLEEP)
		input = 0;

	control_value = readl(mmio + YHGCH_POWER_MODE_CTRL);
	control_value &= ~(YHGCH_PW_MODE_CTL_MODE_MASK |
			   YHGCH_PW_MODE_CTL_OSC_INPUT_MASK);
	control_value |= FIELD_PREP(YHGCH_PW_MODE_CTL_MODE_MASK, power_mode);
	control_value |= FIELD_PREP(YHGCH_PW_MODE_CTL_OSC_INPUT_MASK, input);
	writel(control_value, mmio + YHGCH_POWER_MODE_CTRL);
}

void yhgch_set_current_gate(struct yhgch_drm_private *priv, unsigned int gate)
{
	u32 gate_reg;
	u32 mode;
	void __iomem *mmio = priv->mmio;

	/* Get current power mode. */
	mode = (readl(mmio + YHGCH_POWER_MODE_CTRL) &
		YHGCH_PW_MODE_CTL_MODE_MASK) >> YHGCH_PW_MODE_CTL_MODE_SHIFT;

	switch (mode) {
	case YHGCH_PW_MODE_CTL_MODE_MODE0:
		gate_reg = YHGCH_MODE0_GATE;
		break;

	case YHGCH_PW_MODE_CTL_MODE_MODE1:
		gate_reg = YHGCH_MODE1_GATE;
		break;

	default:
		gate_reg = YHGCH_MODE0_GATE;
		break;
	}
	writel(gate, mmio + gate_reg);
}

static void yhgch_hw_config(struct yhgch_drm_private *priv)
{
	u32 reg;

	/* On hardware reset, power mode 0 is default. */
	yhgch_set_power_mode(priv, YHGCH_PW_MODE_CTL_MODE_MODE0);

	/* Enable display power gate & LOCALMEM power gate */
	reg = readl(priv->mmio + YHGCH_CURRENT_GATE);
	reg &= ~YHGCH_CURR_GATE_DISPLAY_MASK;
	reg &= ~YHGCH_CURR_GATE_LOCALMEM_MASK;
	reg |= YHGCH_CURR_GATE_DISPLAY(1);
	reg |= YHGCH_CURR_GATE_LOCALMEM(1);

	yhgch_set_current_gate(priv, reg);

	/*
	 * Reset the memory controller. If the memory controller
	 * is not reset in chip,the system might hang when sw accesses
	 * the memory.The memory should be resetted after
	 * changing the MXCLK.
	 */
	reg = readl(priv->mmio + YHGCH_MISC_CTRL);
	reg &= ~YHGCH_MSCCTL_LOCALMEM_RESET_MASK;
	reg |= YHGCH_MSCCTL_LOCALMEM_RESET(0);
	writel(reg, priv->mmio + YHGCH_MISC_CTRL);

	reg &= ~YHGCH_MSCCTL_LOCALMEM_RESET_MASK;
	reg |= YHGCH_MSCCTL_LOCALMEM_RESET(1);

	writel(reg, priv->mmio + YHGCH_MISC_CTRL);
}

static int yhgch_hw_map(struct yhgch_drm_private *priv)
{
	struct drm_device *dev = &priv->dev;
	struct pci_dev *pdev = to_pci_dev(dev->dev);
	resource_size_t ioaddr, iosize;

	ioaddr = pci_resource_start(pdev, 1);
	iosize = pci_resource_len(pdev, 1);
	priv->mmio = devm_ioremap(dev->dev, ioaddr, iosize);
	if (!priv->mmio) {
		drm_err(dev, "Cannot map mmio region\n");
		return -ENOMEM;
	}

	ioaddr = pci_resource_start(pdev, 0);
	iosize = pci_resource_len(pdev, 0);
	priv->vram_base = devm_ioremap_wc(dev->dev, ioaddr, iosize);
	if (!priv->vram_base) {
		drm_err(dev, "Cannot map vram region\n");
		return -ENOMEM;
	}
	return 0;
}

static int yhgch_hw_init(struct yhgch_drm_private *priv)
{
	int ret;

	ret = yhgch_hw_map(priv);
	if (ret)
		return ret;
	yhgch_hw_config(priv);
	return 0;
}

static int yhgch_pci_probe(struct pci_dev *pdev,
			   const struct pci_device_id *ent)
{
	struct yhgch_drm_private *priv;
	struct drm_device *dev;
	int ret;

	ret = aperture_remove_conflicting_pci_devices(pdev, yhgch_driver.name);

	if (ret)
		return ret;

	priv = devm_drm_dev_alloc(&pdev->dev, &yhgch_driver,
				  struct yhgch_drm_private, dev);

	if (IS_ERR(priv))
		return PTR_ERR(priv);

	dev = &priv->dev;
	pci_set_drvdata(pdev, dev);

	ret = pcim_enable_device(pdev);
	if (ret) {
		drm_err(dev, "failed to enable pci device: %d\n", ret);
		goto err_return;
	}

	ret = yhgch_hw_init(priv);
	if (ret)
		goto err_return;

	ret = yhgch_kms_init(priv);
	if (ret)
		goto err_return;

	ret = pci_enable_msi(pdev);
	if (ret)
		drm_warn(dev, "enabling MSI failed: %d\n", ret);
	/* reset all the states of crtc/plane/encoder/connector */
	drm_mode_config_reset(dev);

	ret = drm_dev_register(dev, 0);
	if (ret) {
		drm_err(dev, "failed to register drv for userspace access: %d\n",
			ret);
		goto err_return;
	}
	drm_client_setup(dev, NULL);

	return 0;

err_return:
	return ret;
}

static void yhgch_pci_remove(struct pci_dev *pdev)
{
	struct drm_device *dev = pci_get_drvdata(pdev);

	drm_dev_unregister(dev);
	drm_dev_put(dev);
}

static void yhgch_pci_shutdown(struct pci_dev *pdev)
{
	drm_atomic_helper_shutdown(pci_get_drvdata(pdev));
}

static struct pci_device_id yhgch_pci_table[] = {
	{ 0x1bd4, 0x0750, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ 0, }
};

static struct pci_driver yhgch_pci_driver = {
	.name = "yhgch-drm",
	.id_table = yhgch_pci_table,
	.probe = yhgch_pci_probe,
	.remove = yhgch_pci_remove,
	.shutdown = yhgch_pci_shutdown,
	.driver.pm = &yhgch_pm_ops,
};

drm_module_pci_driver(yhgch_pci_driver);

MODULE_DEVICE_TABLE(pci, yhgch_pci_table);
MODULE_AUTHOR("Chu Guangqing <chuguangqing@inspur.com>");
MODULE_DESCRIPTION("DRM Driver for YHGCH ZX1000 BMC. company website: https://www.yhgch.com");
MODULE_LICENSE("GPL");
MODULE_VERSION("3.1");
