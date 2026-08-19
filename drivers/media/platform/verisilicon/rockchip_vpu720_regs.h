/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Rockchip VPU720 JPEG decoder register definitions
 *
 * Derived from downstream Rockchip MPP HAL (hal_jpegd_rkv_reg.h).
 * Copyright (C) 2020 Rockchip Electronics Co., Ltd.
 * Copyright (C) 2026 WolfVision GmbH
 */
#ifndef ROCKCHIP_VPU720_REGS_H_
#define ROCKCHIP_VPU720_REGS_H_

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/align.h>
#include <linux/types.h>

/* ------------------------------------------------------------------ */
/* Register byte offsets from dec_base                                 */
/* ------------------------------------------------------------------ */

/* REG0: IP version / product ID */
#define VDPU720_REG_VERSION		0x000
#define VDPU720_PROD_NUM		GENMASK(31, 16)
#define VDPU720_BIT_DEPTH		BIT(8)

/* REG1: Interrupt control and status */
#define VDPU720_REG_INT			0x004
#define VDPU720_DEC_E			BIT(0)
#define VDPU720_IRQ_DIS			BIT(1)
#define VDPU720_TIMEOUT_E		BIT(2)
#define VDPU720_BUF_EMPTY_E		BIT(3)
#define VDPU720_BUF_EMPTY_RELOAD	BIT(4)
#define VDPU720_SOFT_RST_EN		BIT(5)
#define VDPU720_IRQ_RAW			BIT(6)
#define VDPU720_WAIT_RESET_E		BIT(7)
#define VDPU720_IRQ			BIT(8)
#define VDPU720_DEC_RDY			BIT(9)
#define VDPU720_BUS_ERR			BIT(10)
#define VDPU720_DEC_ERR			BIT(11)
#define VDPU720_TIMEOUT			BIT(12)
#define VDPU720_BUF_EMPTY		BIT(13)
#define VDPU720_SOFT_RST_RDY		BIT(14)

/* Error status bits used to decide whether a hardware reset is needed */
#define VDPU720_ERR_MASK		(VDPU720_BUS_ERR | VDPU720_DEC_ERR | \
					 VDPU720_TIMEOUT | VDPU720_BUF_EMPTY)

/*
 * VPU720-specific IRQ clear mask.
 *
 * The VPU720 requires a two-step IRQ acknowledgment before checking
 * VDPU720_IRQ_RAW.  Compute the masked value and write it back, then
 * write 0 to fully clear.  The downstream BSP uses:
 *
 *   clr = (~(0x00fe7f40 & status)) & (0xff0180bf & status)
 *
 * Preserve "status" bits from 0xff0180bf while clearing only those
 * bits NOT already set in 0x00fe7f40.
 */
#define VDPU720_IRQ_CLR_KEEP		0xff0180bf
#define VDPU720_IRQ_CLR_COND		0x00fe7f40

/* REG2: System configuration */
#define VDPU720_REG_SYS			0x008
#define VDPU720_FORCE_SOFTRST		BIT(17)	/* set when dec_e=0 before soft-reset */
#define VDPU720_FILL_DOWN_E		BIT(24)	/* fill bottom padding rows */
#define VDPU720_FILL_RIGHT_E		BIT(25)
#define VDPU720_OUT_SEQ			BIT(26)	/* 0=raster, 1=tile */
#define VDPU720_YUV_OUT_FMT		GENMASK(29, 27)
#define VDPU720_YUV_OUT_FMT_NATIVE	0	/* no format conversion */
#define VDPU720_YUV_OUT_FMT_NV12	3	/* output as NV12 */

/* REG3: Picture dimensions (width/height in pixels, minus 1) */
#define VDPU720_REG_PIC_SIZE		0x00c
#define VDPU720_PIC_W_M1		GENMASK(15, 0)
#define VDPU720_PIC_H_M1		GENMASK(31, 16)

/* REG4: JPEG picture format */
#define VDPU720_REG_PIC_FMT		0x010
#define VDPU720_JPEG_MODE		GENMASK(2, 0)
#define VDPU720_JPEG_MODE_YUV400	0
#define VDPU720_JPEG_MODE_YUV411	1
#define VDPU720_JPEG_MODE_YUV420	2
#define VDPU720_JPEG_MODE_YUV422	3
#define VDPU720_JPEG_MODE_YUV440	4
#define VDPU720_JPEG_MODE_YUV444	5
#define VDPU720_PIX_DEPTH		GENMASK(6, 4)
#define VDPU720_PIX_DEPTH_8		0
#define VDPU720_PIX_DEPTH_12		1
/* qtables_sel: number of Q-table sets (0..3) */
#define VDPU720_QTBL_SEL		GENMASK(9, 8)
/* htables_sel: number of H-table sets (0..3) */
#define VDPU720_HTBL_SEL		GENMASK(13, 12)
/* dri_e: restart interval enable */
#define VDPU720_DRI_E			BIT(15)
/* dri_mcu_num_m1: restart interval MCU count minus 1 */
#define VDPU720_DRI_MCU_M1		GENMASK(31, 16)

