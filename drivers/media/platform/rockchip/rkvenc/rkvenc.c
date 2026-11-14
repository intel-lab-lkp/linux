// SPDX-License-Identifier: GPL-2.0-only
/*
 * Rockchip RKVENC v1 H.264/HEVC encoder: platform driver, interrupt and
 * job handling.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "rkvenc-vepu541-regs.h"
#include "rkvenc.h"

#define RKVENC_WATCHDOG_MS	1000
#define RKVENC_SAFE_CLEAR_MS	100
#define RKVENC_RESET_PULSE_US	5

#define RKVENC_L1_CFG_START 0x00c

static const struct rkvenc_variant rkvenc_variant_rv1126 = {
	.vepu540 = false,
};

static const struct rkvenc_variant rkvenc_variant_rk3568 = {
	.vepu540 = true,
};

int rkvenc_hw_run(struct rkvenc_dev *dev, const struct rkvenc_frame *f)
{
	struct rkvenc_img *img = &dev->img;
	void __iomem *regs = dev->regs;
	unsigned int a;
	int ret;

	ret = rkvenc_vepu541_img_build(f, img);
	if (WARN_ON(ret))
		return ret;

	/* INT_STA keeps its bits across a start */
	writel(RKVENC_INT_ALL, regs + RKVENC_INT_CLR);

	writel(RKVENC_L2_FIRST | RKVENC_L2CFG_ADDR_AUTO_INC,
	       regs + RKVENC_L2CFG_ADDR);
	for (a = RKVENC_L2_FIRST; a <= img->l2_last; a += 4)
		writel(img->l2[a / 4], regs + RKVENC_L2CFG_WDATA);

	/* A write to ENC_CLR would clear the job, so L1 starts above it */
	for (a = RKVENC_L1_CFG_END - 4; a >= RKVENC_L1_CFG_START; a -= 4)
		if (rkvenc_vepu541_l1_written(a))
			writel(img->l1[a / 4], regs + a);

	/* A timed out job must not be judged by the previous status. */
	dev->irq_sta = 0;
	writel(img->enc_strt, regs + RKVENC_ENC_STRT);

	schedule_delayed_work(&dev->watchdog,
			      msecs_to_jiffies(RKVENC_WATCHDOG_MS));
	return 0;
}

void rkvenc_hw_status_read(struct rkvenc_dev *dev, u32 *int_sta, u32 *bytes,
			   u32 *qp_sum)
{
	void __iomem *regs = dev->regs;

	*int_sta = dev->irq_sta;
	*bytes = FIELD_GET(RKVENC_ST_BSL_LEN, readl(regs + RKVENC_ST_BSL));
	*qp_sum = readl(regs + RKVENC_ST_SSE_QP) & GENMASK(23, 0);
}

static bool rkvenc_hw_force_clear(struct rkvenc_dev *dev)
{
	void __iomem *regs = dev->regs;

	writel(RKVENC_INT_ALL, regs + RKVENC_INT_MSK);
	writel(RKVENC_ENC_CLR_FORCE, regs + RKVENC_ENC_CLR);
	writel(0, regs + RKVENC_ENC_CLR);
	writel(RKVENC_INT_ALL, regs + RKVENC_INT_CLR);
	readl(regs + RKVENC_INT_STA);
	writel(0, regs + RKVENC_INT_MSK);

	return !(readl(regs + RKVENC_ST_ENC) & GENMASK(1, 0));
}

void rkvenc_hw_recover(struct rkvenc_dev *dev)
{
	void __iomem *regs = dev->regs;

	if (rkvenc_hw_force_clear(dev))
		return;

	dev_err(dev->dev, "job did not stop on force clear, resetting\n");

	writel(RKVENC_INT_ALL & ~RKVENC_INT_SCLR_DONE, regs + RKVENC_INT_MSK);
	writel(RKVENC_INT_ALL, regs + RKVENC_INT_CLR);

	reinit_completion(&dev->sclr_done);
	writel(RKVENC_ENC_CLR_SAFE, regs + RKVENC_ENC_CLR);
	wait_for_completion_timeout(&dev->sclr_done,
				    msecs_to_jiffies(RKVENC_SAFE_CLEAR_MS));

	if (dev->num_resets) {
		reset_control_bulk_assert(dev->num_resets, dev->resets);
		udelay(RKVENC_RESET_PULSE_US);
		reset_control_bulk_deassert(dev->num_resets, dev->resets);
	}

	writel(0, regs + RKVENC_INT_MSK);
}

