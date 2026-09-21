// SPDX-License-Identifier: GPL-2.0+
// Cadence XSPI flash controller driver
// Copyright (C) 2020-21 Cadence

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mtd/spinand.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>
#include <linux/bitfield.h>
#include <linux/limits.h>
#include <linux/log2.h>
#include <linux/bitrev.h>
#include <linux/util_macros.h>

#define CDNS_XSPI_MAGIC_NUM_VALUE	0x6522
#define CDNS_XSPI_MAX_BANKS		8
#define CDNS_XSPI_NAME			"cadence-xspi"

/*
 * Note: below are additional auxiliary registers to
 * configure XSPI controller pin-strap settings
 */

/* PHY DQ timing register */
#define CDNS_XSPI_CCP_PHY_DQ_TIMING		0x0000

/* PHY DQS timing register */
#define CDNS_XSPI_CCP_PHY_DQS_TIMING		0x0004

/* PHY gate loopback control register */
#define CDNS_XSPI_CCP_PHY_GATE_LPBCK_CTRL	0x0008

/* PHY DLL slave control register */
#define CDNS_XSPI_CCP_PHY_DLL_SLAVE_CTRL	0x0010

/* DLL PHY control register */
#define CDNS_XSPI_DLL_PHY_CTRL			0x1034

/* Command registers */
#define CDNS_XSPI_CMD_REG_0			0x0000
#define CDNS_XSPI_CMD_REG_1			0x0004
#define CDNS_XSPI_CMD_REG_2			0x0008
#define CDNS_XSPI_CMD_REG_3			0x000C
#define CDNS_XSPI_CMD_REG_4			0x0010
#define CDNS_XSPI_CMD_REG_5			0x0014

/* Auto command fields in command register 0 */
#define CDNS_XSPI_ACMD_MODE			GENMASK(31, 30)
#define CDNS_XSPI_ACMD_TRD_NUM			GENMASK(26, 24)
#define CDNS_XSPI_ACMD_BANK_NUM			GENMASK(22, 20)
#define CDNS_XSPI_ACMD_DMA_SEL			BIT(19)
#define CDNS_XSPI_ACMD_INT_EN			BIT(18)
#define CDNS_XSPI_ACMD_CMD_TYPE			GENMASK(15, 0)

#define CDNS_XSPI_ACMD_READ_OP			0x2200
#define CDNS_XSPI_ACMD_PROG_OP			0x2100
#define CDNS_XSPI_ACMD_ERASE_OP			0x1000
#define CDNS_XSPI_ACMD_RESET_OP			0x1100
#define CDNS_XSPI_ACMD_DATA_THREAD		7
#define CDNS_XSPI_ACMD_ERASE_THREAD		5
#define CDNS_XSPI_ACMD_TIMEOUT_MS		1000
#define CDNS_XSPI_ACMD_MODE_PIO			1

#define CDNS_XSPI_NAND_OP_GET_FEATURE		0x0f
#define CDNS_XSPI_NAND_OP_WRITE_ENABLE		0x06
#define CDNS_XSPI_NAND_OP_PROGRAM_EXECUTE	0x10
#define CDNS_XSPI_NAND_OP_PAGE_READ		0x13
#define CDNS_XSPI_NAND_OP_BLOCK_ERASE		0xd8
#define CDNS_XSPI_NAND_OP_RESET			0xff
#define CDNS_XSPI_NAND_STATUS_REG		0xc0

/* Command status registers */
#define CDNS_XSPI_CMD_STATUS_PTR_REG	0x0040
#define CDNS_XSPI_CMD_STATUS_REG		0x0044

/* Controller status register */
#define CDNS_XSPI_CTRL_STATUS_REG		0x0100
#define CDNS_XSPI_INIT_COMPLETED		BIT(16)
#define CDNS_XSPI_INIT_LEGACY			BIT(9)
#define CDNS_XSPI_INIT_FAIL			BIT(8)
#define CDNS_XSPI_CTRL_BUSY			BIT(7)

/* Controller interrupt status register */
#define CDNS_XSPI_INTR_STATUS_REG		0x0110
#define CDNS_XSPI_STIG_DONE			BIT(23)
#define CDNS_XSPI_SDMA_ERROR			BIT(22)
#define CDNS_XSPI_SDMA_TRIGGER			BIT(21)
#define CDNS_XSPI_CMD_IGNRD_EN			BIT(20)
#define CDNS_XSPI_DDMA_TERR_EN			BIT(18)
#define CDNS_XSPI_CDMA_TREE_EN			BIT(17)
#define CDNS_XSPI_CTRL_IDLE_EN			BIT(16)

#define CDNS_XSPI_TRD_COMP_INTR_STATUS		0x0120
#define CDNS_XSPI_TRD_ERR_INTR_STATUS		0x0130
#define CDNS_XSPI_TRD_ERR_INTR_EN		0x0134

/* Controller interrupt enable register */
#define CDNS_XSPI_INTR_ENABLE_REG		0x0114
#define CDNS_XSPI_INTR_EN			BIT(31)
#define CDNS_XSPI_STIG_DONE_EN			BIT(23)
#define CDNS_XSPI_SDMA_ERROR_EN			BIT(22)
#define CDNS_XSPI_SDMA_TRIGGER_EN		BIT(21)

#define CDNS_XSPI_INTR_MASK (CDNS_XSPI_INTR_EN | \
	CDNS_XSPI_STIG_DONE_EN  | \
	CDNS_XSPI_SDMA_ERROR_EN | \
	CDNS_XSPI_SDMA_TRIGGER_EN)

/* Controller config register */
#define CDNS_XSPI_CTRL_CONFIG_REG		0x0230
#define CDNS_XSPI_CTRL_WORK_MODE		GENMASK(6, 5)

#define CDNS_XSPI_WORK_MODE_DIRECT		0
#define CDNS_XSPI_WORK_MODE_STIG		1
#define CDNS_XSPI_WORK_MODE_ACMD		3

/* SDMA trigger transaction registers */
#define CDNS_XSPI_SDMA_SIZE_REG			0x0240
#define CDNS_XSPI_SDMA_TRD_INFO_REG		0x0244
#define CDNS_XSPI_SDMA_DIR			BIT(8)

/* Controller features register */
#define CDNS_XSPI_CTRL_FEATURES_REG		0x0F04
#define CDNS_XSPI_NUM_BANKS			GENMASK(25, 24)
#define CDNS_XSPI_DMA_DATA_WIDTH		BIT(21)
#define CDNS_XSPI_NUM_THREADS			GENMASK(3, 0)

/* Controller version register */
#define CDNS_XSPI_CTRL_VERSION_REG		0x0F00
#define CDNS_XSPI_MAGIC_NUM			GENMASK(31, 16)
#define CDNS_XSPI_CTRL_REV			GENMASK(7, 0)

/* STIG Profile 1.0 instruction fields (split into registers) */
#define CDNS_XSPI_CMD_INSTR_TYPE		GENMASK(6, 0)
#define CDNS_XSPI_CMD_P1_R1_ADDR0		GENMASK(31, 24)
#define CDNS_XSPI_CMD_P1_R2_ADDR1		GENMASK(7, 0)
#define CDNS_XSPI_CMD_P1_R2_ADDR2		GENMASK(15, 8)
#define CDNS_XSPI_CMD_P1_R2_ADDR3		GENMASK(23, 16)
#define CDNS_XSPI_CMD_P1_R2_ADDR4		GENMASK(31, 24)
#define CDNS_XSPI_CMD_P1_R3_ADDR5		GENMASK(7, 0)
#define CDNS_XSPI_CMD_P1_R3_CMD			GENMASK(23, 16)
#define CDNS_XSPI_CMD_P1_R3_NUM_ADDR_BYTES	GENMASK(30, 28)
#define CDNS_XSPI_CMD_P1_R4_ADDR_IOS		GENMASK(1, 0)
#define CDNS_XSPI_CMD_P1_R4_CMD_IOS		GENMASK(9, 8)
#define CDNS_XSPI_CMD_P1_R4_BANK		GENMASK(14, 12)

/* STIG data sequence instruction fields (split into registers) */
#define CDNS_XSPI_CMD_DSEQ_R2_DCNT_L		GENMASK(31, 16)
#define CDNS_XSPI_CMD_DSEQ_R3_DCNT_H		GENMASK(15, 0)
#define CDNS_XSPI_CMD_DSEQ_R3_NUM_OF_DUMMY	GENMASK(25, 20)
#define CDNS_XSPI_CMD_DSEQ_R4_BANK		GENMASK(14, 12)
#define CDNS_XSPI_CMD_DSEQ_R4_DATA_IOS		GENMASK(9, 8)
#define CDNS_XSPI_CMD_DSEQ_R4_DIR		BIT(4)

/* STIG command status fields */
#define CDNS_XSPI_CMD_STATUS_COMPLETED		BIT(15)
#define CDNS_XSPI_CMD_STATUS_FAILED		BIT(14)
#define CDNS_XSPI_CMD_STATUS_DQS_ERROR		BIT(3)
#define CDNS_XSPI_CMD_STATUS_CRC_ERROR		BIT(2)
#define CDNS_XSPI_CMD_STATUS_BUS_ERROR		BIT(1)
#define CDNS_XSPI_CMD_STATUS_INV_SEQ_ERROR	BIT(0)
/* Reset sequence config register */
#define CDNS_XSPI_RST_SEQ_CFG_0			0x0400
#define CDNS_XSPI_RST_SEQ_P1_CMD1_VAL		GENMASK(15, 8)

/* Erase sequence config registers */
#define CDNS_XSPI_ERSS_SEQ_CFG_0			0x0410
#define CDNS_XSPI_ERSS_SEQ_P1_CMD_VAL		GENMASK(7, 0)
#define CDNS_XSPI_ERSS_SEQ_P1_CMD_IOS		GENMASK(9, 8)
#define CDNS_XSPI_ERSS_SEQ_P1_CMD_EDGE		BIT(11)
#define CDNS_XSPI_ERSS_SEQ_P1_ADDR_CNT		GENMASK(14, 12)
#define CDNS_XSPI_ERSS_SEQ_P1_CMD_EXT_EN	BIT(15)
#define CDNS_XSPI_ERSS_SEQ_P1_CMD_EXT_VAL	GENMASK(23, 16)
#define CDNS_XSPI_ERSS_SEQ_P1_ADDR_IOS		GENMASK(25, 24)
#define CDNS_XSPI_ERSS_SEQ_P1_ADDR_EDGE		BIT(28)

#define CDNS_XSPI_ERSS_SEQ_CFG_1			0x0414
#define CDNS_XSPI_ERSS_SEQ_P1_SECT_SIZE		GENMASK(4, 0)

#define CDNS_XSPI_ERSS_SEQ_CFG_2			0x0418

/* Program sequence config registers */
#define CDNS_XSPI_PROG_SEQ_CFG_0			0x0420
#define CDNS_XSPI_PROG_SEQ_P1_CMD_VAL		GENMASK(7, 0)
#define CDNS_XSPI_PROG_SEQ_P1_CMD_IOS		GENMASK(9, 8)
#define CDNS_XSPI_PROG_SEQ_P1_CMD_EDGE		BIT(11)
#define CDNS_XSPI_PROG_SEQ_P1_ADDR_CNT		GENMASK(14, 12)
#define CDNS_XSPI_PROG_SEQ_P1_ADDR_IOS		GENMASK(17, 16)
#define CDNS_XSPI_PROG_SEQ_P1_ADDR_EDGE		BIT(19)
#define CDNS_XSPI_PROG_SEQ_P1_DATA_IOS		GENMASK(21, 20)
#define CDNS_XSPI_PROG_SEQ_P1_DATA_EDGE		BIT(23)
#define CDNS_XSPI_PROG_SEQ_P1_DUMMY_CNT		GENMASK(29, 24)

#define CDNS_XSPI_PROG_SEQ_CFG_1			0x0424
#define CDNS_XSPI_PROG_SEQ_P1_CMD_EXT_EN		BIT(0)
#define CDNS_XSPI_PROG_SEQ_P1_CMD_EXT_VAL		GENMASK(15, 8)

#define CDNS_XSPI_PROG_SEQ_CFG_2			0x0428

/* Read sequence config registers */
#define CDNS_XSPI_READ_SEQ_CFG_0			0x0430
#define CDNS_XSPI_READ_SEQ_P1_CMD_VAL		GENMASK(7, 0)
#define CDNS_XSPI_READ_SEQ_P1_CMD_IOS		GENMASK(9, 8)
#define CDNS_XSPI_READ_SEQ_P1_CMD_EDGE		BIT(11)
#define CDNS_XSPI_READ_SEQ_P1_ADDR_CNT		GENMASK(14, 12)
#define CDNS_XSPI_READ_SEQ_P1_ADDR_IOS		GENMASK(17, 16)
#define CDNS_XSPI_READ_SEQ_P1_ADDR_EDGE		BIT(19)
#define CDNS_XSPI_READ_SEQ_P1_DATA_IOS		GENMASK(21, 20)
#define CDNS_XSPI_READ_SEQ_P1_DATA_EDGE		BIT(23)
#define CDNS_XSPI_READ_SEQ_P1_DUMMY_CNT		GENMASK(29, 24)

