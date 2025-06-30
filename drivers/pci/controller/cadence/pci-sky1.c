// SPDX-License-Identifier: GPL-2.0
/*
 * pci-sky1 - PCIe controller driver for CIX's sky1 SoCs
 *
 * Author: Hans Zhang <hans.zhang@cixtech.com>
 */

#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pci.h>
#include <linux/pci-ecam.h>

#include "../../pci.h"
#include "pcie-cadence.h"
#include "pcie-cadence-host-common.h"

#define STRAP_REG(n) ((n) * 0x04)
#define STATUS_REG(n) ((n) * 0x04)

#define RCSU_STRAP_REG 0x300
#define RCSU_STATUS_REG 0x400

#define RCSU_STRAP_STATUS_SUBREG_X2 0x40
#define RCSU_STRAP_STATUS_SUBREG_X10 0x60
#define RCSU_STRAP_STATUS_SUBREG_X11 0x80

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
#define SKY1_MAX_LANES 8

#define BYPASS_PHASE23_MASK BIT(26)
#define BYPASS_REMOTE_TX_EQ_MASK BIT(25)
#define DC_MAX_EVAL_ITERATION_MASK GENMASK(24, 18)
#define LANE_COUNT_IN_MASK GENMASK(17, 15)
#define PCIE_RATE_MAX_MASK GENMASK(14, 12)
#define SUPPORTED_PRESET_MASK GENMASK(10, 0)

enum sky1_pcie_id {
	PCIE_ID_x8,
	PCIE_ID_x4,
	PCIE_ID_x2,
	PCIE_ID_x1_1,
	PCIE_ID_x1_0,
};

struct sky1_def_speed_lane {
	u32 link_speed;
	u32 max_lanes;
};

struct sky1_pcie_data {
	const struct sky1_def_speed_lane *speed_lane;
	struct cdns_plat_pcie_of_data reg_off;
};

struct sky1_pcie {
	struct device *dev;
	const struct sky1_pcie_data *data;
	const struct sky1_def_speed_lane *speed_lane;
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

	u32 id;
	u32 link_speed;
	u32 num_lanes;
};

static const struct sky1_def_speed_lane def_speed_lane[] = {
	[PCIE_ID_x8] = { 4, 8 },
	[PCIE_ID_x4] = { 4, 4 },
	[PCIE_ID_x2] = { 4, 2 },
	[PCIE_ID_x1_1] = { 4, 1 },
	[PCIE_ID_x1_0] = { 4, 1 },
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
	u32 strap = 0, status = 0;

	switch (pcie->id) {
	case PCIE_ID_x1_1:
		strap = status = RCSU_STRAP_STATUS_SUBREG_X11;
		break;
	case PCIE_ID_x1_0:
		strap = status = RCSU_STRAP_STATUS_SUBREG_X10;
		break;
	case PCIE_ID_x2:
		strap = status = RCSU_STRAP_STATUS_SUBREG_X2;
		break;
	case PCIE_ID_x8:
	case PCIE_ID_x4:
	default:
		break;
	}

	pcie->strap_base = pcie->rcsu_base + RCSU_STRAP_REG + strap;
	pcie->status_base = pcie->rcsu_base + RCSU_STATUS_REG + status;
}

static int sky1_pcie_parse_mem(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct resource *res;
	void __iomem *base;
	int ret = 0;

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

	base = devm_platform_ioremap_resource_byname(pdev, "reg");
	if (IS_ERR(base)) {
		dev_err(dev, "Parse \"reg\" resource err\n");
		return PTR_ERR(base);
	}
	pcie->reg_base = base;

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

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
	if (!res) {
		dev_err(dev, "Parse \"cfg\" resource err\n");
		return -ENXIO;
	}
	pcie->cfg_res = res;

	return ret;
}

static int sky1_pcie_parse_ctrl_id(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	int id, ret = 0;

	ret = of_property_read_u32(dev->of_node, "sky1,pcie-ctrl-id", &id);
	if (ret < 0) {
		dev_err(dev, "Failed to read sky1,pcie-ctrl-id: %d\n", ret);
		return ret;
	}

	if ((id < PCIE_ID_x8) || (id > PCIE_ID_x1_0)) {
		dev_err(dev, "get illegal pcie-ctrl-id %d\n", id);
		return -EINVAL;
	}
	pcie->id = id;
	pcie->speed_lane = &def_speed_lane[id];

	return ret;
}

static void sky1_pcie_parse_link_speed(struct sky1_pcie *pcie)
{
	int link_speed;

	link_speed = of_pci_get_max_link_speed(pcie->dev->of_node);
	if (link_speed < 0)
		link_speed = pcie->speed_lane->link_speed;
	pcie->link_speed = link_speed;
}

static int sky1_pcie_parse_num_lanes(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	int ret = 0;
	u32 lanes;

	ret = of_property_read_u32(dev->of_node, "num-lanes", &lanes);
	if (ret) {
		dev_err(dev, "error:%x, lane number:%d\n", ret, lanes);
		ret = -EINVAL;
		return ret;
	}

	if ((lanes < 1) || (lanes > pcie->speed_lane->max_lanes))
		lanes = pcie->speed_lane->max_lanes;
	pcie->num_lanes = lanes;

	return ret;
}

static int sky1_pcie_get_max_lane_count(struct sky1_pcie *pcie)
{
	if (is_power_of_2(pcie->num_lanes) && pcie->num_lanes <= SKY1_MAX_LANES)
		return ilog2(pcie->num_lanes);

	pcie->num_lanes = 1;
	return pcie->num_lanes;
}

static void sky1_pcie_set_strap_pin0(struct sky1_pcie *pcie)
{
	u32 val;

	val = readl(pcie->strap_base + STRAP_REG(0));

	/* clear bypass_phase23 and bypass_remote_eq */
	val &= ~(BYPASS_PHASE23_MASK | BYPASS_REMOTE_TX_EQ_MASK);

	/* set iteration timeout */
	val &= ~DC_MAX_EVAL_ITERATION_MASK;
	val |= FIELD_PREP(DC_MAX_EVAL_ITERATION_MASK, 0x2);

	/* set support preset val */
	val &= ~SUPPORTED_PRESET_MASK;
	val |= FIELD_PREP(SUPPORTED_PRESET_MASK, 0x7ff);

	/* Set link speed */
	val &= ~PCIE_RATE_MAX_MASK;
	val |= FIELD_PREP(PCIE_RATE_MAX_MASK, pcie->link_speed - 1);

	/* Set lane number */
	val &= ~LANE_COUNT_IN_MASK;
	val |= FIELD_PREP(LANE_COUNT_IN_MASK,
		sky1_pcie_get_max_lane_count(pcie));

	writel(val, pcie->strap_base + STRAP_REG(0));
}

static int sky1_pcie_parse_property(struct platform_device *pdev,
				    struct sky1_pcie *pcie)
{
	int ret = 0;

	ret = sky1_pcie_parse_ctrl_id(pcie);
	if (ret < 0)
		return ret;

	sky1_pcie_parse_link_speed(pcie);

	ret = sky1_pcie_parse_num_lanes(pcie);
	if (ret < 0)
		return ret;

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

	sky1_pcie_set_strap_pin0(pcie);

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

	ret = cdns_pcie_hpa_host_setup(rc);
	if (ret < 0) {
		pci_ecam_free(pcie->cfg);
		return ret;
	}

	return 0;
}

static const struct sky1_pcie_data sky1_pcie_rc_data = {
	.speed_lane = &def_speed_lane[0],
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
