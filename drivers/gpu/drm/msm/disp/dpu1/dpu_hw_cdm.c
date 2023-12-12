// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023, The Linux Foundation. All rights reserved.
 */

#include <drm/drm_managed.h>

#include "dpu_hw_mdss.h"
#include "dpu_hw_util.h"
#include "dpu_hw_catalog.h"
#include "dpu_hw_cdm.h"
#include "dpu_kms.h"

#define CDM_CSC_10_OPMODE                  0x000
#define CDM_CSC_10_BASE                    0x004

#define CDM_CDWN2_OP_MODE                  0x100
#define CDM_CDWN2_CLAMP_OUT                0x104
#define CDM_CDWN2_PARAMS_3D_0              0x108
#define CDM_CDWN2_PARAMS_3D_1              0x10C
#define CDM_CDWN2_COEFF_COSITE_H_0         0x110
#define CDM_CDWN2_COEFF_COSITE_H_1         0x114
#define CDM_CDWN2_COEFF_COSITE_H_2         0x118
#define CDM_CDWN2_COEFF_OFFSITE_H_0        0x11C
#define CDM_CDWN2_COEFF_OFFSITE_H_1        0x120
#define CDM_CDWN2_COEFF_OFFSITE_H_2        0x124
#define CDM_CDWN2_COEFF_COSITE_V           0x128
#define CDM_CDWN2_COEFF_OFFSITE_V          0x12C
#define CDM_CDWN2_OUT_SIZE                 0x130

#define CDM_HDMI_PACK_OP_MODE              0x200
#define CDM_CSC_10_MATRIX_COEFF_0          0x004

#define CDM_MUX                            0x224

/* CDM CDWN2 sub-block bit definitions */
#define CDM_CDWN2_OP_MODE_EN                  BIT(0)
#define CDM_CDWN2_OP_MODE_ENABLE_H            BIT(1)
#define CDM_CDWN2_OP_MODE_ENABLE_V            BIT(2)
#define CDM_CDWN2_OP_MODE_METHOD_H_AVG        BIT(3)
#define CDM_CDWN2_OP_MODE_METHOD_H_COSITE     BIT(4)
#define CDM_CDWN2_OP_MODE_METHOD_V_AVG        BIT(5)
#define CDM_CDWN2_OP_MODE_METHOD_V_COSITE     BIT(6)
#define CDM_CDWN2_OP_MODE_BITS_OUT_8BIT       BIT(7)
#define CDM_CDWN2_OP_MODE_METHOD_H_OFFSITE    GENMASK(4, 3)
#define CDM_CDWN2_OP_MODE_METHOD_V_OFFSITE    GENMASK(6, 5)
#define CDM_CDWN2_V_PIXEL_DROP_MASK           GENMASK(6, 5)
#define CDM_CDWN2_H_PIXEL_DROP_MASK           GENMASK(4, 3)

/* CDM CSC10 sub-block bit definitions */
#define CDM_CSC10_OP_MODE_EN               BIT(0)
#define CDM_CSC10_OP_MODE_SRC_FMT_YUV      BIT(1)
#define CDM_CSC10_OP_MODE_DST_FMT_YUV      BIT(2)

/* CDM HDMI pack sub-block bit definitions */
#define CDM_HDMI_PACK_OP_MODE_EN           BIT(0)

/*
 * Horizontal coefficients for cosite chroma downscale
 * s13 representation of coefficients
 */
static u32 cosite_h_coeff[] = {0x00000016, 0x000001cc, 0x0100009e};

/*
 * Horizontal coefficients for offsite chroma downscale
 */
static u32 offsite_h_coeff[] = {0x000b0005, 0x01db01eb, 0x00e40046};

/*
 * Vertical coefficients for cosite chroma downscale
 */
static u32 cosite_v_coeff[] = {0x00080004};
/*
 * Vertical coefficients for offsite chroma downscale
 */
static u32 offsite_v_coeff[] = {0x00060002};

