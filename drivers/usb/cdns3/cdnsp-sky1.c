// SPDX-License-Identifier: GPL-2.0
/*
 * cdnsp-sky1.c - CIX Sky1 glue for Cadence USBSSP DRD controller
 *
 * Copyright (C) 2026 CIX Technology Group Co., Ltd.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include <linux/usb.h>
#include <linux/usb/hcd.h>

#include "cdnsp-sky1.h"
#include "core.h"

static const char *cix_usb_clk_names[CIX_USB_CLK_NUM] = {
	"sof",
	"axi",
	"lpm",
	"apb",
};

struct cdnsp_sky1_strap_signal {
	unsigned int offset, bit;
};

static const struct cdnsp_sky1_strap_signal strap_signals[SKY1_USB_S5_NUM] = {
	/* usb config in s5 domain */
	[U3_TYPEC_DRD_ID]	= { USB_MODE_STRAP_S5_DOMAIN, U3_TYPEC_DRD_MODE_STRAP_BIT },
	[U3_TYPEC_HOST0_ID]	= { USB_MODE_STRAP_S5_DOMAIN, U3_TYPEC_HOST0_MODE_STRAP_BIT },
	[U3_TYPEC_HOST1_ID]	= { USB_MODE_STRAP_S5_DOMAIN, U3_TYPEC_HOST1_MODE_STRAP_BIT },
	[U3_TYPEC_HOST2_ID]	= { USB_MODE_STRAP_S5_DOMAIN, U3_TYPEC_HOST2_MODE_STRAP_BIT },
	[U3_TYPEA_CTRL0_ID]	= { USB_MODE_STRAP_S5_DOMAIN, U3_TYPEA_CTRL0_MODE_STRAP_BIT },
	[U3_TYPEA_CTRL1_ID]	= { USB_MODE_STRAP_S5_DOMAIN, U3_TYPEA_CTRL1_MODE_STRAP_BIT},
	[U2_HOST0_ID]		= { USB_MODE_STRAP_S5_DOMAIN, U2_HOST0_MODE_STRAP_BIT },
	[U2_HOST1_ID]		= { USB_MODE_STRAP_S5_DOMAIN, U2_HOST1_MODE_STRAP_BIT },
	[U2_HOST2_ID]		= { USB_MODE_STRAP_S5_DOMAIN, U2_HOST2_MODE_STRAP_BIT },
	[U2_HOST3_ID]		= { USB_MODE_STRAP_S5_DOMAIN, U2_HOST3_MODE_STRAP_BIT },
};

static int cdnsp_sky1_set_mode_by_id(struct device *dev, int mode)
{
	struct cdnsp_sky1 *data = dev_get_drvdata(dev);

	return regmap_update_bits(data->usb_syscon,
				  strap_signals[data->id].offset,
				  GENMASK(strap_signals[data->id].bit + 1,
					  strap_signals[data->id].bit),
				  mode << strap_signals[data->id].bit);
}

/**
 * cdnsp_sky1_clk_enable_all() - enable all clocks for usb controller
 * @dev:	Pointer to the device of platform_device
 *
 */

static int cdnsp_sky1_clk_enable_all(struct device *dev)
{
	int i, ret = 0;
	struct cdnsp_sky1 *data = dev_get_drvdata(dev);
	struct clk **cix_usb_clks = data->cix_usb_clks;

	for (i = 0; i < CIX_USB_CLK_NUM; i++) {
		cix_usb_clks[i] = devm_clk_get(dev, cix_usb_clk_names[i]);
		if (IS_ERR(cix_usb_clks[i])) {
			ret = dev_err_probe(dev, PTR_ERR(cix_usb_clks[i]),
					    "could not get %s clock\n",
					    cix_usb_clk_names[i]);
			goto err_usb_clks;
		}
		ret = clk_prepare_enable(cix_usb_clks[i]);
		if (ret) {
			dev_err(dev, "%s enable failed:%d\n", cix_usb_clk_names[i], ret);
			goto err_usb_clks;
		}
	}
	dev_dbg(dev, "enable sky1 USB clock done\n");
	return ret;

err_usb_clks:
	while (--i >= 0)
		clk_disable_unprepare(cix_usb_clks[i]);
	return ret;
};