/* REG5: Horizontal virtual stride */
#define VDPU720_REG_HOR_STRIDE		0x014
#define VDPU720_Y_HOR_STRIDE		GENMASK(15, 0)
#define VDPU720_UV_HOR_STRIDE		GENMASK(31, 16)

/* REG6: Vertical virtual stride (Y plane total) */
#define VDPU720_REG_Y_VSTRIDE		0x018
#define VDPU720_Y_VSTRIDE		GENMASK(31, 4)

/* REG7: Table and stride lengths */
#define VDPU720_REG_TBL_LEN		0x01c
#define VDPU720_QTBL_LEN		GENMASK(4, 0)
#define VDPU720_HTBL_MINCODE_LEN	GENMASK(12, 8)
#define VDPU720_HTBL_VALUE_LEN		GENMASK(21, 16)
/* bit 16 of the Y horizontal stride, low 16 bits live in REG5 */
#define VDPU720_Y_HOR_STRIDE_H		BIT(24)

/* REG8: Stream length and start byte */
#define VDPU720_REG_STRM_LEN		0x020
#define VDPU720_STRM_START_BYTE		GENMASK(3, 0)
#define VDPU720_STRM_LEN		GENMASK(31, 4)

/* REG9-REG13: DMA buffer base addresses (all word-sized, in IOVA units) */
#define VDPU720_REG_QTBL_BASE		0x024	/* Q-table side buffer,   64-byte aligned */
#define VDPU720_REG_HTBL_MINCODE	0x028	/* H-mincode table,       64-byte aligned */
#define VDPU720_REG_HTBL_VALUE		0x02c	/* H-value table,         64-byte aligned */
#define VDPU720_REG_STRM_BASE		0x030	/* JPEG entropy stream,   16-byte aligned */
#define VDPU720_REG_OUT_BASE		0x034	/* NV12 output buffer,    64-byte aligned */

/* REG14: Stream error handling */
#define VDPU720_REG_STRM_ERR		0x038
#define VDPU720_ERROR_PRC_MODE		BIT(0)
#define VDPU720_STRM_FFFF_ERR_MODE	GENMASK(6, 5)
#define VDPU720_STRM_OTHER_MODE		GENMASK(8, 7)
/* Recommended default: accept errors, skip 0xFFFF, skip unknown markers */
#define VDPU720_STRM_ERR_DFLT		(VDPU720_ERROR_PRC_MODE | \
					 FIELD_PREP_CONST(VDPU720_STRM_FFFF_ERR_MODE, 2) | \
					 FIELD_PREP_CONST(VDPU720_STRM_OTHER_MODE, 2))

/* REG16: Clock gate (write 0xff to enable all internal clocks) */
#define VDPU720_REG_CLK_GATE		0x040
#define VDPU720_CLK_GATE_ALL		0xff

/* REG30: AXI performance counter control */
#define VDPU720_REG_PERF_CTRL		0x078
#define VDPU720_PERF_WORK_E		BIT(0)
#define VDPU720_PERF_CLR_E		BIT(1)
#define VDPU720_PERF_CNT_TYPE		BIT(3)
#define VDPU720_PERF_RD_LAT_ID		GENMASK(7, 4)

/*
 * REG31: performance-counter channel select.
 *
 * sw_ar/aw_count_id select which AXI channel the performance counters track.
 * The driver leaves this register alone, as does the reference driver.
 */
#define VDPU720_REG_AXI_CFG		0x07c
#define VDPU720_ADDR_ALIGN_TYPE		GENMASK(1, 0)
#define VDPU720_AR_CNT_ID_TYPE		BIT(2)	/* 1 = count sw_ar_count_id only */
#define VDPU720_AW_CNT_ID_TYPE		BIT(3)	/* 1 = count sw_aw_count_id only */
#define VDPU720_AR_COUNT_ID		GENMASK(7, 4)
#define VDPU720_AW_COUNT_ID		GENMASK(11, 8)
#define VDPU720_RD_TOTAL_BYTES_MODE	BIT(12)	/* 1 = count sw_ar_count_id bytes only */

/* REG32: MCU position when first decode error occurred (read-only) */
#define VDPU720_REG_DBG_MCU_POS		0x080
#define VDPU720_DBG_MCU_POS_X		GENMASK(15, 0)	/* column in MCU units */
#define VDPU720_DBG_MCU_POS_Y		GENMASK(31, 16)	/* row in MCU units */

/*
 * REG33: Detailed JPEG decode error flags.
 *
 * All bits are RW (write-to-clear); read them in the IRQ handler when
 * VDPU720_DEC_ERR is set to identify exactly what went wrong.
 */
