// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton MA35D1 USB 2.0 OTG PHY driver
 *
 * PHY0 (USB0) is shared between DWC2 gadget and EHCI0/OHCI0 host
 * controllers. The hardware mux switches automatically via the USB
 * ID pin. PHY1 (USB1) is host-only.
 *
 * Copyright (C) 2026 Nuvoton Technology Corp.
 */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/usb/role.h>

#define MA35_SYS_PWRONOTP		0x04
#define PWRONOTP_USBP0ID		BIT(16)

#define MA35_SYS_USBPMISCR		0x60
#define USBPMISCR_PHY_POR(n)		BIT(0 + (n) * 16)
#define USBPMISCR_PHY_SUSPEND(n)	BIT(1 + (n) * 16)
#define USBPMISCR_PHY_COMN(n)		BIT(2 + (n) * 16)
#define USBPMISCR_PHY_HSTCKSTB(n)	BIT(8 + (n) * 16)
#define USBPMISCR_PHY_CK12MSTB(n)	BIT(9 + (n) * 16)
/* Mask for control bits (POR, SUSPEND, COMN) of one PHY */
#define USBPMISCR_PHY_CTL_MASK(n)	(0x7 << ((n) * 16))
/* Host-mode ready: SUSPEND + HSTCKSTB + CK12MSTB */
#define USBPMISCR_PHY_HOST_READY(n)	(USBPMISCR_PHY_SUSPEND(n)  | \
					 USBPMISCR_PHY_HSTCKSTB(n) | \
					 USBPMISCR_PHY_CK12MSTB(n))
/* RCALCODE: 4-bit resistor trim at bits [15:12] (PHY0) or [31:28] (PHY1) */
#define USBPMISCR_RCAL_SHIFT(n)		(12 + (n) * 16)
#define USBPMISCR_RCAL_MASK(n)		GENMASK(USBPMISCR_RCAL_SHIFT(n) + 3, \
						USBPMISCR_RCAL_SHIFT(n))

#define MA35_SYS_MISCFCR0		0x70
/* MISCFCR0[12]: USB host over-current detect polarity (shared, both ports) */
#define MISCFCR0_UHOVRCURH		BIT(12)

struct ma35_otg_phy {
	struct clk *clk;
	struct device *dev;
	struct regmap *sysreg;
	unsigned int phy_idx;
	struct usb_role_switch *role_sw;
	enum usb_role cur_role;
};

static int ma35_otg_phy_init(struct phy *phy)
{
	struct ma35_otg_phy *p = phy_get_drvdata(phy);
	unsigned int n = p->phy_idx;
	u32 ready_mask = USBPMISCR_PHY_HOST_READY(n);
	unsigned int val;
	int ret;

	regmap_read(p->sysreg, MA35_SYS_USBPMISCR, &val);
	if ((val & ready_mask) == ready_mask)
		return 0;

	regmap_update_bits(p->sysreg, MA35_SYS_USBPMISCR,
			   USBPMISCR_PHY_CTL_MASK(n),
			   USBPMISCR_PHY_POR(n) | USBPMISCR_PHY_SUSPEND(n));
	msleep(20);

	regmap_update_bits(p->sysreg, MA35_SYS_USBPMISCR,
			   USBPMISCR_PHY_CTL_MASK(n),
			   USBPMISCR_PHY_SUSPEND(n));

	ret = regmap_read_poll_timeout(p->sysreg, MA35_SYS_USBPMISCR, val,
				       (val & ready_mask) == ready_mask,
				       10, 1000);
	if (ret) {
		dev_err(p->dev, "USB PHY%u clock not stable (USBPMISCR=0x%08x)\n",
			n, val);
		return ret;
	}

	return 0;
}

static int ma35_otg_phy_power_on(struct phy *phy)
{
	struct ma35_otg_phy *p = phy_get_drvdata(phy);

	return clk_prepare_enable(p->clk);
}

static int ma35_otg_phy_power_off(struct phy *phy)
{
	struct ma35_otg_phy *p = phy_get_drvdata(phy);

	clk_disable_unprepare(p->clk);
	return 0;
}

static const struct phy_ops ma35_otg_phy_ops = {
	.init = ma35_otg_phy_init,
	.power_on = ma35_otg_phy_power_on,
	.power_off = ma35_otg_phy_power_off,
	.owner = THIS_MODULE,
};

static enum usb_role ma35_otg_read_id(struct ma35_otg_phy *p)
{
	unsigned int val;

	regmap_read(p->sysreg, MA35_SYS_PWRONOTP, &val);
	return (val & PWRONOTP_USBP0ID) ? USB_ROLE_HOST : USB_ROLE_DEVICE;
}

