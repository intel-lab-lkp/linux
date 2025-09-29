// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2025 NXP
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware/imx/svc/misc.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include <dt-bindings/firmware/imx/rsrc.h>

#include "dc-dprc.h"
#include "dc-prg.h"

#define SET					0x4
#define CLR					0x8
#define TOG					0xc

#define SYSTEM_CTRL0				0x00
#define  SW_SHADOW_LOAD_SEL			BIT(4)
#define  SHADOW_LOAD_EN				BIT(3)
#define  REPEAT_EN				BIT(2)
#define  SOFT_RESET				BIT(1)
#define  RUN_EN					BIT(0)	/* self-clearing */

#define IRQ_MASK				0x20
#define IRQ_MASK_STATUS				0x30
#define IRQ_NONMASK_STATUS			0x40
#define  DPR2RTR_FIFO_LOAD_BUF_RDY_UV_ERROR	BIT(7)
#define  DPR2RTR_FIFO_LOAD_BUF_RDY_YRGB_ERROR	BIT(6)
#define  DPR2RTR_UV_FIFO_OVFL			BIT(5)
#define  DPR2RTR_YRGB_FIFO_OVFL			BIT(4)
#define  IRQ_AXI_READ_ERROR			BIT(3)
#define  IRQ_DPR_SHADOW_LOADED_MASK		BIT(2)
#define  IRQ_DPR_RUN				BIT(1)
#define  IRQ_DPR_CRTL_DONE			BIT(0)
#define  IRQ_CTRL_MASK				GENMASK(2, 0)

#define MODE_CTRL0				0x50
#define  A_COMP_SEL(byte)			FIELD_PREP(GENMASK(17, 16), (byte))
#define  R_COMP_SEL(byte)			FIELD_PREP(GENMASK(15, 14), (byte))
#define  G_COMP_SEL(byte)			FIELD_PREP(GENMASK(13, 12), (byte))
#define  B_COMP_SEL(byte)			FIELD_PREP(GENMASK(11, 10), (byte))
#define  PIX_SIZE_32BIT				FIELD_PREP(GENMASK(7, 6), 0x2)
#define  LINE4					BIT(1)
#define  BUF2					0

#define FRAME_CTRL0				0x70
#define  PITCH(n)				FIELD_PREP(GENMASK(31, 16), (n))

#define FRAME_1P_CTRL0				0x90
#define FRAME_2P_CTRL0				0xe0
#define  MAX_BYTES_PREQ_MASK			GENMASK(2, 0)
#define  BYTE_1K				FIELD_PREP(MAX_BYTES_PREQ_MASK, 0x4)

#define FRAME_1P_PIX_X_CTRL			0xa0
#define  NUM_X_PIX_WIDE(n)			FIELD_PREP(GENMASK(15, 0), (n))

#define FRAME_1P_PIX_Y_CTRL			0xb0
#define  NUM_Y_PIX_HIGH(n)			FIELD_PREP(GENMASK(15, 0), (n))

#define FRAME_1P_BASE_ADDR_CTRL0		0xc0

#define FRAME_PIX_X_ULC_CTRL			0xf0
#define  CROP_ULC_X(n)				FIELD_PREP(GENMASK(15, 0), (n))

#define FRAME_PIX_Y_ULC_CTRL			0x100
#define  CROP_ULC_Y(n)				FIELD_PREP(GENMASK(15, 0), (n))

#define FRAME_2P_BASE_ADDR_CTRL0		0x110

#define STATUS_CTRL0				0x130
#define STATUS_CTRL1				0x140

#define RTRAM_CTRL0				0x200
#define  THRES_LOW(n)				FIELD_PREP(GENMASK(6, 4), (n))
#define  THRES_HIGH(n)				FIELD_PREP(GENMASK(3, 1), (n))

#define DPU_DRPC_MAX_STRIDE			0x10000
#define DPU_DPRC_MAX_RTRAM_WIDTH		2880

struct dc_dprc {
	struct device *dev;
	struct regmap *reg;
	struct clk_bulk_data *clks;
	int num_clks;
	struct imx_sc_ipc *ipc_handle;
	spinlock_t lock;	/* protect IRQ registers */
	u32 sc_resource;
	struct dc_prg *prg;
};

