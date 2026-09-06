/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2026 Airoha Technology Corp.
 * Copyright (C) 2026 Collabora Ltd.
 *                    Louis-Alexis Eyraud <louisalexis.eyraud@collabora.com>
 * Copyright (C) 2026 Aleksei Sviridkin <f@lex.la>
 */

#ifndef __LINUX_MDIO_AIROHA_EN8811H_H
#define __LINUX_MDIO_AIROHA_EN8811H_H

#include <linux/types.h>

struct mdio_device;

#define EN8811H_MD32_DM			"airoha/EthMD32.dm.bin"
#define EN8811H_MD32_DSP		"airoha/EthMD32.DSP.bin"

/* Returns 1 running, 0 dormant, negative on a failed status read. */
int air_en8811h_mcu_running(struct mdio_device *mdiodev, bool nested);
/* Returns 1 when it adopted firmware that was already running. */
int air_en8811h_fw_download(struct mdio_device *mdiodev, u32 *fw_version,
			    bool nested);

#endif /* __LINUX_MDIO_AIROHA_EN8811H_H */