static int dpu_hw_cdm_setup_cdwn(struct dpu_hw_cdm *ctx, struct dpu_hw_cdm_cfg *cfg)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;
	u32 opmode = 0;
	u32 out_size = 0;

	if (cfg->output_bit_depth != CDM_CDWN_OUTPUT_10BIT)
		opmode |= CDM_CDWN2_OP_MODE_BITS_OUT_8BIT;

	/* ENABLE DWNS_H bit */
	opmode |= CDM_CDWN2_OP_MODE_ENABLE_H;

	switch (cfg->h_cdwn_type) {
	case CDM_CDWN_DISABLE:
		/* CLEAR METHOD_H field */
		opmode &= ~CDM_CDWN2_H_PIXEL_DROP_MASK;
		/* CLEAR DWNS_H bit */
		opmode &= ~CDM_CDWN2_OP_MODE_ENABLE_H;
		break;
	case CDM_CDWN_PIXEL_DROP:
		/* Clear METHOD_H field (pixel drop is 0) */
		opmode &= ~CDM_CDWN2_H_PIXEL_DROP_MASK;
		break;
	case CDM_CDWN_AVG:
		/* Clear METHOD_H field (Average is 0x1) */
		opmode &= ~CDM_CDWN2_H_PIXEL_DROP_MASK;
		opmode |= CDM_CDWN2_OP_MODE_METHOD_H_AVG;
		break;
	case CDM_CDWN_COSITE:
		/* Clear METHOD_H field (Average is 0x2) */
		opmode &= ~CDM_CDWN2_H_PIXEL_DROP_MASK;
		opmode |= CDM_CDWN2_OP_MODE_METHOD_H_COSITE;
		/* Co-site horizontal coefficients */
		DPU_REG_WRITE(c, CDM_CDWN2_COEFF_COSITE_H_0,
			      cosite_h_coeff[0]);
		DPU_REG_WRITE(c, CDM_CDWN2_COEFF_COSITE_H_1,
			      cosite_h_coeff[1]);
		DPU_REG_WRITE(c, CDM_CDWN2_COEFF_COSITE_H_2,
			      cosite_h_coeff[2]);
		break;
	case CDM_CDWN_OFFSITE:
		/* Clear METHOD_H field (Average is 0x3) */
		opmode &= ~CDM_CDWN2_H_PIXEL_DROP_MASK;
		opmode |= CDM_CDWN2_OP_MODE_METHOD_H_OFFSITE;

		/* Off-site horizontal coefficients */
		DPU_REG_WRITE(c, CDM_CDWN2_COEFF_OFFSITE_H_0,
			      offsite_h_coeff[0]);
		DPU_REG_WRITE(c, CDM_CDWN2_COEFF_OFFSITE_H_1,
			      offsite_h_coeff[1]);
		DPU_REG_WRITE(c, CDM_CDWN2_COEFF_OFFSITE_H_2,
			      offsite_h_coeff[2]);
		break;
	default:
		DPU_ERROR("%s invalid horz down sampling type\n", __func__);
		return -EINVAL;
	}

	/* ENABLE DWNS_V bit */
	opmode |= CDM_CDWN2_OP_MODE_ENABLE_V;

	switch (cfg->v_cdwn_type) {
	case CDM_CDWN_DISABLE:
		/* CLEAR METHOD_V field */
		opmode &= ~CDM_CDWN2_V_PIXEL_DROP_MASK;
		/* CLEAR DWNS_V bit */
		opmode &= ~CDM_CDWN2_OP_MODE_ENABLE_V;
		break;
	case CDM_CDWN_PIXEL_DROP:
		/* Clear METHOD_V field (pixel drop is 0) */
		opmode &= ~CDM_CDWN2_V_PIXEL_DROP_MASK;
		break;
	case CDM_CDWN_AVG:
		/* Clear METHOD_V field (Average is 0x1) */
		opmode &= ~CDM_CDWN2_V_PIXEL_DROP_MASK;
		opmode |= CDM_CDWN2_OP_MODE_METHOD_V_AVG;
		break;
	case CDM_CDWN_COSITE:
		/* Clear METHOD_V field (Average is 0x2) */
		opmode &= ~CDM_CDWN2_V_PIXEL_DROP_MASK;
		opmode |= CDM_CDWN2_OP_MODE_METHOD_V_COSITE;
		/* Co-site vertical coefficients */
		DPU_REG_WRITE(c,
			      CDM_CDWN2_COEFF_COSITE_V,
			      cosite_v_coeff[0]);
		break;
	case CDM_CDWN_OFFSITE:
		/* Clear METHOD_V field (Average is 0x3) */
		opmode &= ~CDM_CDWN2_V_PIXEL_DROP_MASK;
		opmode |= CDM_CDWN2_OP_MODE_METHOD_V_OFFSITE;

		/* Off-site vertical coefficients */
		DPU_REG_WRITE(c,
			      CDM_CDWN2_COEFF_OFFSITE_V,
			      offsite_v_coeff[0]);
		break;
	default:
		return -EINVAL;
	}

	if (cfg->v_cdwn_type || cfg->h_cdwn_type)
		opmode |= CDM_CDWN2_OP_MODE_EN; /* EN CDWN module */
	else
		opmode &= ~CDM_CDWN2_OP_MODE_EN;

	out_size = (cfg->output_width & 0xFFFF) | ((cfg->output_height & 0xFFFF) << 16);
	DPU_REG_WRITE(c, CDM_CDWN2_OUT_SIZE, out_size);
	DPU_REG_WRITE(c, CDM_CDWN2_OP_MODE, opmode);
	DPU_REG_WRITE(c, CDM_CDWN2_CLAMP_OUT, ((0x3FF << 16) | 0x0));

	return 0;
}