#define CDNS_XSPI_READ_SEQ_CFG_1			0x0434
#define CDNS_XSPI_READ_SEQ_P1_CMD_EXT_EN		BIT(0)
#define CDNS_XSPI_READ_SEQ_P1_CACHE_RANDOM_READ_EN	BIT(4)
#define CDNS_XSPI_READ_SEQ_CFG_2			0x0438

/* Write enable sequence config register */
#define CDNS_XSPI_WE_SEQ_CFG_0			0x0440
#define CDNS_XSPI_WE_SEQ_P1_CMD_VAL		GENMASK(7, 0)
#define CDNS_XSPI_WE_SEQ_P1_CMD_IOS		GENMASK(9, 8)
#define CDNS_XSPI_WE_SEQ_P1_CMD_EDGE		BIT(11)
#define CDNS_XSPI_WE_SEQ_P1_EN			BIT(24)

/* Status sequence config registers */
#define CDNS_XSPI_STAT_SEQ_CFG_0			0x0450
#define CDNS_XSPI_STAT_SEQ_P1_ADDR_CNT			GENMASK(9, 8)
#define CDNS_XSPI_STAT_SEQ_CFG_1			0x0454
#define CDNS_XSPI_P1_DEV_RDY_ADDR_EN		BIT(6)
#define CDNS_XSPI_P1_PROG_FAIL_ADDR_EN		BIT(22)
#define CDNS_XSPI_P1_ERS_FAIL_ADDR_EN		BIT(30)

#define CDNS_XSPI_STAT_SEQ_CFG_2			0x0458
#define CDNS_XSPI_STAT_SEQ_P1_DEV_RDY_CMD_VAL	GENMASK(7, 0)
#define CDNS_XSPI_STAT_SEQ_P1_ERS_FAIL_CMD_VAL	GENMASK(15, 8)
#define CDNS_XSPI_STAT_SEQ_P1_PROG_FAIL_CMD_VAL	GENMASK(31, 24)

#define CDNS_XSPI_STAT_SEQ_CFG_3			0x045c
#define CDNS_XSPI_STAT_SEQ_CFG_4			0x0460
#define CDNS_XSPI_STAT_SEQ_CFG_5			0x0464
#define CDNS_XSPI_STAT_SEQ_DEV_RDY_IDX		GENMASK(3, 0)
#define CDNS_XSPI_STAT_SEQ_DEV_RDY_EN		BIT(6)
#define CDNS_XSPI_STAT_SEQ_ERS_FAIL_IDX		GENMASK(11, 8)
#define CDNS_XSPI_STAT_SEQ_ERS_FAIL_VAL		BIT(12)
#define CDNS_XSPI_STAT_SEQ_ERS_FAIL_EN		BIT(14)
#define CDNS_XSPI_STAT_SEQ_PROG_FAIL_IDX		GENMASK(27, 24)
#define CDNS_XSPI_STAT_SEQ_PROG_FAIL_VAL		BIT(28)
#define CDNS_XSPI_STAT_SEQ_PROG_FAIL_EN			BIT(30)

#define CDNS_XSPI_STAT_SEQ_CFG_7			0x046c
#define CDNS_XSPI_STAT_SEQ_CFG_8			0x0470
#define CDNS_XSPI_STAT_SEQ_CFG_9			0x0474

#define CDNS_XSPI_STAT_SEQ_CFG_10		0x0478
#define CDNS_XSPI_STAT_SEQ_ECC_FAIL_EN		BIT(31)
#define CDNS_XSPI_STAT_SEQ_CRDY_VAL		BIT(27)
#define CDNS_XSPI_STAT_SEQ_CRDY_IDX		GENMASK(26, 24)
#define CDNS_XSPI_STAT_SEQ_ECC_CORR_VAL		GENMASK(23, 16)
#define CDNS_XSPI_STAT_SEQ_ECC_FAIL_VAL		GENMASK(15, 8)
#define CDNS_XSPI_STAT_SEQ_ECC_FAIL_MASK		GENMASK(7, 0)

#define CDNS_XSPI_STIG_DONE_FLAG		BIT(0)
#define CDNS_XSPI_TRD_STATUS			0x0104

#define CDNS_XSPI_FLASH_TYPE_NOR		0
#define CDNS_XSPI_FLASH_TYPE_NAND		1

#define CDNS_XSPI_GLOBAL_SEQ_CFG			0x0390
#define CDNS_XSPI_SEQ_TYPE			GENMASK(24, 23)
#define CDNS_XSPI_SEQ_PAGE_SIZE_PGM		GENMASK(7, 4)
#define CDNS_XSPI_SEQ_PAGE_SIZE_RD		GENMASK(3, 0)
#define CDNS_XSPI_SEQ_SPI_NAND			3

#define CDNS_XSPI_GLOBAL_SEQ_CFG_1		0x0394
#define CDNS_XSPI_SEQ_PLANE_CNT			GENMASK(29, 28)
#define CDNS_XSPI_SEQ_PAGE_PER_BLOCK		GENMASK(26, 24)
#define CDNS_XSPI_SEQ_PAGE_CA_SIZE		BIT(16)
#define CDNS_XSPI_SEQ_PAGE_SIZE_EXT		GENMASK(8, 0)

#define CDNS_XSPI_XIP_MODE_CFG			0x0388
#define CDNS_XSPI_XIP_EN			BIT(0)

#define MODE_NO_OF_BYTES			GENMASK(25, 24)
#define MODEBYTES_COUNT			1

/* Helper macros for filling command registers */
#define CDNS_XSPI_CMD_FLD_P1_INSTR_CMD_1(op, data_phase) ( \
	FIELD_PREP(CDNS_XSPI_CMD_INSTR_TYPE, (data_phase) ? \
		CDNS_XSPI_STIG_INSTR_TYPE_1 : CDNS_XSPI_STIG_INSTR_TYPE_0) | \
	FIELD_PREP(CDNS_XSPI_CMD_P1_R1_ADDR0, (op)->addr.val & 0xff))

#define CDNS_XSPI_CMD_FLD_P1_INSTR_CMD_2(op) ( \
	FIELD_PREP(CDNS_XSPI_CMD_P1_R2_ADDR1, ((op)->addr.val >> 8)  & 0xFF) | \
	FIELD_PREP(CDNS_XSPI_CMD_P1_R2_ADDR2, ((op)->addr.val >> 16) & 0xFF) | \
	FIELD_PREP(CDNS_XSPI_CMD_P1_R2_ADDR3, ((op)->addr.val >> 24) & 0xFF) | \
	FIELD_PREP(CDNS_XSPI_CMD_P1_R2_ADDR4, ((op)->addr.val >> 32) & 0xFF))

#define CDNS_XSPI_CMD_FLD_P1_INSTR_CMD_3(op, modebytes) ( \
	FIELD_PREP(CDNS_XSPI_CMD_P1_R3_ADDR5, ((op)->addr.val >> 40) & 0xFF) | \
	FIELD_PREP(CDNS_XSPI_CMD_P1_R3_CMD, (op)->cmd.opcode) | \
	FIELD_PREP(MODE_NO_OF_BYTES, modebytes) | \
	FIELD_PREP(CDNS_XSPI_CMD_P1_R3_NUM_ADDR_BYTES, (op)->addr.nbytes))

#define CDNS_XSPI_CMD_FLD_P1_INSTR_CMD_4(op, chipsel) ( \
	FIELD_PREP(CDNS_XSPI_CMD_P1_R4_ADDR_IOS, ilog2((op)->addr.buswidth)) | \
	FIELD_PREP(CDNS_XSPI_CMD_P1_R4_CMD_IOS, ilog2((op)->cmd.buswidth)) | \
	FIELD_PREP(CDNS_XSPI_CMD_P1_R4_BANK, chipsel))

#define CDNS_XSPI_CMD_FLD_DSEQ_CMD_1(op) \
	FIELD_PREP(CDNS_XSPI_CMD_INSTR_TYPE, CDNS_XSPI_STIG_INSTR_TYPE_DATA_SEQ)

#define CDNS_XSPI_CMD_FLD_DSEQ_CMD_2(op) \
	FIELD_PREP(CDNS_XSPI_CMD_DSEQ_R2_DCNT_L, (op)->data.nbytes & 0xFFFF)

#define CDNS_XSPI_CMD_FLD_DSEQ_CMD_3(op, dummybytes) ( \
	FIELD_PREP(CDNS_XSPI_CMD_DSEQ_R3_DCNT_H, \
		((op)->data.nbytes >> 16) & 0xffff) | \
	FIELD_PREP(CDNS_XSPI_CMD_DSEQ_R3_NUM_OF_DUMMY, \
		  (op)->dummy.buswidth != 0 ? \
		  (((dummybytes) * 8) / (op)->dummy.buswidth) : \
		  0))

#define CDNS_XSPI_CMD_FLD_DSEQ_CMD_4(op, chipsel) ( \
	FIELD_PREP(CDNS_XSPI_CMD_DSEQ_R4_BANK, chipsel) | \
	FIELD_PREP(CDNS_XSPI_CMD_DSEQ_R4_DATA_IOS, \
		ilog2((op)->data.buswidth)) | \
	FIELD_PREP(CDNS_XSPI_CMD_DSEQ_R4_DIR, \
		((op)->data.dir == SPI_MEM_DATA_IN) ? \
		CDNS_XSPI_STIG_CMD_DIR_READ : CDNS_XSPI_STIG_CMD_DIR_WRITE))

/* Helper macros for GENERIC and GENERIC-DSEQ instruction type */
#define CMD_REG_LEN (6*4)
#define INSTRUCTION_TYPE_GENERIC 96
#define CDNS_XSPI_CMD_FLD_P1_GENERIC_CMD (\
	FIELD_PREP(CDNS_XSPI_CMD_INSTR_TYPE, INSTRUCTION_TYPE_GENERIC))

#define GENERIC_NUM_OF_BYTES GENMASK(27, 24)
#define CDNS_XSPI_CMD_FLD_P3_GENERIC_CMD(len) (\
	FIELD_PREP(GENERIC_NUM_OF_BYTES, len))

#define GENERIC_BANK_NUM GENMASK(14, 12)
#define GENERIC_GLUE_CMD BIT(28)
#define CDNS_XSPI_CMD_FLD_P4_GENERIC_CMD(cs, glue) (\
	FIELD_PREP(GENERIC_BANK_NUM, cs) | FIELD_PREP(GENERIC_GLUE_CMD, glue))

#define CDNS_XSPI_CMD_FLD_GENERIC_DSEQ_CMD_1 (\
	FIELD_PREP(CDNS_XSPI_CMD_INSTR_TYPE, CDNS_XSPI_STIG_INSTR_TYPE_DATA_SEQ))

#define CDNS_XSPI_CMD_FLD_GENERIC_DSEQ_CMD_2(nbytes) (\
	FIELD_PREP(CDNS_XSPI_CMD_DSEQ_R2_DCNT_L, nbytes & 0xffff))

#define CDNS_XSPI_CMD_FLD_GENERIC_DSEQ_CMD_3(nbytes) ( \
	FIELD_PREP(CDNS_XSPI_CMD_DSEQ_R3_DCNT_H, (nbytes >> 16) & 0xffff))

#define CDNS_XSPI_CMD_FLD_GENERIC_DSEQ_CMD_4(dir, chipsel) ( \
	FIELD_PREP(CDNS_XSPI_CMD_DSEQ_R4_BANK, chipsel) | \
	FIELD_PREP(CDNS_XSPI_CMD_DSEQ_R4_DIR, dir))

/* Marvell PHY default values */
#define MARVELL_REGS_DLL_PHY_CTRL		0x00000707
#define MARVELL_CTB_RFILE_PHY_CTRL		0x00004000
#define MARVELL_RFILE_PHY_TSEL			0x00000000
#define MARVELL_RFILE_PHY_DQ_TIMING		0x00000101
#define MARVELL_RFILE_PHY_DQS_TIMING		0x00700404
#define MARVELL_RFILE_PHY_GATE_LPBK_CTRL	0x00200030
#define MARVELL_RFILE_PHY_DLL_MASTER_CTRL	0x00800000
#define MARVELL_RFILE_PHY_DLL_SLAVE_CTRL	0x0000ff01

/* PHY config registers */
#define CDNS_XSPI_RF_MINICTRL_REGS_DLL_PHY_CTRL			0x1034
#define CDNS_XSPI_PHY_CTB_RFILE_PHY_CTRL			0x0080
#define CDNS_XSPI_PHY_CTB_RFILE_PHY_TSEL			0x0084
#define CDNS_XSPI_PHY_DATASLICE_RFILE_PHY_DQ_TIMING		0x0000
#define CDNS_XSPI_PHY_DATASLICE_RFILE_PHY_DQS_TIMING		0x0004
#define CDNS_XSPI_PHY_DATASLICE_RFILE_PHY_GATE_LPBK_CTRL	0x0008
#define CDNS_XSPI_PHY_DATASLICE_RFILE_PHY_DLL_MASTER_CTRL	0x000c
#define CDNS_XSPI_PHY_DATASLICE_RFILE_PHY_DLL_SLAVE_CTRL	0x0010
#define CDNS_XSPI_DATASLICE_RFILE_PHY_DLL_OBS_REG_0		0x001c

#define CDNS_XSPI_DLL_RST_N BIT(24)
#define CDNS_XSPI_DLL_LOCK  BIT(0)

