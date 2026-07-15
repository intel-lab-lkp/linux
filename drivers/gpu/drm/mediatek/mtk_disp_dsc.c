// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021 MediaTek Inc.
 * Copyright (c) 2025 Collabora Ltd
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>

#include "mtk_crtc.h"
#include "mtk_ddp_comp.h"
#include "mtk_disp_drv.h"

#define DISP_REG_DSC_CON		0x0
#  define DSC_EN			BIT(0)
#  define DSC_DUAL_INOUT		BIT(2)
#  define DSC_IN_SRC_SEL		BIT(3)
#  define DSC_BYPASS			BIT(4)
#  define DSC_RELAY			BIT(5)
#  define DSC_V1_1_EXT			BIT(6)
#  define DSC_PT_MEM_EN			BIT(7)
#  define DSC_SW_RESET			BIT(8)
#  define DSC_EMPTY_FLAG_SEL		GENMASK(15, 14)
 #  define DSC_EMPTY_FLAG_NORMAL_DET	0
 #  define DSC_EMPTY_FLAG_ALWAYS_HIGH	1
 #  define DSC_EMPTY_FLAG_ALWAYS_LOW	2
 #  define DSC_EMPTY_FLAG_DO_NOT_SEND	3
#  define DSC_UFOE_SEL			BIT(16)
#  define DSC_OUTPUT_SWAP		BIT(18)
#  define DSC_ZERO_FIFO_STALL_DISABLE	BIT(20)

#define DISP_REG_DSC_SPR		0x14
#define DISP_REG_DSC_PIC_W		0x18
#  define DSC_PIC_WIDTH			GENMASK(15, 0)
#  define DSC_PIC_GROUP_WIDTH_M1	GENMASK(31, 16)

#define DISP_REG_DSC_PIC_H		0x1c
#  define DSC_PIC_HEIGHT		GENMASK(15, 0)
#  define DSC_PIC_HEIGHT_EXT_M1		GENMASK(31, 16)

#define DISP_REG_DSC_SLICE_W		0x20
#  define DSC_SLICE_WIDTH		GENMASK(15, 0)
#  define DSC_SLICE_GROUP_WIDTH_M1	GENMASK(31, 16)

#define DISP_REG_DSC_SLICE_H		0x24
#  define DSC_SLICE_HEIGHT_M1		GENMASK(15, 0)
#  define DSC_SLICE_NUM_M1		GENMASK(29, 16)
#  define DSC_SLICE_WIDTH_MOD3		GENMASK(31, 30)

#define DISP_REG_DSC_CHUNK_SIZE		0x28

#define DISP_REG_DSC_BUF_SIZE		0x2c
#  define DISP_DSC_BUF_SIZE_MASK	GENMASK(23, 0)

#define DISP_REG_DSC_MODE		0x30
#  define DSC_RGB_SWAP			BIT(2)
#  define DSC_INIT_DELAY_HEIGHT		GENMASK(11, 8)

#define DISP_REG_DSC_CFG		0x34
#  define DSC_CFG_FLATNESS_DET_THRES	GENMASK(4, 0)
#  define DSC_CFG_FLATNESS_8BITS	2
#  define DSC_CFG_FLATNESS_10BITS	8
#  define DSC_CFG_ICH_EN		BIT(5)
#  define DSC_CFG_ICH_LINE_CLEAR	GENMASK(7, 6)
#  define DSC_CFG_V1P1			BIT(8)
#  define DSC_CFG_IDLE_MODE		BIT(9)
#  define DSC_CFG_CRC_EN		BIT(12)
#  define DSC_CFG_DSC12_BUGFIX		BIT(14)
#  define DSC_CFG_CORE_CHECKSUM		BIT(15)

#define DISP_REG_DSC_PAD		0x38
#  define DSC_PAD_NUMBER		GENMASK(2, 0)

#define DISP_REG_DSC_ENC_WIDTH		0x3c
#  define DSC_ENC_WIDTH_SLICE		GENMASK(15, 0)
#  define DSC_ENC_WIDTH_PIC		GENMASK(31, 16)