/**
 * cdnsp_sky1_clk_disable_all() - disable all clocks for usb controller
 * @dev:	Pointer to the device of platform_device
 *
 */

static void cdnsp_sky1_clk_disable_all(struct device *dev)
{
	int i;
	struct cdnsp_sky1 *data = dev_get_drvdata(dev);
	struct clk **cix_usb_clks = data->cix_usb_clks;

	for (i = 0; i < CIX_USB_CLK_NUM; i++)
		clk_disable_unprepare(cix_usb_clks[i]);
};

/**
 * cdnsp_sky1_clk_enable_resume() - enable the clocks that are turned
 * off while suspend
 * @dev:	Pointer to the device of platform_device
 *
 */

static int cdnsp_sky1_clk_enable_resume(struct device *dev)
{
	int i, ret;
	struct cdnsp_sky1 *data = dev_get_drvdata(dev);
	struct clk **cix_usb_clks = data->cix_usb_clks;

	for (i = 0; i < CIX_USB_CLK_OFF_NUM; i++) {
		ret = clk_prepare_enable(cix_usb_clks[i]);
		if (ret) {
			dev_err(dev, "failed to enable clock %s: %d\n",
				cix_usb_clk_names[i], ret);
			goto err_usb_clks;
		}
	}
	return 0;

err_usb_clks:
	while (--i >= 0)
		clk_disable_unprepare(cix_usb_clks[i]);
	return ret;
};

/**
 * cdnsp_sky1_clk_disable_suspend() - disable the clocks which are not
 * needed when suspend
 * @dev:	Pointer to the device of platform_device
 *
 */

static void cdnsp_sky1_clk_disable_suspend(struct device *dev)
{
	int i;
	struct cdnsp_sky1 *data = dev_get_drvdata(dev);
	struct clk **cix_usb_clks = data->cix_usb_clks;

	for (i = 0; i < CIX_USB_CLK_OFF_NUM; i++)
		clk_disable_unprepare(cix_usb_clks[i]);
};

static int sky1_handshake(void __iomem *ptr, u32 mask, u32 done, u64 timeout_us)
{
	u32	result;
	int	ret;

	ret = readl_poll_timeout(ptr, result,
				 (result & mask) == done || result == U32_MAX,
				 10, timeout_us);
	if (result == U32_MAX)		/* card removed */
		return -ENODEV;
	return ret;
}

static int cdns_sky1_platform_suspend(struct device *dev,
				      bool suspend, bool wakeup)
{
	struct cdns *cdns = dev_get_drvdata(dev);
	struct platform_device *xhci_dev = cdns->host_dev;
	struct usb_hcd  *hcd;
	struct device *parent = cdns->dev->parent;
	struct cdnsp_sky1 *data = dev_get_drvdata(parent);
	u32 value;
	int ret = 0;
	int count = 3;

	data->wakeup = wakeup;

	if (cdns->role != USB_ROLE_HOST)
		return 0;

	if (!xhci_dev) {
		dev_dbg(dev, "host controller has not been registered\n");
		return 0;
	}

	hcd = dev_get_drvdata(&xhci_dev->dev);
	if (!hcd) {
		dev_dbg(dev, "host controller have not registered\n");
		return 0;
	}

	if (suspend) {
		while (count--) {
			value = readl(hcd->regs + XECP_PM_PMCSR);
			value &= ~PS_MASK;
			value |= PS_D3 | PS_PME_En;
			writel(value, hcd->regs + XECP_PM_PMCSR);
			/* After controller enters D3, disable AXI and SOF
			 * until AXI valid flag changes to 0.
			 */
			if (sky1_handshake(data->ctst_base, AXI_CLOCK_VALID, 0, 100ULL * 1000))
				dev_dbg(dev, "enter D3 failed,register value:%x\n",
					readl(data->ctst_base));
			else
				break;
		}
		if (count < 0) {
			dev_err(dev, "enter D3 failed after retries, register value:%x\n",
				readl(data->ctst_base));
		}
	} else {
		while (count--) {
			value = readl(hcd->regs + XECP_PM_PMCSR);
			value &= ~PS_MASK;
			value |= PS_D0;
			value &= ~PS_PME_En;
			writel(value, hcd->regs + XECP_PM_PMCSR);
			/* Wait power state back to D0 */
			if (sky1_handshake(hcd->regs + XECP_PM_PMCSR, PS_MASK, 0, 100ULL * 1000)) {
				dev_dbg(dev, "exit D3 timeout, power state=0x%lx\n",
					readl(hcd->regs + XECP_PM_PMCSR) & PS_MASK);
			} else {
				break;
			}
		}
		if (count < 0) {
			dev_err(dev, "exit D3 timeout after retries, power state=0x%lx\n",
				readl(hcd->regs + XECP_PM_PMCSR) & PS_MASK);
		}
	}

	return ret;
}