static const struct regmap_range dc_dprc_regmap_write_ranges[] = {
	regmap_reg_range(SYSTEM_CTRL0, SYSTEM_CTRL0 + TOG),
	regmap_reg_range(IRQ_MASK, IRQ_MASK + TOG),
	regmap_reg_range(IRQ_NONMASK_STATUS, MODE_CTRL0 + TOG),
	regmap_reg_range(FRAME_CTRL0, FRAME_CTRL0 + TOG),
	regmap_reg_range(FRAME_1P_CTRL0, FRAME_1P_BASE_ADDR_CTRL0 + TOG),
	regmap_reg_range(FRAME_PIX_X_ULC_CTRL, FRAME_2P_BASE_ADDR_CTRL0 + TOG),
	regmap_reg_range(STATUS_CTRL0, STATUS_CTRL0 + TOG),
	regmap_reg_range(RTRAM_CTRL0, RTRAM_CTRL0 + TOG),
};

static const struct regmap_range dc_dprc_regmap_read_ranges[] = {
	regmap_reg_range(SYSTEM_CTRL0, SYSTEM_CTRL0 + TOG),
	regmap_reg_range(IRQ_MASK, IRQ_MASK_STATUS + TOG),
	regmap_reg_range(MODE_CTRL0, MODE_CTRL0 + TOG),
	regmap_reg_range(FRAME_CTRL0, FRAME_CTRL0 + TOG),
	regmap_reg_range(FRAME_1P_CTRL0, FRAME_1P_BASE_ADDR_CTRL0 + TOG),
	regmap_reg_range(FRAME_PIX_X_ULC_CTRL, FRAME_2P_BASE_ADDR_CTRL0 + TOG),
	regmap_reg_range(STATUS_CTRL0, STATUS_CTRL1 + TOG),
	regmap_reg_range(RTRAM_CTRL0, RTRAM_CTRL0 + TOG),
};

static const struct regmap_access_table dc_dprc_regmap_write_table = {
	.yes_ranges = dc_dprc_regmap_write_ranges,
	.n_yes_ranges = ARRAY_SIZE(dc_dprc_regmap_write_ranges),
};

static const struct regmap_access_table dc_dprc_regmap_read_table = {
	.yes_ranges = dc_dprc_regmap_read_ranges,
	.n_yes_ranges = ARRAY_SIZE(dc_dprc_regmap_read_ranges),
};

static const struct regmap_config dc_dprc_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.fast_io = true,
	.wr_table = &dc_dprc_regmap_write_table,
	.rd_table = &dc_dprc_regmap_read_table,
	.max_register = RTRAM_CTRL0 + TOG,
};

static void dc_dprc_set_stream_id(struct dc_dprc *dprc, unsigned int stream_id)
{
	int ret;

	ret = imx_sc_misc_set_control(dprc->ipc_handle, dprc->sc_resource,
				      IMX_SC_C_KACHUNK_SEL, stream_id);
	if (ret)
		dev_err(dprc->dev, "failed to set KACHUNK_SEL: %d\n", ret);
}

static void dc_dprc_reset(struct dc_dprc *dprc)
{
	regmap_write(dprc->reg, SYSTEM_CTRL0 + SET, SOFT_RESET);
	fsleep(20);
	regmap_write(dprc->reg, SYSTEM_CTRL0 + CLR, SOFT_RESET);
	fsleep(20);
}

static void dc_dprc_enable(struct dc_dprc *dprc)
{
	dc_prg_enable(dprc->prg);
}

static void dc_dprc_reg_update(struct dc_dprc *dprc)
{
	dc_prg_reg_update(dprc->prg);
}

static void dc_dprc_enable_ctrl_done_irq(struct dc_dprc *dprc)
{
	guard(spinlock_irqsave)(&dprc->lock);
	regmap_write(dprc->reg, IRQ_MASK + CLR, IRQ_DPR_CRTL_DONE);
}