#define DISP_REG_DSC_PIC_PRE_PAD_SIZE	0x40
#  define DSC_PIC_PREPAD_HEIGHT		GENMASK(15, 0)
#  define DSC_PIC_PREPAD_WIDTH		GENMASK(31, 16)

#define DISP_REG_DSC_DBG_CON		0x60
#  define DSC_CKSM_CAL_EN		BIT(9)

#define DISP_REG_DSC_OUTBUF		0x70
#  define DSC_OBUF_SIZE			GENMASK(11, 0)

#define DISP_REG_DSC_PPS(x)		(0x80 + (x * 4)) /* 0..19 */
#  define DSC_P0_UP_LINE_BUF_DEPTH	GENMASK(3, 0)
#  define DSC_P0_BPC			GENMASK(7, 4)
#  define DSC_P0_BPP			GENMASK(17, 8)
#  define DSC_P0_RCT_ON			BIT(18)
#  define DSC_P0_BLOCK_PRED_EN		BIT(19)
#  define DSC_P1_INITIAL_XMIT_DELAY	GENMASK(15, 0)
#  define DSC_P1_INITIAL_DEC_DELAY	GENMASK(31, 16)
#  define DSC_P2_INITIAL_SCALE_VALUE	GENMASK(15, 0)
#  define DSC_P2_SCALE_INCR_INTERVAL	GENMASK(31, 16)
#  define DSC_P3_SCALE_DECR_INTERVAL	GENMASK(15, 0)
#  define DSC_P3_FIRST_LINE_BPG_OFFSET	GENMASK(31, 16)
#  define DSC_P4_NFL_BPG_OFFSET		GENMASK(15, 0)
#  define DSC_P4_SLICE_BPG_OFFSET	GENMASK(31, 16)
#  define DSC_P5_INITIAL_OFFSET		GENMASK(15, 0)
#  define DSC_P5_FINAL_OFFSET		GENMASK(31, 16)
#  define DSC_P6_FLATNESS_MIN_QP	GENMASK(4, 0)
#  define DSC_P6_FLATNESS_MAX_QP	GENMASK(12, 8)
#  define DSC_P6_RC_MODEL_SIZE		GENMASK(31, 16)
#  define DSC_P7_RC_EDGE_FACTOR		GENMASK(7, 0)
#  define DSC_P7_RC_QUANT_INCR_LIMIT0	GENMASK(12, 8)
#  define DSC_P7_RC_QUANT_INCR_LIMIT1	GENMASK(20, 16)
#  define DSC_P7_RC_TGT_OFFSET_HI	GENMASK(27, 24)
#  define DSC_P7_RC_TGT_OFFSET_LO	GENMASK(31, 28)
#  define DSC_P8_RC_BUF_THR_X		GENMASK(7, 0)
#  define DSC_P12_RC_RANGE_MIN_QP	GENMASK(4, 0)
#  define DSC_P12_RC_RANGE_MAX_QP	GENMASK(9, 5)
#  define DSC_P12_RC_RANGE_BPG_OFFSET	GENMASK(15, 10)

#define DISP_REG_DSC_SHADOW		0x200
#  define DSC_FORCE_COMMIT		BIT(0)
#  define DSC_BYPASS_SHADOW		BIT(1)
#  define DSC_READ_WORKING		BIT(2)
#  define DSC_SHADOW_DSC_VERSION_MINOR	GENMASK(8, 5)

struct mtk_dsc {
	struct clk		*clk;
	void __iomem		*reg;
	struct cmdq_client_reg	cmdq_reg;
	bool			dsc_config_done;
};

int mtk_dsc_clk_enable(struct device *dev)
{
	struct mtk_dsc *disp_dsc = dev_get_drvdata(dev);

	return clk_prepare_enable(disp_dsc->clk);
}

void mtk_dsc_clk_disable(struct device *dev)
{
	struct mtk_dsc *disp_dsc = dev_get_drvdata(dev);

	clk_disable_unprepare(disp_dsc->clk);
}