static void cdnsp_sky1_configure_controller(struct cdnsp_sky1 *data)
{
	int clk;
	int v0, v1, v2;
	u32 val = 0;

	if (data->u3_disable) {
		dev_dbg(data->dev, "disable u3 port\n");
		writel(D_XEC_CFG_3XPORT_MODE_VALUE, data->device_base
			+ D_XEC_CFG_3XPORT_MODE);
	}
	writel(AXI_HALT, data->device_base + D_XEC_AXI_CAP);
	writel(AXI_HALT, data->xhci_base + D_XEC_AXI_CAP);
	writel(data->axi_bmax_value, data->device_base + D_XEC_AXI_CTRL0);
	writel(data->axi_bmax_value, data->xhci_base + D_XEC_AXI_CTRL0);
	writel((~(u32)(AXI_HALT)), data->device_base + D_XEC_AXI_CAP);
	writel((~(u32)(AXI_HALT)), data->xhci_base + D_XEC_AXI_CAP);
	clk = data->sof_clk_freq;
	v0 =  25 * clk / 100000000;
	v1 = clk / 10000;
	v2 = clk / 10;
	writel(((v0 > 1) ? v0 - 1 : 1), data->device_base
		+ D_XEC_PRE_REG_250NS);
	writel((unsigned int)((v1 / 100 > 1) > 0 ? (v1 / 100) - 1 : 1),
	       data->device_base + D_XEC_PRE_REG_1US);
	writel((unsigned int)((v1 / 10 > 1) > 0 ? (v1 / 10) - 1 : 1),
	       data->device_base + D_XEC_PRE_REG_10US);
	writel(((v1) > 1 ? v1 - 1 : 1), data->device_base
		+ D_XEC_PRE_REG_100US);
	writel((unsigned int)((125 * clk / 1000000) > 1 ? (125 * clk / 1000000) : 1),
	       data->device_base + D_XEC_PRE_REG_125US);
	writel(((v2 / 100 > 1) ? (v2 / 100) - 1 : 1), data->device_base
		+ D_XEC_PRE_REG_1MS);
	writel(((v2 / 10 > 1) ? (v2 / 10) - 1 : 1), data->device_base
		+ D_XEC_PRE_REG_10MS);
	writel((v2 > 1 ? v2 - 1 : 1), data->device_base
		+ D_XEC_PRE_REG_100MS);
	dev_dbg(data->dev, "readl:%x, %x ,%x, %x, %x, %x, %x, %x\n",
		readl(data->device_base + D_XEC_PRE_REG_250NS),
		readl(data->device_base + D_XEC_PRE_REG_1US),
		readl(data->device_base + D_XEC_PRE_REG_10US),
		readl(data->device_base + D_XEC_PRE_REG_100US),
		readl(data->device_base + D_XEC_PRE_REG_125US),
		readl(data->device_base + D_XEC_PRE_REG_1MS),
		readl(data->device_base + D_XEC_PRE_REG_10MS),
		readl(data->device_base + D_XEC_PRE_REG_100MS));
	clk = data->lpm_clk_freq;
	v0 =  25 * clk / 100000000;
	v1 = clk / 10000;
	v2 = clk / 10;
	writel(((v0 > 1) ? v0 - 1 : 1), data->device_base
		+ D_XEC_LPM_PRE_REG_250NS);
	writel((unsigned int)((v1 / 100 > 1) > 0 ? (v1 / 100) - 1 : 1), data->device_base
		+ D_XEC_LPM_PRE_REG_1US);
	writel((unsigned int)((v1 / 10 > 1) > 0 ? (v1 / 10) - 1 : 1), data->device_base
		+ D_XEC_LPM_PRE_REG_10US);
	writel(((v1) > 1 ? v1 - 1 : 1), data->device_base
		+ D_XEC_LPM_PRE_REG_100US);
	writel((unsigned int)((125 * clk / 1000000) > 1 ? (125 * clk / 1000000) : 1),
	       data->device_base + D_XEC_LPM_PRE_REG_125US);
	writel(((v2 / 100 > 1) ? (v2 / 100) - 1 : 1), data->device_base
		+ D_XEC_LPM_PRE_REG_1MS);
	writel(((v2 / 10 > 1) ? (v2 / 10) - 1 : 1), data->device_base
		+ D_XEC_LPM_PRE_REG_10MS);
	writel((v2 > 1 ? v2 - 1 : 1), data->device_base
		+ D_XEC_LPM_PRE_REG_100MS);
	v0 = readl(data->xhci_base + XEC_USBSSP_CHICKEN_BITS_3);
	v0 &= ~(CFG_APB_TIMEOUT_PSLVERR_EN | CFG_APB_PSLVERR_EN);
	writel(v0, data->xhci_base + XEC_USBSSP_CHICKEN_BITS_3);
	if (data->u3_disable) {
		dev_dbg(data->dev, "disable u3 port\n");
		writel(XEC_CFG_3XPORT_MODE_VALUE, data->xhci_base
			+ XEC_CFG_3XPORT_MODE);
	} else if (data->ssp_disable) {
		dev_dbg(data->dev, "disable ssp\n");
		v0 = readl(data->xhci_base + XEC_CFG_3XPORT_MODE);
		writel(v0  & CFG_3XPORT_MODE_DIS_SSP, data->xhci_base
			+ XEC_CFG_3XPORT_MODE);
	}
	clk = data->sof_clk_freq;
	v0 =  25 * clk / 100000000;
	v1 = clk / 10000;
	v2 = clk / 10;
	writel(((v0 > 1) ? v0 - 1 : 0), data->xhci_base
		+ XEC_PRE_REG_250NS);
	writel((unsigned int)((v1 / 100 > 1) > 0 ? (v1 / 100) - 1 : 0), data->xhci_base
		+ XEC_PRE_REG_1US);
	writel((unsigned int)((v1 / 10 > 1) > 0 ? (v1 / 10) - 1 : 0), data->xhci_base
		+ XEC_PRE_REG_10US);
	writel(((v1) > 1 ? v1 - 1 : 0), data->xhci_base
		+ XEC_PRE_REG_100US);
	writel((unsigned int)((125 * clk / 1000000) > 1 ? (125 * clk / 1000000) : 0),
	       data->xhci_base + XEC_PRE_REG_125US);
	writel(((v2 / 100 > 1) ? (v2 / 100) - 1 : 0), data->xhci_base
		+ XEC_PRE_REG_1MS);
	writel(((v2 / 10 > 1) ? (v2 / 10) - 1 : 0), data->xhci_base
		+ XEC_PRE_REG_10MS);
	writel((v2 > 1 ? v2 - 1 : 0), data->xhci_base
		+ XEC_PRE_REG_100MS);
	clk = data->lpm_clk_freq;
	v0 =  25 * clk / 100000000;
	v1 = clk / 10000;
	v2 = clk / 10;
	writel(((v0 > 1) ? v0 - 1 : 0), data->xhci_base
		+ XEC_LPM_PRE_REG_250NS);
	writel((unsigned int)((v1 / 100 > 1) > 0 ? (v1 / 100) - 1 : 0), data->xhci_base
		+ XEC_LPM_PRE_REG_1US);
	writel((unsigned int)((v1 / 10 > 1) > 0 ? (v1 / 10) - 1 : 0), data->xhci_base
		+ XEC_LPM_PRE_REG_10US);
	writel(((v1) > 1 ? v1 - 1 : 0), data->xhci_base
		+ XEC_LPM_PRE_REG_100US);
	writel((unsigned int)((125 * clk / 1000000) > 1 ? (125 * clk / 1000000) : 0),
	       data->xhci_base + XEC_LPM_PRE_REG_125US);
	writel(((v2 / 100 > 1) ? (v2 / 100) - 1 : 0), data->xhci_base
		+ XEC_LPM_PRE_REG_1MS);
	writel(((v2 / 10 > 1) ? (v2 / 10) - 1 : 0), data->xhci_base
		+ XEC_LPM_PRE_REG_10MS);
	writel((v2 > 1 ? v2 - 1 : 0), data->xhci_base
		+ XEC_LPM_PRE_REG_100MS);
	val = readl(data->xhci_base + XEC_USBSSP_CLK_GATING_CTRL);
	val |= HOST20_ACLK_GATING_DISABLE | HOST20_UTMI_GATING_DISABLE;
	writel(val, data->xhci_base + XEC_USBSSP_CLK_GATING_CTRL);
}