/* Marvell overlay registers - clock */
#define MRVL_XSPI_CLK_CTRL_AUX_REG   0x2020
#define MRVL_XSPI_CLK_ENABLE	     BIT(0)
#define MRVL_XSPI_CLK_DIV	     GENMASK(4, 1)
#define MRVL_XSPI_IRQ_ENABLE	     BIT(6)
#define MRVL_XSPI_CLOCK_IO_HZ	     800000000
#define MRVL_XSPI_CLOCK_DIVIDED(div) ((MRVL_XSPI_CLOCK_IO_HZ) / (div))
#define MRVL_DEFAULT_CLK	     25000000

/* Marvell overlay registers - xfer */
#define MRVL_XFER_FUNC_CTRL		 0x210
#define MRVL_XFER_FUNC_CTRL_READ_DATA(i) (0x000 + 8 * (i))
#define MRVL_XFER_SOFT_RESET		 BIT(11)
#define MRVL_XFER_CS_N_HOLD		 GENMASK(9, 6)
#define MRVL_XFER_RECEIVE_ENABLE	 BIT(4)
#define MRVL_XFER_FUNC_ENABLE		 BIT(3)
#define MRVL_XFER_CLK_CAPTURE_POL	 BIT(2)
#define MRVL_XFER_CLK_DRIVE_POL		 BIT(1)
#define MRVL_XFER_FUNC_START		 BIT(0)
#define MRVL_XFER_QWORD_COUNT		 32
#define MRVL_XFER_QWORD_BYTECOUNT	 8

#define MRVL_XSPI_POLL_TIMEOUT_US	1000
#define MRVL_XSPI_POLL_DELAY_US		10

/* Macros for calculating data bits in generic command
 * Up to 10 bytes can be fit into cmd_registers
 * least significant is placed in cmd_reg[1]
 * Other bits are inserted after it in cmd_reg[1,2,3] register
 */
#define GENERIC_CMD_DATA_REG_3_COUNT(len)	(len >= 10 ? 2 : len - 8)
#define GENERIC_CMD_DATA_REG_2_COUNT(len)	(len >= 7 ? 3 : len - 4)
#define GENERIC_CMD_DATA_REG_1_COUNT(len)	(len >= 3 ? 2 : len - 1)
#define GENERIC_CMD_DATA_3_OFFSET(position)	(8*(position))
#define GENERIC_CMD_DATA_2_OFFSET(position)	(8*(position))
#define GENERIC_CMD_DATA_1_OFFSET(position)	(8 + 8*(position))
#define GENERIC_CMD_DATA_INSERT(data, pos)	((data) << (pos))
#define GENERIC_CMD_REG_3_NEEDED(len)		(len > 7)
#define GENERIC_CMD_REG_2_NEEDED(len)		(len > 3)

enum cdns_xspi_stig_instr_type {
	CDNS_XSPI_STIG_INSTR_TYPE_0,
	CDNS_XSPI_STIG_INSTR_TYPE_1,
	CDNS_XSPI_STIG_INSTR_TYPE_DATA_SEQ = 127,
};

enum cdns_xspi_sdma_dir {
	CDNS_XSPI_SDMA_DIR_READ,
	CDNS_XSPI_SDMA_DIR_WRITE,
};

enum cdns_xspi_stig_cmd_dir {
	CDNS_XSPI_STIG_CMD_DIR_READ,
	CDNS_XSPI_STIG_CMD_DIR_WRITE,
};

struct cdns_xspi_driver_data {
	bool mrvl_hw_overlay;
	bool use_acmd;
	u32 dll_phy_ctrl;
	u32 ctb_rfile_phy_ctrl;
	u32 rfile_phy_tsel;
	u32 rfile_phy_dq_timing;
	u32 rfile_phy_dqs_timing;
	u32 rfile_phy_gate_lpbk_ctrl;
	u32 rfile_phy_dll_master_ctrl;
	u32 rfile_phy_dll_slave_ctrl;
	u8 flash_type;
};

static struct cdns_xspi_driver_data cdns_driver_data = {
	.mrvl_hw_overlay = false,
};

static struct cdns_xspi_driver_data cdns_nand_driver_data = {
	.mrvl_hw_overlay = false,
	.use_acmd = true,
	.flash_type = CDNS_XSPI_FLASH_TYPE_NAND,
};

struct cdns_xspi_acmd_info {
	u64 row_addr;
	u64 column_addr;
	size_t data_nbytes;
	bool row_addr_valid;
	bool initialized;
};
struct cdns_xspi_dev {
	struct platform_device *pdev;
	struct spi_controller *host;
	struct device *dev;

	void __iomem *iobase;
	void __iomem *auxbase;
	void __iomem *sdmabase;
	void __iomem *xferbase;

	int irq;
	int cur_cs;
	unsigned int sdmasize;

	struct completion cmd_complete;
	struct completion auto_cmd_complete;
	struct completion sdma_complete;
	bool sdma_error;

	void *in_buffer;
	const void *out_buffer;
	/* Slave DMA data width in bytes (4 or 8). */
	u8 dma_data_width;

	u8 hw_num_banks;

	const struct cdns_xspi_driver_data *driver_data;
	void (*sdma_handler)(struct cdns_xspi_dev *cdns_xspi);
	void (*set_interrupts_handler)(struct cdns_xspi_dev *cdns_xspi, bool enabled);

	bool xfer_in_progress;
	int current_xfer_qword;
	u32 work_mode;
	u8 flash_type;

	struct cdns_xspi_acmd_info acmd_info;
	void *dma_buf;
	dma_addr_t dma_addr;
	u32 dma_buf_len;
};

static int cdns_xspi_wait_for_controller_idle(struct cdns_xspi_dev *cdns_xspi)
{
	u32 ctrl_stat;

	return readl_relaxed_poll_timeout(cdns_xspi->iobase +
					  CDNS_XSPI_CTRL_STATUS_REG,
					  ctrl_stat,
					  ((ctrl_stat &
					    CDNS_XSPI_CTRL_BUSY) == 0),
					  100, 1000);
}

static void cdns_xspi_trigger_command(struct cdns_xspi_dev *cdns_xspi,
				      u32 cmd_regs[6])
{
	writel(cmd_regs[5], cdns_xspi->iobase + CDNS_XSPI_CMD_REG_5);
	writel(cmd_regs[4], cdns_xspi->iobase + CDNS_XSPI_CMD_REG_4);
	writel(cmd_regs[3], cdns_xspi->iobase + CDNS_XSPI_CMD_REG_3);
	writel(cmd_regs[2], cdns_xspi->iobase + CDNS_XSPI_CMD_REG_2);
	writel(cmd_regs[1], cdns_xspi->iobase + CDNS_XSPI_CMD_REG_1);
	writel(cmd_regs[0], cdns_xspi->iobase + CDNS_XSPI_CMD_REG_0);
}

static int cdns_xspi_check_command_status(struct cdns_xspi_dev *cdns_xspi)
{
	int ret = 0;
	u32 cmd_status = readl(cdns_xspi->iobase + CDNS_XSPI_CMD_STATUS_REG);

	if (cmd_status & CDNS_XSPI_CMD_STATUS_COMPLETED) {
		if ((cmd_status & CDNS_XSPI_CMD_STATUS_FAILED) != 0) {
			if (cmd_status & CDNS_XSPI_CMD_STATUS_DQS_ERROR) {
				dev_err(cdns_xspi->dev,
					"Incorrect DQS pulses detected\n");
				ret = -EPROTO;
			}
			if (cmd_status & CDNS_XSPI_CMD_STATUS_CRC_ERROR) {
				dev_err(cdns_xspi->dev,
					"CRC error received\n");
				ret = -EPROTO;
			}
			if (cmd_status & CDNS_XSPI_CMD_STATUS_BUS_ERROR) {
				dev_err(cdns_xspi->dev,
					"Error resp on system DMA interface\n");
				ret = -EPROTO;
			}
			if (cmd_status & CDNS_XSPI_CMD_STATUS_INV_SEQ_ERROR) {
				dev_err(cdns_xspi->dev,
					"Invalid command sequence detected\n");
				ret = -EPROTO;
			}
		}
	} else {
		dev_err(cdns_xspi->dev, "Fatal err - command not completed\n");
		ret = -EPROTO;
	}

	return ret;
}

static void cdns_xspi_set_interrupts(struct cdns_xspi_dev *cdns_xspi,
				     bool enabled)
{
	u32 intr_enable;

	intr_enable = readl(cdns_xspi->iobase + CDNS_XSPI_INTR_ENABLE_REG);
	if (enabled)
		intr_enable |= CDNS_XSPI_INTR_MASK;
	else
		intr_enable &= ~CDNS_XSPI_INTR_MASK;
	writel(intr_enable, cdns_xspi->iobase + CDNS_XSPI_INTR_ENABLE_REG);
}

static void cdns_xspi_nand_cfg_seq_init(struct cdns_xspi_dev *cdns_xspi,
					struct spinand_device *spinand)
{
	u32 seq_cfg, seq_cfg1;

	seq_cfg = readl(cdns_xspi->iobase + CDNS_XSPI_GLOBAL_SEQ_CFG);
	seq_cfg1 = readl(cdns_xspi->iobase + CDNS_XSPI_GLOBAL_SEQ_CFG_1);
	seq_cfg = u32_replace_bits(seq_cfg, CDNS_XSPI_SEQ_SPI_NAND,
				   CDNS_XSPI_SEQ_TYPE);
	seq_cfg = u32_replace_bits(seq_cfg,
				   ilog2(spinand->base.memorg.pagesize),
				   CDNS_XSPI_SEQ_PAGE_SIZE_PGM);
	seq_cfg = u32_replace_bits(seq_cfg,
				   ilog2(spinand->base.memorg.pagesize),
				   CDNS_XSPI_SEQ_PAGE_SIZE_RD);
	seq_cfg1 = u32_replace_bits(seq_cfg1,
				    ilog2(spinand->base.memorg.planes_per_lun),
				    CDNS_XSPI_SEQ_PLANE_CNT);
	seq_cfg1 = u32_replace_bits(seq_cfg1,
				    ilog2(spinand->base.memorg.pages_per_eraseblock),
				    CDNS_XSPI_SEQ_PAGE_PER_BLOCK);
	seq_cfg1 = u32_replace_bits(seq_cfg1,
				    !!(spinand->base.memorg.pagesize & BIT(12)),
				    CDNS_XSPI_SEQ_PAGE_CA_SIZE);
	seq_cfg1 = u32_replace_bits(seq_cfg1, spinand->base.memorg.oobsize,
				    CDNS_XSPI_SEQ_PAGE_SIZE_EXT);

	writel(seq_cfg, cdns_xspi->iobase + CDNS_XSPI_GLOBAL_SEQ_CFG);
	writel(seq_cfg1, cdns_xspi->iobase + CDNS_XSPI_GLOBAL_SEQ_CFG_1);
}

static void cdns_xspi_nand_read_seq_init(struct cdns_xspi_dev *cdns_xspi,
					 struct spinand_device *spinand)
{
	const struct spi_mem_op *op = spinand->op_templates->read_cache;
	u32 dummy_cycles = 0;
	u32 read_seq_cfg0;
	u32 read_seq_cfg1;

	if (op->dummy.nbytes && op->dummy.buswidth)
		dummy_cycles = op->dummy.nbytes * BITS_PER_BYTE /
			       (op->dummy.buswidth * (op->dummy.dtr + 1));

	read_seq_cfg0 =
		FIELD_PREP(CDNS_XSPI_READ_SEQ_P1_CMD_VAL, op->cmd.opcode) |
		FIELD_PREP(CDNS_XSPI_READ_SEQ_P1_CMD_IOS,
			   ilog2(op->cmd.buswidth)) |
		FIELD_PREP(CDNS_XSPI_READ_SEQ_P1_CMD_EDGE, op->cmd.dtr) |
		FIELD_PREP(CDNS_XSPI_READ_SEQ_P1_ADDR_CNT,
			   op->addr.nbytes) |
		FIELD_PREP(CDNS_XSPI_READ_SEQ_P1_ADDR_IOS,
			   ilog2(op->addr.buswidth)) |
		FIELD_PREP(CDNS_XSPI_READ_SEQ_P1_ADDR_EDGE, op->addr.dtr) |
		FIELD_PREP(CDNS_XSPI_READ_SEQ_P1_DATA_IOS,
			   ilog2(op->data.buswidth)) |
		FIELD_PREP(CDNS_XSPI_READ_SEQ_P1_DATA_EDGE, op->data.dtr) |
		FIELD_PREP(CDNS_XSPI_READ_SEQ_P1_DUMMY_CNT, dummy_cycles);
	read_seq_cfg1 = FIELD_PREP(CDNS_XSPI_READ_SEQ_P1_CMD_EXT_EN,
				   op->cmd.nbytes > 1) |
		CDNS_XSPI_READ_SEQ_P1_CACHE_RANDOM_READ_EN;

	writel(read_seq_cfg0, cdns_xspi->iobase + CDNS_XSPI_READ_SEQ_CFG_0);
	writel(read_seq_cfg1, cdns_xspi->iobase + CDNS_XSPI_READ_SEQ_CFG_1);
	writel(0, cdns_xspi->iobase + CDNS_XSPI_READ_SEQ_CFG_2);

	dev_dbg(cdns_xspi->dev,
		"ACMD read: op=%02x %u-%u-%u%s addr=%u dummy=%u cfg=%08x/%08x\n",
		op->cmd.opcode, op->cmd.buswidth, op->addr.buswidth,
		op->data.buswidth, op->data.dtr ? " DTR" : " SDR",
		op->addr.nbytes, dummy_cycles, read_seq_cfg0, read_seq_cfg1);
}

