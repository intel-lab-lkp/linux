/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Header for the Advantech EIO core driver and its sub-drivers
 *
 * Copyright (C) 2025 Advantech Co., Ltd.
 */

#ifndef _MFD_EIO_H_
#define _MFD_EIO_H_
#include <linux/io.h>
#include <linux/regmap.h>

/* Definition */
#define EIO_CHIPID1		0x20
#define EIO_CHIPID2		0x21
#define EIO_CHIPVER		0x22
#define EIO_SIOCTRL		0x23
#define EIO_SIOCTRL_SIOEN	BIT(0)
#define EIO_SIOCTRL_SWRST	BIT(1)
#define EIO_IRQCTRL		0x70
#define EIO200_CHIPID		0x9610
#define EIO201_211_CHIPID	0x9620
#define EIO200_ICCODE		0x10
#define EIO201_ICCODE		0x20
#define EIO211_ICCODE		0x21

/* LPC PNP */
#define EIO_PNP_INDEX		0x299
#define EIO_PNP_DATA		0x29A
#define EIO_SUB_PNP_INDEX	0x499
#define EIO_SUB_PNP_DATA	0x49A
#define EIO_EXT_MODE_ENTER	0x87
#define EIO_EXT_MODE_EXIT	0xAA

/* LPC LDN */
#define EIO_LDN			0x07
#define EIO_LDN_PMC0		0x0C
#define EIO_LDN_PMC1		0x0D

/* PMC registers */
#define EIO_PMC_STATUS_IBF	BIT(1)
#define EIO_PMC_STATUS_OBF	BIT(0)
#define EIO_LDAR		0x30
#define EIO_LDAR_LDACT		BIT(0)
#define EIO_IOBA0H		0x60
#define EIO_IOBA0L		0x61
#define EIO_IOBA1H		0x62
#define EIO_IOBA1L		0x63
#define EIO_FLAG_PMC_READ	BIT(0)

/* PMC command list */
#define EIO_PMC_CMD_ACPIRAM_READ	0x31
#define EIO_PMC_CMD_CFG_SAVE		0x56

/* OLD PMC */
#define EIO_PMC_NO_INDEX	0xFF

/* ACPI RAM Address Table */
#define EIO_ACPIRAM_VERSIONSECTION	(0xFA)
#define EIO_ACPIRAM_ICVENDOR		(EIO_ACPIRAM_VERSIONSECTION + 0x00)
#define EIO_ACPIRAM_ICCODE		(EIO_ACPIRAM_VERSIONSECTION + 0x01)
#define EIO_ACPIRAM_CODEBASE		(EIO_ACPIRAM_VERSIONSECTION + 0x02)

#define EIO_ACPIRAM_CODEBASE_NEW	BIT(7)

/* Firmware */
#define EIO_F_SUB_NEW_CODE_BASE	BIT(6)
#define EIO_F_SUB_CHANGED	BIT(7)
#define EIO_F_NEW_CODE_BASE	BIT(8)
#define EIO_F_CHANGED		BIT(9)
#define EIO_F_SUB_CHIP_EXIST	BIT(30)
#define EIO_F_CHIP_EXIST	BIT(31)

/* Others */
#define EIO_EC_NUM	2

struct _pmc_port {
	union {
		u16 cmd;
		u16 status;
	};
	u16 data;
};

struct pmc_op {
	u8  cmd;
	u8  control;
	u8  device_id;
	u8  size;
	u8  *payload;
	u8  chip;
	u16 timeout;
};

enum eio_rw_operation {
	OPERATION_READ,
	OPERATION_WRITE,
};

struct eio_dev {
	struct device *dev;
	struct regmap *map;
	void __iomem  *iomem;
	struct mutex mutex; /* Protects PMC command access */
	struct _pmc_port pmc[EIO_EC_NUM];
	u32 flag;
};

int eio_core_pmc_operation(struct device *dev, struct pmc_op *operation);

enum eio_pmc_wait {
	PMC_WAIT_INPUT,
	PMC_WAIT_OUTPUT,
};

int eio_core_pmc_wait(struct device *dev, int id, enum eio_pmc_wait wait,
		      uint timeout);

#define WAIT_IBF(dev, id, timeout)	eio_core_pmc_wait(dev, id, PMC_WAIT_INPUT, timeout)
#define WAIT_OBF(dev, id, timeout)	eio_core_pmc_wait(dev, id, PMC_WAIT_OUTPUT, timeout)

#ifdef pr_fmt
#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#endif

#endif
