/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Arm CLA driver - register definitions
 *
 * Copyright 2026 Arm Limited.
 */
#ifndef _ARM_CLA_REGS_H_
#define _ARM_CLA_REGS_H_

#include <linux/bitfield.h>
#include <linux/bits.h>

/* Registers */
#define CLA_REG_DATA(i)			(0x00 + (8 * (i)))
#define CLA_REG_LAUNCH			0x40
#define CLA_REG_LRESP			0x48
#define CLA_REG_PL0CTRL			0x60
#define CLA_REG_PL1CTRL			0x68
#define CLA_REG_PL2CTRL			0x70
#define CLA_REG_EVENT			0x78
#define CLA_REG_STATUS(i)		(0x80 + (8 * (i)))
#define CLA_REG_CLAAIDR			0xC0

#define CLA_LAUNCH_OP			GENMASK(3, 0)
#define CLA_LAUNCH_NDATA_M1		GENMASK(6, 4)
#define CLA_LAUNCH_SEQ			BIT(7)
#define CLA_LAUNCH_ACCID		GENMASK(10, 8)
#define CLA_LAUNCH_REGIDX		GENMASK(63, 32)

#define CLA_LRESP_PENDING		BIT(0)
#define CLA_LRESP_CODE			GENMASK(2, 1)
#define CLA_LRESP_ERRCODE		GENMASK(7, 3)
#define CLA_LRESP_DATANZ		BIT(15)

#define CLA_PLxCTRL_AVAIL		BIT(0)
#define CLA_PLxCTRL_DBGPERM		GENMASK(3, 1)
#define CLA_PLxCTRL_PREP(accid, v)	((u64)(v) << (8 * (accid)))

#define CLA_STATUS_AVAIL		BIT(0)
#define CLA_STATUS_DMB			BIT(1)
#define CLA_STATUS_EABORT		BIT(2)
#define CLA_STATUS_IDLE			BIT(4)
#define CLA_STATUS_READY		BIT(5)
#define CLA_STATUS_FAULT		BIT(6)
#define CLA_STATUS_EXCEPT		BIT(7)
#define CLA_STATUS_SRMODE		BIT(8)
#define CLA_STATUS_USER			GENMASK(63, 16)

/* Some useful values for sanity checks */
#define CLA_STATUS_STATE_IDLE		(CLA_STATUS_AVAIL | \
					 CLA_STATUS_IDLE | \
					 CLA_STATUS_READY)
#define CLA_STATUS_STATE_SRMODE		(CLA_STATUS_AVAIL | \
					 CLA_STATUS_IDLE | \
					 CLA_STATUS_READY | \
					 CLA_STATUS_SRMODE)
/* Bits we care about when checking the state */
#define CLA_STATUS_STATE_MASK		(CLA_STATUS_AVAIL | \
					 CLA_STATUS_EABORT | \
					 CLA_STATUS_IDLE | \
					 CLA_STATUS_READY | \
					 CLA_STATUS_FAULT | \
					 CLA_STATUS_EXCEPT | \
					 CLA_STATUS_SRMODE)

/* Standard accelerator registers */
#define CLA_REG_IIDR			0x0000
#define CLA_REG_DEVARCH			0x0001
#define CLA_REG_REVIDR			0x0002
#define CLA_REG_IASSIZE			0x0003
#define CLA_REG_ACAP			0x0004
#define CLA_REG_FSARV			0x001f
#define CLA_REG_FSAR(n)			(0x0020 + (n))
#define CLA_REG_TSCTRLOWNER		0x00c0
#define CLA_REG_TSCTRL			0x00c8
#define CLA_REG_TSOFFOWNER		0x00d0
#define CLA_REG_TSVOFF			0x00d8
#define CLA_REG_TSPOFF			0x00d9
#define CLA_REG_PMUOWNER		0x0100
#define CLA_REG_PMURESET		0x0108
#define CLA_REG_PMUCTRL			0x0110
#define CLA_REG_PMUSNAP			0x0111
#define CLA_REG_PMUEVT(n)		(0x0120 + (n))
#define CLA_REG_PMUCNT(n)		(0x0140 + (n))
#define CLA_REG_PMUSCNT(n)		(0x0160 + (n))
#define CLA_REG_IASn			0x8000