void dc_dprc_configure(struct dc_dprc *dprc, unsigned int stream_id,
		       unsigned int width, unsigned int height,
		       unsigned int stride,
		       const struct drm_format_info *format,
		       dma_addr_t baddr, bool start)
{
	unsigned int prg_stride = width * format->cpp[0];
	unsigned int bpp = format->cpp[0] * 8;
	struct device *dev = dprc->dev;
	unsigned int p1_w, p1_h;
	u32 val;
	int ret;

	if (start) {
		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0) {
			dev_err(dev, "failed to get RPM: %d\n", ret);
			return;
		}

		dc_dprc_set_stream_id(dprc, stream_id);
	}

	p1_w = round_up(width, format->cpp[0] == 2 ? 32 : 16);
	p1_h = round_up(height, 4);

	regmap_write(dprc->reg, FRAME_CTRL0, PITCH(stride));
	regmap_write(dprc->reg, FRAME_1P_CTRL0, BYTE_1K);
	regmap_write(dprc->reg, FRAME_1P_PIX_X_CTRL, NUM_X_PIX_WIDE(p1_w));
	regmap_write(dprc->reg, FRAME_1P_PIX_Y_CTRL, NUM_Y_PIX_HIGH(p1_h));
	regmap_write(dprc->reg, FRAME_1P_BASE_ADDR_CTRL0, baddr);
	regmap_write(dprc->reg, FRAME_PIX_X_ULC_CTRL, CROP_ULC_X(0));
	regmap_write(dprc->reg, FRAME_PIX_Y_ULC_CTRL, CROP_ULC_Y(0));

	regmap_write(dprc->reg, RTRAM_CTRL0, THRES_LOW(3) | THRES_HIGH(7));

	val = LINE4 | BUF2;
	switch (format->format) {
	case DRM_FORMAT_XRGB8888:
		/*
		 * It turns out pixel components are mapped directly
		 * without position change via DPR processing with
		 * the following color component configurations.
		 * Leave the pixel format to be handled by the
		 * display controllers.
		 */
		val |= A_COMP_SEL(3) | R_COMP_SEL(2) |
		       G_COMP_SEL(1) | B_COMP_SEL(0);
		val |= PIX_SIZE_32BIT;
		break;
	default:
		dev_err(dev, "unsupported format 0x%08x\n", format->format);
		return;
	}
	regmap_write(dprc->reg, MODE_CTRL0, val);

	if (start) {
		/* software shadow load for the first frame */
		val = SW_SHADOW_LOAD_SEL | SHADOW_LOAD_EN;
		regmap_write(dprc->reg, SYSTEM_CTRL0, val);

		/* and then, run... */
		val |= RUN_EN | REPEAT_EN;
		regmap_write(dprc->reg, SYSTEM_CTRL0, val);
	}

	dc_prg_configure(dprc->prg, width, height, prg_stride, bpp, baddr, start);

	if (start)
		dc_dprc_enable(dprc);

	dc_dprc_reg_update(dprc);

	if (start)
		dc_dprc_enable_ctrl_done_irq(dprc);

	dev_dbg(dev, "w: %u, h: %u, s: %u, fmt: 0x%08x\n",
		width, height, stride, format->format);
}

void dc_dprc_disable_repeat_en(struct dc_dprc *dprc)
{
	regmap_write(dprc->reg, SYSTEM_CTRL0 + CLR, REPEAT_EN);
	dev_dbg(dprc->dev, "disable REPEAT_EN\n");
}

void dc_dprc_disable(struct dc_dprc *dprc)
{
	dc_prg_disable(dprc->prg);

	pm_runtime_put(dprc->dev);

	dev_dbg(dprc->dev, "disable\n");
}

void dc_dprc_disable_at_boot(struct dc_dprc *dprc)
{
	dc_prg_disable_at_boot(dprc->prg);

	clk_bulk_disable_unprepare(dprc->num_clks, dprc->clks);

	dev_dbg(dprc->dev, "disable at boot\n");
}

static void dc_dprc_ctrl_done_handle(struct dc_dprc *dprc)
{
	regmap_write(dprc->reg, SYSTEM_CTRL0, REPEAT_EN);

	dc_prg_shadow_enable(dprc->prg);

	dev_dbg(dprc->dev, "CTRL done handle\n");
}

static irqreturn_t dc_dprc_wrap_irq_handler(int irq, void *data)
{
	struct dc_dprc *dprc = data;
	struct device *dev = dprc->dev;
	u32 mask, status;

	scoped_guard(spinlock, &dprc->lock) {
		/* cache valid IRQ status */
		regmap_read(dprc->reg, IRQ_MASK, &mask);
		regmap_read(dprc->reg, IRQ_MASK_STATUS, &status);
		status &= ~mask;

		/* mask the IRQ(s) being handled */
		regmap_write(dprc->reg, IRQ_MASK + SET, status);

		/* clear status register */
		regmap_write(dprc->reg, IRQ_MASK_STATUS, status);
	}

	if (status & DPR2RTR_FIFO_LOAD_BUF_RDY_UV_ERROR)
		dev_err(dev, "DPR to RTRAM FIFO load UV buffer ready error\n");

	if (status & DPR2RTR_FIFO_LOAD_BUF_RDY_YRGB_ERROR)
		dev_err(dev, "DPR to RTRAM FIFO load YRGB buffer ready error\n");

	if (status & DPR2RTR_UV_FIFO_OVFL)
		dev_err(dev, "DPR to RTRAM FIFO UV FIFO overflow\n");

	if (status & DPR2RTR_YRGB_FIFO_OVFL)
		dev_err(dev, "DPR to RTRAM FIFO YRGB FIFO overflow\n");

	if (status & IRQ_AXI_READ_ERROR)
		dev_err(dev, "AXI read error\n");

	if (status & IRQ_DPR_CRTL_DONE)
		dc_dprc_ctrl_done_handle(dprc);

	return IRQ_HANDLED;
}

