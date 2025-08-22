// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  MFD southbridge driver for Vortex SoCs
 *
 *  Author: Marcos Del Sol Vives <marcos@orca.pet>
 *
 *  Based on the RDC321x MFD driver by Florian Fainelli and Bernhard Loos
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/mfd/core.h>

struct vortex_southbridge {
	const struct mfd_cell *cells;
	int n_cells;
};

/* Layout for Vortex86DX/MX */
static const struct resource vortex_dx_gpio_resources[] = {
	{
		.name	= "dat1",
		.start	= 0x78,
		.end	= 0x7C,
		.flags	= IORESOURCE_IO,
	}, {
		.name	= "dir1",
		.start	= 0x98,
		.end	= 0x9C,
		.flags	= IORESOURCE_IO,
	}
};

static const struct mfd_cell vortex_dx_sb_cells[] = {
	{
		.name		= "vortex-gpio",
		.resources	= vortex_dx_gpio_resources,
		.num_resources	= ARRAY_SIZE(vortex_dx_gpio_resources),
	},
};

static const struct vortex_southbridge vortex_dx_sb = {
	.cells = vortex_dx_sb_cells,
	.n_cells = ARRAY_SIZE(vortex_dx_sb_cells),
};

/* Layout for Vortex86DX2/DX3 */
static const struct resource vortex_dx2_gpio_resources[] = {
	{
		.name	= "dat1",
		.start	= 0x78,
		.end	= 0x7C,
		.flags	= IORESOURCE_IO,
	}, {
		.name	= "dat2",
		.start	= 0x100,
		.end	= 0x105,
		.flags	= IORESOURCE_IO,
	}, {
		.name	= "dir1",
		.start	= 0x98,
		.end	= 0x9D,
		.flags	= IORESOURCE_IO,
	}, {
		.name	= "dir2",
		.start	= 0x93,
		.end	= 0x97,
		.flags	= IORESOURCE_IO,
	}
};

static const struct mfd_cell vortex_dx2_sb_cells[] = {
	{
		.name		= "vortex-gpio",
		.resources	= vortex_dx2_gpio_resources,
		.num_resources	= ARRAY_SIZE(vortex_dx2_gpio_resources),
	},
};

static const struct vortex_southbridge vortex_dx2_sb = {
	.cells = vortex_dx2_sb_cells,
	.n_cells = ARRAY_SIZE(vortex_dx2_sb_cells),
};

static int vortex_sb_probe(struct pci_dev *pdev,
			   const struct pci_device_id *ent)
{
	struct vortex_southbridge *priv = (struct vortex_southbridge *) ent->driver_data;
	int err;

	/*
	 * In the Vortex86DX3, the southbridge appears twice (on both 00:07.0
	 * and 00:07.1). Register only once for .0.
	 *
	 * Other Vortex boards (eg Vortex86MX+) have the southbridge exposed
	 * only once, also at 00:07.0.
	 */
	if (PCI_FUNC(pdev->devfn) != 0)
		return -ENODEV;

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "failed to enable device\n");
		return err;
	}

	return devm_mfd_add_devices(&pdev->dev, PLATFORM_DEVID_NONE,
				    priv->cells, priv->n_cells,
				    NULL, 0, NULL);
}

static const struct pci_device_id vortex_sb_table[] = {
	/* Vortex86DX */
	{ PCI_DEVICE_DATA(RDC, R6031, &vortex_dx_sb) },
	/* Vortex86DX2/DX3 */
	{ PCI_DEVICE_DATA(RDC, R6035, &vortex_dx2_sb) },
	/* Vortex86MX */
	{ PCI_DEVICE_DATA(RDC, R6036, &vortex_dx_sb) },
	{}
};
MODULE_DEVICE_TABLE(pci, vortex_sb_table);

static struct pci_driver vortex_sb_driver = {
	.name		= "vortex-sb",
	.id_table	= vortex_sb_table,
	.probe		= vortex_sb_probe,
};

module_pci_driver(vortex_sb_driver);

MODULE_AUTHOR("Marcos Del Sol Vives <marcos@orca.pet>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Vortex MFD southbridge driver");
