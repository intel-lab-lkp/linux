/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Header for the Advantech EIO-IS200 core driver and its sub-drivers
 *
 * Copyright (C) 2023 Advantech Co., Ltd.
 * Author: wenkai.chung <wenkai.chung@advantech.com.tw>
 */

#ifndef _MFD_EIOIS200_H_
#define _MFD_EIOIS200_H_
#include <linux/io.h>
#include <linux/regmap.h>

/* Definition */
#define EIOIS200_CHIPID1		0x20
#define EIOIS200_CHIPID2		0x21
#define EIOIS200_CHIPVER		0x22
#define EIOIS200_SIOCTRL		0x23
#define EIOIS200_SIOCTRL_SIOEN		BIT(0)
#define EIOIS200_SIOCTRL_SWRST		BIT(1)
#define EIOIS200_IRQCTRL		0x70
#define EIOIS200_CHIPID			0x9610
#define EIO201_211_CHIPID		0x9620
#define EIOIS200_ICCODE			0x10
#define EIO201_ICCODE			0x20
#define EIO211_ICCODE			0x21

/* LPC PNP */
#define EIOIS200_PNP_INDEX		0x299
#define EIOIS200_PNP_DATA		0x29A
#define EIOIS200_SUB_PNP_INDEX		0x499
#define EIOIS200_SUB_PNP_DATA		0x49A
#define EIOIS200_EXT_MODE_ENTER		0x87
#define EIOIS200_EXT_MODE_EXIT		0xAA

/* LPC LDN */
#define EIOIS200_LDN			0x07
#define EIOIS200_LDN_PMC0		0x0C
#define EIOIS200_LDN_PMC1		0x0D

/* PMC registers */
#define EIOIS200_PMC_PORT		0x2F0
#define EIOIS200_PMC_PORT_SUB		0x60
#define EIOIS200_PMC_STATUS_IBF		BIT(1)
#define EIOIS200_PMC_STATUS_OBF		BIT(0)
#define EIOIS200_LDAR			0x30
#define EIOIS200_LDAR_LDACT		BIT(0)
#define EIOIS200_IOBA0H			0x60
#define EIOIS200_IOBA0L			0x61
#define EIOIS200_IOBA1H			0x62
#define EIOIS200_IOBA1L			0x63

/* PMC command list */
#define EIOIS200_PMC_CMD_ACPIRAM_READ	0x31
#define EIOIS200_PMC_CMD_CFG_SAVE	0x56

/* OLD PMC */
#define EIOIS200_PMC_NO_INDEX		0xFF

/* ACPI RAM Address Table */
#define EIOIS200_ACPIRAM_VERSIONSECTION	(0xFA)
#define EIOIS200_ACPIRAM_ICVENDOR	(EIOIS200_ACPIRAM_VERSIONSECTION + 0x00)
#define EIOIS200_ACPIRAM_ICCODE		(EIOIS200_ACPIRAM_VERSIONSECTION + 0x01)
#define EIOIS200_ACPIRAM_CODEBASE	(EIOIS200_ACPIRAM_VERSIONSECTION + 0x02)

/* Firmware */
#define EIOIS200_F_SUB_NEW_CODE_BASE	BIT(6)
#define EIOIS200_F_SUB_CHANGED		BIT(7)
#define EIOIS200_F_NEW_CODE_BASE	BIT(8)
#define EIOIS200_F_CHANGED		BIT(9)
#define EIOIS200_F_SUB_CHIP_EXIST	BIT(30)
#define EIOIS200_F_CHIP_EXIST		BIT(31)

/* Others */
#define EC_NUM	2

struct _pmc_port {
	union {
		u16 cmd;
		u16 status;
	};
	u16 data;
};

struct _pmc_op {
	u8  cmd;
	u8  control;
	u8  device_id;
	u8  size;
	u8  *payload;
	u8  chip;
	u16 timeout;
};

enum eiois200_rw_operation {
	OPERATION_READ,
	OPERATION_WRITE,
};

struct eiois200_dev {
	u32 flag;

	struct _pmc_port  pmc[2];

	struct mutex mutex; /* Protects PMC command access */
};

/**
 * eiois200_core_pmc_operation - Execute a new pmc command
 * @op:		Pointer to an new pmc command.
 */
int eiois200_core_pmc_operation(struct _pmc_op *operation);

enum eiois200_pmc_wait {
	PMC_WAIT_INPUT,
	PMC_WAIT_OUTPUT,
};

/**
 * eiois200_core_pmc_wait - Wait for input / output buffer to be ready
 * @id:		0 for main chip, 1 for sub chip.
 * @wait:	%PMC_WAIT_INPUT or %PMC_WAIT_OUTPUT.
 *		%PMC_WAIT_INPUT for waiting input buffer data ready.
 *		%PMC_WAIT_OUTPUT for waiting output buffer empty.
 * @timeout:	The timeout value.
 */
int eiois200_core_pmc_wait(int id, enum eiois200_pmc_wait wait, uint timeout);

#define WAIT_IBF(id, timeout)	eiois200_core_pmc_wait(id, PMC_WAIT_INPUT, timeout)
#define WAIT_OBF(id, timeout)	eiois200_core_pmc_wait(id, PMC_WAIT_OUTPUT, timeout)

#ifdef pr_fmt
#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#endif

#endif
