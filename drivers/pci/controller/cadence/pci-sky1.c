// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe controller driver for CIX's sky1 SoCs
 *
 * Author: Hans Zhang <hans.zhang@cixtech.com>
 */

#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pci.h>
#include <linux/pci-ecam.h>
#include <linux/pci_ids.h>

#include "pcie-cadence.h"
#include "pcie-cadence-host-common.h"

#define STRAP_REG(n) ((n) * 0x04)
#define STATUS_REG(n) ((n) * 0x04)

#define RCSU_STRAP_REG 0x300
#define RCSU_STATUS_REG 0x400

#define SKY1_IP_REG_BANK_OFFSET 0x1000
#define SKY1_IP_CFG_CTRL_REG_BANK_OFFSET 0x4c00
#define SKY1_IP_AXI_MASTER_COMMON_OFFSET 0xf000
#define SKY1_AXI_SLAVE_OFFSET 0x9000
#define SKY1_AXI_MASTER_OFFSET 0xb000
#define SKY1_AXI_HLS_REGISTERS_OFFSET 0xc000
#define SKY1_AXI_RAS_REGISTERS_OFFSET 0xe000
#define SKY1_DTI_REGISTERS_OFFSET 0xd000

#define IP_REG_I_DBG_STS_0 0x420

#define LINK_TRAINING_ENABLE BIT(0)
#define LINK_COMPLETE BIT(0)

enum cix_soc_type {
	CIX_SKY1,
};

struct sky1_pcie_data {
	struct cdns_plat_pcie_of_data reg_off;
	enum cix_soc_type soc_type;
};

struct sky1_pcie {
	struct device *dev;
	const struct sky1_pcie_data *data;
	struct cdns_pcie *cdns_pcie;
	struct cdns_pcie_rc *cdns_pcie_rc;

	struct resource *cfg_res;
	struct resource *msg_res;
	struct pci_config_window *cfg;
	void __iomem *rcsu_base;
	void __iomem *strap_base;
	void __iomem *status_base;
	void __iomem *reg_base;
	void __iomem *cfg_base;
	void __iomem *msg_base;
};

static void sky1_pcie_clear_and_set_dword(void __iomem *addr, u32 clear,
					  u32 set)
{
	u32 val;

	val = readl(addr);
	val &= ~clear;
	val |= set;
	writel(val, addr);
}

static void sky1_pcie_init_bases(struct sky1_pcie *pcie)
{
	pcie->strap_base = pcie->rcsu_base + RCSU_STRAP_REG;
	pcie->status_base = pcie->rcsu_base + RCSU_STATUS_REG;
}

static int sky1_pcie_parse_mem(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct resource *res;
	void __iomem *base;
	int ret = 0;

	base = devm_platform_ioremap_resource_byname(pdev, "reg");
	if (IS_ERR(base)) {
		dev_err(dev, "Parse \"reg\" resource err\n");
		return PTR_ERR(base);
	}
	pcie->reg_base = base;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
	if (!res) {
		dev_err(dev, "Parse \"cfg\" resource err\n");
		return -ENXIO;
	}
	pcie->cfg_res = res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rcsu");
	if (!res) {
		dev_err(dev, "Parse \"rcsu\" resource err\n");
		return -ENXIO;
	}
	pcie->rcsu_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!pcie->rcsu_base) {
		dev_err(dev, "ioremap failed for resource %pR\n", res);
		return -ENOMEM;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "msg");
	if (!res) {
		dev_err(dev, "Parse \"msg\" resource err\n");
		return -ENXIO;
	}
	pcie->msg_res = res;
	pcie->msg_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!pcie->msg_base) {
		dev_err(dev, "ioremap failed for resource %pR\n", res);
		return -ENOMEM;
	}

	return ret;
}

static int sky1_pcie_parse_property(struct platform_device *pdev,
				    struct sky1_pcie *pcie)
{
	int ret = 0;

	ret = sky1_pcie_parse_mem(pcie);
	if (ret < 0)
		return ret;

	sky1_pcie_init_bases(pcie);

	return ret;
}

static int sky1_pcie_start_link(struct cdns_pcie *cdns_pcie)
{
	struct sky1_pcie *pcie = dev_get_drvdata(cdns_pcie->dev);

	sky1_pcie_clear_and_set_dword(pcie->strap_base + STRAP_REG(1),
				      0, LINK_TRAINING_ENABLE);

	return 0;
}

static void sky1_pcie_stop_link(struct cdns_pcie *cdns_pcie)
{
	struct sky1_pcie *pcie = dev_get_drvdata(cdns_pcie->dev);

	sky1_pcie_clear_and_set_dword(pcie->strap_base + STRAP_REG(1),
				      LINK_TRAINING_ENABLE, 0);
}


