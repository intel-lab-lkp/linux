/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PIIX4/SB800 SMBus Interfaces
 *
 * Copyright (c) 2024, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Authors: Shyam Sundar S K <Shyam-sundar.S-k@amd.com>
 *	    Sanket Goswami <Sanket.Goswami@amd.com>
 */

#ifndef I2C_PIIX4_H
#define I2C_PIIX4_H

/* PIIX4 SMBus address offsets */
#define SMBHSTSTS	(0 + piix4_smba)
#define SMBHSLVSTS	(1 + piix4_smba)
#define SMBHSTCNT	(2 + piix4_smba)
#define SMBHSTCMD	(3 + piix4_smba)
#define SMBHSTADD	(4 + piix4_smba)
#define SMBHSTDAT0	(5 + piix4_smba)
#define SMBHSTDAT1	(6 + piix4_smba)
#define SMBBLKDAT	(7 + piix4_smba)
#define SMBSLVCNT	(8 + piix4_smba)
#define SMBSHDWCMD	(9 + piix4_smba)
#define SMBSLVEVT	(0xA + piix4_smba)
#define SMBSLVDAT	(0xC + piix4_smba)

/* Count for request_region */
#define SMBIOSIZE	9

/* PIIX4 constants */
#define PIIX4_BLOCK_DATA	0x14

struct sb800_mmio_cfg {
	void __iomem *addr;
	bool use_mmio;
};

int piix4_sb800_port_sel(u8 port, struct sb800_mmio_cfg *mmio_cfg);
int piix4_transaction(struct i2c_adapter *piix4_adapter, unsigned short piix4_smba);
int piix4_sb800_region_request(struct device *dev, struct sb800_mmio_cfg *mmio_cfg);
void piix4_sb800_region_release(struct device *dev, struct sb800_mmio_cfg *mmio_cfg);

#endif /* I2C_PIIX4_H */