static void cdns_xspi_nand_write_seq_init(struct cdns_xspi_dev *cdns_xspi,
					  struct spinand_device *spinand)
{
	const struct spi_mem_op *op = spinand->op_templates->write_cache;
	u32 dummy_cycles = 0;
	u32 write_seq_cfg0;
	u32 write_seq_cfg1;

	if (op->dummy.nbytes && op->dummy.buswidth)
		dummy_cycles = op->dummy.nbytes * BITS_PER_BYTE /
			       (op->dummy.buswidth * (op->dummy.dtr + 1));

	write_seq_cfg0 =
		FIELD_PREP(CDNS_XSPI_PROG_SEQ_P1_CMD_VAL, op->cmd.opcode) |
		FIELD_PREP(CDNS_XSPI_PROG_SEQ_P1_CMD_IOS,
			   ilog2(op->cmd.buswidth)) |
		FIELD_PREP(CDNS_XSPI_PROG_SEQ_P1_CMD_EDGE, op->cmd.dtr) |
		FIELD_PREP(CDNS_XSPI_PROG_SEQ_P1_ADDR_CNT,
			   op->addr.nbytes) |
		FIELD_PREP(CDNS_XSPI_PROG_SEQ_P1_ADDR_IOS,
			   ilog2(op->addr.buswidth)) |
		FIELD_PREP(CDNS_XSPI_PROG_SEQ_P1_ADDR_EDGE, op->addr.dtr) |
		FIELD_PREP(CDNS_XSPI_PROG_SEQ_P1_DATA_IOS,
			   ilog2(op->data.buswidth)) |
		FIELD_PREP(CDNS_XSPI_PROG_SEQ_P1_DATA_EDGE, op->data.dtr) |
		FIELD_PREP(CDNS_XSPI_PROG_SEQ_P1_DUMMY_CNT, dummy_cycles);
	write_seq_cfg1 = FIELD_PREP(CDNS_XSPI_PROG_SEQ_P1_CMD_EXT_EN,
				    op->cmd.nbytes > 1);

	writel(write_seq_cfg0, cdns_xspi->iobase + CDNS_XSPI_PROG_SEQ_CFG_0);
	writel(write_seq_cfg1, cdns_xspi->iobase + CDNS_XSPI_PROG_SEQ_CFG_1);
	writel(0, cdns_xspi->iobase + CDNS_XSPI_PROG_SEQ_CFG_2);

	dev_dbg(cdns_xspi->dev,
		"ACMD program: op=%02x %u-%u-%u%s addr=%u dummy=%u cfg=%08x/%08x\n",
		op->cmd.opcode, op->cmd.buswidth, op->addr.buswidth,
		op->data.buswidth, op->data.dtr ? " DTR" : " SDR",
		op->addr.nbytes, dummy_cycles, write_seq_cfg0, write_seq_cfg1);
}

static void cdns_xspi_nand_erase_seq_init(struct cdns_xspi_dev *cdns_xspi,
					  struct spinand_device *spinand)
{
	u32 erase_seq_cfg0;
	u32 erase_seq_cfg1;

	/* SPI-NAND block erase is always D8h with a 3-byte row address. */
	erase_seq_cfg0 =
		FIELD_PREP(CDNS_XSPI_ERSS_SEQ_P1_CMD_VAL, 0xd8) |
		FIELD_PREP(CDNS_XSPI_ERSS_SEQ_P1_CMD_IOS, 0) |
		FIELD_PREP(CDNS_XSPI_ERSS_SEQ_P1_CMD_EDGE, 0) |
		FIELD_PREP(CDNS_XSPI_ERSS_SEQ_P1_ADDR_CNT, 3) |
		FIELD_PREP(CDNS_XSPI_ERSS_SEQ_P1_CMD_EXT_EN, 0) |
		FIELD_PREP(CDNS_XSPI_ERSS_SEQ_P1_CMD_EXT_VAL, 0) |
		FIELD_PREP(CDNS_XSPI_ERSS_SEQ_P1_ADDR_IOS, 0) |
		FIELD_PREP(CDNS_XSPI_ERSS_SEQ_P1_ADDR_EDGE, 0);
	erase_seq_cfg1 = FIELD_PREP(CDNS_XSPI_ERSS_SEQ_P1_SECT_SIZE,
				    ilog2(nanddev_eraseblock_size(&spinand->base)));

	writel(erase_seq_cfg0,
	       cdns_xspi->iobase + CDNS_XSPI_ERSS_SEQ_CFG_0);
	writel(erase_seq_cfg1,
	       cdns_xspi->iobase + CDNS_XSPI_ERSS_SEQ_CFG_1);
	writel(0, cdns_xspi->iobase + CDNS_XSPI_ERSS_SEQ_CFG_2);

	dev_dbg(cdns_xspi->dev,
		"ACMD erase sequence: cfg0=%08x cfg1=%08x\n",
		erase_seq_cfg0, erase_seq_cfg1);
}

static void cdns_xspi_nand_status_seq_init(struct cdns_xspi_dev *cdns_xspi)
{
	u32 stat_seq_cfg1;
	u32 stat_seq_cfg2;
	u32 stat_seq_cfg5;

	writel(FIELD_PREP(CDNS_XSPI_STAT_SEQ_P1_ADDR_CNT, 0),
	       cdns_xspi->iobase + CDNS_XSPI_STAT_SEQ_CFG_0);

	stat_seq_cfg1 = CDNS_XSPI_P1_DEV_RDY_ADDR_EN |
			CDNS_XSPI_P1_PROG_FAIL_ADDR_EN |
			CDNS_XSPI_P1_ERS_FAIL_ADDR_EN;
	writel(stat_seq_cfg1, cdns_xspi->iobase + CDNS_XSPI_STAT_SEQ_CFG_1);

	stat_seq_cfg2 = FIELD_PREP(CDNS_XSPI_STAT_SEQ_P1_DEV_RDY_CMD_VAL,
				   CDNS_XSPI_NAND_OP_GET_FEATURE) |
		FIELD_PREP(CDNS_XSPI_STAT_SEQ_P1_PROG_FAIL_CMD_VAL,
			   CDNS_XSPI_NAND_OP_GET_FEATURE) |
		FIELD_PREP(CDNS_XSPI_STAT_SEQ_P1_ERS_FAIL_CMD_VAL,
			   CDNS_XSPI_NAND_OP_GET_FEATURE);
	writel(stat_seq_cfg2, cdns_xspi->iobase + CDNS_XSPI_STAT_SEQ_CFG_2);
	writel(0, cdns_xspi->iobase + CDNS_XSPI_STAT_SEQ_CFG_3);
	writel(0, cdns_xspi->iobase + CDNS_XSPI_STAT_SEQ_CFG_4);

	stat_seq_cfg5 =
		FIELD_PREP(CDNS_XSPI_STAT_SEQ_DEV_RDY_IDX,
			   __ffs(STATUS_BUSY)) |
		CDNS_XSPI_STAT_SEQ_DEV_RDY_EN |
		FIELD_PREP(CDNS_XSPI_STAT_SEQ_ERS_FAIL_IDX,
			   __ffs(STATUS_ERASE_FAILED)) |
		CDNS_XSPI_STAT_SEQ_ERS_FAIL_VAL |
		CDNS_XSPI_STAT_SEQ_ERS_FAIL_EN |
		FIELD_PREP(CDNS_XSPI_STAT_SEQ_PROG_FAIL_IDX,
			   __ffs(STATUS_PROG_FAILED)) |
		CDNS_XSPI_STAT_SEQ_PROG_FAIL_VAL |
		CDNS_XSPI_STAT_SEQ_PROG_FAIL_EN;
	writel(stat_seq_cfg5, cdns_xspi->iobase + CDNS_XSPI_STAT_SEQ_CFG_5);
	writel(CDNS_XSPI_NAND_STATUS_REG,
	       cdns_xspi->iobase + CDNS_XSPI_STAT_SEQ_CFG_7);
	writel(CDNS_XSPI_NAND_STATUS_REG,
	       cdns_xspi->iobase + CDNS_XSPI_STAT_SEQ_CFG_8);
	writel(CDNS_XSPI_NAND_STATUS_REG,
	       cdns_xspi->iobase + CDNS_XSPI_STAT_SEQ_CFG_9);

	writel(CDNS_XSPI_STAT_SEQ_ECC_FAIL_EN |
	       FIELD_PREP(CDNS_XSPI_STAT_SEQ_CRDY_IDX, __ffs(STATUS_BUSY)) |
	       FIELD_PREP(CDNS_XSPI_STAT_SEQ_CRDY_VAL, 0) |
	       FIELD_PREP(CDNS_XSPI_STAT_SEQ_ECC_CORR_VAL,
			  STATUS_ECC_HAS_BITFLIPS) |
	       FIELD_PREP(CDNS_XSPI_STAT_SEQ_ECC_FAIL_VAL,
			  STATUS_ECC_UNCOR_ERROR) |
	       FIELD_PREP(CDNS_XSPI_STAT_SEQ_ECC_FAIL_MASK,
			  STATUS_ECC_MASK),
	       cdns_xspi->iobase + CDNS_XSPI_STAT_SEQ_CFG_10);
}

static void cdns_xspi_nand_write_enable_seq_init(struct cdns_xspi_dev *cdns_xspi)
{
	u32 cfg;

	cfg = readl(cdns_xspi->iobase + CDNS_XSPI_WE_SEQ_CFG_0);
	cfg = u32_replace_bits(cfg, 1, CDNS_XSPI_WE_SEQ_P1_EN);
	cfg = u32_replace_bits(cfg, CDNS_XSPI_NAND_OP_WRITE_ENABLE,
			       CDNS_XSPI_WE_SEQ_P1_CMD_VAL);
	cfg = u32_replace_bits(cfg, 0, CDNS_XSPI_WE_SEQ_P1_CMD_IOS);
	cfg = u32_replace_bits(cfg, 0, CDNS_XSPI_WE_SEQ_P1_CMD_EDGE);
	writel(cfg, cdns_xspi->iobase + CDNS_XSPI_WE_SEQ_CFG_0);
}

static void cdns_xspi_set_mode_acmd(struct cdns_xspi_dev *cdns_xspi)
{
	u32 reg_val;

	reg_val = readl(cdns_xspi->iobase + CDNS_XSPI_CTRL_CONFIG_REG);
	reg_val = u32_replace_bits(reg_val, CDNS_XSPI_WORK_MODE_ACMD,
				   CDNS_XSPI_CTRL_WORK_MODE);
	writel(reg_val, cdns_xspi->iobase + CDNS_XSPI_CTRL_CONFIG_REG);
}

static void cdns_xspi_nand_reset_seq_init(struct cdns_xspi_dev *cdns_xspi)
{
	u32 cfg;

	cfg = FIELD_PREP(CDNS_XSPI_RST_SEQ_P1_CMD1_VAL,
			 CDNS_XSPI_NAND_OP_RESET);
	writel(cfg, cdns_xspi->iobase + CDNS_XSPI_RST_SEQ_CFG_0);
}

static int cdns_xspi_nand_init(struct cdns_xspi_dev *cdns_xspi,
			       struct spinand_device *spinand)
{
	u32 reg_val;

	cdns_xspi_nand_cfg_seq_init(cdns_xspi, spinand);
	cdns_xspi_nand_read_seq_init(cdns_xspi, spinand);
	cdns_xspi_nand_reset_seq_init(cdns_xspi);
	cdns_xspi_nand_write_seq_init(cdns_xspi, spinand);
	cdns_xspi_nand_write_enable_seq_init(cdns_xspi);
	cdns_xspi_nand_status_seq_init(cdns_xspi);
	cdns_xspi_nand_erase_seq_init(cdns_xspi, spinand);

	reg_val = readl(cdns_xspi->iobase + CDNS_XSPI_XIP_MODE_CFG);
	if (reg_val & CDNS_XSPI_XIP_EN) {
		reg_val &= ~CDNS_XSPI_XIP_EN;
		writel(reg_val, cdns_xspi->iobase + CDNS_XSPI_XIP_MODE_CFG);
	}
	cdns_xspi->dma_buf_len = spinand->base.memorg.pagesize +
				 spinand->base.memorg.oobsize;
	cdns_xspi->dma_buf = dmam_alloc_coherent(cdns_xspi->dev,
						 cdns_xspi->dma_buf_len,
						 &cdns_xspi->dma_addr,
						 GFP_KERNEL);
	if (!cdns_xspi->dma_buf)
		return -ENOMEM;

	cdns_xspi->acmd_info.initialized = true;

	return 0;
}

static int cdns_xspi_controller_init(struct cdns_xspi_dev *cdns_xspi)
{
	u32 ctrl_ver;
	u32 ctrl_features;
	u16 hw_magic_num;

	ctrl_ver = readl(cdns_xspi->iobase + CDNS_XSPI_CTRL_VERSION_REG);
	hw_magic_num = FIELD_GET(CDNS_XSPI_MAGIC_NUM, ctrl_ver);
	if (hw_magic_num != CDNS_XSPI_MAGIC_NUM_VALUE) {
		dev_err(cdns_xspi->dev,
			"Incorrect XSPI magic number: %x, expected: %x\n",
			hw_magic_num, CDNS_XSPI_MAGIC_NUM_VALUE);
		return -EIO;
	}

	ctrl_features = readl(cdns_xspi->iobase + CDNS_XSPI_CTRL_FEATURES_REG);
	cdns_xspi->hw_num_banks = FIELD_GET(CDNS_XSPI_NUM_BANKS, ctrl_features);
	cdns_xspi->dma_data_width = (ctrl_features & CDNS_XSPI_DMA_DATA_WIDTH) ? 8 : 4;
	cdns_xspi->set_interrupts_handler(cdns_xspi, false);

	return 0;
}

