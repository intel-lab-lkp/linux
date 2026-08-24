// SPDX-License-Identifier: (GPL-2.0-or-later OR MIT)
/*
 * Rockchip ISP2 Driver - Params subdevice
 *
 * Copyright (C) 2017 Rockchip Electronics Co., Ltd.
 * Copyright (C) 2026 Ideas on Board Oy.
 */

#include <linux/bitfield.h>
#include <linux/build_bug.h>
#include <linux/math.h>
#include <linux/string.h>

#include <media/v4l2-common.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-isp.h>
#include <media/v4l2-rect.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-vmalloc.h>

#include "rkisp2-common.h"

#define RKISP2_PARAMS_DEV_NAME	RKISP2_DRIVER_NAME "_params"

#define RKISP2_PARAMS_BLOCK_INFO(block, data) \
	[RKISP2_PARAMS_BLOCK_ ## block] = { \
		.size = sizeof(struct rkisp2_params_ ## data ), \
	}

#define RKISP2_PARAMS_BLOCK_HANDLER_INFO(block, handler_postfix, prio_postfix) \
	[RKISP2_PARAMS_BLOCK_ ## block] = { \
		.handler = rkisp2_params_ ## handler_postfix,\
		.priority = RKISP2_PARAMS_CONFIG_PRIO_ ## prio_postfix, \
	}

union rkisp2_params_block {
	const struct v4l2_isp_params_block_header *header;
	const struct rkisp2_params_bls *bls;
	const struct rkisp2_params_awb_gains *awb_gains;
	const struct rkisp2_params_csm *csm;
	const struct rkisp2_params_ccm *ccm;
	const struct rkisp2_params_goc *goc;
	const struct rkisp2_params_lsc *lsc;
	const struct rkisp2_params_crop *crop;
	const __u8 *data;
};

static void rkisp2_params_bls(struct rkisp2_params *params,
			      union rkisp2_params_block block);
static void rkisp2_params_awb_gains(struct rkisp2_params *params,
				    union rkisp2_params_block block);
static void rkisp2_params_csm(struct rkisp2_params *params,
			      union rkisp2_params_block block);
static void rkisp2_params_ccm(struct rkisp2_params *params,
			      union rkisp2_params_block block);
static void rkisp2_params_goc(struct rkisp2_params *params,
			      union rkisp2_params_block block);
static void rkisp2_params_lsc(struct rkisp2_params *params,
			      union rkisp2_params_block block);
static void rkisp2_params_crop(struct rkisp2_params *params,
			       union rkisp2_params_block block);

typedef void (*rkisp2_params_handler)(struct rkisp2_params *params,
				      const union rkisp2_params_block block);

enum rkisp2_params_configure_priority {
	RKISP2_PARAMS_CONFIG_PRIO_NONE = 0,
	RKISP2_PARAMS_CONFIG_PRIO_PRE,
	RKISP2_PARAMS_CONFIG_PRIO_POST,
};

struct rkisp2_params_block_handler_info {
	rkisp2_params_handler handler;
	enum rkisp2_params_configure_priority priority;
};

static const struct rkisp2_params_block_handler_info
rkisp2_params_handlers[] = {
	RKISP2_PARAMS_BLOCK_HANDLER_INFO(BLS, bls, PRE),
	RKISP2_PARAMS_BLOCK_HANDLER_INFO(AWB_GAINS, awb_gains, PRE),
	RKISP2_PARAMS_BLOCK_HANDLER_INFO(CSM, csm, PRE),
	RKISP2_PARAMS_BLOCK_HANDLER_INFO(CCM, ccm, PRE),
	RKISP2_PARAMS_BLOCK_HANDLER_INFO(GOC, goc, PRE),
	RKISP2_PARAMS_BLOCK_HANDLER_INFO(LSC, lsc, POST),
	RKISP2_PARAMS_BLOCK_HANDLER_INFO(CROP, crop, PRE),
};

static const struct v4l2_isp_params_block_type_info
rkisp2_params_block_types_info[] = {
	RKISP2_PARAMS_BLOCK_INFO(BLS, bls),
	RKISP2_PARAMS_BLOCK_INFO(AWB_GAINS, awb_gains),
	RKISP2_PARAMS_BLOCK_INFO(CSM, csm),
	RKISP2_PARAMS_BLOCK_INFO(CCM, ccm),
	RKISP2_PARAMS_BLOCK_INFO(GOC, goc),
	RKISP2_PARAMS_BLOCK_INFO(LSC, lsc),
	RKISP2_PARAMS_BLOCK_INFO(CROP, crop),
};

static_assert(ARRAY_SIZE(rkisp2_params_handlers) ==
	      ARRAY_SIZE(rkisp2_params_block_types_info));

static inline void
rkisp2_param_set_bits(struct rkisp2_params *params, u32 reg, u32 bit_mask)
{
	u32 val;

	val = rkisp2_read(params->rkisp2, reg);
	rkisp2_write(params->rkisp2, reg, val | bit_mask);
}

static inline void
rkisp2_param_clear_bits(struct rkisp2_params *params, u32 reg, u32 bit_mask)
{
	u32 val;

	val = rkisp2_read(params->rkisp2, reg);
	rkisp2_write(params->rkisp2, reg, val & ~bit_mask);
}

static void rkisp2_params_bls(struct rkisp2_params *params,
			      union rkisp2_params_block block)
{
	const struct rkisp2_params_bls *arg = block.bls;
	u32 control;

	if (block.header->flags & V4L2_ISP_PARAMS_FL_BLOCK_DISABLE) {
		rkisp2_param_clear_bits(params, ISP_BLS_CTRL, ISP_BLS_ENA | ISP_BLS_BLS1_EN);
		return;
	}

	if (!(block.header->flags & V4L2_ISP_PARAMS_FL_BLOCK_ENABLE))
		return;

	control = ISP_BLS_ENA;

	if (!arg->enable_auto) {
		/* TODO plumb BLS1 */

		rkisp2_write(params->rkisp2, ISP_BLS_A_FIXED, arg->bls_fixed_val.a);
		rkisp2_write(params->rkisp2, ISP_BLS_B_FIXED, arg->bls_fixed_val.b);
		rkisp2_write(params->rkisp2, ISP_BLS_C_FIXED, arg->bls_fixed_val.c);
		rkisp2_write(params->rkisp2, ISP_BLS_D_FIXED, arg->bls_fixed_val.d);

		/* Set fixed mode */
		rkisp2_write(params->rkisp2, ISP_BLS_CTRL, control);
		return;
	}

	if (arg->enabled_windows & BIT(1)) {
		rkisp2_write(params->rkisp2, ISP_BLS_H2_START,
			     arg->bls_window2.h_offs);
		rkisp2_write(params->rkisp2, ISP_BLS_H2_STOP,
			     arg->bls_window2.h_size);
		rkisp2_write(params->rkisp2, ISP_BLS_V2_START,
			     arg->bls_window2.v_offs);
		rkisp2_write(params->rkisp2, ISP_BLS_V2_STOP,
			     arg->bls_window2.v_size);
		control |= ISP_BLS_WINDOW_2;
	}

	if (arg->enabled_windows & BIT(0)) {
		rkisp2_write(params->rkisp2, ISP_BLS_H1_START,
			     arg->bls_window1.h_offs);
		rkisp2_write(params->rkisp2, ISP_BLS_H1_STOP,
			     arg->bls_window1.h_size);
		rkisp2_write(params->rkisp2, ISP_BLS_V1_START,
			     arg->bls_window1.v_offs);
		rkisp2_write(params->rkisp2, ISP_BLS_V1_STOP,
			     arg->bls_window1.v_size);
		control |= ISP_BLS_WINDOW_1;
	}

	rkisp2_write(params->rkisp2, ISP_BLS_SAMPLES,
		     arg->bls_samples);

	control |= ISP_BLS_MODE_MEASURED;
	rkisp2_write(params->rkisp2, ISP_BLS_CTRL, control);
}

static void rkisp2_params_awb_gains(struct rkisp2_params *params,
				    union rkisp2_params_block block)
{
	const struct rkisp2_params_awb_gains *arg = block.awb_gains;
	unsigned int i;

	if (block.header->flags & V4L2_ISP_PARAMS_FL_BLOCK_DISABLE) {
		rkisp2_param_clear_bits(params, RKISP2_CIF_ISP_CTRL,
					RKISP2_CIF_ISP_CTRL_ISP_AWB_ENA);
		return;
	}

	if (!(block.header->flags & V4L2_ISP_PARAMS_FL_BLOCK_ENABLE))
		return;

	for (i = 0; i < ARRAY_SIZE(arg->gains); i++) {
		rkisp2_write(params->rkisp2, ISP21_AWB_GAIN0_G + i * 8,
			     arg->gains[i].gr << 16 | arg->gains[i].gb);
		rkisp2_write(params->rkisp2, ISP21_AWB_GAIN0_RB + i * 8,
			     arg->gains[i].r << 16 | arg->gains[i].b);
	}

	rkisp2_param_set_bits(params, RKISP2_CIF_ISP_CTRL,
			      RKISP2_CIF_ISP_CTRL_ISP_AWB_ENA);
}

static void rkisp2_params_csm_reset(struct rkisp2_params *params)
{
	/* Write back the default values. */
	rkisp2_write(params->rkisp2, RKISP2_CIF_ISP_CC_COEFF_0, 0x80);
	rkisp2_write(params->rkisp2, RKISP2_CIF_ISP_CC_COEFF_1, 0);
	rkisp2_write(params->rkisp2, RKISP2_CIF_ISP_CC_COEFF_2, 0);
	rkisp2_write(params->rkisp2, RKISP2_CIF_ISP_CC_COEFF_3, 0);
	rkisp2_write(params->rkisp2, RKISP2_CIF_ISP_CC_COEFF_4, 0x80);
	rkisp2_write(params->rkisp2, RKISP2_CIF_ISP_CC_COEFF_5, 0);
	rkisp2_write(params->rkisp2, RKISP2_CIF_ISP_CC_COEFF_6, 0);
	rkisp2_write(params->rkisp2, RKISP2_CIF_ISP_CC_COEFF_7, 0);
	rkisp2_write(params->rkisp2, RKISP2_CIF_ISP_CC_COEFF_8, 0x80);
}

static void rkisp2_params_csm(struct rkisp2_params *params,
			      union rkisp2_params_block block)
{
	const struct rkisp2_params_csm *arg = block.csm;
	unsigned int i, j, k = 0;

	if (block.header->flags & V4L2_ISP_PARAMS_FL_BLOCK_DISABLE) {
		rkisp2_params_csm_reset(params);
		return;
	}

	if (!(block.header->flags & V4L2_ISP_PARAMS_FL_BLOCK_ENABLE))
		return;

	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			rkisp2_write(params->rkisp2,
				     RKISP2_CIF_ISP_CC_COEFF_0 + 4 * k++,
				     arg->coeff[i][j]);
}

static void rkisp2_params_ccm(struct rkisp2_params *params,
			      union rkisp2_params_block block)
{
	const struct rkisp2_params_ccm *arg = block.ccm;
	unsigned int i;
	u32 control = 0;

	if (block.header->flags & V4L2_ISP_PARAMS_FL_BLOCK_DISABLE) {
		rkisp2_param_clear_bits(params, ISP_CCM_CTRL, ISP_CCM_EN);
		return;
	}

	if (!(block.header->flags & V4L2_ISP_PARAMS_FL_BLOCK_ENABLE))
		return;

	for (i = 0; i < 3; i++) {
		rkisp2_write(params->rkisp2, ISP_CCM_COEFF0_R + 8 * i,
			     ISP3X_CCM_COEFF(arg->coeff[i][0], arg->coeff[i][1]));
		rkisp2_write(params->rkisp2, ISP_CCM_COEFF1_R + 8 * i,
			     ISP3X_CCM_COEFF(arg->coeff[i][2], arg->offset[i]));
	}

	rkisp2_write(params->rkisp2, ISP_CCM_COEFF0_Y,
		     ISP3X_CCM_COEFF(arg->y_coeff[0], arg->y_coeff[1]));
	rkisp2_write(params->rkisp2, ISP_CCM_COEFF1_Y, arg->y_coeff[2]);

	for (i = 0; i < 8; i++)
		rkisp2_write(params->rkisp2, ISP_CCM_ALP_Y0 + 4 * i,
			     ISP3X_CCM_COEFF(arg->alp[2 * i], arg->alp[2 * i + 1]));
	rkisp2_write(params->rkisp2, ISP_CCM_ALP_Y8, arg->alp[16]);

	rkisp2_write(params->rkisp2, ISP_CCM_BOUND_BIT, arg->inflection_point);

	if (!arg->high_y_alpha_adj_en)
		control = ISP3X_CCM_HIGHY_ADJ_DIS;
	control |= ISP_CCM_EN;
	rkisp2_write(params->rkisp2, ISP_CCM_CTRL, control);
}

static void rkisp2_params_goc(struct rkisp2_params *params,
			      union rkisp2_params_block block)
{
	const struct rkisp2_params_goc *arg = block.goc;
	unsigned int i;
	u32 control = 0;

	if (block.header->flags & V4L2_ISP_PARAMS_FL_BLOCK_DISABLE) {
		rkisp2_param_clear_bits(params, ISP3X_GAMMA_OUT_CTRL,
					ISP3X_GAMMA_OUT_CTRL_EN);
		return;
	}

	if (!(block.header->flags & V4L2_ISP_PARAMS_FL_BLOCK_ENABLE))
		return;

	for (i = 0; i < RKISP2_ISP_GAMMA_OUT_MAX_SEGMENTS >> 1; i++)
		rkisp2_write(params->rkisp2,
			     ISP3X_GAMMA_OUT_Y0 + 4 * i,
			     ISP3X_GAMMA_OUT_SAMPLE(arg->gamma_y[2 * i],
						  arg->gamma_y[2 * i + 1]));

	rkisp2_write(params->rkisp2, ISP3X_GAMMA_OUT_Y24,
		     arg->gamma_y[RKISP2_ISP_GAMMA_OUT_MAX_SEGMENTS - 1]);

	rkisp2_write(params->rkisp2, ISP3X_GAMMA_OUT_OFFSET, arg->offset);

	if (arg->mode == RKISP2_ISP_GOC_MODE_EQUIDISTANT)
		control = ISP3X_GAMMA_OUT_CTRL_MODE_EQUIDISTANT;
	if (arg->mode == RKISP2_ISP_GOC_SEGMENTS_48)
		control |= ISP3X_GAMMA_OUT_CTRL_SEGMENTS_48;
	control |= ISP3X_GAMMA_OUT_CTRL_EN;
	rkisp2_write(params->rkisp2, ISP3X_GAMMA_OUT_CTRL, control);
}

static void rkisp2_params_lsc(struct rkisp2_params *params,
			      union rkisp2_params_block block)
{
	const struct rkisp2_params_lsc *arg = block.lsc;
	struct rkisp2_device *rkisp2 = params->rkisp2;
	u32 sram_addr;
	u32 data;
	unsigned int i, j, table_i;

	if (block.header->flags & V4L2_ISP_PARAMS_FL_BLOCK_DISABLE) {
		rkisp2_param_clear_bits(params, ISP3X_LSC_CTRL, ISP3X_LSC_CTRL_EN);
		return;
	}

	if (!(block.header->flags & V4L2_ISP_PARAMS_FL_BLOCK_ENABLE))
		return;

	if (arg->set_active_table_when == RKISP2_ISP_LSC_SET_ACTIVE_TABLE_BEFORE)
		rkisp2_write(rkisp2, ISP3X_LSC_TABLE_SEL, arg->active_table ? 1 : 0);

	/*
	 * - No need to disable the lsc before writing the table 
	 * - Table 0 starts at 0, table 1 starts at 153.
	 * - TABLE_SEL selects which table is active, but programming the tables
	 *   is done by just writing to the right address.
	 * - The address automatically increments, so no need to increment it.
	 */

	for (table_i = 0; table_i < 2; table_i++) {
		if (!arg->write_table[table_i])
			continue;

		sram_addr = table_i * 153;

		rkisp2_write(rkisp2, ISP3X_LSC_R_TABLE_ADDR, sram_addr);
		rkisp2_write(rkisp2, ISP3X_LSC_GR_TABLE_ADDR, sram_addr);
		rkisp2_write(rkisp2, ISP3X_LSC_B_TABLE_ADDR, sram_addr);
		rkisp2_write(rkisp2, ISP3X_LSC_GB_TABLE_ADDR, sram_addr);

		/* Program data tables (table size is 9 * 17 = 153) */
		for (i = 0; i < RKISP2_ISP_LSC_SAMPLES_MAX; i++) {
			const __u16 *r_row = arg->r_data_tbl[table_i][i];
			const __u16 *gr_row = arg->gr_data_tbl[table_i][i];
			const __u16 *gb_row = arg->gb_data_tbl[table_i][i];
			const __u16 *b_row = arg->b_data_tbl[table_i][i];

			/*
			 * 17 sectors with 2 values in one DWORD = 9
			 * DWORDs (2nd value of last DWORD unused)
			 */
			for (j = 0; j < RKISP2_ISP_LSC_SAMPLES_MAX / 2; j++) {
				rkisp2_write(rkisp2, ISP3X_LSC_R_TABLE_DATA,
					     ISP3X_LSC_TABLE_DATA(r_row[2 * j], r_row[2 * j + 1]));
				rkisp2_write(rkisp2, ISP3X_LSC_GR_TABLE_DATA,
					     ISP3X_LSC_TABLE_DATA(gr_row[2 * j], gr_row[2 * j + 1]));
				rkisp2_write(rkisp2, ISP3X_LSC_GB_TABLE_DATA,
					     ISP3X_LSC_TABLE_DATA(gb_row[2 * j], gb_row[2 * j + 1]));
				rkisp2_write(rkisp2, ISP3X_LSC_B_TABLE_DATA,
					     ISP3X_LSC_TABLE_DATA(b_row[2 * j], b_row[2 * j + 1]));
			}

			rkisp2_write(rkisp2, ISP3X_LSC_R_TABLE_DATA,
				     ISP3X_LSC_TABLE_DATA(r_row[2 * j], 0));
			rkisp2_write(rkisp2, ISP3X_LSC_GR_TABLE_DATA,
				     ISP3X_LSC_TABLE_DATA(gr_row[2 * j], 0));
			rkisp2_write(rkisp2, ISP3X_LSC_GB_TABLE_DATA,
				     ISP3X_LSC_TABLE_DATA(gb_row[2 * j], 0));
			rkisp2_write(rkisp2, ISP3X_LSC_B_TABLE_DATA,
				     ISP3X_LSC_TABLE_DATA(b_row[2 * j], 0));

		}
	}

	/* Program grid sizes and interpolation gradients */
	for (i = 0; i < RKISP2_ISP_LSC_SECTORS_TBL_SIZE_MAX / 2; i++) {
		/* program x size tables */
		data = ISP3X_LSC_SECT_SIZE(arg->x_sizes[i * 2],
					   arg->x_sizes[i * 2 + 1]);
		rkisp2_write(rkisp2, ISP3X_LSC_XSIZE(i), data);

		/* program x grad tables */
		data = ISP3X_LSC_GRAD_SIZE(arg->x_grads[i * 2],
					   arg->x_grads[i * 2 + 1]);
		rkisp2_write(rkisp2, ISP3X_LSC_XGRAD(i), data);

		/* program y size tables */
		data = ISP3X_LSC_SECT_SIZE(arg->y_sizes[i * 2],
					   arg->y_sizes[i * 2 + 1]);
		rkisp2_write(rkisp2, ISP3X_LSC_YSIZE(i), data);

		/* program y grad tables */
		data = ISP3X_LSC_GRAD_SIZE(arg->y_grads[i * 2],
					   arg->y_grads[i * 2 + 1]);
		rkisp2_write(rkisp2, ISP3X_LSC_YGRAD(i), data);
	}

	rkisp2_write(rkisp2, ISP3X_LSC_TABLE_SEL, arg->active_table ? 1 : 0);

	data = 0;
	if (arg->window_mode)
		data |= ISP3X_LSC_SECTOR_16X16;
	/* TODO plumb the rest of the ctrl fields */
	data |= ISP3X_LSC_CTRL_EN;

	rkisp2_param_set_bits(params, ISP3X_LSC_CTRL, data);
}

static void __rkisp2_params_crop(struct rkisp2_params *params,
				 const struct rkisp2_params_crop *arg,
				 __u16 flags)
{
	unsigned int i;

	/*
	 * Based on the params provided by userspace and the ISP stream
	 * configurations and validation, determine the input and output
	 * rectangles. Then delegate to the resizer routines about how to
	 * configure the dual crop + resizer.
	 *
	 * This handler has a different pattern from the other params handlers
	 * because the crop + resizer is always active (depending on the stream
	 * configuration), and the hardware configuration is only adjusted
	 * based on the parameters.
	 */

	if (!(flags & V4L2_ISP_PARAMS_FL_BLOCK_ENABLE) &&
	    !(flags & V4L2_ISP_PARAMS_FL_BLOCK_DISABLE))
		return;

	for (i = 0; i < 2; i++) {
		struct rkisp2_resizer *rsz = &params->rkisp2->resizer_devs[i];

		const struct rkisp2_isp_window *window =
			(i == 0 ? &arg->mp_crop : &arg->sp_crop);
		u16 en_mask =
			(i == 0 ? RKISP2_ISP_CROP_ENABLE_MAIN :
			 	  RKISP2_ISP_CROP_ENABLE_SELF);

		/*
		 * This is input of the combined dual crop + resizer. When crop
		 * is disabled (either by crop_en flags or by disabling the
		 * entire block), the input size will instead be set to equal
		 * ISP sink size (corresponding to output of IS). They are
		 * clamped to the min/max sizes.
		 */
		struct v4l2_rect crop = {
			.left = window->h_offs,
			.top = window->v_offs,
			.width = window->h_size,
			.height = window->v_size
		};

		/*
		 * Resizer (and dual crop if applicable) always needs to be
		 * handled to do format conversion and resizing to achieve the
		 * requested format type and size for the stream.
		 */
		if (!(arg->crop_en & en_mask) ||
		    (flags & V4L2_ISP_PARAMS_FL_BLOCK_DISABLE)) {
			crop = rsz->sink_size;
		}

		/*
		 * Resizer does not work in raw mode but dual crop does, so
		 * make crop = source_size if we are in raw mode. If there is
		 * no dual crop then whatever.
		 *
		 * TODO Use image stabilizer to do the correction in the case
		 * that there is no dual crop. This logic should be in the
		 * resizer code.
		 */
		if (!v4l2_is_format_yuv(rsz->source_fmt_info)) {
			/* Match the crop size to the source size first */
			if (crop.width != rsz->source_size.width || crop.height != rsz->source_size.height) {
				crop.width = rsz->source_size.width;
				crop.height = rsz->source_size.height;
			}
		}

		/*
		 * Out-of-bounds rectangles just produces garbage data and
		 * doesn't break the hardware so don't correct for it.
		 */

		/*
		 * TODO Invalidate invalid stream configurations in rkisp2_params_vb2_buf_prepare
		 *
		 * We should validate configurations that would cause
		 * memory-related or hardware-breaking errors in buf_prepare
		 * (like crop != source_size on raw mode). Other non-breaking
		 * errors we can just forward the parameters to hardware as-is
		 * and let userspace deal with the consequences. They shouldn't
		 * be setting bad parameters (as long as it doesn't break the
		 * system).
		 */

		rkisp2_resizer_configure(rsz, &crop);
	}
}

static void rkisp2_params_crop(struct rkisp2_params *params,
			       union rkisp2_params_block block)
{
	const struct rkisp2_params_crop *arg = block.crop;
	__rkisp2_params_crop(params, arg, block.header->flags);
}

static void rkisp2_params_crop_init(struct rkisp2_params *params)
{
	struct rkisp2_device *rkisp2 = params->rkisp2;

	/* Create an initial crop configuration to configure */
	struct rkisp2_params_crop arg = {
		.header.type = RKISP2_PARAMS_BLOCK_CROP,
		.header.flags = 0,
		.header.size = sizeof(struct rkisp2_params_crop),
		.crop_en = RKISP2_ISP_CROP_ENABLE_MAIN | RKISP2_ISP_CROP_ENABLE_SELF,

		.mp_crop.h_offs = 0,
		.mp_crop.v_offs = 0,
		.mp_crop.h_size = rkisp2->resizer_devs[0].source_size.width,
		.mp_crop.v_size = rkisp2->resizer_devs[0].source_size.height,

		.sp_crop.h_offs = 0,
		.sp_crop.v_offs = 0,
		.sp_crop.h_size = rkisp2->resizer_devs[1].source_size.width,
		.sp_crop.v_size = rkisp2->resizer_devs[1].source_size.height
	};

	dev_dbg(rkisp2->dev,
		"%s: set mp crop to %dx%d\n",
		__func__, arg.mp_crop.h_size, arg.mp_crop.v_size);

	__rkisp2_params_crop(params, &arg, V4L2_ISP_PARAMS_FL_BLOCK_ENABLE);
}

static void rkisp2_params_configure(struct rkisp2_params *params,
				    struct rkisp2_params_buffer *buf,
				    enum rkisp2_params_configure_priority prio)
{
	struct v4l2_isp_params_buffer *cfg;
	const struct rkisp2_params_block_handler_info *info;
	size_t block_offset = 0;
	size_t max_offset;

	buf->vb.sequence = params->rkisp2->isp.frame_sequence + 1;
	cfg = buf->cfg;

	max_offset = cfg->data_size;

	/* Walk the list of parameter blocks and process them. */
	while (max_offset && block_offset < max_offset) {
		union rkisp2_params_block block;

		/* \todo Check if we want to avoid this copy */
		block.data = &cfg->data[block_offset];

		block_offset += block.header->size;

		info = &rkisp2_params_handlers[block.header->type];

		if (prio != RKISP2_PARAMS_CONFIG_PRIO_NONE &&
		    prio != info->priority)
			continue;

		info->handler(params, block);
	}

	/* update shadow register immediately */
	rkisp2_param_set_bits(params, RKISP2_CIF_ISP_CTRL,
			      RKISP2_CIF_ISP_CTRL_ISP_CFG_UPD);

	if (prio == RKISP2_PARAMS_CONFIG_PRIO_NONE)
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

void rkisp2_params_isr(struct rkisp2_params *params)
{
	struct rkisp2_params_buffer *buf;

	spin_lock(&params->buf_lock);
	buf = list_first_entry_or_null(&params->params,
				       struct rkisp2_params_buffer, queue);
	if (buf)
		list_del(&buf->queue);
	spin_unlock(&params->buf_lock);

	if (!buf)
		return;

	rkisp2_params_configure(params, buf, RKISP2_PARAMS_CONFIG_PRIO_NONE);
}

void rkisp2_params_pre_configure(struct rkisp2_params *params,
				 enum rkisp2_fmt_raw_pat_type bayer_pat,
				 const struct v4l2_mbus_framefmt *sink_frm,
				 const struct v4l2_mbus_framefmt *mp_src_frm,
				 const struct v4l2_mbus_framefmt *sp_src_frm)
{
	struct rkisp2_device *rkisp2 = params->rkisp2;
	struct rkisp2_params_buffer *buf;

	/* Save configuration information that params requires */

	params->raw_type = bayer_pat;

	rkisp2_resizer_pre_configure(rkisp2, sink_frm, mp_src_frm, sp_src_frm);

	spin_lock_irq(&params->buf_lock);
	buf = list_first_entry_or_null(&params->params,
				       struct rkisp2_params_buffer, queue);
	spin_unlock_irq(&params->buf_lock);

	if (!buf) {
		/*
		 * Initialize crop in the event that are are no pre-queued
		 * parameter buffers
		 */
		rkisp2_params_crop_init(params);
		return;
	}

	rkisp2_params_configure(params, buf, RKISP2_PARAMS_CONFIG_PRIO_PRE);
}

void rkisp2_params_post_configure(struct rkisp2_params *params)
{
	struct rkisp2_params_buffer *buf;

	spin_lock_irq(&params->buf_lock);
	buf = list_first_entry_or_null(&params->params,
				       struct rkisp2_params_buffer, queue);
	if (buf)
		list_del(&buf->queue);
	spin_unlock_irq(&params->buf_lock);

	if (!buf)
		return;

	rkisp2_params_configure(params, buf, RKISP2_PARAMS_CONFIG_PRIO_POST);
}

/*** V4L2 ***/

static int rkisp2_params_enum_fmt_meta_out(struct file *file, void *fh,
					   struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	if (f->mbus_code && f->mbus_code != MEDIA_BUS_FMT_METADATA_FIXED)
		return -EINVAL;

	f->pixelformat = V4L2_META_FMT_RKISP2_PARAMS;

	return 0;
}

static int rkisp2_params_g_fmt_meta_out(struct file *file, void *fh,
					struct v4l2_format *f)
{

	static const struct v4l2_meta_format mfmt = {
		.dataformat = V4L2_META_FMT_RKISP2_PARAMS,
		.buffersize = v4l2_isp_buffer_size(RKISP2_PARAMS_MAX_SIZE),
	};

	f->fmt.meta = mfmt;

	return 0;
}

static int rkisp2_params_querycap(struct file *file,
				  void *priv, struct v4l2_capability *cap)
{
	struct video_device *vdev = video_devdata(file);

	strscpy(cap->driver, RKISP2_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, vdev->name, sizeof(cap->card));
	strscpy(cap->bus_info, RKISP2_BUS_INFO, sizeof(cap->bus_info));

	return 0;
}

/* ISP params video device IOCTLs */
static const struct v4l2_ioctl_ops rkisp2_params_ioctl = {
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_enum_fmt_meta_out = rkisp2_params_enum_fmt_meta_out,
	.vidioc_g_fmt_meta_out = rkisp2_params_g_fmt_meta_out,
	.vidioc_s_fmt_meta_out = rkisp2_params_g_fmt_meta_out,
	.vidioc_try_fmt_meta_out = rkisp2_params_g_fmt_meta_out,
	.vidioc_querycap = rkisp2_params_querycap,
};

static int rkisp2_params_vb2_queue_setup(struct vb2_queue *vq,
					 unsigned int *num_buffers,
					 unsigned int *num_planes,
					 unsigned int sizes[],
					 struct device *alloc_devs[])
{
	/* \todo num_buffers? */

	*num_planes = 1;

	sizes[0] = v4l2_isp_buffer_size(RKISP2_PARAMS_MAX_SIZE);

	return 0;
}

static int rkisp2_params_vb2_buf_init(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct rkisp2_params_buffer *params_buf = to_rkisp2_params_buffer(vbuf);

	params_buf->cfg = kvmalloc(v4l2_isp_buffer_size(RKISP2_PARAMS_MAX_SIZE),
				   GFP_KERNEL);
	if (!params_buf->cfg)
		return -ENOMEM;

	return 0;
}

static void rkisp2_params_vb2_buf_cleanup(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct rkisp2_params_buffer *params_buf = to_rkisp2_params_buffer(vbuf);

	kvfree(params_buf->cfg);
	params_buf->cfg = NULL;
}

static void rkisp2_params_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct rkisp2_params_buffer *params_buf = to_rkisp2_params_buffer(vbuf);
	struct vb2_queue *vq = vb->vb2_queue;
	struct rkisp2_params *params = vq->drv_priv;

	spin_lock_irq(&params->buf_lock);
	list_add_tail(&params_buf->queue, &params->params);
	spin_unlock_irq(&params->buf_lock);
}

static int rkisp2_params_vb2_buf_prepare(struct vb2_buffer *vb)
{
	struct rkisp2_params *params = vb->vb2_queue->drv_priv;
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct rkisp2_params_buffer *params_buf = to_rkisp2_params_buffer(vbuf);
	struct v4l2_isp_params_buffer *cfg = vb2_plane_vaddr(&vbuf->vb2_buf, 0);
	size_t payload_size = vb2_get_plane_payload(vb, 0);
	int ret;

	ret = v4l2_isp_params_validate_buffer_size(params->rkisp2->dev, vb,
						   v4l2_isp_buffer_size(RKISP2_PARAMS_MAX_SIZE));
	if (ret)
		return ret;

	/*
	 * Copy the parameters buffer to the internal scratch buffer to avoid
	 * userspace modifying the buffer content while the driver processes it.
	 */
	memcpy(params_buf->cfg, cfg, payload_size);

	return v4l2_isp_params_validate_buffer(params->rkisp2->dev, vb, cfg,
				rkisp2_params_block_types_info,
				ARRAY_SIZE(rkisp2_params_block_types_info));
}

static void rkisp2_params_vb2_stop_streaming(struct vb2_queue *vq)
{
	struct rkisp2_params *params = vq->drv_priv;
	struct rkisp2_params_buffer *buf;
	LIST_HEAD(tmp_list);

	/*
	 * we first move the buffers into a local list 'tmp_list'
	 * and then we can iterate it and call vb2_buffer_done
	 * without holding the lock
	 */
	spin_lock_irq(&params->buf_lock);
	list_splice_init(&params->params, &tmp_list);
	spin_unlock_irq(&params->buf_lock);

	list_for_each_entry(buf, &tmp_list, queue)
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops rkisp2_params_vb2_ops = {
	.queue_setup = rkisp2_params_vb2_queue_setup,
	.buf_init = rkisp2_params_vb2_buf_init,
	.buf_cleanup = rkisp2_params_vb2_buf_cleanup,
	.buf_queue = rkisp2_params_vb2_buf_queue,
	.buf_prepare = rkisp2_params_vb2_buf_prepare,
	.stop_streaming = rkisp2_params_vb2_stop_streaming,
};

static const struct v4l2_file_operations rkisp2_params_fops = {
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
	.poll = vb2_fop_poll,
	.open = v4l2_fh_open,
	.release = vb2_fop_release
};

static int rkisp2_params_link_validate(struct media_link *link)
{
	/* \todo implement this */
	return 0;
}

static const struct media_entity_operations rkisp2_params_media_ops = {
	.link_validate = rkisp2_params_link_validate,
};

static int rkisp2_params_init_vb2_queue(struct vb2_queue *q,
					struct rkisp2_params *params)
{
	struct rkisp2_vdev_node *node;

	node = container_of(q, struct rkisp2_vdev_node, buf_queue);

	q->type = V4L2_BUF_TYPE_META_OUTPUT;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = params;
	q->ops = &rkisp2_params_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->buf_struct_size = sizeof(struct rkisp2_params_buffer);
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &node->vlock;

	return vb2_queue_init(q);
}

int rkisp2_params_register(struct rkisp2_device *rkisp2)
{
	struct rkisp2_params *params = &rkisp2->params;
	struct rkisp2_vdev_node *node = &params->vnode;
	struct video_device *vdev = &node->vdev;
	int ret;

	params->rkisp2 = rkisp2;
	mutex_init(&node->vlock);
	INIT_LIST_HEAD(&params->params);
	spin_lock_init(&params->buf_lock);

	strscpy(vdev->name, RKISP2_PARAMS_DEV_NAME, sizeof(vdev->name));

	video_set_drvdata(vdev, params);
	vdev->ioctl_ops = &rkisp2_params_ioctl;
	vdev->fops = &rkisp2_params_fops;
	vdev->release = video_device_release_empty;
	/*
	 * Provide a mutex to v4l2 core. It will be used
	 * to protect all fops and v4l2 ioctls.
	 */
	vdev->lock = &node->vlock;
	vdev->v4l2_dev = &rkisp2->v4l2_dev;
	vdev->queue = &node->buf_queue;
	vdev->device_caps = V4L2_CAP_STREAMING | V4L2_CAP_META_OUTPUT;
	vdev->entity.ops = &rkisp2_params_media_ops;
	vdev->vfl_dir = VFL_DIR_TX;
	ret = rkisp2_params_init_vb2_queue(vdev->queue, params);
	if (ret)
		goto err_media;

	video_set_drvdata(vdev, params);

	node->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&vdev->entity, 1, &node->pad);
	if (ret)
		goto err_media;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(rkisp2->dev,
			"failed to register %s, ret=%d\n", vdev->name, ret);
		return ret;
	}

	return 0;

err_media:
	media_entity_cleanup(&vdev->entity);
	mutex_destroy(&node->vlock);
	return ret;
}

void rkisp2_params_unregister(struct rkisp2_device *rkisp2)
{
	struct rkisp2_params *params = &rkisp2->params;
	struct rkisp2_vdev_node *node = &params->vnode;
	struct video_device *vdev = &node->vdev;

	if (!video_is_registered(vdev))
		return;

	vb2_video_unregister_device(vdev);
	media_entity_cleanup(&vdev->entity);
	mutex_destroy(&node->vlock);
}