static int dpu_hw_cdm_enable(struct dpu_hw_cdm *ctx, struct dpu_hw_cdm_cfg *cdm)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;
	const struct dpu_format *fmt;
	u32 opmode = 0;
	u32 csc = 0;

	if (!ctx || !cdm)
		return -EINVAL;

	fmt = cdm->output_fmt;

	if (!DPU_FORMAT_IS_YUV(fmt))
		return -EINVAL;

	dpu_hw_csc_setup(&ctx->hw, CDM_CSC_10_MATRIX_COEFF_0, cdm->csc_cfg, true);
	dpu_hw_cdm_setup_cdwn(ctx, cdm);

	if (cdm->output_type == CDM_CDWN_OUTPUT_HDMI) {
		if (fmt->chroma_sample != DPU_CHROMA_H1V2)
			return -EINVAL; /*unsupported format */
		opmode = CDM_HDMI_PACK_OP_MODE_EN;
		opmode |= (fmt->chroma_sample << 1);
	}

	csc |= CDM_CSC10_OP_MODE_DST_FMT_YUV;
	csc &= ~CDM_CSC10_OP_MODE_SRC_FMT_YUV;
	csc |= CDM_CSC10_OP_MODE_EN;

	if (ctx && ctx->ops.bind_pingpong_blk)
		ctx->ops.bind_pingpong_blk(ctx, cdm->pp_id);

	DPU_REG_WRITE(c, CDM_CSC_10_OPMODE, csc);
	DPU_REG_WRITE(c, CDM_HDMI_PACK_OP_MODE, opmode);
	return 0;
}

static void dpu_hw_cdm_bind_pingpong_blk(struct dpu_hw_cdm *ctx, const enum dpu_pingpong pp)
{
	struct dpu_hw_blk_reg_map *c;
	int mux_cfg = 0xF; /* Disabled */

	c = &ctx->hw;

	if (pp)
		mux_cfg = (pp - PINGPONG_0) & 0x7;

	DPU_REG_WRITE(c, CDM_MUX, mux_cfg);
}

struct dpu_hw_cdm *dpu_hw_cdm_init(struct drm_device *dev,
				   const struct dpu_cdm_cfg *cfg, void __iomem *addr,
				   const struct dpu_mdss_version *mdss_rev)
{
	struct dpu_hw_cdm *c;

	c = drmm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return ERR_PTR(-ENOMEM);

	c->hw.blk_addr = addr + cfg->base;
	c->hw.log_mask = DPU_DBG_MASK_CDM;

	/* Assign ops */
	c->idx = cfg->id;
	c->caps = cfg;

	c->ops.enable = dpu_hw_cdm_enable;
	if (mdss_rev->core_major_ver >= 5)
		c->ops.bind_pingpong_blk = dpu_hw_cdm_bind_pingpong_blk;

	return c;
}
