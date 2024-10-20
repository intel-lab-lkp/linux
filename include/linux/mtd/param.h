/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 - Mediatek
 *
 * Author: Sky Huang <SkyLake.Huang@mediatek.com>
 */

#ifndef __LINUX_NAND_PARAM
#define __LINUX_NAND_PARAM

#include <linux/io.h>

u16 nanddev_crc16(u16 crc, u8 const *p, size_t len);
void nanddev_bit_wise_majority(const void **srcbufs,
				   unsigned int nsrcbufs,
				   void *dstbuf,
				   unsigned int bufsize);

#endif /* __LINUX_NAND_PARAM */