static inline void cdns_xspi_sdma_read(struct cdns_xspi_dev *cdns_xspi, size_t len)
{
	void __iomem *src = cdns_xspi->sdmabase;
	void *buf = cdns_xspi->in_buffer;
	size_t offset = 0;

	if (!IS_ENABLED(CONFIG_64BIT) || cdns_xspi->dma_data_width == 4) {
		if (IS_ALIGNED((uintptr_t)src, 4) && IS_ALIGNED((uintptr_t)buf, 4)) {
			ioread32_rep(src, buf, len >> 2);
			offset = len & ~0x3;
			len -= offset;
		}
#ifdef CONFIG_64BIT
	} else {
		if (IS_ALIGNED((uintptr_t)src, 8) && IS_ALIGNED((uintptr_t)buf, 8)) {
			readsq(src, buf, len >> 3);
			offset = len & ~0x7;
			len -= offset;
		}
#endif
	}
	ioread8_rep(src, (u8 *)buf + offset, len);
}

static inline void cdns_xspi_sdma_write(struct cdns_xspi_dev *cdns_xspi, size_t len)
{
	void __iomem *dst = cdns_xspi->sdmabase;
	const void *buf = cdns_xspi->out_buffer;
	size_t offset = 0;

	if (!IS_ENABLED(CONFIG_64BIT) || cdns_xspi->dma_data_width == 4) {
		if (IS_ALIGNED((uintptr_t)dst, 4) && IS_ALIGNED((uintptr_t)buf, 4)) {
			iowrite32_rep(dst, buf, len >> 2);
			offset = len & ~0x3;
			len -= offset;
		}
#ifdef CONFIG_64BIT
	} else {
		if (IS_ALIGNED((uintptr_t)dst, 8) && IS_ALIGNED((uintptr_t)buf, 8)) {
			writesq(dst, buf, len >> 3);
			offset = len & ~0x7;
			len -= offset;
		}
#endif
	}
	iowrite8_rep(dst, (const u8 *)buf + offset, len);
}

static void cdns_xspi_sdma_handle(struct cdns_xspi_dev *cdns_xspi)
{
	u32 sdma_size, sdma_trd_info;
	u8 sdma_dir;

	sdma_size = readl(cdns_xspi->iobase + CDNS_XSPI_SDMA_SIZE_REG);
	sdma_trd_info = readl(cdns_xspi->iobase + CDNS_XSPI_SDMA_TRD_INFO_REG);
	sdma_dir = FIELD_GET(CDNS_XSPI_SDMA_DIR, sdma_trd_info);

	switch (sdma_dir) {
	case CDNS_XSPI_SDMA_DIR_READ:
		cdns_xspi_sdma_read(cdns_xspi, sdma_size);
		break;

	case CDNS_XSPI_SDMA_DIR_WRITE:
		cdns_xspi_sdma_write(cdns_xspi, sdma_size);
		break;
	}
}

static int cdns_xspi_send_stig_command(struct cdns_xspi_dev *cdns_xspi,
				       const struct spi_mem_op *op,
				       bool data_phase)
{
	u32 cmd_regs[6];
	u32 cmd_status;
	int ret;
	int dummybytes = op->dummy.nbytes;

	ret = cdns_xspi_wait_for_controller_idle(cdns_xspi);
	if (ret < 0)
		return -EIO;

	writel(FIELD_PREP(CDNS_XSPI_CTRL_WORK_MODE, CDNS_XSPI_WORK_MODE_STIG),
	       cdns_xspi->iobase + CDNS_XSPI_CTRL_CONFIG_REG);

	cdns_xspi->set_interrupts_handler(cdns_xspi, true);
	cdns_xspi->sdma_error = false;

	memset(cmd_regs, 0, sizeof(cmd_regs));
	cmd_regs[1] = CDNS_XSPI_CMD_FLD_P1_INSTR_CMD_1(op, data_phase);
	cmd_regs[2] = CDNS_XSPI_CMD_FLD_P1_INSTR_CMD_2(op);
	if (dummybytes != 0) {
		cmd_regs[3] = CDNS_XSPI_CMD_FLD_P1_INSTR_CMD_3(op, 1);
		dummybytes--;
	} else {
		cmd_regs[3] = CDNS_XSPI_CMD_FLD_P1_INSTR_CMD_3(op, 0);
	}
	cmd_regs[4] = CDNS_XSPI_CMD_FLD_P1_INSTR_CMD_4(op,
						       cdns_xspi->cur_cs);

	cdns_xspi_trigger_command(cdns_xspi, cmd_regs);

	if (data_phase) {
		cmd_regs[0] = CDNS_XSPI_STIG_DONE_FLAG;
		cmd_regs[1] = CDNS_XSPI_CMD_FLD_DSEQ_CMD_1(op);
		cmd_regs[2] = CDNS_XSPI_CMD_FLD_DSEQ_CMD_2(op);
		cmd_regs[3] = CDNS_XSPI_CMD_FLD_DSEQ_CMD_3(op, dummybytes);
		cmd_regs[4] = CDNS_XSPI_CMD_FLD_DSEQ_CMD_4(op,
							   cdns_xspi->cur_cs);

		cdns_xspi->in_buffer = op->data.buf.in;
		cdns_xspi->out_buffer = op->data.buf.out;

		cdns_xspi_trigger_command(cdns_xspi, cmd_regs);

		wait_for_completion(&cdns_xspi->sdma_complete);
		if (cdns_xspi->sdma_error) {
			cdns_xspi->set_interrupts_handler(cdns_xspi, false);
			return -EIO;
		}
		cdns_xspi->sdma_handler(cdns_xspi);
	}

	wait_for_completion(&cdns_xspi->cmd_complete);
	cdns_xspi->set_interrupts_handler(cdns_xspi, false);

	cmd_status = cdns_xspi_check_command_status(cdns_xspi);
	if (cmd_status)
		return -EPROTO;

	return 0;
}

static int cdns_xspi_acmd_get_thread_status(struct cdns_xspi_dev *cdns_xspi,
					    u32 thread)
{
	writel(thread, cdns_xspi->iobase + CDNS_XSPI_CMD_STATUS_PTR_REG);

	return cdns_xspi_check_command_status(cdns_xspi);
}

static int cdns_xspi_acmd_run(struct cdns_xspi_dev *cdns_xspi, u32 cmd_regs[6],
			      u32 thread)
{
	unsigned long timeout;
	int ret;

	cdns_xspi_set_mode_acmd(cdns_xspi);
	reinit_completion(&cdns_xspi->auto_cmd_complete);
	cdns_xspi_set_interrupts(cdns_xspi, true);
	cdns_xspi_trigger_command(cdns_xspi, cmd_regs);

	timeout = msecs_to_jiffies(CDNS_XSPI_ACMD_TIMEOUT_MS);
	if (!wait_for_completion_timeout(&cdns_xspi->auto_cmd_complete,
					 timeout)) {
		dev_err(cdns_xspi->dev, "ACMD command timed out\n");
		ret = -ETIMEDOUT;
	} else {
		ret = cdns_xspi_acmd_get_thread_status(cdns_xspi, thread);
	}

	cdns_xspi_set_interrupts(cdns_xspi, false);

	return ret;
}

static int cdns_xspi_nand_addr(struct spinand_device *spinand, u64 row,
			       u64 column, u64 *xspi_addr)
{
	const struct nand_memory_organization *memorg = &spinand->base.memorg;
	u64 column_limit, page_stride;

	page_stride = roundup_pow_of_two(memorg->pagesize + memorg->oobsize);
	if (check_mul_overflow(page_stride, memorg->planes_per_lun,
			       &column_limit) ||
	    column >= column_limit ||
	    check_mul_overflow(row, page_stride, xspi_addr) ||
	    check_add_overflow(*xspi_addr, column, xspi_addr))
		return -ERANGE;

	return 0;
}

static u32 cdns_xspi_acmd_cmd(struct cdns_xspi_dev *cdns_xspi, u32 type,
			      u32 thread, bool use_dma)
{
	return FIELD_PREP(CDNS_XSPI_ACMD_MODE, CDNS_XSPI_ACMD_MODE_PIO) |
	       FIELD_PREP(CDNS_XSPI_ACMD_TRD_NUM, thread) |
	       FIELD_PREP(CDNS_XSPI_ACMD_BANK_NUM, cdns_xspi->cur_cs) |
	       (use_dma ? CDNS_XSPI_ACMD_DMA_SEL : 0) |
	       CDNS_XSPI_ACMD_INT_EN |
	       FIELD_PREP(CDNS_XSPI_ACMD_CMD_TYPE, type);
}

static int cdns_xspi_pio_mdma_erase(struct cdns_xspi_dev *cdns_xspi,
				    struct spinand_device *spinand,
				    const struct spi_mem_op *op)
{
	u32 cmd_regs[6] = { 0 };
	u64 xspi_addr;
	int ret;

	ret = cdns_xspi_wait_for_controller_idle(cdns_xspi);
	if (ret) {
		dev_err(cdns_xspi->dev,
			"controller did not become idle before erase\n");
		return ret;
	}

	ret = cdns_xspi_nand_addr(spinand, op->addr.val, 0, &xspi_addr);
	if (ret)
		return ret;

	if (upper_32_bits(xspi_addr)) {
		dev_err(cdns_xspi->dev,
			"erase address 0x%llx exceeds the ACMD PIO range\n",
			xspi_addr);
		return -ERANGE;
	}

	cmd_regs[1] = lower_32_bits(xspi_addr);
	cmd_regs[0] = cdns_xspi_acmd_cmd(cdns_xspi,
					 CDNS_XSPI_ACMD_ERASE_OP,
					 CDNS_XSPI_ACMD_ERASE_THREAD, false);

	ret = cdns_xspi_acmd_run(cdns_xspi, cmd_regs,
				 CDNS_XSPI_ACMD_ERASE_THREAD);
	if (ret)
		dev_err(cdns_xspi->dev, "ACMD erase failed: %d\n", ret);

	return ret;
}

static int cdns_xspi_pio_reset(struct cdns_xspi_dev *cdns_xspi)
{
	u32 cmd_regs[6] = { 0 };
	int ret;

	ret = cdns_xspi_wait_for_controller_idle(cdns_xspi);
	if (ret) {
		dev_err(cdns_xspi->dev,
			"controller did not become idle before reset\n");
		return ret;
	}
	cmd_regs[0] = cdns_xspi_acmd_cmd(cdns_xspi,
					 CDNS_XSPI_ACMD_RESET_OP,
					 CDNS_XSPI_ACMD_DATA_THREAD, false);
	ret = cdns_xspi_acmd_run(cdns_xspi, cmd_regs,
				 CDNS_XSPI_ACMD_DATA_THREAD);
	if (ret)
		return ret;

	cdns_xspi->acmd_info.row_addr_valid = false;
	cdns_xspi->acmd_info.column_addr = 0;
	cdns_xspi->acmd_info.data_nbytes = 0;
	cdns_xspi->out_buffer = NULL;

	return 0;
}

static int cdns_xspi_pio_mdma_program(struct cdns_xspi_dev *cdns_xspi,
				      struct spinand_device *spinand,
				      const struct spi_mem_op *op)
{
	u32 cmd_regs[6] = { 0 };
	u64 xspi_addr;
	int ret;

	if (!cdns_xspi->out_buffer || !cdns_xspi->acmd_info.data_nbytes) {
		dev_err(cdns_xspi->dev, "missing program-load data\n");
		ret = -EINVAL;
		goto out_clear_program_state;
	}

	if (cdns_xspi->acmd_info.data_nbytes > cdns_xspi->dma_buf_len) {
		ret = -EMSGSIZE;
		goto out_clear_program_state;
	}

	memcpy(cdns_xspi->dma_buf, cdns_xspi->out_buffer,
	       cdns_xspi->acmd_info.data_nbytes);

	ret = cdns_xspi_wait_for_controller_idle(cdns_xspi);
	if (ret) {
		dev_err(cdns_xspi->dev,
			"controller did not become idle before program\n");
		goto out_clear_program_state;
	}

	ret = cdns_xspi_nand_addr(spinand, op->addr.val,
				  cdns_xspi->acmd_info.column_addr,
				  &xspi_addr);
	if (ret)
		goto out_clear_program_state;

	if (upper_32_bits(xspi_addr)) {
		dev_err(cdns_xspi->dev,
			"program address 0x%llx exceeds the ACMD PIO range\n",
			xspi_addr);
		ret = -ERANGE;
		goto out_clear_program_state;
	}

	cmd_regs[1] = lower_32_bits(xspi_addr);
	cmd_regs[2] = lower_32_bits(cdns_xspi->dma_addr);
	cmd_regs[3] = upper_32_bits(cdns_xspi->dma_addr);
	cmd_regs[4] = cdns_xspi->acmd_info.data_nbytes - 1;
	cmd_regs[0] = cdns_xspi_acmd_cmd(cdns_xspi,
					 CDNS_XSPI_ACMD_PROG_OP,
					 CDNS_XSPI_ACMD_DATA_THREAD, true);

	ret = cdns_xspi_acmd_run(cdns_xspi, cmd_regs,
				 CDNS_XSPI_ACMD_DATA_THREAD);

out_clear_program_state:
	cdns_xspi->acmd_info.column_addr = 0;
	cdns_xspi->acmd_info.data_nbytes = 0;
	cdns_xspi->out_buffer = NULL;
	return ret;
}

