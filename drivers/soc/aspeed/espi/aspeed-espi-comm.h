/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Aspeed eSPI protocol packet definitions
 * Copyright 2026 Aspeed Technology Inc.
 */
#ifndef __ASPEED_ESPI_COMM_H__
#define __ASPEED_ESPI_COMM_H__

#include <linux/bits.h>
#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * eSPI cycle type encoding
 *
 * Section 5.1 Cycle Types and Packet Format,
 * Intel eSPI Interface Base Specification, Rev 1.0, Jan. 2016.
 */
#define ESPI_FLASH_READ			0x00
#define ESPI_FLASH_WRITE		0x01
#define ESPI_FLASH_ERASE		0x02
#define ESPI_FLASH_SUC_CMPLT		0x06
#define ESPI_FLASH_SUC_CMPLT_D_MIDDLE	0x09
#define ESPI_FLASH_SUC_CMPLT_D_FIRST	0x0b
#define ESPI_FLASH_SUC_CMPLT_D_LAST	0x0d
#define ESPI_FLASH_SUC_CMPLT_D_ONLY	0x0f
#define ESPI_FLASH_UNSUC_CMPLT		0x0c

#define ESPI_PLD_LEN_MIN		BIT(6)
#define ESPI_MAX_PLD_LEN		BIT(12)

/*
 * eSPI packet format structure
 *
 * Section 5.1 Cycle Types and Packet Format,
 * Intel eSPI Interface Base Specification, Rev 1.0, Jan. 2016.
 */
struct espi_comm_hdr {
	u8 cyc;
	u8 len_h : 4;
	u8 tag : 4;
	u8 len_l;
};

struct espi_flash_rwe {
	u8 cyc;
	u8 len_h : 4;
	u8 tag : 4;
	u8 len_l;
	u32 addr_be;
	u8 data[];
} __packed;

struct espi_flash_cmplt {
	u8 cyc;
	u8 len_h : 4;
	u8 tag : 4;
	u8 len_l;
	u8 data[];
} __packed;

#endif
