// SPDX-License-Identifier: GPL-2.0
/*
 * Synopsys DesignWare XPCS Management Interface driver
 *
 * Copyright (C) 2023 BAIKAL ELECTRONICS, JSC
 */

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/sizes.h>

/* Page select register for the indirect MMIO CSRs access */
#define DW_VR_CSR_VIEWPORT		0xff

struct dw_xpcs_mi {
	struct platform_device *pdev;
	struct mii_bus *bus;
	bool reg_indir;
	int reg_width;
	void __iomem *reg_base;
	struct clk *pclk;
};

static inline ptrdiff_t dw_xpcs_mmio_addr_format(int dev, int reg)
{
	return FIELD_PREP(0x1f0000, dev) | FIELD_PREP(0xffff, reg);
}

static inline u16 dw_xpcs_mmio_addr_page(ptrdiff_t csr)
{
	return FIELD_GET(0x1fff00, csr);
}

static inline ptrdiff_t dw_xpcs_mmio_addr_offset(ptrdiff_t csr)
{
	return FIELD_GET(0xff, csr);
}

static int dw_xpcs_mmio_read_reg_indirect(struct dw_xpcs_mi *dxmi,
					  int dev, int reg)
{
	ptrdiff_t csr, ofs;
	u16 page;
	int ret;

	csr = dw_xpcs_mmio_addr_format(dev, reg);
	page = dw_xpcs_mmio_addr_page(csr);
	ofs = dw_xpcs_mmio_addr_offset(csr);

	ret = pm_runtime_resume_and_get(&dxmi->pdev->dev);
	if (ret)
		return ret;

	switch (dxmi->reg_width) {
	case 4:
		writel(page, dxmi->reg_base + (DW_VR_CSR_VIEWPORT << 2));
		ret = readl(dxmi->reg_base + (ofs << 2));
		break;
	default:
		writew(page, dxmi->reg_base + (DW_VR_CSR_VIEWPORT << 1));
		ret = readw(dxmi->reg_base + (ofs << 1));
		break;
	}

	pm_runtime_put(&dxmi->pdev->dev);

	return ret;
}

static int dw_xpcs_mmio_write_reg_indirect(struct dw_xpcs_mi *dxmi,
					   int dev, int reg, u16 val)
{
	ptrdiff_t csr, ofs;
	u16 page;
	int ret;

	csr = dw_xpcs_mmio_addr_format(dev, reg);
	page = dw_xpcs_mmio_addr_page(csr);
	ofs = dw_xpcs_mmio_addr_offset(csr);

	ret = pm_runtime_resume_and_get(&dxmi->pdev->dev);
	if (ret)
		return ret;

	switch (dxmi->reg_width) {
	case 4:
		writel(page, dxmi->reg_base + (DW_VR_CSR_VIEWPORT << 2));
		writel(val, dxmi->reg_base + (ofs << 2));
		break;
	default:
		writew(page, dxmi->reg_base + (DW_VR_CSR_VIEWPORT << 1));
		writew(val, dxmi->reg_base + (ofs << 1));
		break;
	}

	pm_runtime_put(&dxmi->pdev->dev);

	return 0;
}

static int dw_xpcs_mmio_read_reg_direct(struct dw_xpcs_mi *dxmi,
					int dev, int reg)
{
	ptrdiff_t csr;
	int ret;

	csr = dw_xpcs_mmio_addr_format(dev, reg);

	ret = pm_runtime_resume_and_get(&dxmi->pdev->dev);
	if (ret)
		return ret;

	switch (dxmi->reg_width) {
	case 4:
		ret = readl(dxmi->reg_base + (csr << 2));
		break;
	default:
		ret = readw(dxmi->reg_base + (csr << 1));
		break;
	}

	pm_runtime_put(&dxmi->pdev->dev);

	return ret;
}