static int cdnsp_sky1_drd_init(struct cdnsp_sky1 *data)
{
	int ret = 0;

	reset_control_assert(data->reset);
	reset_control_assert(data->preset);
	cdnsp_sky1_clk_disable_all(data->dev);
	ret = cdnsp_sky1_clk_enable_all(data->dev);
	if (ret)
		return ret;
	writel(CIX_USB_AXI_WR_CACHE_VALUE, data->axi_base);
	cdnsp_sky1_set_mode_by_id(data->dev, MODE_STRAP_OTG);
	reset_control_deassert(data->preset);
	cdnsp_sky1_configure_controller(data);
	reset_control_deassert(data->reset);
	return ret;
}

static void __iomem *sky1_of_get_addr_by_name(struct device_node *parent, const char *name)
{
	void __iomem *addr = NULL;
	struct device_node *node;
	int index;

	node = of_get_next_child(parent, NULL);
	if (!node)
		return NULL;

	index = of_property_match_string(node, "reg-names", name);
	if (index >= 0)
		addr = of_iomap(node, index);

	of_node_put(node);
	return addr;
}

static void __iomem *sky1_get_addr_by_name(struct device *dev, const char *name)
{
	return sky1_of_get_addr_by_name(dev->of_node, name);
}

static void sky1_put_addr(void __iomem *regs)
{
	if (regs)
		iounmap(regs);
}