bool dc_dprc_rtram_width_supported(struct dc_dprc *dprc, unsigned int width)
{
	return width <= DPU_DPRC_MAX_RTRAM_WIDTH;
}

bool dc_dprc_stride_supported(struct dc_dprc *dprc,
			      unsigned int stride, unsigned int width,
			      const struct drm_format_info *format,
			      dma_addr_t baddr)
{
	unsigned int prg_stride = width * format->cpp[0];

	if (stride > DPU_DRPC_MAX_STRIDE)
		return false;

	if (!dc_prg_stride_supported(dprc->prg, prg_stride, baddr))
		return false;

	return true;
}

static int dc_dprc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct resource *res;
	struct dc_dprc *dprc;
	void __iomem *base;
	int ret, wrap_irq;

	dprc = devm_kzalloc(dev, sizeof(*dprc), GFP_KERNEL);
	if (!dprc)
		return -ENOMEM;

	ret = imx_scu_get_handle(&dprc->ipc_handle);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get SCU ipc handle\n");

	base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	dprc->reg = devm_regmap_init_mmio(dev, base, &dc_dprc_regmap_config);
	if (IS_ERR(dprc->reg))
		return PTR_ERR(dprc->reg);

	wrap_irq = platform_get_irq_byname(pdev, "dpr_wrap");
	if (wrap_irq < 0)
		return -ENODEV;

	dprc->num_clks = devm_clk_bulk_get_all(dev, &dprc->clks);
	if (dprc->num_clks < 0)
		return dev_err_probe(dev, dprc->num_clks, "failed to get clocks\n");

	ret = of_property_read_u32(np, "fsl,sc-resource", &dprc->sc_resource);
	if (ret) {
		dev_err(dev, "failed to get SC resource %d\n", ret);
		return ret;
	}

	dprc->prg = dc_prg_lookup_by_phandle(dev, "fsl,prgs", 0);
	if (!dprc->prg)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "failed to lookup PRG\n");

	dc_prg_set_dprc(dprc->prg, dprc);

	dprc->dev = dev;
	spin_lock_init(&dprc->lock);

	ret = devm_request_irq(dev, wrap_irq, dc_dprc_wrap_irq_handler,
			       IRQF_SHARED, dev_name(dev), dprc);
	if (ret < 0) {
		dev_err(dev, "failed to request dpr_wrap IRQ(%d): %d\n",
			wrap_irq, ret);
		return ret;
	}

	dev_set_drvdata(dev, dprc);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable PM runtime\n");

	return 0;
}

static int dc_dprc_runtime_suspend(struct device *dev)
{
	struct dc_dprc *dprc = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(dprc->num_clks, dprc->clks);

	return 0;
}

static int dc_dprc_runtime_resume(struct device *dev)
{
	struct dc_dprc *dprc = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(dprc->num_clks, dprc->clks);
	if (ret) {
		dev_err(dev, "failed to enable clocks: %d\n", ret);
		return ret;
	}

	dc_dprc_reset(dprc);

	/* disable all control IRQs and enable all error IRQs */
	guard(spinlock_irqsave)(&dprc->lock);
	regmap_write(dprc->reg, IRQ_MASK, IRQ_CTRL_MASK);

	return 0;
}

static const struct dev_pm_ops dc_dprc_pm_ops = {
	RUNTIME_PM_OPS(dc_dprc_runtime_suspend, dc_dprc_runtime_resume, NULL)
};

static const struct of_device_id dc_dprc_dt_ids[] = {
	{ .compatible = "fsl,imx8qxp-dpr-channel", },
	{ /* sentinel */ }
};

struct platform_driver dc_dprc_driver = {
	.probe = dc_dprc_probe,
	.driver = {
		.name = "imx8-dc-dpr-channel",
		.suppress_bind_attrs = true,
		.of_match_table = dc_dprc_dt_ids,
		.pm = pm_ptr(&dc_dprc_pm_ops),
	},
};