static irqreturn_t rkvenc_irq(int irq, void *priv)
{
	struct rkvenc_dev *dev = priv;
	u32 sta;

	sta = readl(dev->regs + RKVENC_INT_STA);
	if (!sta)
		return IRQ_NONE;

	writel(sta, dev->regs + RKVENC_INT_CLR);
	/* Flush the posted clear before the interrupt line is sampled again */
	readl(dev->regs + RKVENC_INT_STA);

	if (sta & RKVENC_INT_SCLR_DONE)
		complete(&dev->sclr_done);

	dev->irq_sta = sta;
	return IRQ_WAKE_THREAD;
}

static void rkvenc_finish_job(struct rkvenc_dev *dev, struct rkvenc_ctx *ctx,
			      enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *src, *dst;
	u32 payload, int_sta, bytes, qp_sum;
	u8 frame_type, qp_avg;
	struct v4l2_ctrl *avg_qp;

	rkvenc_hw_status_read(dev, &int_sta, &bytes, &qp_sum);

	if (state == VB2_BUF_STATE_DONE) {
		src = v4l2_m2m_next_src_buf(ctx->m2m_ctx);
		dst = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);

		payload = ctx->cur_prepend + bytes;
		frame_type = ctx->cur_intra ? 0 : 1;
		if (ctx->qp_blocks)
			qp_avg = (u8)(qp_sum / ctx->qp_blocks);
		else
			qp_avg = ctx->cur_qp;

		if (dst) {
			vb2_set_plane_payload(&dst->vb2_buf, 0, payload);
			if (src)
				v4l2_m2m_buf_copy_metadata(src, dst);
			dst->flags |= ctx->cur_intra ? V4L2_BUF_FLAG_KEYFRAME
						     : V4L2_BUF_FLAG_PFRAME;
		}

		rkvenc_rc_update(ctx, payload, qp_avg, frame_type);
		rkvenc_gop_commit(ctx);
		ctx->consecutive_errors = 0;
		ctx->overflow_retried = false;

		avg_qp = v4l2_ctrl_find(&ctx->ctrl_hdl,
					V4L2_CID_MPEG_VIDEO_AVERAGE_QP);
		if (avg_qp)
			v4l2_ctrl_s_ctrl(avg_qp, qp_avg);
	} else if (ctx->rc_active && !ctx->overflow_retried &&
		   (int_sta & RKVENC_INT_ERRORS) == RKVENC_INT_OFLW) {
		/* The job stays running for the framework: the frame is retried */
		dst = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);
		rkvenc_hw_recover(dev);
		rkvenc_rc_overflow(ctx,
				   dst ? vb2_plane_size(&dst->vb2_buf, 0) : 0,
				   ctx->cur_intra ? 0 : 1);
		ctx->overflow_retried = true;
		if (ctx->cur_prepend)
			ctx->joined_header_done = false;
		dev->job_ctx = NULL;
		pm_runtime_put(dev->dev);
		rkvenc_m2m_ops.device_run(ctx);
		return;
	} else {
		rkvenc_hw_recover(dev);
		rkvenc_gop_rollback(ctx);
		ctx->overflow_retried = false;
		ctx->consecutive_errors++;
		if (ctx->consecutive_errors >= RKVENC_MAX_CONSEC_ERRORS) {
			vb2_queue_error(&ctx->m2m_ctx->out_q_ctx.q);
			vb2_queue_error(&ctx->m2m_ctx->cap_q_ctx.q);
		}
	}

	src = v4l2_m2m_next_src_buf(ctx->m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);
	if (src && dst &&
	    v4l2_m2m_is_last_draining_src_buf(ctx->m2m_ctx, src)) {
		dst->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_m2m_mark_stopped(ctx->m2m_ctx);
	}

	dev->job_ctx = NULL;
	pm_runtime_put(dev->dev);

	v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev, ctx->m2m_ctx, state);
}