static int cdnsp_sky1_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct of_dev_auxdata cdns_sky1_auxdata[] = {
		{ .compatible = "cdns,usb3", },
		{},
	};
	struct cdnsp_sky1 *data;
	struct cdns3_platform_data *cdns_sky1_pdata;
	int ret = 0;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->axi_base = devm_platform_ioremap_resource_byname(pdev, "axi");
	if (IS_ERR(data->axi_base)) {
		dev_err(dev, "can't map IOMEM resource\n");
		return PTR_ERR(data->axi_base);
	}
	data->ctst_base = devm_platform_ioremap_resource_byname(pdev, "controller");
	if (IS_ERR(data->ctst_base)) {
		dev_err(dev, "can't map IOMEM resource\n");
		return PTR_ERR(data->ctst_base);
	}
	data->reset = devm_reset_control_get_exclusive(&pdev->dev, "core");
	if (IS_ERR(data->reset)) {
		ret = PTR_ERR(data->reset);
		dev_err(dev, "get reset error:%d\n", ret);
		return ret;
	}
	data->preset = devm_reset_control_get_exclusive(&pdev->dev, "apb");
	if (IS_ERR(data->preset)) {
		ret = PTR_ERR(data->preset);
		dev_err(dev, "get reset error:%d\n", ret);
		return ret;
	}
	platform_set_drvdata(pdev, data);
	data->dev = dev;
	ret = of_alias_get_id(dev->of_node, "usb");
	if (ret == -ENODEV) {
		if (device_property_read_u32(dev, "id", &ret))
			ret = -ENODEV;
	}
	if (ret < 0 || ret >=  SKY1_USB_S5_NUM) {
		dev_err(dev, "get alias failed.\n");
		return ret;
	}
	data->id = ret;
	data->usb_syscon = syscon_regmap_lookup_by_phandle(dev->of_node,
							   "cix,usb-syscon");
	if (IS_ERR(data->usb_syscon)) {
		dev_err(dev, "Unable to get cix,usb-syscon regmap");
		return PTR_ERR(data->usb_syscon);
	}
	data->u3_disable = device_property_read_bool(dev, "u3-port-disable");
	data->ssp_disable = device_property_read_bool(dev, "ssp-disable");
	data->sof_clk_freq = CIX_USB_CLK_8M;
	data->lpm_clk_freq = CIX_USB_CLK_32K;
	data->axi_bmax_value = AXI_BMAX_VALUE_DEFAULT;
	data->xhci_base = sky1_get_addr_by_name(dev, "xhci");
	if (!data->xhci_base) {
		ret = -ENODEV;
		goto err_put_xhci;
	}
	data->device_base = sky1_get_addr_by_name(dev, "dev");
	if (!data->device_base) {
		ret = -ENODEV;
		goto err_put_addrs;
	}
	ret = cdnsp_sky1_drd_init(data);
	if (ret == -ETIMEDOUT)
		ret = -EPROBE_DEFER;
	if (ret)
		goto err_quiesce;
	data->oc_gpio = devm_gpiod_get_optional(data->dev, "oc", GPIOD_IN);
	if (IS_ERR(data->oc_gpio)) {
		dev_err(data->dev, "can not get oc_gpio\n");
		ret = PTR_ERR(data->oc_gpio);
		goto err_quiesce;
	}
	if (data->oc_gpio) {
		ret = gpiod_direction_input(data->oc_gpio);
		if (ret < 0)
			dev_err(data->dev, "set oc_gpio input failed:%d\n", ret);
	}
	/*
	 * The platform data is devm-allocated, so it stays alive as long as
	 * this device; the child devices are depopulated before it is freed.
	 */
	cdns_sky1_pdata = devm_kzalloc(dev, sizeof(*cdns_sky1_pdata), GFP_KERNEL);
	if (!cdns_sky1_pdata) {
		ret = -ENOMEM;
		goto err_quiesce;
	}
	cdns_sky1_pdata->platform_suspend = cdns_sky1_platform_suspend;
	cdns_sky1_pdata->quirks = CDNS3_DEFAULT_PM_RUNTIME_ALLOW;
	cdns_sky1_auxdata[0].platform_data = cdns_sky1_pdata;
	ret = of_platform_populate(node, NULL, cdns_sky1_auxdata, dev);
	if (ret) {
		dev_err(dev, "failed to create children: %d\n", ret);
		goto err_quiesce;
	}
	device_set_wakeup_capable(dev, true);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	return 0;

