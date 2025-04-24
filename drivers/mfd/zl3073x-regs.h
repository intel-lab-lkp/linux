/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ZL3073X_REGS_H
#define __ZL3073X_REGS_H

#include <linux/bitfield.h>
#include <linux/bits.h>

/*
 * Register address structure:
 * ===========================
 *  25        19 18   16 15     7 6           0
 * +-------------------------------------------+
 * | max_offset | width |  page  | page_offset |
 * +-------------------------------------------+
 *
 * page_offset ... <0x00..0x7F>
 * page .......... HW page number
 * size .......... register byte size (1, 2, 4 or 6)
 * max_offset .... maximal offset for indexed registers
 *                 (for non-indexed regs max_offset == page_offset)
 */

#define ZL_REG_OFFSET_MASK	GENMASK(6, 0)
#define ZL_REG_PAGE_MASK	GENMASK(15, 7)
#define ZL_REG_SIZE_MASK	GENMASK(18, 16)
#define ZL_REG_MAX_OFFSET_MASK	GENMASK(25, 19)
#define ZL_REG_ADDR_MASK	GENMASK(15, 0)

#define ZL_REG_OFFSET(_reg)	FIELD_GET(ZL_REG_OFFSET_MASK, _reg)
#define ZL_REG_MAX_OFFSET(_reg)	FIELD_GET(ZL_REG_MAX_OFFSET_MASK, _reg)
#define ZL_REG_SIZE(_reg)	FIELD_GET(ZL_REG_SIZE_MASK, _reg)
#define ZL_REG_ADDR(_reg)	FIELD_GET(ZL_REG_ADDR_MASK, _reg)

/**
 * ZL_REG_IDX - define indexed register
 * @_idx: index of register to access
 * @_page: register page
 * @_offset: register offset in page
 * @_size: register byte size (1, 2, 4 or 6)
 * @_items: number of register indices
 * @_stride: stride between items in bytes
 *
 * All parameters except @_idx should be constant.
 */
#define ZL_REG_IDX(_idx, _page, _offset, _size, _items, _stride)	\
	(FIELD_PREP(ZL_REG_OFFSET_MASK,					\
		    (_offset) + (_idx) * (_stride))		|	\
	 FIELD_PREP_CONST(ZL_REG_PAGE_MASK, _page)		|	\
	 FIELD_PREP_CONST(ZL_REG_SIZE_MASK, _size)		|	\
	 FIELD_PREP_CONST(ZL_REG_MAX_OFFSET_MASK,			\
			  (_offset) + ((_items) - 1) * (_stride)))

/**
 * ZL_REG - define simple (non-indexed) register
 * @_page: register page
 * @_offset: register offset in page
 * @_size: register byte size (1, 2, 4 or 6)
 *
 * All parameters should be constant.
 */
#define ZL_REG(_page, _offset, _size)					\
	ZL_REG_IDX(0, _page, _offset, _size, 1, 0)

/**************************
 * Register Page 0, General
 **************************/

#define ZL_REG_ID				ZL_REG(0, 0x01, 2)
#define ZL_REG_REVISION				ZL_REG(0, 0x03, 2)
#define ZL_REG_FW_VER				ZL_REG(0, 0x05, 2)
#define ZL_REG_CUSTOM_CONFIG_VER		ZL_REG(0, 0x07, 4)

/*******************************
 * Register Page 10, Ref Mailbox
 *******************************/

#define ZL_REG_REF_MB_MASK			ZL_REG(10, 0x02, 2)

#define ZL_REG_REF_MB_SEM			ZL_REG(10, 0x04, 1)
#define ZL_REF_MB_SEM_WR			BIT(0)
#define ZL_REF_MB_SEM_RD			BIT(1)

#define ZL_REG_REF_FREQ_BASE			ZL_REG(10, 0x05, 2)
#define ZL_REG_REF_FREQ_MULT			ZL_REG(10, 0x07, 2)
#define ZL_REG_REF_RATIO_M			ZL_REG(10, 0x09, 2)
#define ZL_REG_REF_RATIO_N			ZL_REG(10, 0x0b, 2)
#define ZL_REG_REF_CONFIG			ZL_REG(10, 0x0d, 1)
#define ZL_REG_REF_PHASE_OFFSET_COMP		ZL_REG(10, 0x28, 6)
#define ZL_REG_REF_SYNC_CTRL			ZL_REG(10, 0x2e, 1)
#define ZL_REG_REF_ESYNC_DIV			ZL_REG(10, 0x30, 4)

/********************************
 * Register Page 12, DPLL Mailbox
 ********************************/

#define ZL_REG_DPLL_MB_MASK			ZL_REG(12, 0x02, 2)

#define ZL_REG_DPLL_MB_SEM			ZL_REG(12, 0x04, 1)
#define ZL_DPLL_MB_SEM_WR			BIT(0)
#define ZL_DPLL_MB_SEM_RD			BIT(1)

#define ZL_REG_DPLL_REF_PRIO(_idx)					\
	ZL_REG_IDX(_idx, 12, 0x52, 1, ZL3073X_NUM_INPUTS / 2, 1)

/*********************************
 * Register Page 13, Synth Mailbox
 *********************************/

#define ZL_REG_SYNTH_MB_MASK			ZL_REG(13, 0x02, 2)

#define ZL_REG_SYNTH_MB_SEM			ZL_REG(13, 0x04, 1)
#define ZL_SYNTH_MB_SEM_WR			BIT(0)
#define ZL_SYNTH_MB_SEM_RD			BIT(1)

#define ZL_REG_SYNTH_FREQ_BASE			ZL_REG(13, 0x06, 2)
#define ZL_REG_SYNTH_FREQ_MULT			ZL_REG(13, 0x08, 4)
#define ZL_REG_SYNTH_FREQ_M			ZL_REG(13, 0x0c, 2)
#define ZL_REG_SYNTH_FREQ_N			ZL_REG(13, 0x0e, 2)

/**********************************
 * Register Page 14, Output Mailbox
 **********************************/

#define ZL_REG_OUTPUT_MB_MASK			ZL_REG(14, 0x02, 2)

#define ZL_REG_OUTPUT_MB_SEM			ZL_REG(14, 0x04, 1)
#define ZL_OUTPUT_MB_SEM_WR			BIT(0)
#define ZL_OUTPUT_MB_SEM_RD			BIT(1)

#define ZL_REG_OUTPUT_MODE			ZL_REG(14, 0x05, 1)
#define ZL_REG_OUTPUT_DIV			ZL_REG(14, 0x0c, 4)
#define ZL_REG_OUTPUT_WIDTH			ZL_REG(14, 0x10, 4)
#define ZL_REG_OUTPUT_ESYNC_PERIOD		ZL_REG(14, 0x14, 4)
#define ZL_REG_OUTPUT_ESYNC_WIDTH		ZL_REG(14, 0x18, 4)
#define ZL_REG_OUTPUT_PHASE_COMP		ZL_REG(14, 0x20, 4)

#endif /* __ZL3073X_REGS_H */