static int ma35_otg_role_sw_set(struct usb_role_switch *sw,
				enum usb_role role)
{
	struct ma35_otg_phy *p = usb_role_switch_get_drvdata(sw);

	p->cur_role = role;

	return 0;
}

static enum usb_role ma35_otg_role_sw_get(struct usb_role_switch *sw)
{
	struct ma35_otg_phy *p = usb_role_switch_get_drvdata(sw);

	return ma35_otg_read_id(p);
}

static int ma35_otg_role_switch_init(struct platform_device *pdev,
				     struct ma35_otg_phy *p)
{
	struct usb_role_switch_desc sw_desc = { };

	p->cur_role = ma35_otg_read_id(p);

	sw_desc.set = ma35_otg_role_sw_set;
	sw_desc.get = ma35_otg_role_sw_get;
	sw_desc.allow_userspace_control = true;
	sw_desc.driver_data = p;
	sw_desc.fwnode = dev_fwnode(&pdev->dev);

	p->role_sw = usb_role_switch_register(&pdev->dev, &sw_desc);
	if (IS_ERR(p->role_sw))
		return dev_err_probe(&pdev->dev, PTR_ERR(p->role_sw),
				     "failed to register role switch\n");

	return 0;
}

static void ma35_otg_role_switch_exit(struct ma35_otg_phy *p)
{
	if (!p->role_sw)
		return;

	usb_role_switch_unregister(p->role_sw);
	p->role_sw = NULL;
}

static int ma35_otg_phy_probe(struct platform_device *pdev)
{
	struct phy_provider *provider;
	struct ma35_otg_phy *p;
	unsigned int sys_args[1];
	struct phy *phy;
	u32 rcalcode;
	int ret;

	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->dev = &pdev->dev;
	platform_set_drvdata(pdev, p);

	p->sysreg = syscon_regmap_lookup_by_phandle_args(pdev->dev.of_node,
							 "nuvoton,sys",
							 1, sys_args);
	if (IS_ERR(p->sysreg))
		return dev_err_probe(&pdev->dev, PTR_ERR(p->sysreg),
				     "Failed to get SYS regmap\n");

	p->phy_idx = sys_args[0];

	if (p->phy_idx > 1)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid PHY index %u (must be 0 or 1)\n",
				     p->phy_idx);

	p->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(p->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(p->clk),
				     "failed to get PHY clock\n");

	if (!of_property_read_u32(pdev->dev.of_node, "nuvoton,rcalcode",
				  &rcalcode)) {
		if (rcalcode > 15)
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "rcalcode %u out of range (0-15)\n",
					     rcalcode);
		regmap_update_bits(p->sysreg, MA35_SYS_USBPMISCR,
				   USBPMISCR_RCAL_MASK(p->phy_idx),
				   rcalcode << USBPMISCR_RCAL_SHIFT(p->phy_idx));
	}

	if (of_property_read_bool(pdev->dev.of_node, "nuvoton,oc-active-high"))
		regmap_update_bits(p->sysreg, MA35_SYS_MISCFCR0,
				   MISCFCR0_UHOVRCURH, MISCFCR0_UHOVRCURH);

	phy = devm_phy_create(&pdev->dev, pdev->dev.of_node, &ma35_otg_phy_ops);
	if (IS_ERR(phy))
		return dev_err_probe(&pdev->dev, PTR_ERR(phy),
				     "Failed to create PHY\n");

	phy_set_drvdata(phy, p);

	provider = devm_of_phy_provider_register(&pdev->dev,
						 of_phy_simple_xlate);
	if (IS_ERR(provider))
		return dev_err_probe(&pdev->dev, PTR_ERR(provider),
				     "Failed to register PHY provider\n");

	if (p->phy_idx == 0) {
		ret = ma35_otg_role_switch_init(pdev, p);
		if (ret)
			return ret;
	}

	return 0;
}

static void ma35_otg_phy_remove(struct platform_device *pdev)
{
	struct ma35_otg_phy *p = platform_get_drvdata(pdev);

	ma35_otg_role_switch_exit(p);
}

static const struct of_device_id ma35_otg_phy_of_match[] = {
	{ .compatible = "nuvoton,ma35d1-usb2-phy-otg" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ma35_otg_phy_of_match);

static struct platform_driver ma35_otg_phy_driver = {
	.probe	= ma35_otg_phy_probe,
	.remove	= ma35_otg_phy_remove,
	.driver	= {
		.name		= "ma35d1-usb2-phy-otg",
		.of_match_table	= ma35_otg_phy_of_match,
	},
};
module_platform_driver(ma35_otg_phy_driver);

MODULE_DESCRIPTION("Nuvoton MA35D1 USB 2.0 OTG PHY driver");
MODULE_AUTHOR("Joey Lu <a0987203069@gmail.com>");
MODULE_LICENSE("GPL");