#define VDPU720_REG_DBG_ERROR		0x084
#define VDPU720_DERR_DRI_SEQ		BIT(0)	/* DRI not at expected sequence */
#define VDPU720_DERR_STREAM_R0		BIT(1)	/* special marker 0 detected */
#define VDPU720_DERR_STREAM_R1		BIT(2)	/* special marker 1 detected */
#define VDPU720_DERR_STREAM_FFFF	BIT(3)	/* 0xFFFF sequence in stream */
#define VDPU720_DERR_OTHER_MARK		BIT(4)	/* unknown JPEG marker */
#define VDPU720_DERR_MCU_CNT_L		BIT(8)	/* restart mark arrived too early */
#define VDPU720_DERR_MCU_CNT_M		BIT(9)	/* restart mark arrived too late */
#define VDPU720_DERR_EOI_NO_END		BIT(10)	/* EOI before frame complete */
#define VDPU720_DERR_END_NO_EOI		BIT(11)	/* frame complete without EOI */
#define VDPU720_DERR_OVERFLOW		BIT(12)	/* Huffman coefficient overflow */
#define VDPU720_DERR_HUFF_EMPTY		BIT(13)	/* bitstream empty before EOI */
#define VDPU720_DERR_FLAGS		GENMASK(13, 0)	/* all of the above */
#define VDPU720_DERR_FIRST_IDX		GENMASK(19, 16)	/* index of first error */

/*
 * REG34-REG38: AXI bus performance counters.
 *
 * Read after decode completes (in the IRQ handler) to measure actual
 * memory bandwidth consumed per frame.  Counters are reset each frame
 * by VDPU720_PERF_CLR_E in REG30.  All registers are 32-bit read-only.
 */
#define VDPU720_REG_PERF_RD_MAX_LAT	0x088	/* peak read latency (clock cycles) */
#define VDPU720_REG_PERF_RD_LAT_SAMP	0x08c	/* read transactions above lat threshold */
#define VDPU720_REG_PERF_RD_LAT_ACC	0x090	/* accumulated read latency sum */
#define VDPU720_REG_PERF_RD_BYTES	0x094	/* total AXI read bytes this frame */
#define VDPU720_REG_PERF_WR_BYTES	0x098	/* total AXI write bytes this frame */

/* REG39: Hardware working cycle counter (counts clock cycles HW was active) */
#define VDPU720_REG_PERF_CYCLES		0x09c

/* ------------------------------------------------------------------ */
/* Side-buffer layout for Q-tables and Huffman tables                  */
/*                                                                     */
/* The VPU720 JPEG decoder reads quantisation tables and Huffman       */
/* tables from a contiguous DMA buffer with the following layout:      */
/*                                                                     */
/*   [0,         QTBL_SIZE):    Q-table data (u16, raster-scan order) */
/*   [HMINCODE_OFF, +HMIN_SZ):  Huffman mincode table                 */
/*   [HVALUE_OFF,  +HVAL_SZ):   Huffman value table                   */
/* ------------------------------------------------------------------ */
/* The Q-tables are per component, one entry each */
#define VDPU720_NB_COMPONENTS		3
#define VDPU720_QTBL_ENTRIES		64	/* 64 coefficients per table */
#define VDPU720_QTBL_COMP_SIZE		(VDPU720_QTBL_ENTRIES * sizeof(u16))
#define VDPU720_QTBL_SIZE		(VDPU720_QTBL_COMP_SIZE * VDPU720_NB_COMPONENTS)

/*
 * The Huffman tables are not per component.  The hardware holds two sets and
 * has no per component selector register, so the mapping is fixed: the first
 * set is used for the luma component and the second one for both chroma
 * components.  A grayscale frame only needs the first.
 */
#define VDPU720_NB_HTBL_SETS		2

/*
 * Per-set mincode layout: 16 DC mincodes + 8 DC accaddr pairs +
 * 16 AC mincodes + 8 AC accaddr pairs = 48 u16 = 96 bytes
 */
#define VDPU720_HMINCODE_SET_SIZE	(48 * sizeof(u16))
#define VDPU720_HMINCODE_SIZE		(VDPU720_HMINCODE_SET_SIZE * VDPU720_NB_HTBL_SETS)
#define VDPU720_HMINCODE_OFF		VDPU720_QTBL_SIZE

/* Per-set value layout: 16 DC values + 176 AC values = 192 bytes */
#define VDPU720_HVALUE_SET_SIZE		192
#define VDPU720_HVALUE_SIZE		(VDPU720_HVALUE_SET_SIZE * VDPU720_NB_HTBL_SETS)
#define VDPU720_HVALUE_OFF		(VDPU720_HMINCODE_OFF + \
					 ALIGN(VDPU720_HMINCODE_SIZE, 64))

#define VDPU720_TABLE_BUF_SIZE		(VDPU720_HVALUE_OFF + VDPU720_HVALUE_SIZE)

/*
 * The three table length registers count 16 byte units, minus one.  Derive
 * them from the sizes above so that what is programmed always matches what
 * the driver writes into the side buffer.
 */
#define VDPU720_TBL_LEN_UNIT		16
#define VDPU720_TBL_LEN(bytes)		((bytes) / VDPU720_TBL_LEN_UNIT - 1)

/*
 * Huffman value sub-layout per set (192 bytes total):
 *   bytes [0..15]:   DC code values (up to 12 valid entries)
 *   bytes [16..191]: AC code values (up to 162 valid entries)
 */
#define VDPU720_DC_VALUES_MAX		16
#define VDPU720_AC_VALUES_MAX		176	/* 12*16 - 16 */

#endif /* ROCKCHIP_VPU720_REGS_H_ */
