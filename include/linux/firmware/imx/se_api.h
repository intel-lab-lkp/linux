/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2025 NXP
 */

#ifndef __SE_API_H__
#define __SE_API_H__

#include <linux/types.h>

#define SOC_ID_OF_IMX8ULP		0x084d
#define SOC_ID_OF_IMX93			0x9300

int imx_se_read_fuse(void *se_if_data, uint16_t fuse_id, u32 *value);
int imx_se_write_fuse(void *se_if_data, uint16_t fuse_id, u32 value);

#endif /* __SE_API_H__ */