static irqreturn_t rkvenc_irq_thread(int irq, void *priv)
{
	struct rkvenc_dev *dev = priv;
	struct rkvenc_ctx *ctx;
	enum vb2_buffer_state state;
	u32 int_sta, bytes, qp_sum;
	unsigned long flags;

	/* Whoever clears job_running first, IRQ or watchdog, finishes the job */
	spin_lock_irqsave(&dev->irq_lock, flags);
	if (!dev->job_running) {
		spin_unlock_irqrestore(&dev->irq_lock, flags);
		return IRQ_HANDLED;
	}
	ctx = dev->job_ctx;
	dev->job_running = false;
	spin_unlock_irqrestore(&dev->irq_lock, flags);

	cancel_delayed_work(&dev->watchdog);

	rkvenc_hw_status_read(dev, &int_sta, &bytes, &qp_sum);

	if ((int_sta & RKVENC_INT_ENC_DONE) &&
	    !(int_sta & RKVENC_INT_ERRORS))
		state = VB2_BUF_STATE_DONE;
	else
		state = VB2_BUF_STATE_ERROR;

	rkvenc_finish_job(dev, ctx, state);
	return IRQ_HANDLED;
}

static void rkvenc_watchdog_work(struct work_struct *work)
{
	struct rkvenc_dev *dev = container_of(to_delayed_work(work),
					      struct rkvenc_dev, watchdog);
	struct rkvenc_ctx *ctx;
	unsigned long flags;

	spin_lock_irqsave(&dev->irq_lock, flags);
	if (!dev->job_running) {
		spin_unlock_irqrestore(&dev->irq_lock, flags);
		return;
	}
	ctx = dev->job_ctx;
	dev->job_running = false;
	spin_unlock_irqrestore(&dev->irq_lock, flags);

	dev_err(dev->dev, "RKVENC encode timed out\n");
	rkvenc_finish_job(dev, ctx, VB2_BUF_STATE_ERROR);
}

static int rkvenc_runtime_resume(struct device *dev)
{
	struct rkvenc_dev *rkvenc = dev_get_drvdata(dev);

	return clk_bulk_prepare_enable(rkvenc->num_clocks, rkvenc->clocks);
}

static int rkvenc_runtime_suspend(struct device *dev)
{
	struct rkvenc_dev *rkvenc = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(rkvenc->num_clocks, rkvenc->clocks);
	return 0;
}

static int rkvenc_suspend(struct device *dev)
{
	struct rkvenc_dev *rkvenc = dev_get_drvdata(dev);

	v4l2_m2m_suspend(rkvenc->m2m_dev);
	return pm_runtime_force_suspend(dev);
}

static int rkvenc_resume(struct device *dev)
{
	struct rkvenc_dev *rkvenc = dev_get_drvdata(dev);
	int ret;

	ret = pm_runtime_force_resume(dev);
	if (ret)
		return ret;
	/* Jobs rewrite every register and the references stay in memory */
	v4l2_m2m_resume(rkvenc->m2m_dev);
	return 0;
}

static void rkvenc_init_clocks_resets(struct rkvenc_dev *rkvenc)
{
	static const char *const clk_names[] = { "axi", "ahb", "core" };
	static const char *const rst_names[] = { "axi", "ahb", "core" };
	unsigned int i;

	for (i = 0; i < RKVENC_NUM_CLKS; i++)
		rkvenc->clocks[i].id = clk_names[i];
	for (i = 0; i < RKVENC_NUM_RESETS; i++)
		rkvenc->resets[i].id = rst_names[i];
	rkvenc->num_clocks = RKVENC_NUM_CLKS;
	rkvenc->num_resets = RKVENC_NUM_RESETS;
}

