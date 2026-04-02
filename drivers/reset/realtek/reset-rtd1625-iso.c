// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Realtek Semiconductor Corporation
 */

#include <dt-bindings/reset/realtek,rtd1625.h>
#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/slab.h>
#include "common.h"

#define RTD1625_ISO_RSTN_MAX	29
#define RTD1625_ISO_S_RSTN_MAX	5

static struct rtk_reset_desc rtd1625_iso_reset_descs[] = {
	[RTD1625_ISO_RSTN_VFD]                 = { .ofs = 0x88, .bit = 0 },
	[RTD1625_ISO_RSTN_CEC0]                = { .ofs = 0x88, .bit = 2 },
	[RTD1625_ISO_RSTN_CEC1]                = { .ofs = 0x88, .bit = 3 },
	[RTD1625_ISO_RSTN_CBUSTX]              = { .ofs = 0x88, .bit = 5 },
	[RTD1625_ISO_RSTN_CBUSRX]              = { .ofs = 0x88, .bit = 6 },
	[RTD1625_ISO_RSTN_USB3_PHY2_XTAL_POW]  = { .ofs = 0x88, .bit = 7 },
	[RTD1625_ISO_RSTN_UR0]                 = { .ofs = 0x88, .bit = 8 },
	[RTD1625_ISO_RSTN_GMAC]                = { .ofs = 0x88, .bit = 9 },
	[RTD1625_ISO_RSTN_GPHY]                = { .ofs = 0x88, .bit = 10 },
	[RTD1625_ISO_RSTN_I2C_0]               = { .ofs = 0x88, .bit = 11 },
	[RTD1625_ISO_RSTN_I2C_1]               = { .ofs = 0x88, .bit = 12 },
	[RTD1625_ISO_RSTN_CBUS]                = { .ofs = 0x88, .bit = 13 },
	[RTD1625_ISO_RSTN_USB_DRD]             = { .ofs = 0x88, .bit = 14 },
	[RTD1625_ISO_RSTN_USB_HOST]            = { .ofs = 0x88, .bit = 15 },
	[RTD1625_ISO_RSTN_USB_PHY_0]           = { .ofs = 0x88, .bit = 16 },
	[RTD1625_ISO_RSTN_USB_PHY_1]           = { .ofs = 0x88, .bit = 17 },
	[RTD1625_ISO_RSTN_USB_PHY_2]           = { .ofs = 0x88, .bit = 18 },
	[RTD1625_ISO_RSTN_USB]                 = { .ofs = 0x88, .bit = 19 },
	[RTD1625_ISO_RSTN_TYPE_C]              = { .ofs = 0x88, .bit = 20 },
	[RTD1625_ISO_RSTN_USB_U3_HOST]         = { .ofs = 0x88, .bit = 21 },
	[RTD1625_ISO_RSTN_USB3_PHY0_POW]       = { .ofs = 0x88, .bit = 22 },
	[RTD1625_ISO_RSTN_USB3_P0_MDIO]        = { .ofs = 0x88, .bit = 23 },
	[RTD1625_ISO_RSTN_USB3_PHY1_POW]       = { .ofs = 0x88, .bit = 24 },
	[RTD1625_ISO_RSTN_USB3_P1_MDIO]        = { .ofs = 0x88, .bit = 25 },
	[RTD1625_ISO_RSTN_VTC]                 = { .ofs = 0x88, .bit = 26 },
	[RTD1625_ISO_RSTN_USB3_PHY2_POW]       = { .ofs = 0x88, .bit = 27 },
	[RTD1625_ISO_RSTN_USB3_P2_MDIO]        = { .ofs = 0x88, .bit = 28 },
	[RTD1625_ISO_RSTN_USB_PHY_3]           = { .ofs = 0x88, .bit = 29 },
	[RTD1625_ISO_RSTN_USB_PHY_4]           = { .ofs = 0x88, .bit = 30 },
};

static struct rtk_reset_desc rtd1625_iso_s_reset_descs[] = {
	[RTD1625_ISO_S_RSTN_ISOM_MIS] = { .ofs = 0x310, .bit = 0, .write_en = 1 },
	[RTD1625_ISO_S_RSTN_GPIOM]    = { .ofs = 0x310, .bit = 2, .write_en = 1 },
	[RTD1625_ISO_S_RSTN_TIMER7]   = { .ofs = 0x310, .bit = 4, .write_en = 1 },
	[RTD1625_ISO_S_RSTN_IRDA]     = { .ofs = 0x310, .bit = 6, .write_en = 1 },
	[RTD1625_ISO_S_RSTN_UR10]     = { .ofs = 0x310, .bit = 8, .write_en = 1 },
};

static int rtd1625_iso_reset_probe(struct auxiliary_device *adev,
				   const struct auxiliary_device_id *id)
{
	struct device *dev = &adev->dev;
	struct device *parent = dev->parent;
	struct rtk_reset_data *data;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if (of_device_is_compatible(parent->of_node, "realtek,rtd1625-iso-s-clk")) {
		data->descs           = rtd1625_iso_s_reset_descs;
		data->rcdev.nr_resets = RTD1625_ISO_S_RSTN_MAX;
	} else {
		data->descs           = rtd1625_iso_reset_descs;
		data->rcdev.nr_resets = RTD1625_ISO_RSTN_MAX;
	}
	return rtk_reset_controller_add(dev, data);
}

static const struct auxiliary_device_id rtd1625_iso_reset_ids[] = {
	{
		.name = "clk_rtk.iso_rst",
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(auxiliary, rtd1625_iso_reset_ids);

static struct auxiliary_driver rtd1625_iso_driver = {
	.probe = rtd1625_iso_reset_probe,
	.id_table = rtd1625_iso_reset_ids,
	.driver = {
		.name = "rtd1625-iso-reset",
	},
};
module_auxiliary_driver(rtd1625_iso_driver);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("REALTEK_RESET");