static int cdns_xspi_pio_mdma_read(struct cdns_xspi_dev *cdns_xspi,
				   struct spinand_device *spinand,
				   const struct spi_mem_op *op)
{
	u32 cmd_regs[6] = { 0 };
	u64 xspi_addr;
	int ret;

	if (!cdns_xspi->acmd_info.row_addr_valid) {
		dev_err(cdns_xspi->dev,
			"read cache command without a pending page read\n");
		return -EINVAL;
	}

	if (op->data.dir != SPI_MEM_DATA_IN || !op->data.nbytes ||
	    !op->data.buf.in) {
		ret = -EINVAL;
		goto out_clear_read_state;
	}

	if (op->data.nbytes > cdns_xspi->dma_buf_len) {
		ret = -EMSGSIZE;
		goto out_clear_read_state;
	}

	ret = cdns_xspi_wait_for_controller_idle(cdns_xspi);
	if (ret) {
		dev_err(cdns_xspi->dev,
			"controller did not become idle before read\n");
		goto out_clear_read_state;
	}

	ret = cdns_xspi_nand_addr(spinand,
				  cdns_xspi->acmd_info.row_addr,
				  op->addr.val, &xspi_addr);
	if (ret)
		goto out_clear_read_state;

	if (upper_32_bits(xspi_addr)) {
		dev_err(cdns_xspi->dev,
			"read address 0x%llx exceeds the ACMD PIO range\n",
			xspi_addr);
		ret = -ERANGE;
		goto out_clear_read_state;
	}

	cmd_regs[1] = lower_32_bits(xspi_addr);
	cmd_regs[2] = lower_32_bits(cdns_xspi->dma_addr);
	cmd_regs[3] = upper_32_bits(cdns_xspi->dma_addr);
	cmd_regs[4] = op->data.nbytes - 1;
	cmd_regs[0] = cdns_xspi_acmd_cmd(cdns_xspi,
					 CDNS_XSPI_ACMD_READ_OP,
					 CDNS_XSPI_ACMD_DATA_THREAD, true);

	ret = cdns_xspi_acmd_run(cdns_xspi, cmd_regs,
				 CDNS_XSPI_ACMD_DATA_THREAD);
	if (ret) {
		dev_err(cdns_xspi->dev, "ACMD read failed: %d\n", ret);
		goto out_clear_read_state;
	}

	memcpy(op->data.buf.in, cdns_xspi->dma_buf, op->data.nbytes);

out_clear_read_state:
	cdns_xspi->acmd_info.row_addr_valid = false;
	cdns_xspi->acmd_info.row_addr = 0;
	return ret;
}

static int cdns_xspi_send_pio_command(struct cdns_xspi_dev *cdns_xspi,
				      struct spi_mem *mem,
				      const struct spi_mem_op *op)
{
	struct spinand_device *spinand;
	const struct spi_mem_op *read_cache;
	const struct spi_mem_op *write_cache;
	const struct spi_mem_op *update_cache;
	int ret;

	if (cdns_xspi->flash_type != CDNS_XSPI_FLASH_TYPE_NAND)
		goto use_stig;

	spinand = spi_mem_get_drvdata(mem);
	if (!spinand || !spinand->op_templates ||
	    !spinand->op_templates->read_cache ||
	    !spinand->op_templates->write_cache ||
	    !spinand->op_templates->update_cache)
		goto use_stig;

	if (!cdns_xspi->acmd_info.initialized) {
		ret = cdns_xspi_nand_init(cdns_xspi, spinand);
		if (ret) {
			dev_err(cdns_xspi->dev,
				"failed to initialize ACMD: %d\n", ret);
			return ret;
		}
	}

	read_cache = spinand->op_templates->read_cache;
	write_cache = spinand->op_templates->write_cache;
	update_cache = spinand->op_templates->update_cache;

	if ((write_cache && op->cmd.opcode == write_cache->cmd.opcode) ||
	    (update_cache && op->cmd.opcode == update_cache->cmd.opcode)) {
		if (op->data.dir != SPI_MEM_DATA_OUT || !op->data.nbytes ||
		    !op->data.buf.out)
			return -EINVAL;

		cdns_xspi_nand_write_enable_seq_init(cdns_xspi);
		cdns_xspi->out_buffer = op->data.buf.out;
		cdns_xspi->acmd_info.column_addr = op->addr.val;
		cdns_xspi->acmd_info.data_nbytes = op->data.nbytes;
		return 0;
	}

	switch (op->cmd.opcode) {
	case CDNS_XSPI_NAND_OP_PAGE_READ:
		cdns_xspi->acmd_info.row_addr = op->addr.val;
		cdns_xspi->acmd_info.row_addr_valid = true;
		return 0;

	case CDNS_XSPI_NAND_OP_GET_FEATURE:
		if (op->addr.val != CDNS_XSPI_NAND_STATUS_REG ||
		    !cdns_xspi->acmd_info.row_addr_valid)
			break;

		if (op->data.dir != SPI_MEM_DATA_IN || !op->data.nbytes ||
		    !op->data.buf.in)
			return -EINVAL;

		memset(op->data.buf.in, 0, op->data.nbytes);
		return 0;

	case CDNS_XSPI_NAND_OP_RESET:
		return cdns_xspi_pio_reset(cdns_xspi);

	case CDNS_XSPI_NAND_OP_BLOCK_ERASE:
		return cdns_xspi_pio_mdma_erase(cdns_xspi, spinand, op);

	case CDNS_XSPI_NAND_OP_PROGRAM_EXECUTE:
		return cdns_xspi_pio_mdma_program(cdns_xspi, spinand, op);

	default:
		break;
	}

	if (read_cache && op->cmd.opcode == read_cache->cmd.opcode)
		return cdns_xspi_pio_mdma_read(cdns_xspi, spinand, op);

use_stig:
	/* ACMD does not consume this operation; execute it through STIG. */
	return cdns_xspi_send_stig_command(cdns_xspi, op,
					   op->data.dir != SPI_MEM_NO_DATA);
}

static int cdns_xspi_mem_op(struct cdns_xspi_dev *cdns_xspi,
			    struct spi_mem *mem,
			    const struct spi_mem_op *op)
{
	enum spi_mem_data_dir dir = op->data.dir;

	if (cdns_xspi->cur_cs != spi_get_chipselect(mem->spi, 0))
		cdns_xspi->cur_cs = spi_get_chipselect(mem->spi, 0);

	if (cdns_xspi->work_mode == CDNS_XSPI_WORK_MODE_ACMD)
		return cdns_xspi_send_pio_command(cdns_xspi, mem, op);

	return cdns_xspi_send_stig_command(cdns_xspi, op,
					   (dir != SPI_MEM_NO_DATA));
}

static int cdns_xspi_mem_op_execute(struct spi_mem *mem,
				    const struct spi_mem_op *op)
{
	struct cdns_xspi_dev *cdns_xspi =
		spi_controller_get_devdata(mem->spi->controller);
	int ret = 0;

	ret = cdns_xspi_mem_op(cdns_xspi, mem, op);

	return ret;
}

static bool cdns_xspi_supports_op(struct spi_mem *mem,
				  const struct spi_mem_op *op)
{
	struct spi_device *spi = mem->spi;
	struct device *dev = &spi->dev;
	u32 value;

	if (!device_property_read_u32(dev, "spi-tx-bus-width", &value)) {
		switch (value) {
		case 1:
			break;
		case 2:
			spi->mode |= SPI_TX_DUAL;
			break;
		case 4:
			spi->mode |= SPI_TX_QUAD;
			break;
		case 8:
			spi->mode |= SPI_TX_OCTAL;
			break;
		default:
			dev_warn(dev, "spi-tx-bus-width %u not supported\n", value);
			break;
		}
	}

	if (!device_property_read_u32(dev, "spi-rx-bus-width", &value)) {
		switch (value) {
		case 1:
			break;
		case 2:
			spi->mode |= SPI_RX_DUAL;
			break;
		case 4:
			spi->mode |= SPI_RX_QUAD;
			break;
		case 8:
			spi->mode |= SPI_RX_OCTAL;
			break;
		default:
			dev_warn(dev, "spi-rx-bus-width %u not supported\n", value);
			break;
		}
	}

	if (!spi_mem_default_supports_op(mem, op))
		return false;

	return true;
}

static int cdns_xspi_adjust_mem_op_size(struct spi_mem *mem, struct spi_mem_op *op)
{
	struct cdns_xspi_dev *cdns_xspi =
		spi_controller_get_devdata(mem->spi->controller);

	op->data.nbytes = clamp_val(op->data.nbytes, 0, cdns_xspi->sdmasize);

	return 0;
}

static const struct spi_controller_mem_ops cadence_xspi_mem_ops = {
	.supports_op = PTR_IF(IS_ENABLED(CONFIG_ACPI), cdns_xspi_supports_op),
	.exec_op = cdns_xspi_mem_op_execute,
	.adjust_op_size = cdns_xspi_adjust_mem_op_size,
};

static irqreturn_t cdns_xspi_irq_handler(int this_irq, void *dev)
{
	struct cdns_xspi_dev *cdns_xspi = dev;
	u32 irq_status;
	irqreturn_t result = IRQ_NONE;

	irq_status = readl(cdns_xspi->iobase + CDNS_XSPI_INTR_STATUS_REG);
	writel(irq_status, cdns_xspi->iobase + CDNS_XSPI_INTR_STATUS_REG);

	if (irq_status &
	    (CDNS_XSPI_SDMA_ERROR | CDNS_XSPI_SDMA_TRIGGER |
	     CDNS_XSPI_STIG_DONE)) {
		if (irq_status & CDNS_XSPI_SDMA_ERROR) {
			dev_err(cdns_xspi->dev,
				"Slave DMA transaction error\n");
			cdns_xspi->sdma_error = true;
			complete(&cdns_xspi->sdma_complete);
		}

		if (irq_status & CDNS_XSPI_SDMA_TRIGGER)
			complete(&cdns_xspi->sdma_complete);

		if (irq_status & CDNS_XSPI_STIG_DONE)
			complete(&cdns_xspi->cmd_complete);

		result = IRQ_HANDLED;
	}

	irq_status = readl(cdns_xspi->iobase + CDNS_XSPI_TRD_COMP_INTR_STATUS);
	if (irq_status) {
		writel(irq_status,
		       cdns_xspi->iobase + CDNS_XSPI_TRD_COMP_INTR_STATUS);

		complete(&cdns_xspi->auto_cmd_complete);

		result = IRQ_HANDLED;
	}

	return result;
}

static int cdns_xspi_of_get_plat_data(struct platform_device *pdev)
{
	struct fwnode_handle *fwnode_child;
	unsigned int cs;

	device_for_each_child_node(&pdev->dev, fwnode_child) {
		if (!fwnode_device_is_available(fwnode_child))
			continue;

		if (fwnode_property_read_u32(fwnode_child, "reg", &cs)) {
			dev_err(&pdev->dev, "Couldn't get memory chip select\n");
			fwnode_handle_put(fwnode_child);
			return -ENXIO;
		} else if (cs >= CDNS_XSPI_MAX_BANKS) {
			dev_err(&pdev->dev, "reg (cs) parameter value too large\n");
			fwnode_handle_put(fwnode_child);
			return -ENXIO;
		}
	}

	return 0;
}

static void cdns_xspi_print_phy_config(struct cdns_xspi_dev *cdns_xspi)
{
	struct device *dev = cdns_xspi->dev;

	dev_info(dev, "PHY configuration\n");
	dev_info(dev, "   * xspi_dll_phy_ctrl: %08x\n",
		 readl(cdns_xspi->iobase + CDNS_XSPI_DLL_PHY_CTRL));
	dev_info(dev, "   * phy_dq_timing: %08x\n",
		 readl(cdns_xspi->auxbase + CDNS_XSPI_CCP_PHY_DQ_TIMING));
	dev_info(dev, "   * phy_dqs_timing: %08x\n",
		 readl(cdns_xspi->auxbase + CDNS_XSPI_CCP_PHY_DQS_TIMING));
	dev_info(dev, "   * phy_gate_loopback_ctrl: %08x\n",
		 readl(cdns_xspi->auxbase + CDNS_XSPI_CCP_PHY_GATE_LPBCK_CTRL));
	dev_info(dev, "   * phy_dll_slave_ctrl: %08x\n",
		 readl(cdns_xspi->auxbase + CDNS_XSPI_CCP_PHY_DLL_SLAVE_CTRL));
}

