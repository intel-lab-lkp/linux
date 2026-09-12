/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2023, 2026 Airoha Technology Corp.
 * Copyright (C) 2026 Aleksei Sviridkin <f@lex.la>
 */

#ifndef __LINUX_MDIO_AIROHA_EN8811H_H
#define __LINUX_MDIO_AIROHA_EN8811H_H

#include <linux/types.h>

struct mdio_device;

#define EN8811H_MD32_DM			"airoha/EthMD32.dm.bin"
#define EN8811H_MD32_DSP		"airoha/EthMD32.DSP.bin"

/* Returns 1 when the firmware runs, 0 when the MD32 is still in its
 * bootloader, and negative on a failed status read.
 */
int air_en8811h_mcu_running(struct mdio_device *mdiodev);
/* Returns 1 when firmware was already running and was left in place. */
int air_en8811h_fw_download(struct mdio_device *mdiodev, u32 *fw_version);

#endif /* __LINUX_MDIO_AIROHA_EN8811H_H */
