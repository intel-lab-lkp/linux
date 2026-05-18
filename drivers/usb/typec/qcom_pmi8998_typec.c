// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm PMI8998 USB Type-C role-switch driver.
 *
 * The PMI8998 PMIC integrates a USB Type-C detection block inside its
 * SMB2 charger USBIN region at offset 0x1300. The block performs CC
 * sensing, debounce and Rp/Rd resolution in hardware, then reports the
 * negotiated role via TYPE_C_STATUS_4 and a single consolidated
 * "type-c-change" interrupt.
 *
 * This driver translates the hardware-decided role into a
 * usb_role_switch_set_role() call so the USB DRD controller (typically
 * dwc3) can flip between peripheral and host. An optional VBUS supply
 * regulator is enabled on USB_ROLE_HOST transitions so bus-powered
 * peripherals can be powered.
 *
 * No software TCPM state machine is needed because the hardware handles
 * the Type-C protocol natively. Power Delivery is not supported by this
 * driver; the PMI8998 PDPHY block at offset 0x1700 (identical register
 * layout to PM8150B) can be wired up separately by an additional driver
 * if PD negotiation is required.
 *
 * Copyright (c) 2026 Maxim Furman <taygoth@gmail.com>
 */

#include <linux/bits.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/role.h>

#define TYPE_C_STATUS_1_REG			0x0b
#define   UFP_TYPEC_RDSTD				BIT(7)
#define   UFP_TYPEC_RD1P5				BIT(6)
#define   UFP_TYPEC_RD3P0				BIT(5)

#define TYPE_C_STATUS_2_REG			0x0c
#define   DFP_RD_OPEN					BIT(3)
#define   DFP_RD_RA_VCONN				BIT(2)
#define   DFP_RD_RD					BIT(1)
#define   DFP_RA_RA					BIT(0)

#define TYPE_C_STATUS_4_REG			0x0e
#define   UFP_DFP_MODE_STATUS				BIT(7)
#define   TYPEC_VBUS_STATUS				BIT(6)
#define   TYPEC_VBUS_ERROR_STATUS			BIT(5)
#define   TYPEC_DEBOUNCE_DONE_STATUS			BIT(4)
#define   CC_ORIENTATION				BIT(1)
#define   CC_ATTACHED					BIT(0)

#define TYPE_C_STATUS_5_REG			0x0f

#define TYPE_C_INTRPT_ENB_REG			0x67
#define TYPE_C_INTRPT_ENB_SW_CTRL_REG		0x68

struct pmi8998_typec {
	struct device		*dev;
	struct regmap		*regmap;
	u32			base;
	int			irq;
	struct usb_role_switch	*role_sw;
	struct regulator	*vbus;
	bool			vbus_enabled;
	enum usb_role		role;
};

static enum usb_role pmi8998_typec_decode(unsigned int status)
{
	if (!(status & CC_ATTACHED))
		return USB_ROLE_NONE;
	if (!(status & TYPEC_DEBOUNCE_DONE_STATUS))
		return USB_ROLE_NONE;

	return (status & UFP_DFP_MODE_STATUS) ? USB_ROLE_HOST : USB_ROLE_DEVICE;
}

static int pmi8998_typec_apply(struct pmi8998_typec *typec)
{
	unsigned int status;
	enum usb_role role;
	int ret;

	ret = regmap_read(typec->regmap,
			  typec->base + TYPE_C_STATUS_4_REG, &status);
	if (ret)
		return ret;

	role = pmi8998_typec_decode(status);
	if (role == typec->role)
		return 0;

	if (typec->vbus) {
		bool want_vbus = (role == USB_ROLE_HOST);

		if (want_vbus && !typec->vbus_enabled) {
			ret = regulator_enable(typec->vbus);
			if (ret)
				return ret;
			typec->vbus_enabled = true;
		} else if (!want_vbus && typec->vbus_enabled) {
			regulator_disable(typec->vbus);
			typec->vbus_enabled = false;
		}
	}

	ret = usb_role_switch_set_role(typec->role_sw, role);
	if (ret)
		return ret;

	typec->role = role;
	return 0;
}

static irqreturn_t pmi8998_typec_isr(int irq, void *data)
{
	pmi8998_typec_apply(data);
	return IRQ_HANDLED;
}

static int pmi8998_typec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fwnode_handle *connector;
	struct pmi8998_typec *typec;
	int ret;

	typec = devm_kzalloc(dev, sizeof(*typec), GFP_KERNEL);
	if (!typec)
		return -ENOMEM;

	typec->dev = dev;
	typec->role = USB_ROLE_NONE;

	typec->regmap = dev_get_regmap(dev->parent, NULL);
	if (!typec->regmap)
		return -ENODEV;

	ret = of_property_read_u32_index(dev->of_node, "reg", 0, &typec->base);
	if (ret)
		return ret;

	typec->irq = platform_get_irq_byname(pdev, "type-c-change");
	if (typec->irq < 0)
		return typec->irq;

	connector = device_get_named_child_node(dev, "connector");
	if (!connector)
		return -EINVAL;

	typec->role_sw = fwnode_usb_role_switch_get(connector);
	fwnode_handle_put(connector);
	if (IS_ERR(typec->role_sw))
		return PTR_ERR(typec->role_sw);

	typec->vbus = devm_regulator_get_optional(dev, "vdd-vbus");
	if (IS_ERR(typec->vbus)) {
		if (PTR_ERR(typec->vbus) != -ENODEV) {
			ret = PTR_ERR(typec->vbus);
			goto err_role_put;
		}
		typec->vbus = NULL;
	}

	platform_set_drvdata(pdev, typec);

	pmi8998_typec_apply(typec);

	ret = devm_request_threaded_irq(dev, typec->irq, NULL,
					pmi8998_typec_isr, IRQF_ONESHOT,
					dev_name(dev), typec);
	if (ret)
		goto err_role_put;

	return 0;

err_role_put:
	usb_role_switch_put(typec->role_sw);
	return ret;
}

static void pmi8998_typec_remove(struct platform_device *pdev)
{
	struct pmi8998_typec *typec = platform_get_drvdata(pdev);

	if (typec->vbus_enabled)
		regulator_disable(typec->vbus);
	usb_role_switch_put(typec->role_sw);
}

static const struct of_device_id pmi8998_typec_of_match[] = {
	{ .compatible = "qcom,pmi8998-typec" },
	{ }
};
MODULE_DEVICE_TABLE(of, pmi8998_typec_of_match);

static struct platform_driver pmi8998_typec_driver = {
	.probe		= pmi8998_typec_probe,
	.remove		= pmi8998_typec_remove,
	.driver		= {
		.name		= "qcom-pmi8998-typec",
		.of_match_table	= pmi8998_typec_of_match,
	},
};
module_platform_driver(pmi8998_typec_driver);

MODULE_AUTHOR("Maxim Furman <taygoth@gmail.com>");
MODULE_DESCRIPTION("Qualcomm PMI8998 USB Type-C role-switch driver");
MODULE_LICENSE("GPL");
