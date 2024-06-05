/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) IBM Corporation 2023 */

#ifndef DRIVERS_FSI_SLAVE_H
#define DRIVERS_FSI_SLAVE_H

#include <linux/cdev.h>
#include <linux/device.h>

#define FSI_SLAVE_BASE			0x800

/*
 * FSI slave engine control register offsets
 */
#define FSI_SMODE		0x0	/* R/W: Mode register */
#define FSI_SISC		0x8	/* R  : Interrupt condition */
#define FSI_SCISC		0x8	/* C  : Clear interrupt condition */
#define FSI_SISM		0xc	/* R/W: Interrupt mask */
#define FSI_SISS		0x10	/* R  : Interrupt status */
#define FSI_SSISM		0x10	/* S  : Set interrupt mask */
#define FSI_SCISM		0x14	/* C  : Clear interrupt mask */
#define FSI_SSTAT		0x14	/* R  : Slave status */
#define FSI_SI1S		0x1c	/* R  : Slave interrupt 1 status */
#define FSI_SSI1M		0x1c	/* S  : Set slave interrupt 1 mask */
#define FSI_SCI1M		0x20	/* C  : Clear slave interrupt 1 mask */
#define FSI_SLBUS		0x30	/* W  : LBUS Ownership */
#define FSI_SRSIC0		0x68	/* C  : Clear remote interrupt condition */
#define FSI_SRSIC4		0x6c	/* C  : Clear remote interrupt condition */
#define FSI_SRSIM0		0x70	/* R/W: Remote interrupt mask */
#define FSI_SRSIM4		0x74	/* R/W: Remote interrupt mask */
#define FSI_SRSIS0		0x78	/* R  : Remote interrupt status */
#define FSI_SRSIS4		0x7c	/* R  : Remote interrupt status */
#define FSI_LLMODE		0x100	/* R/W: Link layer mode register */

/*
 * SMODE fields
 */
#define FSI_SMODE_WSC		0x80000000	/* Warm start done */
#define FSI_SMODE_ECRC		0x20000000	/* Hw CRC check */
#define FSI_SMODE_SID_SHIFT	24		/* ID shift */
#define FSI_SMODE_SID_MASK	3		/* ID Mask */
#define FSI_SMODE_SID_BREAK	3		/* ID after break command */
#define FSI_SMODE_ED_SHIFT	20		/* Echo delay shift */
#define FSI_SMODE_ED_MASK	0xf		/* Echo delay mask */
#define FSI_SMODE_ED_DEFAULT	 16		/* Default echo delay */
#define FSI_SMODE_SD_SHIFT	16		/* Send delay shift */
#define FSI_SMODE_SD_MASK	0xf		/* Send delay mask */
#define FSI_SMODE_SD_DEFAULT	 16		/* Default send delay */
#define FSI_SMODE_LBCRR_SHIFT	8		/* Clk ratio shift */
#define FSI_SMODE_LBCRR_MASK	0xf		/* Clk ratio mask */

/*
 * SISS fields
 */
#define FSI_SISS_CRC_ERROR		BIT(31)
#define FSI_SISS_PROTO_ERROR		BIT(30)
#define FSI_SISS_LBUS_PARITY_ERROR	BIT(29)
#define FSI_SISS_LBUS_PROTO_ERROR	BIT(28)
#define FSI_SISS_ACCESS_ERROR		BIT(27)
#define FSI_SISS_LBUS_OWNERSHIP_ERROR	BIT(26)
#define FSI_SISS_LBUS_OWNERSHIP_CHANGE	BIT(25)
#define FSI_SISS_ASYNC_MODE_ERROR	BIT(14)
#define FSI_SISS_OPB_ACCESS_ERROR	BIT(13)
#define FSI_SISS_OPB_FENCED		BIT(12)
#define FSI_SISS_OPB_PARITY_ERROR	BIT(11)
#define FSI_SISS_OPB_PROTO_ERROR	BIT(10)
#define FSI_SISS_OPB_TIMEOUT		BIT(9)
#define FSI_SISS_OPB_ERROR_ACK		BIT(8)
#define FSI_SISS_MFSI_MASTER_ERROR	BIT(3)
#define FSI_SISS_MFSI_PORT_ERROR	BIT(2)
#define FSI_SISS_MFSI_HP		BIT(1)
#define FSI_SISS_MFSI_CR_PARITY_ERROR	BIT(0)
#define FSI_SISS_ALL			0xfe007f00

/*
 * SI1S fields
 */
#define FSI_SI1S_SLAVE_BIT	31
#define FSI_SI1S_SHIFT_BIT	30
#define FSI_SI1S_SCOM_BIT	29
#define FSI_SI1S_SCRATCH_BIT	28
#define FSI_SI1S_I2C_BIT	27
#define FSI_SI1S_SPI_BIT	26
#define FSI_SI1S_SBEFIFO_BIT	25
#define FSI_SI1S_MBOX_BIT	24

/*
 * SLBUS fields
 */
#define FSI_SLBUS_FORCE		0x80000000	/* Force LBUS ownership */

/*
 * LLMODE fields
 */
#define FSI_LLMODE_ASYNC	0x1

struct fsi_master;

struct fsi_slave {
	struct device		dev;
	struct fsi_master	*master;
	struct cdev		cdev;
	int			cdev_idx;
	int			id;	/* FSI address */
	int			link;	/* FSI link# */
	u32			cfam_id;
	int			chip_id;
	uint32_t		size;	/* size of slave address space */
	u8			t_send_delay;
	u8			t_echo_delay;
};

#define to_fsi_slave(d) container_of(d, struct fsi_slave, dev)

#endif /* DRIVERS_FSI_SLAVE_H */