err_quiesce:
	reset_control_assert(data->reset);
	reset_control_assert(data->preset);
	cdnsp_sky1_clk_disable_all(dev);
err_put_addrs:
	sky1_put_addr(data->device_base);
err_put_xhci:
	sky1_put_addr(data->xhci_base);
	return ret;
}

static void cdnsp_sky1_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cdnsp_sky1 *data = dev_get_drvdata(dev);

	pm_runtime_get_sync(dev);
	of_platform_depopulate(dev);
	sky1_put_addr(data->xhci_base);
	sky1_put_addr(data->device_base);
	reset_control_assert(data->reset);
	reset_control_assert(data->preset);
	cdnsp_sky1_clk_disable_all(dev);
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	pm_runtime_put_noidle(dev);
}

/*
 * Because the wake-up interrupt and host interrupt are the same interrupt,
 * closing the axi and sof clock will result in the inability to generate
 * port status change interrupt.
 */
static int cdnsp_sky1_system_suspend(struct device *dev)
{
	struct cdnsp_sky1 *data = dev_get_drvdata(dev);

	if (!data->wakeup) {
		reset_control_assert(data->reset);
		reset_control_assert(data->preset);
	}
	cdnsp_sky1_clk_disable_suspend(dev);
	return 0;
}

static int cdnsp_sky1_system_resume(struct device *dev)
{
	int ret = 0;
	struct cdnsp_sky1 *data = dev_get_drvdata(dev);

	ret = cdnsp_sky1_clk_enable_resume(dev);
	if (ret)
		return ret;
	if (!data->wakeup) {
		writel(CIX_USB_AXI_WR_CACHE_VALUE, data->axi_base);
		cdnsp_sky1_set_mode_by_id(data->dev, MODE_STRAP_OTG);
		reset_control_deassert(data->preset);
		cdnsp_sky1_configure_controller(data);
		reset_control_deassert(data->reset);
	}
	return 0;
}

