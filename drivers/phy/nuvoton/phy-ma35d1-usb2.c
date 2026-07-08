// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton MA35D1 USB 2.0 PHY driver
 *
 * Supports PHY0 (USB0 OTG port, shared between DWC2 gadget and EHCI0/OHCI0)
 * and PHY1 (USB1 host-only port, used by EHCI1/OHCI1). The hardware mux on
 * PHY0 switches automatically via the USB ID pin.
 *
 * Copyright (C) 2026 Nuvoton Technology Corp.
 */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
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
#define USBPMISCR_PHY_POR(n)		BIT(0  + (n) * 16)
#define USBPMISCR_PHY_SUSPEND(n)	BIT(1  + (n) * 16)
#define USBPMISCR_PHY_COMN(n)		BIT(2  + (n) * 16)
#define USBPMISCR_PHY_HSTCKSTB(n)	BIT(8  + (n) * 16)
#define USBPMISCR_PHY_CK12MSTB(n)	BIT(9  + (n) * 16)
#define USBPMISCR_PHY_DEVCKSTB(n)	BIT(10 + (n) * 16)
/* Mask for control bits (POR, SUSPEND, COMN) */
#define USBPMISCR_PHY_CTL_MASK(n)	(0x7u << ((n) * 16))
/* Host-mode ready */
#define USBPMISCR_PHY_HOST_READY(n)	(USBPMISCR_PHY_SUSPEND(n)  | \
					 USBPMISCR_PHY_HSTCKSTB(n) | \
					 USBPMISCR_PHY_CK12MSTB(n))
/* Device-mode ready */
#define USBPMISCR_PHY_DEV_READY(n)	(USBPMISCR_PHY_SUSPEND(n)  | \
					 USBPMISCR_PHY_DEVCKSTB(n))
/* RCALCODE: 4-bit resistor trim */
#define USBPMISCR_RCAL_SHIFT(n)		(12 + (n) * 16)
#define USBPMISCR_RCAL_MASK(n)		GENMASK(USBPMISCR_RCAL_SHIFT(n) + 3, \
						USBPMISCR_RCAL_SHIFT(n))

#define MA35_SYS_MISCFCR0		0x70
/* USB host over-current detect polarity (shared, both ports) */
#define MISCFCR0_UHOVRCURH		BIT(12)

#define MA35_PHY_NUM			2

struct ma35_phy_port {
	struct phy *phy;
	unsigned int idx;
};

struct ma35_usb_phy {
	struct device *dev;
	struct regmap *sysreg;
	struct ma35_phy_port port[MA35_PHY_NUM];
	struct usb_role_switch *role_sw;
};

static int ma35_usb_phy_init(struct phy *phy)
{
	struct ma35_phy_port *port = phy_get_drvdata(phy);
	struct ma35_usb_phy *p = container_of(port - port->idx,
					      struct ma35_usb_phy, port[0]);
	unsigned int n = port->idx;
	unsigned int val;
	int ret;

	regmap_read(p->sysreg, MA35_SYS_USBPMISCR, &val);

	if (val & USBPMISCR_PHY_SUSPEND(n))
		return 0;

	regmap_update_bits(p->sysreg, MA35_SYS_USBPMISCR,
			   USBPMISCR_PHY_CTL_MASK(n),
			   USBPMISCR_PHY_POR(n) | USBPMISCR_PHY_SUSPEND(n));
	udelay(20);

	regmap_update_bits(p->sysreg, MA35_SYS_USBPMISCR,
			   USBPMISCR_PHY_CTL_MASK(n),
			   USBPMISCR_PHY_SUSPEND(n));

	if (n == 0) {
		ret = regmap_read_poll_timeout(p->sysreg, MA35_SYS_USBPMISCR,
					       val,
					       ((val & USBPMISCR_PHY_HOST_READY(0)) ==
						USBPMISCR_PHY_HOST_READY(0)) ||
					       ((val & USBPMISCR_PHY_DEV_READY(0)) ==
						USBPMISCR_PHY_DEV_READY(0)),
					       10, 1000);
	} else {
		ret = regmap_read_poll_timeout(p->sysreg, MA35_SYS_USBPMISCR,
					       val,
					       (val & USBPMISCR_PHY_HOST_READY(n)) ==
					       USBPMISCR_PHY_HOST_READY(n),
					       10, 1000);
	}

	if (ret) {
		dev_err(p->dev, "USB PHY%u clock not stable (USBPMISCR=0x%08x)\n",
			n, val);
		return ret;
	}

	return 0;
}

static const struct phy_ops ma35_usb_phy_ops = {
	.init		= ma35_usb_phy_init,
	.owner		= THIS_MODULE,
};

static int ma35_role_sw_set(struct usb_role_switch *sw, enum usb_role role)
{
	return -EOPNOTSUPP;
}

static enum usb_role ma35_role_sw_get(struct usb_role_switch *sw)
{
	struct ma35_usb_phy *p = usb_role_switch_get_drvdata(sw);
	u32 val;