static bool sky1_pcie_link_up(struct cdns_pcie *cdns_pcie)
{
	u32 val;

	val = cdns_pcie_hpa_readl(cdns_pcie, REG_BANK_IP_REG,
				  IP_REG_I_DBG_STS_0);
	return val & LINK_COMPLETE;
}

static const struct cdns_pcie_ops sky1_pcie_ops = {
	.start_link = sky1_pcie_start_link,
	.stop_link = sky1_pcie_stop_link,
	.link_up = sky1_pcie_link_up,
};

static int sky1_pcie_probe(struct platform_device *pdev)
{
	const struct sky1_pcie_data *data;
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *bridge;
	struct cdns_pcie *cdns_pcie;
	struct resource_entry *bus;
	struct cdns_pcie_rc *rc;
	struct sky1_pcie *pcie;
	int ret;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	data = of_device_get_match_data(dev);
	if (!data)
		return -EINVAL;

	pcie->data = data;
	pcie->dev = dev;
	dev_set_drvdata(dev, pcie);

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*rc));
	if (!bridge)
		return -ENOMEM;

	bus = resource_list_first_type(&bridge->windows, IORESOURCE_BUS);
	if (!bus)
		return -ENODEV;

	ret = sky1_pcie_parse_property(pdev, pcie);
	if (ret < 0)
		return -ENXIO;

	pcie->cfg = pci_ecam_create(dev, pcie->cfg_res, bus->res,
				    &pci_generic_ecam_ops);
	if (IS_ERR(pcie->cfg))
		return PTR_ERR(pcie->cfg);

	bridge->ops = (struct pci_ops *)&pci_generic_ecam_ops.pci_ops;
	rc = pci_host_bridge_priv(bridge);
	rc->ecam_support_flag = 1;
	rc->cfg_base = pcie->cfg->win;
	rc->cfg_res = &pcie->cfg->res;

	cdns_pcie = &rc->pcie;
	cdns_pcie->dev = dev;
	cdns_pcie->ops = &sky1_pcie_ops;
	cdns_pcie->reg_base = pcie->reg_base;
	cdns_pcie->msg_res = pcie->msg_res;
	cdns_pcie->cdns_pcie_reg_offsets = &data->reg_off;
	cdns_pcie->is_rc = data->reg_off.is_rc;

	pcie->cdns_pcie = cdns_pcie;
	pcie->cdns_pcie_rc = rc;
	pcie->cfg_base = rc->cfg_base;
	bridge->sysdata = pcie->cfg;

	if (data->soc_type == CIX_SKY1) {
		rc->vendor_id = PCI_VENDOR_ID_CIX;
		rc->device_id = PCI_DEVICE_ID_CIX_SKY1;
		rc->no_inbound_flag = 1;
	}

	ret = cdns_pcie_hpa_host_setup(rc);
	if (ret < 0) {
		pci_ecam_free(pcie->cfg);
		return ret;
	}

	return 0;
}

static const struct sky1_pcie_data sky1_pcie_rc_data = {
	.reg_off = {
		.is_rc = true,
		.ip_reg_bank_offset = SKY1_IP_REG_BANK_OFFSET,
		.ip_cfg_ctrl_reg_offset = SKY1_IP_CFG_CTRL_REG_BANK_OFFSET,
		.axi_mstr_common_offset = SKY1_IP_AXI_MASTER_COMMON_OFFSET,
		.axi_slave_offset = SKY1_AXI_SLAVE_OFFSET,
		.axi_master_offset = SKY1_AXI_MASTER_OFFSET,
		.axi_hls_offset = SKY1_AXI_HLS_REGISTERS_OFFSET,
		.axi_ras_offset = SKY1_AXI_RAS_REGISTERS_OFFSET,
		.axi_dti_offset = SKY1_DTI_REGISTERS_OFFSET,
	},
	.soc_type = CIX_SKY1,
};

static const struct of_device_id of_sky1_pcie_match[] = {
	{
		.compatible = "cix,sky1-pcie-host",
		.data = &sky1_pcie_rc_data,
	},
	{},
};

static void sky1_pcie_remove(struct platform_device *pdev)
{
	struct sky1_pcie *pcie = platform_get_drvdata(pdev);

	pci_ecam_free(pcie->cfg);
}

static struct platform_driver sky1_pcie_driver = {
	.probe  = sky1_pcie_probe,
	.remove = sky1_pcie_remove,
	.driver = {
		.name = "sky1-pcie",
		.of_match_table = of_sky1_pcie_match,
	},
};
module_platform_driver(sky1_pcie_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PCIe controller driver for CIX's sky1 SoCs");
MODULE_AUTHOR("Hans Zhang <hans.zhang@cixtech.com>");