static void mtk_dsc_pps_setup(struct mtk_dsc *disp_dsc, struct drm_dsc_config *dsc_cfg)
{
	struct drm_dsc_rc_range_parameters *rcrp = dsc_cfg->rc_range_params;
	u16 *rbt = dsc_cfg->rc_buf_thresh;
	u32 data;
	int i, j;

	/* PPS 0 - Note: Fractional BPP is not supported! */
	data = FIELD_PREP(DSC_P0_UP_LINE_BUF_DEPTH, dsc_cfg->line_buf_depth);
	data |= FIELD_PREP(DSC_P0_BPC, dsc_cfg->bits_per_component);
	data |= FIELD_PREP(DSC_P0_BPP, dsc_cfg->bits_per_pixel);
	data |= DSC_P0_RCT_ON | DSC_P0_BLOCK_PRED_EN;
	writel(data, disp_dsc->reg + DISP_REG_DSC_PPS(0));

	/* PPS 1 */
	data = FIELD_PREP(DSC_P1_INITIAL_XMIT_DELAY, dsc_cfg->initial_xmit_delay);
	data |= FIELD_PREP(DSC_P1_INITIAL_DEC_DELAY, dsc_cfg->initial_dec_delay);
	writel(data, disp_dsc->reg + DISP_REG_DSC_PPS(1));

	/* PPS 2 */
	data = FIELD_PREP(DSC_P2_INITIAL_SCALE_VALUE, dsc_cfg->initial_scale_value);
	data |= FIELD_PREP(DSC_P2_SCALE_INCR_INTERVAL, dsc_cfg->scale_increment_interval);
	writel(data, disp_dsc->reg + DISP_REG_DSC_PPS(2));

	/* PPS 3 */
	data = FIELD_PREP(DSC_P3_SCALE_DECR_INTERVAL, dsc_cfg->scale_decrement_interval);
	data |= FIELD_PREP(DSC_P3_FIRST_LINE_BPG_OFFSET, dsc_cfg->first_line_bpg_offset);
	writel(data, disp_dsc->reg + DISP_REG_DSC_PPS(3));

	/* PPS 4 */
	data = FIELD_PREP(DSC_P4_NFL_BPG_OFFSET, dsc_cfg->nfl_bpg_offset);
	data |= FIELD_PREP(DSC_P4_SLICE_BPG_OFFSET, dsc_cfg->slice_bpg_offset);
	writel(data, disp_dsc->reg + DISP_REG_DSC_PPS(4));

	/* PPS 5 */
	data = FIELD_PREP(DSC_P5_INITIAL_OFFSET, dsc_cfg->initial_offset);
	data |= FIELD_PREP(DSC_P5_FINAL_OFFSET, dsc_cfg->final_offset);
	writel(data, disp_dsc->reg + DISP_REG_DSC_PPS(5));

	/* PPS 6 */
	data = FIELD_PREP(DSC_P6_FLATNESS_MIN_QP, dsc_cfg->flatness_min_qp);
	data |= FIELD_PREP(DSC_P6_FLATNESS_MAX_QP, dsc_cfg->flatness_max_qp);
	writel(data, disp_dsc->reg + DISP_REG_DSC_PPS(6));

	/* PPS 7 */
	data = FIELD_PREP(DSC_P7_RC_EDGE_FACTOR, dsc_cfg->rc_edge_factor);
	data |= FIELD_PREP(DSC_P7_RC_QUANT_INCR_LIMIT0, dsc_cfg->rc_quant_incr_limit0);
	data |= FIELD_PREP(DSC_P7_RC_QUANT_INCR_LIMIT1, dsc_cfg->rc_quant_incr_limit1);
	data |= FIELD_PREP(DSC_P7_RC_TGT_OFFSET_HI, dsc_cfg->rc_tgt_offset_high);
	data |= FIELD_PREP(DSC_P7_RC_TGT_OFFSET_LO, dsc_cfg->rc_tgt_offset_low);
	writel(data, disp_dsc->reg + DISP_REG_DSC_PPS(7));

	/* PPS 8..11 - Each register holds 4 RC buffer thresholds (PPS 11 has two) */
	for (i = 0; i < 4; i++) {
		u8 block_num = i * 4;
		data = 0;

		for (j = 0; j < 4; j++) {
			u8 buf_index = block_num + j;
			u8 data_shift = j * 8;

			/* rc_buf_thresh holds 14 elements in total */
			if (buf_index > 13)
				break;

			data |= (rbt[buf_index] & DSC_P8_RC_BUF_THR_X) << data_shift;
		}
		writel(data, disp_dsc->reg + DISP_REG_DSC_PPS(8 + i));
	}

	/* PPS 12..18 - Each register holds two sets of RC range parameters */
	for (i = 0; i < 7; i++) {
		u8 block_num = i * 2;
		data = 0;

		for (j = 0; j < 2; j++) {
			u8 buf_index = block_num + j;
			u8 data_shift = j * 16;
			u32 range_data;

			range_data = FIELD_PREP(DSC_P12_RC_RANGE_MIN_QP,
						 rcrp[buf_index].range_min_qp);
			range_data |= FIELD_PREP(DSC_P12_RC_RANGE_MAX_QP,
						 rcrp[buf_index].range_max_qp);
			range_data |= FIELD_PREP(DSC_P12_RC_RANGE_BPG_OFFSET,
						 rcrp[buf_index].range_bpg_offset);
			data |= range_data << data_shift;

			/* rc_range_params holds 15 elements in total */
			if (buf_index == 13)
				break;
		}
		writel(data, disp_dsc->reg + DISP_REG_DSC_PPS(12 + i));
	}

	/* PPS 19 - Last RC range register, holds only one set of parameters */
	writel(FIELD_PREP(DSC_P12_RC_RANGE_MIN_QP, rcrp[14].range_min_qp) |
	       FIELD_PREP(DSC_P12_RC_RANGE_MAX_QP, rcrp[14].range_max_qp) |
	       FIELD_PREP(DSC_P12_RC_RANGE_BPG_OFFSET, rcrp[14].range_bpg_offset),
	       disp_dsc->reg + DISP_REG_DSC_PPS(19));
}