#ifdef CONFIG_64BIT
static struct cdns_xspi_driver_data marvell_driver_data = {
	.mrvl_hw_overlay = true,
	.dll_phy_ctrl = MARVELL_REGS_DLL_PHY_CTRL,
	.ctb_rfile_phy_ctrl = MARVELL_CTB_RFILE_PHY_CTRL,
	.rfile_phy_tsel = MARVELL_RFILE_PHY_TSEL,
	.rfile_phy_dq_timing = MARVELL_RFILE_PHY_DQ_TIMING,
	.rfile_phy_dqs_timing = MARVELL_RFILE_PHY_DQS_TIMING,
	.rfile_phy_gate_lpbk_ctrl = MARVELL_RFILE_PHY_GATE_LPBK_CTRL,
	.rfile_phy_dll_master_ctrl = MARVELL_RFILE_PHY_DLL_MASTER_CTRL,
	.rfile_phy_dll_slave_ctrl = MARVELL_RFILE_PHY_DLL_SLAVE_CTRL,
};

static const int cdns_mrvl_xspi_clk_div_list[] = {
	4,	//0x0 = Divide by 4.   SPI clock is 200 MHz.
	6,	//0x1 = Divide by 6.   SPI clock is 133.33 MHz.
	8,	//0x2 = Divide by 8.   SPI clock is 100 MHz.
	10,	//0x3 = Divide by 10.  SPI clock is 80 MHz.
	12,	//0x4 = Divide by 12.  SPI clock is 66.666 MHz.
	16,	//0x5 = Divide by 16.  SPI clock is 50 MHz.
	18,	//0x6 = Divide by 18.  SPI clock is 44.44 MHz.
	20,	//0x7 = Divide by 20.  SPI clock is 40 MHz.
	24,	//0x8 = Divide by 24.  SPI clock is 33.33 MHz.
	32,	//0x9 = Divide by 32.  SPI clock is 25 MHz.
	40,	//0xA = Divide by 40.  SPI clock is 20 MHz.
	50,	//0xB = Divide by 50.  SPI clock is 16 MHz.
	64,	//0xC = Divide by 64.  SPI clock is 12.5 MHz.
	128	//0xD = Divide by 128. SPI clock is 6.25 MHz.
};

static void cdns_xspi_reset_dll(struct cdns_xspi_dev *cdns_xspi)
{
	u32 dll_cntrl = readl(cdns_xspi->iobase +
			      CDNS_XSPI_RF_MINICTRL_REGS_DLL_PHY_CTRL);

	/* Reset DLL */
	dll_cntrl |= CDNS_XSPI_DLL_RST_N;
	writel(dll_cntrl, cdns_xspi->iobase +
			  CDNS_XSPI_RF_MINICTRL_REGS_DLL_PHY_CTRL);
}

static bool cdns_xspi_is_dll_locked(struct cdns_xspi_dev *cdns_xspi)
{
	u32 dll_lock;

	return !readl_relaxed_poll_timeout(cdns_xspi->iobase +
		CDNS_XSPI_INTR_STATUS_REG,
		dll_lock, ((dll_lock & CDNS_XSPI_DLL_LOCK) == 1), 10, 10000);
}

/* Static configuration of PHY */
static bool cdns_xspi_configure_phy(struct cdns_xspi_dev *cdns_xspi)
{
	writel(cdns_xspi->driver_data->dll_phy_ctrl,
	       cdns_xspi->iobase + CDNS_XSPI_RF_MINICTRL_REGS_DLL_PHY_CTRL);
	writel(cdns_xspi->driver_data->ctb_rfile_phy_ctrl,
	       cdns_xspi->auxbase + CDNS_XSPI_PHY_CTB_RFILE_PHY_CTRL);
	writel(cdns_xspi->driver_data->rfile_phy_tsel,
	       cdns_xspi->auxbase + CDNS_XSPI_PHY_CTB_RFILE_PHY_TSEL);
	writel(cdns_xspi->driver_data->rfile_phy_dq_timing,
	       cdns_xspi->auxbase + CDNS_XSPI_PHY_DATASLICE_RFILE_PHY_DQ_TIMING);
	writel(cdns_xspi->driver_data->rfile_phy_dqs_timing,
	       cdns_xspi->auxbase + CDNS_XSPI_PHY_DATASLICE_RFILE_PHY_DQS_TIMING);
	writel(cdns_xspi->driver_data->rfile_phy_gate_lpbk_ctrl,
	       cdns_xspi->auxbase + CDNS_XSPI_PHY_DATASLICE_RFILE_PHY_GATE_LPBK_CTRL);
	writel(cdns_xspi->driver_data->rfile_phy_dll_master_ctrl,
	       cdns_xspi->auxbase + CDNS_XSPI_PHY_DATASLICE_RFILE_PHY_DLL_MASTER_CTRL);
	writel(cdns_xspi->driver_data->rfile_phy_dll_slave_ctrl,
	       cdns_xspi->auxbase + CDNS_XSPI_PHY_DATASLICE_RFILE_PHY_DLL_SLAVE_CTRL);

	cdns_xspi_reset_dll(cdns_xspi);

	return cdns_xspi_is_dll_locked(cdns_xspi);
}

static bool cdns_mrvl_xspi_setup_clock(struct cdns_xspi_dev *cdns_xspi,
				       int requested_clk)
{
	int i = 0;
	int clk_val;
	u32 clk_reg;
	bool update_clk = false;

	while (i < (ARRAY_SIZE(cdns_mrvl_xspi_clk_div_list) - 1)) {
		clk_val = MRVL_XSPI_CLOCK_DIVIDED(
				cdns_mrvl_xspi_clk_div_list[i]);
		if (clk_val <= requested_clk)
			break;
		i++;
	}

	dev_dbg(cdns_xspi->dev, "Found clk div: %d, clk val: %d\n",
		cdns_mrvl_xspi_clk_div_list[i],
		MRVL_XSPI_CLOCK_DIVIDED(
		cdns_mrvl_xspi_clk_div_list[i]));

	clk_reg = readl(cdns_xspi->auxbase + MRVL_XSPI_CLK_CTRL_AUX_REG);

	if (FIELD_GET(MRVL_XSPI_CLK_DIV, clk_reg) != i) {
		clk_reg &= ~MRVL_XSPI_CLK_ENABLE;
		writel(clk_reg,
		       cdns_xspi->auxbase + MRVL_XSPI_CLK_CTRL_AUX_REG);
		clk_reg = FIELD_PREP(MRVL_XSPI_CLK_DIV, i);
		FIELD_MODIFY(MRVL_XSPI_CLK_DIV, &clk_reg, i);
		clk_reg |= MRVL_XSPI_CLK_ENABLE;
		clk_reg |= MRVL_XSPI_IRQ_ENABLE;
		update_clk = true;
	}

	if (update_clk)
		writel(clk_reg,
		       cdns_xspi->auxbase + MRVL_XSPI_CLK_CTRL_AUX_REG);

	return update_clk;
}

static void marvell_xspi_set_interrupts(struct cdns_xspi_dev *cdns_xspi,
				     bool enabled)
{
	u32 intr_enable;
	u32 irq_status;

	irq_status = readl(cdns_xspi->iobase + CDNS_XSPI_INTR_STATUS_REG);
	writel(irq_status, cdns_xspi->iobase + CDNS_XSPI_INTR_STATUS_REG);

	intr_enable = readl(cdns_xspi->iobase + CDNS_XSPI_INTR_ENABLE_REG);
	if (enabled)
		intr_enable |= CDNS_XSPI_INTR_MASK;
	else
		intr_enable &= ~CDNS_XSPI_INTR_MASK;
	writel(intr_enable, cdns_xspi->iobase + CDNS_XSPI_INTR_ENABLE_REG);
}

static int marvell_xspi_mem_op_execute(struct spi_mem *mem,
				    const struct spi_mem_op *op)
{
	struct cdns_xspi_dev *cdns_xspi =
		spi_controller_get_devdata(mem->spi->controller);
	int ret = 0;

	cdns_mrvl_xspi_setup_clock(cdns_xspi, mem->spi->max_speed_hz);

	ret = cdns_xspi_mem_op(cdns_xspi, mem, op);

	return ret;
}

static void m_ioreadq(void __iomem  *addr, void *buf, int len)
{
	if (IS_ALIGNED((long)buf, 8) && len >= 8) {
		u64 full_ops = len / 8;
		u64 *buffer = buf;

		len -= full_ops * 8;
		buf += full_ops * 8;

		do {
			u64 b = readq(addr);
			*buffer++ = b;
		} while (--full_ops);
	}


	while (len) {
		u64 tmp_buf;

		tmp_buf = readq(addr);
		memcpy(buf, &tmp_buf, min(len, 8));
		len = len > 8 ? len - 8 : 0;
		buf += 8;
	}
}

static void m_iowriteq(void __iomem *addr, const void *buf, int len)
{
	if (IS_ALIGNED((long)buf, 8) && len >= 8) {
		u64 full_ops = len / 8;
		const u64 *buffer = buf;

		len -= full_ops * 8;
		buf += full_ops * 8;

		do {
			writeq(*buffer++, addr);
		} while (--full_ops);
	}

	while (len) {
		u64 tmp_buf;

		memcpy(&tmp_buf, buf, min(len, 8));
		writeq(tmp_buf, addr);
		len = len > 8 ? len - 8 : 0;
		buf += 8;
	}
}

static void marvell_xspi_sdma_handle(struct cdns_xspi_dev *cdns_xspi)
{
	u32 sdma_size, sdma_trd_info;
	u8 sdma_dir;

	sdma_size = readl(cdns_xspi->iobase + CDNS_XSPI_SDMA_SIZE_REG);
	sdma_trd_info = readl(cdns_xspi->iobase + CDNS_XSPI_SDMA_TRD_INFO_REG);
	sdma_dir = FIELD_GET(CDNS_XSPI_SDMA_DIR, sdma_trd_info);

	switch (sdma_dir) {
	case CDNS_XSPI_SDMA_DIR_READ:
		m_ioreadq(cdns_xspi->sdmabase,
			    cdns_xspi->in_buffer, sdma_size);
		break;

	case CDNS_XSPI_SDMA_DIR_WRITE:
		m_iowriteq(cdns_xspi->sdmabase,
			     cdns_xspi->out_buffer, sdma_size);
		break;
	}
}

static const struct spi_controller_mem_ops marvell_xspi_mem_ops = {
	.supports_op = PTR_IF(IS_ENABLED(CONFIG_ACPI), cdns_xspi_supports_op),
	.exec_op = marvell_xspi_mem_op_execute,
	.adjust_op_size = cdns_xspi_adjust_mem_op_size,
};

static int cdns_xspi_prepare_generic(int cs, const void *dout, int len, int glue, u32 *cmd_regs)
{
	u8 *data = (u8 *)dout;
	int i;
	int data_counter = 0;

	memset(cmd_regs, 0x00, CMD_REG_LEN);

	if (GENERIC_CMD_REG_3_NEEDED(len)) {
		for (i = GENERIC_CMD_DATA_REG_3_COUNT(len); i >= 0 ; i--)
			cmd_regs[3] |= GENERIC_CMD_DATA_INSERT(data[data_counter++],
							       GENERIC_CMD_DATA_3_OFFSET(i));
	}
	if (GENERIC_CMD_REG_2_NEEDED(len)) {
		for (i = GENERIC_CMD_DATA_REG_2_COUNT(len); i >= 0; i--)
			cmd_regs[2] |= GENERIC_CMD_DATA_INSERT(data[data_counter++],
							       GENERIC_CMD_DATA_2_OFFSET(i));
	}
	for (i = GENERIC_CMD_DATA_REG_1_COUNT(len); i >= 0 ; i--)
		cmd_regs[1] |= GENERIC_CMD_DATA_INSERT(data[data_counter++],
						       GENERIC_CMD_DATA_1_OFFSET(i));

	cmd_regs[1] |= CDNS_XSPI_CMD_FLD_P1_GENERIC_CMD;
	cmd_regs[3] |= CDNS_XSPI_CMD_FLD_P3_GENERIC_CMD(len);
	cmd_regs[4] |= CDNS_XSPI_CMD_FLD_P4_GENERIC_CMD(cs, glue);

	return 0;
}

static void marvell_xspi_read_single_qword(struct cdns_xspi_dev *cdns_xspi, u8 **buffer)
{
	u64 d = readq(cdns_xspi->xferbase +
		      MRVL_XFER_FUNC_CTRL_READ_DATA(cdns_xspi->current_xfer_qword));
	u8 *ptr = (u8 *)&d;
	int k;

	for (k = 0; k < 8; k++) {
		u8 val = bitrev8((ptr[k]));
		**buffer = val;
		*buffer = *buffer + 1;
	}

	cdns_xspi->current_xfer_qword++;
	cdns_xspi->current_xfer_qword %= MRVL_XFER_QWORD_COUNT;
}

static void cdns_xspi_finish_read(struct cdns_xspi_dev *cdns_xspi, u8 **buffer, u32 data_count)
{
	u64 d = readq(cdns_xspi->xferbase +
		      MRVL_XFER_FUNC_CTRL_READ_DATA(cdns_xspi->current_xfer_qword));
	u8 *ptr = (u8 *)&d;
	int k;

	for (k = 0; k < data_count % MRVL_XFER_QWORD_BYTECOUNT; k++) {
		u8 val = bitrev8((ptr[k]));
		**buffer = val;
		*buffer = *buffer + 1;
	}

	cdns_xspi->current_xfer_qword++;
	cdns_xspi->current_xfer_qword %= MRVL_XFER_QWORD_COUNT;
}