static int dw_xpcs_mmio_write_reg_direct(struct dw_xpcs_mi *dxmi,
					 int dev, int reg, u16 val)
{
	ptrdiff_t csr;
	int ret;

	csr = dw_xpcs_mmio_addr_format(dev, reg);

	ret = pm_runtime_resume_and_get(&dxmi->pdev->dev);
	if (ret)
		return ret;

	switch (dxmi->reg_width) {
	case 4:
		writel(val, dxmi->reg_base + (csr << 2));
		break;
	default:
		writew(val, dxmi->reg_base + (csr << 1));
		break;
	}

	pm_runtime_put(&dxmi->pdev->dev);

	return 0;
}

static int dw_xpcs_mmio_read_c22(struct mii_bus *bus, int addr, int reg)
{
	struct dw_xpcs_mi *dxmi = bus->priv;

	if (addr != 0)
		return -ENODEV;

	if (dxmi->reg_indir)
		return dw_xpcs_mmio_read_reg_indirect(dxmi, MDIO_MMD_VEND2, reg);
	else
		return dw_xpcs_mmio_read_reg_direct(dxmi, MDIO_MMD_VEND2, reg);
}

static int dw_xpcs_mmio_write_c22(struct mii_bus *bus, int addr, int reg, u16 val)
{
	struct dw_xpcs_mi *dxmi = bus->priv;

	if (addr != 0)
		return -ENODEV;

	if (dxmi->reg_indir)
		return dw_xpcs_mmio_write_reg_indirect(dxmi, MDIO_MMD_VEND2, reg, val);
	else
		return dw_xpcs_mmio_write_reg_direct(dxmi, MDIO_MMD_VEND2, reg, val);
}

static int dw_xpcs_mmio_read_c45(struct mii_bus *bus, int addr, int dev, int reg)
{
	struct dw_xpcs_mi *dxmi = bus->priv;

	if (addr != 0)
		return -ENODEV;

	if (dxmi->reg_indir)
		return dw_xpcs_mmio_read_reg_indirect(dxmi, dev, reg);
	else
		return dw_xpcs_mmio_read_reg_direct(dxmi, dev, reg);
}

static int dw_xpcs_mmio_write_c45(struct mii_bus *bus, int addr, int dev,
				  int reg, u16 val)
{
	struct dw_xpcs_mi *dxmi = bus->priv;

	if (addr != 0)
		return -ENODEV;

	if (dxmi->reg_indir)
		return dw_xpcs_mmio_write_reg_indirect(dxmi, dev, reg, val);
	else
		return dw_xpcs_mmio_write_reg_direct(dxmi, dev, reg, val);
}

static struct dw_xpcs_mi *dw_xpcs_mi_create_data(struct platform_device *pdev)
{
	struct dw_xpcs_mi *dxmi;

	dxmi = devm_kzalloc(&pdev->dev, sizeof(*dxmi), GFP_KERNEL);
	if (!dxmi)
		return ERR_PTR(-ENOMEM);

	dxmi->pdev = pdev;

	dev_set_drvdata(&pdev->dev, dxmi);

	return dxmi;
}

static int dw_xpcs_mi_init_res(struct dw_xpcs_mi *dxmi)
{
	struct device *dev = &dxmi->pdev->dev;
	struct resource *res;

	if (!device_property_read_u32(dev, "reg-io-width", &dxmi->reg_width)) {
		if (dxmi->reg_width != 2 && dxmi->reg_width != 4) {
			dev_err(dev, "Invalid regspace data width\n");
			return -EINVAL;
		}
	} else {
		dxmi->reg_width = 2;
	}

	res = platform_get_resource_byname(dxmi->pdev, IORESOURCE_MEM, "direct") ?:
	      platform_get_resource_byname(dxmi->pdev, IORESOURCE_MEM, "indirect");
	if (!res) {
		dev_err(dev, "No regspace found\n");
		return -EINVAL;
	}

	if (!strcmp(res->name, "indirect"))
		dxmi->reg_indir = true;

	if ((dxmi->reg_indir && resource_size(res) < dxmi->reg_width * SZ_256) ||
	    (!dxmi->reg_indir && resource_size(res) < dxmi->reg_width * SZ_2M)) {
		dev_err(dev, "Invalid regspace size\n");
		return -EINVAL;
	}

	dxmi->reg_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(dxmi->reg_base)) {
		dev_err(dev, "Failed to map regspace\n");
		return PTR_ERR(dxmi->reg_base);
	}

	return 0;
}