void mtk_dsc_setup(struct device *dev, struct drm_dsc_config *dsc_cfg)
{
	struct mtk_dsc *disp_dsc = dev_get_drvdata(dev);
	u32 dsc_slice_w, dsc_slice_h, dsc_mode, dsc_cfg_rval, dsc_shadow;
	u32 dsc_dbg_con, dsc_con, dsc_enc_width, dsc_pic_w, dsc_pic_h;
	u32 pic_group_width, pic_height_ext_num, slice_group_width;
	u32 chunk_size, dsc_pad_num, dsc_pre_pad_sz;
	bool dsc_en_bit;

	pic_height_ext_num = dsc_cfg->pic_height + dsc_cfg->slice_height - 1;
	pic_group_width = dsc_cfg->slice_width * 4;
	pic_group_width /= 3;

	slice_group_width = dsc_cfg->slice_width + 2;
	slice_group_width /= 3;

	if (dsc_cfg->slice_chunk_size)
		chunk_size = dsc_cfg->slice_chunk_size;
	else
		chunk_size = dsc_cfg->slice_width * dsc_cfg->bits_per_pixel / 8 / 16;

	dsc_enc_width = FIELD_PREP(DSC_ENC_WIDTH_PIC, dsc_cfg->pic_width) |
			FIELD_PREP(DSC_ENC_WIDTH_SLICE, dsc_cfg->slice_width);

	dsc_pic_w = FIELD_PREP(DSC_PIC_GROUP_WIDTH_M1, pic_group_width - 1);
	dsc_pic_w |= FIELD_PREP(DSC_PIC_WIDTH, dsc_cfg->pic_width);
	dsc_pic_h = FIELD_PREP(DSC_PIC_HEIGHT_EXT_M1, pic_height_ext_num - 1);
	dsc_pic_h |= FIELD_PREP(DSC_PIC_HEIGHT, dsc_cfg->pic_height - 1);

	dsc_slice_w = FIELD_PREP(DSC_SLICE_GROUP_WIDTH_M1, slice_group_width - 1);
	dsc_slice_w |= FIELD_PREP(DSC_SLICE_WIDTH, dsc_cfg->slice_width);
	dsc_slice_h = FIELD_PREP(DSC_SLICE_WIDTH_MOD3, dsc_cfg->slice_width % 3);
	dsc_slice_h |= FIELD_PREP(DSC_SLICE_NUM_M1,
				  (pic_height_ext_num / dsc_cfg->slice_height) - 1);
	dsc_slice_h |= FIELD_PREP(DSC_SLICE_HEIGHT_M1, dsc_cfg->slice_height - 1);

	dsc_pad_num = (3 - ((chunk_size * 2) % 3)) % 3;
	dsc_pad_num = FIELD_PREP(DSC_PAD_NUMBER, dsc_pad_num);

	dsc_pre_pad_sz = FIELD_PREP(DSC_PIC_PREPAD_HEIGHT, dsc_cfg->pic_height);
	dsc_pre_pad_sz |= FIELD_PREP(DSC_PIC_PREPAD_WIDTH, dsc_cfg->pic_width);

	dsc_mode = FIELD_PREP(DSC_INIT_DELAY_HEIGHT, 4);
	dsc_mode |= FIELD_PREP(DSC_RGB_SWAP, 0);

	/* Must enable checksum calc in DBG if enabling core checksum in CFG */
	dsc_cfg_rval = DSC_CFG_ICH_EN | DSC_CFG_CRC_EN | DSC_CFG_DSC12_BUGFIX |
		       DSC_CFG_CORE_CHECKSUM;
	dsc_dbg_con = DSC_CKSM_CAL_EN;

	if (dsc_cfg->bits_per_component == 8)
		dsc_cfg_rval |= FIELD_PREP_CONST(DSC_CFG_FLATNESS_DET_THRES,
						 DSC_CFG_FLATNESS_8BITS);
	else
		dsc_cfg_rval |= FIELD_PREP_CONST(DSC_CFG_FLATNESS_DET_THRES,
						 DSC_CFG_FLATNESS_10BITS);

	dsc_shadow = FIELD_PREP(DSC_SHADOW_DSC_VERSION_MINOR,
				dsc_cfg->dsc_version_minor);
	dsc_shadow |= DSC_FORCE_COMMIT | DSC_BYPASS_SHADOW;

	/* If DSC is currently enabled, disable it before setup and re-enable after */
	dsc_con = readl(disp_dsc->reg + DISP_REG_DSC_CON);
	if (dsc_con & DSC_EN) {
		dsc_en_bit = true;
		writel(dsc_con & ~DSC_EN, disp_dsc->reg + DISP_REG_DSC_CON);
	} else {
		dsc_en_bit = false;
	}

	writel(0, disp_dsc->reg + DISP_REG_DSC_SPR);
	writel(dsc_enc_width, disp_dsc->reg + DISP_REG_DSC_ENC_WIDTH);
	writel(dsc_pic_w, disp_dsc->reg + DISP_REG_DSC_PIC_W);
	writel(dsc_pic_h, disp_dsc->reg + DISP_REG_DSC_PIC_H);
	writel(dsc_slice_w, disp_dsc->reg + DISP_REG_DSC_SLICE_W);
	writel(dsc_slice_h, disp_dsc->reg + DISP_REG_DSC_SLICE_H);
	writel(((chunk_size * 4) / 3) << 16 | chunk_size,
	       disp_dsc->reg + DISP_REG_DSC_CHUNK_SIZE);
	writel(dsc_pre_pad_sz, disp_dsc->reg + DISP_REG_DSC_PIC_PRE_PAD_SIZE);
	writel(dsc_pad_num, disp_dsc->reg + DISP_REG_DSC_PAD);
	writel(FIELD_PREP(DISP_DSC_BUF_SIZE_MASK, chunk_size * dsc_cfg->slice_height),
	       disp_dsc->reg + DISP_REG_DSC_BUF_SIZE);
	writel(dsc_mode, disp_dsc->reg + DISP_REG_DSC_MODE);
	writel(dsc_cfg_rval, disp_dsc->reg + DISP_REG_DSC_CFG);
	writel(dsc_dbg_con, disp_dsc->reg + DISP_REG_DSC_DBG_CON);
	writel(FIELD_PREP_CONST(DSC_OBUF_SIZE, 1040), disp_dsc->reg + DISP_REG_DSC_OUTBUF);
	writel(dsc_shadow, disp_dsc->reg + DISP_REG_DSC_SHADOW);

	/* Set PPS registers configuration */
	mtk_dsc_pps_setup(disp_dsc, dsc_cfg);

	dsc_con = FIELD_PREP_CONST(DSC_EMPTY_FLAG_SEL, DSC_EMPTY_FLAG_ALWAYS_LOW);
	dsc_con |= DSC_V1_1_EXT | DSC_UFOE_SEL | DSC_PT_MEM_EN;
	dsc_con |= DSC_ZERO_FIFO_STALL_DISABLE;

	if (dsc_en_bit)
		dsc_con |= DSC_EN;

	writel(dsc_con, disp_dsc->reg + DISP_REG_DSC_CON);

	disp_dsc->dsc_config_done = true;
}