static int cdns_xspi_prepare_transfer(int cs, int dir, int len, u32 *cmd_regs)
{
	memset(cmd_regs, 0x00, CMD_REG_LEN);

	cmd_regs[1] |= CDNS_XSPI_CMD_FLD_GENERIC_DSEQ_CMD_1;
	cmd_regs[2] |= CDNS_XSPI_CMD_FLD_GENERIC_DSEQ_CMD_2(len);
	cmd_regs[4] |= CDNS_XSPI_CMD_FLD_GENERIC_DSEQ_CMD_4(dir, cs);

	return 0;
}

static bool cdns_xspi_is_stig_ready(struct cdns_xspi_dev *cdns_xspi, bool sleep)
{
	u32 ctrl_stat;

	return !readl_relaxed_poll_timeout
		(cdns_xspi->iobase + CDNS_XSPI_CTRL_STATUS_REG,
		ctrl_stat,
		((ctrl_stat & BIT(3)) == 0),
		sleep ? MRVL_XSPI_POLL_DELAY_US : 0,
		sleep ? MRVL_XSPI_POLL_TIMEOUT_US : 0);
}

static bool cdns_xspi_is_sdma_ready(struct cdns_xspi_dev *cdns_xspi, bool sleep)
{
	u32 ctrl_stat;

	return !readl_relaxed_poll_timeout
		(cdns_xspi->iobase + CDNS_XSPI_INTR_STATUS_REG,
		ctrl_stat,
		(ctrl_stat & CDNS_XSPI_SDMA_TRIGGER),
		sleep ? MRVL_XSPI_POLL_DELAY_US : 0,
		sleep ? MRVL_XSPI_POLL_TIMEOUT_US : 0);
}

static int cdns_xspi_transfer_one_message_b0(struct spi_controller *controller,
					   struct spi_message *m)
{
	struct cdns_xspi_dev *cdns_xspi = spi_controller_get_devdata(controller);
	struct spi_device *spi = m->spi;
	struct spi_transfer *t = NULL;

	const unsigned int max_len = MRVL_XFER_QWORD_BYTECOUNT * MRVL_XFER_QWORD_COUNT;
	int current_transfer_len;
	int cs = spi_get_chipselect(spi, 0);
	int cs_change = 0;

	/* Enable xfer state machine */
	if (!cdns_xspi->xfer_in_progress) {
		u32 xfer_control = readl(cdns_xspi->xferbase + MRVL_XFER_FUNC_CTRL);

		cdns_xspi->current_xfer_qword = 0;
		cdns_xspi->xfer_in_progress = true;
		xfer_control |= (MRVL_XFER_RECEIVE_ENABLE |
				 MRVL_XFER_CLK_CAPTURE_POL |
				 MRVL_XFER_FUNC_START |
				 MRVL_XFER_SOFT_RESET |
				 FIELD_PREP(MRVL_XFER_CS_N_HOLD, (1 << cs)));
		xfer_control &= ~(MRVL_XFER_FUNC_ENABLE | MRVL_XFER_CLK_DRIVE_POL);
		writel(xfer_control, cdns_xspi->xferbase + MRVL_XFER_FUNC_CTRL);
	}

	list_for_each_entry(t, &m->transfers, transfer_list) {
		u8 *txd = (u8 *) t->tx_buf;
		u8 *rxd = (u8 *) t->rx_buf;
		u8 data[10];
		u32 cmd_regs[6];

		if (!txd)
			txd = data;

		cdns_xspi->in_buffer = txd + 1;
		cdns_xspi->out_buffer = txd + 1;

		while (t->len) {

			current_transfer_len = min(max_len, t->len);

			if (current_transfer_len < 10) {
				cdns_xspi_prepare_generic(cs, txd, current_transfer_len,
							  false, cmd_regs);
				cdns_xspi_trigger_command(cdns_xspi, cmd_regs);
				if (!cdns_xspi_is_stig_ready(cdns_xspi, true))
					return -EIO;
			} else {
				cdns_xspi_prepare_generic(cs, txd, 1, true, cmd_regs);
				cdns_xspi_trigger_command(cdns_xspi, cmd_regs);
				cdns_xspi_prepare_transfer(cs, 1, current_transfer_len - 1,
							   cmd_regs);
				cdns_xspi_trigger_command(cdns_xspi, cmd_regs);
				if (!cdns_xspi_is_sdma_ready(cdns_xspi, true))
					return -EIO;
				cdns_xspi->sdma_handler(cdns_xspi);
				if (!cdns_xspi_is_stig_ready(cdns_xspi, true))
					return -EIO;

				cdns_xspi->in_buffer += current_transfer_len;
				cdns_xspi->out_buffer += current_transfer_len;
			}

			if (rxd) {
				int j;

				for (j = 0; j < current_transfer_len / 8; j++)
					marvell_xspi_read_single_qword(cdns_xspi, &rxd);
				cdns_xspi_finish_read(cdns_xspi, &rxd, current_transfer_len);
			} else {
				cdns_xspi->current_xfer_qword += current_transfer_len /
								 MRVL_XFER_QWORD_BYTECOUNT;
				if (current_transfer_len % MRVL_XFER_QWORD_BYTECOUNT)
					cdns_xspi->current_xfer_qword++;

				cdns_xspi->current_xfer_qword %= MRVL_XFER_QWORD_COUNT;
			}
			cs_change = t->cs_change;
			t->len -= current_transfer_len;
		}
		spi_transfer_delay_exec(t);
	}

	if (!cs_change) {
		u32 xfer_control = readl(cdns_xspi->xferbase + MRVL_XFER_FUNC_CTRL);

		xfer_control &= ~(MRVL_XFER_RECEIVE_ENABLE |
				  MRVL_XFER_SOFT_RESET);
		writel(xfer_control, cdns_xspi->xferbase + MRVL_XFER_FUNC_CTRL);
		cdns_xspi->xfer_in_progress = false;
	}

	m->status = 0;
	spi_finalize_current_message(controller);

	return 0;
}
#endif

static int cdns_xspi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *host = NULL;
	struct cdns_xspi_dev *cdns_xspi = NULL;
	struct resource *res;
	int ret;

	host = devm_spi_alloc_host(dev, sizeof(*cdns_xspi));
	if (!host)
		return -ENOMEM;

	host->mode_bits = SPI_3WIRE | SPI_TX_DUAL  | SPI_TX_QUAD  |
		SPI_RX_DUAL | SPI_RX_QUAD | SPI_TX_OCTAL | SPI_RX_OCTAL |
		SPI_MODE_0  | SPI_MODE_3;

	cdns_xspi = spi_controller_get_devdata(host);
	cdns_xspi->driver_data = device_get_match_data(dev);
	if (!cdns_xspi->driver_data)
		return -ENODEV;

	host->mem_ops = &cadence_xspi_mem_ops;
	cdns_xspi->sdma_handler = &cdns_xspi_sdma_handle;
	cdns_xspi->set_interrupts_handler = &cdns_xspi_set_interrupts;
#ifdef CONFIG_64BIT
	if (cdns_xspi->driver_data->mrvl_hw_overlay) {
		host->mem_ops = &marvell_xspi_mem_ops;
		host->transfer_one_message = cdns_xspi_transfer_one_message_b0;
		cdns_xspi->sdma_handler = &marvell_xspi_sdma_handle;
		cdns_xspi->set_interrupts_handler = &marvell_xspi_set_interrupts;
	}
#endif
	host->bus_num = -1;

	platform_set_drvdata(pdev, cdns_xspi);

	cdns_xspi->pdev = pdev;
	cdns_xspi->host = host;
	cdns_xspi->dev = &pdev->dev;
	cdns_xspi->cur_cs = 0;
	cdns_xspi->flash_type = cdns_xspi->driver_data->flash_type;
	cdns_xspi->work_mode = cdns_xspi->driver_data->use_acmd ?
		CDNS_XSPI_WORK_MODE_ACMD : CDNS_XSPI_WORK_MODE_STIG;

	init_completion(&cdns_xspi->cmd_complete);
	init_completion(&cdns_xspi->auto_cmd_complete);
	init_completion(&cdns_xspi->sdma_complete);

	ret = cdns_xspi_of_get_plat_data(pdev);
	if (ret)
		return -ENODEV;

	cdns_xspi->iobase = devm_platform_ioremap_resource_byname(pdev, "io");
	if (IS_ERR(cdns_xspi->iobase)) {
		cdns_xspi->iobase = devm_platform_ioremap_resource(pdev, 0);
		if (IS_ERR(cdns_xspi->iobase)) {
			dev_err(dev, "Failed to remap controller base address\n");
			return PTR_ERR(cdns_xspi->iobase);
		}
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sdma");
	cdns_xspi->sdmabase = devm_ioremap_resource(dev, res);
	if (IS_ERR(cdns_xspi->sdmabase)) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		cdns_xspi->sdmabase = devm_ioremap_resource(dev, res);
		if (IS_ERR(cdns_xspi->sdmabase))
			return PTR_ERR(cdns_xspi->sdmabase);
	}
	cdns_xspi->sdmasize = resource_size(res);

	cdns_xspi->auxbase = devm_platform_ioremap_resource_byname(pdev, "aux");
	if (IS_ERR(cdns_xspi->auxbase)) {
		cdns_xspi->auxbase = devm_platform_ioremap_resource(pdev, 2);
		if (IS_ERR(cdns_xspi->auxbase)) {
			dev_err(dev, "Failed to remap AUX address\n");
			return PTR_ERR(cdns_xspi->auxbase);
		}
	}

#ifdef CONFIG_64BIT
	if (cdns_xspi->driver_data->mrvl_hw_overlay) {
		cdns_xspi->xferbase = devm_platform_ioremap_resource_byname(pdev, "xfer");
		if (IS_ERR(cdns_xspi->xferbase)) {
			cdns_xspi->xferbase = devm_platform_ioremap_resource(pdev, 3);
			if (IS_ERR(cdns_xspi->xferbase)) {
				dev_info(dev, "XFER register base not found, set it\n");
				// For compatibility with older firmware
				cdns_xspi->xferbase = cdns_xspi->iobase + 0x8000;
			}
		}
	}
#endif

	cdns_xspi->irq = platform_get_irq(pdev, 0);
	if (cdns_xspi->irq < 0)
		return -ENXIO;

	ret = devm_request_irq(dev, cdns_xspi->irq, cdns_xspi_irq_handler,
			       IRQF_SHARED, pdev->name, cdns_xspi);
	if (ret) {
		dev_err(dev, "Failed to request IRQ: %d\n", cdns_xspi->irq);
		return ret;
	}

#ifdef CONFIG_64BIT
	if (cdns_xspi->driver_data->mrvl_hw_overlay) {
		cdns_mrvl_xspi_setup_clock(cdns_xspi, MRVL_DEFAULT_CLK);
		cdns_xspi_configure_phy(cdns_xspi);
	}
#endif

	cdns_xspi_print_phy_config(cdns_xspi);

	ret = cdns_xspi_controller_init(cdns_xspi);
	if (ret) {
		dev_err(dev, "Failed to initialize controller\n");
		return ret;
	}

	host->num_chipselect = 1 << cdns_xspi->hw_num_banks;

	ret = devm_spi_register_controller(dev, host);
	if (ret) {
		dev_err(dev, "Failed to register SPI host\n");
		return ret;
	}

	dev_info(dev, "Successfully registered SPI host\n");

	return 0;
}

static int cdns_xspi_suspend(struct device *dev)
{
	struct cdns_xspi_dev *cdns_xspi = dev_get_drvdata(dev);

	return spi_controller_suspend(cdns_xspi->host);
}

static int cdns_xspi_resume(struct device *dev)
{
	struct cdns_xspi_dev *cdns_xspi = dev_get_drvdata(dev);

#ifdef CONFIG_64BIT
	if (cdns_xspi->driver_data->mrvl_hw_overlay) {
		cdns_mrvl_xspi_setup_clock(cdns_xspi, MRVL_DEFAULT_CLK);
		cdns_xspi_configure_phy(cdns_xspi);
	}
#endif

	cdns_xspi->set_interrupts_handler(cdns_xspi, false);

	return spi_controller_resume(cdns_xspi->host);
}

static DEFINE_SIMPLE_DEV_PM_OPS(cdns_xspi_pm_ops,
				cdns_xspi_suspend, cdns_xspi_resume);

static const struct of_device_id cdns_xspi_of_match[] = {
	{
		.compatible = "cdns,xspi-nor",
		.data = &cdns_driver_data,
	},
#ifdef CONFIG_64BIT
	{
		.compatible = "marvell,cn10-xspi-nor",
		.data = &marvell_driver_data,
	},
#endif
	{
		.compatible = "cdns,xspi-nand",
		.data = &cdns_nand_driver_data,
	},
	{ /* end of table */}
};
MODULE_DEVICE_TABLE(of, cdns_xspi_of_match);

static struct platform_driver cdns_xspi_platform_driver = {
	.probe          = cdns_xspi_probe,
	.driver = {
		.name = CDNS_XSPI_NAME,
		.of_match_table = cdns_xspi_of_match,
		.pm = pm_sleep_ptr(&cdns_xspi_pm_ops),
	},
};

module_platform_driver(cdns_xspi_platform_driver);

MODULE_DESCRIPTION("Cadence XSPI Controller Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" CDNS_XSPI_NAME);
MODULE_AUTHOR("Konrad Kociolek <konrad@cadence.com>");
MODULE_AUTHOR("Jayshri Pawar <jpawar@cadence.com>");
MODULE_AUTHOR("Parshuram Thombare <pthombar@cadence.com>");