static int dw_xpcs_mi_init_clk(struct dw_xpcs_mi *dxmi)
{
	struct device *dev = &dxmi->pdev->dev;
	int ret;

	dxmi->pclk = devm_clk_get_optional(dev, "pclk");
        if (IS_ERR(dxmi->pclk))
		return dev_err_probe(dev, PTR_ERR(dxmi->pclk),
				     "Failed to get ref clock\n");

	pm_runtime_set_active(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret) {
		dev_err(dev, "Failed to enable runtime-PM\n");
		return ret;
	}

	return 0;
}

static int dw_xpcs_mi_init_mdio(struct dw_xpcs_mi *dxmi)
{
	struct device *dev = &dxmi->pdev->dev;
	static atomic_t id = ATOMIC_INIT(-1);
	int ret;

	dxmi->bus = devm_mdiobus_alloc_size(dev, 0);
	if (!dxmi->bus)
		return -ENOMEM;

	dxmi->bus->name = "DW XPCS MI";
	dxmi->bus->read = dw_xpcs_mmio_read_c22;
	dxmi->bus->write = dw_xpcs_mmio_write_c22;
	dxmi->bus->read_c45 = dw_xpcs_mmio_read_c45;
	dxmi->bus->write_c45 = dw_xpcs_mmio_write_c45;
	dxmi->bus->phy_mask = ~0;
	dxmi->bus->parent = dev;
	dxmi->bus->priv = dxmi;

	snprintf(dxmi->bus->id, MII_BUS_ID_SIZE,
		 "dwxpcs-%x", atomic_inc_return(&id));

	ret = devm_of_mdiobus_register(dev, dxmi->bus, dev_of_node(dev));
	if (ret) {
		dev_err(dev, "Failed to create MDIO bus\n");
		return ret;
	}

	return 0;
}

static int dw_xpcs_mi_probe(struct platform_device *pdev)
{
	struct dw_xpcs_mi *dxmi;
	int ret;

	dxmi = dw_xpcs_mi_create_data(pdev);
	if (IS_ERR(dxmi))
		return PTR_ERR(dxmi);

	ret = dw_xpcs_mi_init_res(dxmi);
	if (ret)
		return ret;

	ret = dw_xpcs_mi_init_clk(dxmi);
	if (ret)
		return ret;

	ret = dw_xpcs_mi_init_mdio(dxmi);
	if (ret)
		return ret;

	return 0;
}

static int __maybe_unused dw_xpcs_mi_pm_runtime_suspend(struct device *dev)
{
	struct dw_xpcs_mi *dxmi = dev_get_drvdata(dev);

	clk_disable_unprepare(dxmi->pclk);

	return 0;
}

static int __maybe_unused dw_xpcs_mi_pm_runtime_resume(struct device *dev)
{
	struct dw_xpcs_mi *dxmi = dev_get_drvdata(dev);

	return clk_prepare_enable(dxmi->pclk);
}

const struct dev_pm_ops dw_xpcs_mi_pm_ops = {
        SET_RUNTIME_PM_OPS(dw_xpcs_mi_pm_runtime_suspend, dw_xpcs_mi_pm_runtime_resume, NULL)
};

static const struct of_device_id dw_xpcs_mi_of_ids[] = {
	{ .compatible = "snps,dw-xpcs-mi" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, dw_xpcs_mi_of_ids);

static struct platform_driver dw_xpcs_mi_driver = {
	.probe = dw_xpcs_mi_probe,
	.driver = {
		.name = "dw-xpcs-mi",
		.pm = &dw_xpcs_mi_pm_ops,
		.of_match_table = dw_xpcs_mi_of_ids,
	},
};

module_platform_driver(dw_xpcs_mi_driver);

MODULE_DESCRIPTION("Synopsys DesignWare XPCS Management Interface driver");
MODULE_AUTHOR("Serge Semin <Sergey.Semin@baikalelectronics.ru>");
MODULE_LICENSE("GPL v2");