void mtk_dsc_start(struct device *dev)
{
	struct mtk_dsc *disp_dsc = dev_get_drvdata(dev);

	/* If no DSC or config not done, set bypass mode */
	if (!disp_dsc->dsc_config_done) {
		mtk_ddp_write_mask(NULL, DSC_BYPASS, &disp_dsc->cmdq_reg,
				   disp_dsc->reg, DISP_REG_DSC_CON, DSC_BYPASS);
		mtk_ddp_write_mask(NULL, DSC_UFOE_SEL, &disp_dsc->cmdq_reg,
				   disp_dsc->reg, DISP_REG_DSC_CON, DSC_UFOE_SEL);
		mtk_ddp_write_mask(NULL, DSC_DUAL_INOUT, &disp_dsc->cmdq_reg,
				   disp_dsc->reg, DISP_REG_DSC_CON, DSC_DUAL_INOUT);
	}

	mtk_ddp_write_mask(NULL, DSC_EN, &disp_dsc->cmdq_reg,
			   disp_dsc->reg, DISP_REG_DSC_CON, DSC_EN);
}

void mtk_dsc_stop(struct device *dev)
{
	struct mtk_dsc *disp_dsc = dev_get_drvdata(dev);

	writel(0, disp_dsc->reg + DISP_REG_DSC_CON);
}