#define CLA_IIDR_PRODUCTID		GENMASK(31, 20)
#define CLA_IIDR_VARIANT		GENMASK(19, 16)
#define CLA_IIDR_REVISION		GENMASK(15, 12)
#define CLA_IIDR_IMPLEMENTER		GENMASK(11, 0)

#define CLA_DEVARCH_ARCHITECT		GENMASK(31, 21)
#define CLA_DEVARCH_PRESENT		BIT(20)
#define CLA_DEVARCH_REVISION		GENMASK(19, 16)
#define CLA_DEVARCH_ARCHID		GENMASK(15, 0)

#define CLA_REVIDR_REVISION		GENMASK(31, 0)

#define CLA_ACAP_SROP			BIT(0)
#define CLA_ACAP_REGSTATE		BIT(1)
#define CLA_ACAP_PMUCNTS		GENMASK(4, 2)
#define CLA_ACAP_TS			BIT(5)

#define CLA_FSAR_READ			BIT(0)
#define CLA_FSAR_WRITE			BIT(1)
#define CLA_FSAR_ADDR			GENMASK(63, 6)

#define CLA_TSCTRLOWNER_PL		GENMASK(1, 0)
#define CLA_TSCTRL_TS			GENMASK(1, 0)
#define CLA_TSOFFOWNER_PL		GENMASK(1, 0)

#define CLA_TSCTRL_ZERO			0
#define CLA_TSCTRL_VIRTUAL		1
#define CLA_TSCTRL_GUESTPHYSICAL	2
#define CLA_TSCTRL_PHYSICAL		3

#define CLA_PMUCTRL_EN			BIT(0)
#define CLA_PMUOWNER_PL			GENMASK(1, 0)

/* LAUNCH operations */
#define CLA_LAUNCH_OP_RESET		0
#define CLA_LAUNCH_OP_CMD		1
#define CLA_LAUNCH_OP_CMDNR		2
#define CLA_LAUNCH_OP_ENTERSR		4
#define CLA_LAUNCH_OP_EXITSR		5
#define CLA_LAUNCH_OP_SAVE		6
#define CLA_LAUNCH_OP_RESTORE		7
#define CLA_LAUNCH_OP_RESOLVE		9
#define CLA_LAUNCH_OP_REGREAD		10
#define CLA_LAUNCH_OP_REGWRITE		11
#define CLA_LAUNCH_OP_SETCTX		12
#define CLA_LAUNCH_OP_GETCTX		13

/* Return codes */
#define CLA_LRESP_OK			0
#define CLA_LRESP_UNAVAIL		1
#define CLA_LRESP_BUSY			2
#define CLA_LRESP_ERROR			3

#define CLA_ERRCODE_CSINT		0
#define CLA_ERRCODE_CSOF		1
#define CLA_ERRCODE_NOTIDLE		2
#define CLA_ERRCODE_PERM		3
#define CLA_ERRCODE_NOACC		4
#define CLA_ERRCODE_INVAL		5
#define CLA_ERRCODE_RESET		6

/* Memory translation context */
#define CLA_MTC_REGIDX_PL1		0
#define CLA_MTC_REGIDX_PL2		64
#define CLA_MTC_PL_SIZE			64

/* Common register offsets */
#define CLA_MTC_PSTATE			0
#define CLA_MTC_TTBR0			1
#define CLA_MTC_TTBR1			2
#define CLA_MTC_TCR			3
#define CLA_MTC_SCTLR			4
#define CLA_MTC_MAIR			5
#define CLA_MTC_TCR2			8

/* EL2 specific register offsets */
#define CLA_MTC_HCR_EL2			80
#define CLA_MTC_VTTBR_EL2		81
#define CLA_MTC_VTCR_EL2		82

#define CLA_MTC_PSTATE_EL		GENMASK(1, 0)
#define CLA_MTC_PSTATE_PAN		BIT(2)

#define CLA_SRSTATE_0_SROP		BIT(0)
#define CLA_SRSTATE_0_REGSTATE		GENMASK(15, 1)
#define CLA_SRSTATE_1_STATUS		GENMASK(63, 0)
#define CLA_SRSTATE_2_SRACTIVE		GENMASK(1, 0)
#define CLA_SRSTATE_2_ADDR_MASK		GENMASK(63, 6)

#endif /* _ARM_CLA_REGS_H_ */