	regmap_read(p->sysreg, MA35_SYS_PWRONOTP, &val);

	return (val & PWRONOTP_USBP0ID) ? USB_ROLE_HOST : USB_ROLE_DEVICE;
}

static int ma35_role_switch_init(struct platform_device *pdev,
				 struct ma35_usb_phy *p)
{
	struct usb_role_switch_desc sw_desc = {0};

	sw_desc.set = ma35_role_sw_set;
	sw_desc.get = ma35_role_sw_get;
	sw_desc.allow_userspace_control = true;
	sw_desc.driver_data = p;
	sw_desc.fwnode = dev_fwnode(&pdev->dev);

	p->role_sw = usb_role_switch_register(&pdev->dev, &sw_desc);
	if (IS_ERR(p->role_sw))
		return dev_err_probe(&pdev->dev, PTR_ERR(p->role_sw),
				     "failed to register role switch\n");

	return 0;
}

static void ma35_role_switch_exit(struct ma35_usb_phy *p)
{
	if (p->role_sw) {
		usb_role_switch_unregister(p->role_sw);
		p->role_sw = NULL;
	}
}

static struct phy *ma35_usb_phy_xlate(struct device *dev,
				      const struct of_phandle_args *args)
{
	struct ma35_usb_phy *p = dev_get_drvdata(dev);
	unsigned int idx;

	if (args->args_count == 0)
		idx = 0;
	else
		idx = args->args[0];

	if (idx >= MA35_PHY_NUM)
		return ERR_PTR(-EINVAL);

	return p->port[idx].phy;
}

static int ma35_usb_phy_probe(struct platform_device *pdev)
{
	struct phy_provider *provider;
	struct ma35_usb_phy *p;
	struct clk *clk;
	int n, ret;
	u32 code;

	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->dev = &pdev->dev;
	platform_set_drvdata(pdev, p);

	p->sysreg = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						     "nuvoton,sys");
	if (IS_ERR(p->sysreg))
		return dev_err_probe(&pdev->dev, PTR_ERR(p->sysreg),
				     "failed to get SYS regmap\n");

	clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk),
				     "failed to get clock\n");

	for (n = 0; n < MA35_PHY_NUM; n++) {
		if (of_property_read_u32_index(pdev->dev.of_node,
					       "nuvoton,rcalcode", n, &code))
			continue;

		if (code > 15)
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "rcalcode[%d] %u out of range (0-15)\n",
					     n, code);

		regmap_update_bits(p->sysreg, MA35_SYS_USBPMISCR,
				   USBPMISCR_RCAL_MASK(n),
				   code << USBPMISCR_RCAL_SHIFT(n));
	}

	if (of_property_read_bool(pdev->dev.of_node, "nuvoton,oc-active-high"))
		regmap_update_bits(p->sysreg, MA35_SYS_MISCFCR0,
				   MISCFCR0_UHOVRCURH, MISCFCR0_UHOVRCURH);

	for (n = 0; n < MA35_PHY_NUM; n++) {
		p->port[n].idx = n;

		p->port[n].phy = devm_phy_create(&pdev->dev, pdev->dev.of_node,
						 &ma35_usb_phy_ops);
		if (IS_ERR(p->port[n].phy))
			return dev_err_probe(&pdev->dev, PTR_ERR(p->port[n].phy),
					     "failed to create PHY%d\n", n);

		phy_set_drvdata(p->port[n].phy, &p->port[n]);
	}

	ret = ma35_role_switch_init(pdev, p);
	if (ret)
		return ret;

	provider = devm_of_phy_provider_register(&pdev->dev, ma35_usb_phy_xlate);
	if (IS_ERR(provider)) {
		ma35_role_switch_exit(p);
		return dev_err_probe(&pdev->dev, PTR_ERR(provider),
				     "failed to register PHY provider\n");
	}

	return 0;
}

static void ma35_usb_phy_remove(struct platform_device *pdev)
{
	struct ma35_usb_phy *p = platform_get_drvdata(pdev);

	ma35_role_switch_exit(p);
}

static const struct of_device_id ma35_usb_phy_of_match[] = {
	{ .compatible = "nuvoton,ma35d1-usb2-phy" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ma35_usb_phy_of_match);

static struct platform_driver ma35_usb_phy_driver = {
	.probe		= ma35_usb_phy_probe,
	.remove		= ma35_usb_phy_remove,
	.driver		= {
		.name		= "ma35d1-usb2-phy",
		.of_match_table	= ma35_usb_phy_of_match,
	},
};
module_platform_driver(ma35_usb_phy_driver);

MODULE_DESCRIPTION("Nuvoton ma35d1 USB2.0 PHY driver");
MODULE_AUTHOR("Hui-Ping Chen <hpchen0nvt@gmail.com>");
MODULE_AUTHOR("Joey Lu <a0987203069@gmail.com>");
MODULE_LICENSE("GPL");