static int mtk_dsc_bind(struct device *dev, struct device *master, void *data)
{
	return 0;
}

static void mtk_dsc_unbind(struct device *dev, struct device *master, void *data)
{
}

static const struct component_ops mtk_dsc_component_ops = {
	.bind	= mtk_dsc_bind,
	.unbind = mtk_dsc_unbind,
};

static int mtk_dsc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_dsc *priv;
	struct resource *res;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk),
				     "failed to get clk\n");

	priv->reg = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(priv->reg))
		return dev_err_probe(dev, PTR_ERR(priv->reg),
				     "failed to do ioremap\n");

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	ret = cmdq_dev_get_client_reg(dev, &priv->cmdq_reg, 0);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get gce client reg\n");
#endif

	platform_set_drvdata(pdev, priv);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = component_add(dev, &mtk_dsc_component_ops);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add component\n");

	return 0;
}

static void mtk_dsc_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mtk_dsc_component_ops);
}

static const struct of_device_id mtk_dsc_driver_dt_match[] = {
	{ .compatible = "mediatek,mt8195-disp-dsc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_dsc_driver_dt_match);

struct platform_driver mtk_disp_dsc_driver = {
	.probe		= mtk_dsc_probe,
	.remove		= mtk_dsc_remove,
	.driver		= {
		.name	= "mediatek-disp-dsc",
		.of_match_table = mtk_dsc_driver_dt_match,
	},
};