static int rkvenc_probe(struct platform_device *pdev)
{
	const struct rkvenc_variant *variant;
	struct rkvenc_dev *rkvenc;
	struct device *dev = &pdev->dev;
	u32 ver, pic_size;
	int ret, irq;

	variant = of_device_get_match_data(dev);
	if (!variant)
		return -EINVAL;

	rkvenc = devm_kzalloc(dev, sizeof(*rkvenc), GFP_KERNEL);
	if (!rkvenc)
		return -ENOMEM;

	platform_set_drvdata(pdev, rkvenc);
	rkvenc->dev = dev;
	rkvenc->variant = variant;

	spin_lock_init(&rkvenc->irq_lock);
	mutex_init(&rkvenc->vdev_lock);
	init_completion(&rkvenc->sclr_done);
	INIT_DELAYED_WORK(&rkvenc->watchdog, rkvenc_watchdog_work);

	rkvenc->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rkvenc->regs))
		return PTR_ERR(rkvenc->regs);

	rkvenc_init_clocks_resets(rkvenc);

	ret = devm_clk_bulk_get(dev, rkvenc->num_clocks, rkvenc->clocks);
	if (ret)
		return ret;

	ret = devm_reset_control_bulk_get_exclusive(dev, rkvenc->num_resets,
						     rkvenc->resets);
	if (ret)
		return ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "Could not set DMA mask\n");
		return ret;
	}

	ret = vb2_dma_contig_set_max_seg_size(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_threaded_irq(dev, irq, rkvenc_irq,
					rkvenc_irq_thread, IRQF_ONESHOT,
					dev_name(dev), rkvenc);
	if (ret) {
		dev_err(dev, "Could not request encoder IRQ\n");
		return ret;
	}

	pm_runtime_enable(dev);

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	/* VERSION tells the 4K encoder from the 1080p one */
	ver = readl(rkvenc->regs + RKVENC_VERSION);
	pic_size = (ver >> 12) & 0xf;
	switch (pic_size) {
	case 0:
		rkvenc->max_width = 4096;
		rkvenc->max_height = 2304;
		break;
	case 1:
		rkvenc->max_width = 1920;
		rkvenc->max_height = 1088;
		break;
	default:
		dev_warn(dev, "unknown VERSION pic_size %u (0x%08x)\n",
			 pic_size, ver);
		rkvenc->max_width = 4096;
		rkvenc->max_height = 2304;
		break;
	}
	if (pic_size != 0)
		dev_info(dev, "RKVENC VERSION 0x%08x (pic_size %u)\n",
			 ver, pic_size);

	pm_runtime_put(dev);

	ret = v4l2_device_register(dev, &rkvenc->v4l2_dev);
	if (ret) {
		dev_err(dev, "Failed to register V4L2 device\n");
		goto err_pm_disable;
	}

	rkvenc->m2m_dev = v4l2_m2m_init(&rkvenc_m2m_ops);
	if (IS_ERR(rkvenc->m2m_dev)) {
		v4l2_err(&rkvenc->v4l2_dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(rkvenc->m2m_dev);
		goto err_unreg_v4l2;
	}

	ret = rkvenc_v4l2_init(rkvenc);
	if (ret)
		goto err_release_m2m;

	return 0;

err_release_m2m:
	v4l2_m2m_release(rkvenc->m2m_dev);
err_unreg_v4l2:
	v4l2_device_unregister(&rkvenc->v4l2_dev);
err_pm_disable:
	pm_runtime_disable(dev);
	return ret;
}

static void rkvenc_remove(struct platform_device *pdev)
{
	struct rkvenc_dev *rkvenc = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&rkvenc->watchdog);
	rkvenc_v4l2_cleanup(rkvenc);
	v4l2_m2m_release(rkvenc->m2m_dev);
	v4l2_device_unregister(&rkvenc->v4l2_dev);
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id rkvenc_dt_match[] = {
	{ .compatible = "rockchip,rv1126-rkvenc", .data = &rkvenc_variant_rv1126 },
	{ .compatible = "rockchip,rk3568-rkvenc", .data = &rkvenc_variant_rk3568 },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, rkvenc_dt_match);

static const struct dev_pm_ops rkvenc_pm_ops = {
	RUNTIME_PM_OPS(rkvenc_runtime_suspend, rkvenc_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(rkvenc_suspend, rkvenc_resume)
};

static struct platform_driver rkvenc_driver = {
	.probe = rkvenc_probe,
	.remove = rkvenc_remove,
	.driver = {
		.name = "rockchip-rkvenc",
		.of_match_table = rkvenc_dt_match,
		.pm = pm_ptr(&rkvenc_pm_ops),
	},
};

static int __init rkvenc_init(void)
{
	rkvenc_tables_init();

	return platform_driver_register(&rkvenc_driver);
}
module_init(rkvenc_init);

static void __exit rkvenc_exit(void)
{
	platform_driver_unregister(&rkvenc_driver);
}
module_exit(rkvenc_exit);

MODULE_AUTHOR("Daniel Golle <daniel@makrotopia.org>");
MODULE_DESCRIPTION("Rockchip RKVENC v1 H.264/HEVC encoder driver");
MODULE_LICENSE("GPL");