static const struct dev_pm_ops cdnsp_sky1_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(cdnsp_sky1_system_suspend, cdnsp_sky1_system_resume)
};

static const struct of_device_id cdns_sky1_of_match[] = {
	{ .compatible = "cix,sky1-usbssp", },
	{},
};
MODULE_DEVICE_TABLE(of, cdns_sky1_of_match);

static int cdnsp_sky1_find_cdns_dev(struct device *dev, void *data)
{
	struct device **cdns_dev = data;

	if (dev->of_node && of_device_is_compatible(dev->of_node, "cdns,usb3")) {
		/* keep a reference: the pointer is used after the iteration */
		*cdns_dev = get_device(dev);
		return 1;
	}
	return 0;
}

static int cdnsp_sky1_find_gadget_match(struct device *dev, void *data)
{
	struct device **gadget_dev = data;
	const char *name = dev_name(dev);
	static const char gadget_prefix[] = "gadget.";

	/*
	 * The gadget device is registered on the gadget bus with name
	 * "gadget.%d" (see usb_add_gadget_udc -> dev_set_name).
	 * It sits on the gadget bus and has the function driver bound to it.
	 * Verify the device is on the gadget bus by checking the bus name.
	 * This prevents matching devices that happen to have "gadget." prefix
	 * in their name but are not real gadget devices, and also avoids
	 * NULL pointer dereference when device bus is being removed.
	 */
	if (name && dev->bus && !strcmp(dev->bus->name, "gadget") &&
	    !strncmp(name, gadget_prefix, sizeof(gadget_prefix) - 1)) {
		/* keep a reference: the pointer is used after the iteration */
		*gadget_dev = get_device(dev);
		return 1;
	}
	return 0;
}

static void cdnsp_sky1_shutdown(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cdnsp_sky1 *data = dev_get_drvdata(dev);
	struct device *cdns_dev = NULL;
	struct device *gadget_dev = NULL;
	struct cdns *cdns = NULL;

	if (!device_may_wakeup(dev)) {
		/*
		 * Find the cdns3 child device, then find its gadget device
		 * and release the function driver before disabling clocks.
		 * This ensures all register accesses in gadget_unbind_driver
		 * complete before clocks are turned off.
		 */
		device_for_each_child(dev, &cdns_dev, cdnsp_sky1_find_cdns_dev);
		if (cdns_dev)
			cdns = dev_get_drvdata(cdns_dev);
		if (cdns)
			device_for_each_child(cdns_dev, &gadget_dev,
					      cdnsp_sky1_find_gadget_match);
		if (gadget_dev)
			device_release_driver(gadget_dev);
		if (cdns && cdns->host_dev) {
			struct usb_hcd *hcd = platform_get_drvdata(cdns->host_dev);

			if (hcd && hcd->irq > 0) {
				disable_irq(hcd->irq);
				synchronize_irq(hcd->irq);
				/*
				 * Clear HCD_FLAG_HW_ACCESSIBLE before disable_irq.
				 * This prevents usb_hcd_irq from calling xhci_irq
				 * (which reads USBSTS) after clocks are disabled.
				 */
				clear_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);
			}
		}
		reset_control_assert(data->reset);
		reset_control_assert(data->preset);
		cdnsp_sky1_clk_disable_all(dev);
		put_device(gadget_dev);
		put_device(cdns_dev);
	}
}

static struct platform_driver cdnsp_sky1_driver = {
	.probe		= cdnsp_sky1_probe,
	.remove		= cdnsp_sky1_remove,
	.shutdown	= cdnsp_sky1_shutdown,
	.driver		= {
		.name	= "cdnsp-sky1",
		.of_match_table	= cdns_sky1_of_match,
		.pm	= pm_sleep_ptr(&cdnsp_sky1_pm_ops),
	},
};

module_platform_driver(cdnsp_sky1_driver);

MODULE_ALIAS("platform:cdnsp-sky1");
MODULE_DESCRIPTION("CIX Sky1 Cadence USBSSP DRD glue driver");
MODULE_AUTHOR("Hongliang Yang <hongliang.yang@cixtech.com>");
MODULE_LICENSE("GPL");
