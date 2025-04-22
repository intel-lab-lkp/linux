// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Wave6 series multi-standard codec IP - wave6 core driver
 *
 * Copyright (C) 2025 CHIPS&MEDIA INC
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>
#include <linux/debugfs.h>
#include "wave6-vpu-core.h"
#include "wave6-regdefine.h"
#include "wave6-vpuconfig.h"
#include "wave6-hw.h"
#include "wave6-vpu.h"
#include "wave6-vpu-dbg.h"

#define CREATE_TRACE_POINTS
#include "wave6-trace.h"

#define VPU_CORE_PLATFORM_DEVICE_NAME "wave6-vpu-core"
#define WAVE6_VPU_DEBUGFS_DIR "wave6"

#define WAVE6_IS_ENC BIT(0)
#define WAVE6_IS_DEC BIT(1)

struct wave6_match_data {
	int codec_types;
	u32 compatible_fw_version;
};

static const struct wave6_match_data wave633c_data = {
	.codec_types = WAVE6_IS_ENC | WAVE6_IS_DEC,
	.compatible_fw_version = 0x2010000,
};

static irqreturn_t wave6_vpu_irq(int irq, void *dev_id)
{
	struct vpu_core_device *dev = dev_id;
	u32 irq_status;

	if (wave6_vdi_readl(dev, W6_VPU_VPU_INT_STS)) {
		irq_status = wave6_vdi_readl(dev, W6_VPU_VINT_REASON);

		wave6_vdi_writel(dev, W6_VPU_VINT_REASON_CLR, irq_status);
		wave6_vdi_writel(dev, W6_VPU_VINT_CLEAR, 0x1);

		trace_irq(dev, irq_status);

		kfifo_in(&dev->irq_status, &irq_status, sizeof(int));

		return IRQ_WAKE_THREAD;
	}

	return IRQ_HANDLED;
}

static irqreturn_t wave6_vpu_irq_thread(int irq, void *dev_id)
{
	struct vpu_core_device *core = dev_id;
	struct wave6_vpu_device *vpu = dev_get_drvdata(core->dev->parent);
	struct vpu_instance *inst;
	int irq_status, ret;

	while (kfifo_len(&core->irq_status)) {
		bool error = false;

		ret = kfifo_out(&core->irq_status, &irq_status, sizeof(int));
		if (!ret)
			break;

		if (irq_status & BIT(W6_INT_BIT_REQ_WORK_BUF)) {
			call_void_vop(vpu, req_work_buffer, &core->entity);
			continue;
		}

		if ((irq_status & BIT(W6_INT_BIT_INIT_SEQ)) ||
		    (irq_status & BIT(W6_INT_BIT_ENC_SET_PARAM))) {
			complete(&core->irq_done);
			continue;
		}

		if (irq_status & BIT(W6_INT_BIT_BSBUF_ERROR))
			error = true;

		inst = v4l2_m2m_get_curr_priv(core->m2m_dev);
		if (inst)
			inst->ops->finish_process(inst, error);
	}

	return IRQ_HANDLED;
}

static u32 wave6_vpu_read_reg(struct device *dev, u32 addr)
{
	struct vpu_core_device *vpu_dev = dev_get_drvdata(dev);

	return wave6_vdi_readl(vpu_dev, addr);
}

static void wave6_vpu_write_reg(struct device *dev, u32 addr, u32 data)
{
	struct vpu_core_device *vpu_dev = dev_get_drvdata(dev);

	wave6_vdi_writel(vpu_dev, addr, data);
}

static void wave6_vpu_on_boot(struct device *dev)
{
	struct vpu_core_device *vpu_dev = dev_get_drvdata(dev);
	u32 product_code;
	u32 version;
	u32 revision;
	u32 hw_version;
	int ret;

	mutex_lock(&vpu_dev->hw_lock);

	product_code = wave6_vdi_readl(vpu_dev, W6_VPU_RET_PRODUCT_VERSION);

	wave6_vpu_enable_interrupt(vpu_dev);
	ret = wave6_vpu_get_version(vpu_dev, &version, &revision);
	if (ret) {
		dev_err(dev, "wave6_vpu_get_version fail\n");
		goto unlock;
	}

	hw_version = wave6_vdi_readl(vpu_dev, W6_RET_CONF_REVISION);

	if (vpu_dev->product_code != product_code ||
	    vpu_dev->fw_version != version ||
	    vpu_dev->fw_revision != revision ||
	    vpu_dev->hw_version != hw_version) {
		vpu_dev->product_code = product_code;
		vpu_dev->fw_version = version;
		vpu_dev->fw_revision = revision;
		vpu_dev->hw_version = hw_version;
		dev_info(dev, "product: %08x, fw_version : %d.%d.%d(r%d), hw_version : 0x%x\n",
			 vpu_dev->product_code,
			 (version >> 24) & 0xFF,
			 (version >> 16) & 0xFF,
			 (version >> 0) & 0xFFFF,
			 revision,
			 vpu_dev->hw_version);
	}

	if (vpu_dev->res->compatible_fw_version > version)
		dev_err(dev, "compatible firmware version is v%d.%d.%d or higher, but only v%d.%d.%d\n",
			(vpu_dev->res->compatible_fw_version >> 24) & 0xFF,
			(vpu_dev->res->compatible_fw_version >> 16) & 0xFF,
			vpu_dev->res->compatible_fw_version & 0xFFFF,
			(version >> 24) & 0xFF,
			(version >> 16) & 0xFF,
			version & 0xFFFF);

unlock:
	mutex_unlock(&vpu_dev->hw_lock);
}

void wave6_vpu_pause(struct device *dev, int resume)
{
	struct vpu_core_device *vpu_dev = dev_get_drvdata(dev);

	mutex_lock(&vpu_dev->pause_lock);

	if (resume) {
		vpu_dev->pause_request--;
		if (!vpu_dev->pause_request)
			v4l2_m2m_resume(vpu_dev->m2m_dev);
	} else {
		if (!vpu_dev->pause_request)
			v4l2_m2m_suspend(vpu_dev->m2m_dev);
		vpu_dev->pause_request++;
	}
	mutex_unlock(&vpu_dev->pause_lock);
}

void wave6_vpu_activate(struct vpu_core_device *dev)
{
	dev->entity.active = true;
}

static void wave6_vpu_wait_activated(struct vpu_core_device *dev)
{
	if (dev->entity.active)
		wave6_vpu_check_state(dev);
}

static int wave6_vpu_core_register_device(struct vpu_core_device *core)
{
	struct wave6_vpu_device *vpu = dev_get_drvdata(core->dev->parent);

	if (!vpu || !vpu->ops)
		return -EPROBE_DEFER;

	core->entity.dev = core->dev;
	core->entity.read_reg = wave6_vpu_read_reg;
	core->entity.write_reg = wave6_vpu_write_reg;
	core->entity.on_boot = wave6_vpu_on_boot;
	core->entity.pause = wave6_vpu_pause;

	return call_vop(vpu, reg_core, &core->entity);
}

static void wave6_vpu_core_unregister_device(struct vpu_core_device *core)
{
	struct wave6_vpu_device *vpu = dev_get_drvdata(core->dev->parent);

	if (!vpu || !vpu->ops)
		return;

	call_void_vop(vpu, unreg_core, &core->entity);
}

static int wave6_vpu_core_probe(struct platform_device *pdev)
{
	int ret;
	struct vpu_core_device *dev;
	const struct wave6_match_data *match_data;

	match_data = device_get_match_data(&pdev->dev);
	if (!match_data) {
		dev_err(&pdev->dev, "missing match_data\n");
		return -EINVAL;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret < 0) {
		dev_err(&pdev->dev, "dma_set_mask_and_coherent failed: %d\n", ret);
		return ret;
	}

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	mutex_init(&dev->dev_lock);
	mutex_init(&dev->hw_lock);
	mutex_init(&dev->pause_lock);
	init_completion(&dev->irq_done);
	dev_set_drvdata(&pdev->dev, dev);
	dev->dev = &pdev->dev;
	dev->res = match_data;

	dev->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dev->reg_base))
		return PTR_ERR(dev->reg_base);

	ret = devm_clk_bulk_get_all(&pdev->dev, &dev->clks);
	if (ret < 0) {
		dev_warn(&pdev->dev, "unable to get clocks: %d\n", ret);
		ret = 0;
	}
	dev->num_clks = ret;

	ret = wave6_vpu_core_register_device(dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "register vpu core fail, ret = %d\n", ret);
		return ret;
	}

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "v4l2_device_register fail: %d\n", ret);
		goto err_register_core;
	}

	ret = wave6_vpu_init_m2m_dev(dev);
	if (ret)
		goto err_v4l2_unregister;

	dev->irq = platform_get_irq(pdev, 0);
	if (dev->irq < 0) {
		dev_err(&pdev->dev, "failed to get irq resource\n");
		ret = -ENXIO;
		goto err_m2m_dev_release;
	}

	ret = kfifo_alloc(&dev->irq_status, 16 * sizeof(int), GFP_KERNEL);
	if (ret) {
		dev_err(&pdev->dev, "failed to allocate fifo\n");
		goto err_m2m_dev_release;
	}

	ret = devm_request_threaded_irq(&pdev->dev, dev->irq, wave6_vpu_irq,
					wave6_vpu_irq_thread, 0, "vpu_irq", dev);
	if (ret) {
		dev_err(&pdev->dev, "fail to register interrupt handler: %d\n", ret);
		goto err_kfifo_free;
	}

	dev->temp_vbuf.size = ALIGN(WAVE6_TEMPBUF_SIZE, 4096);
	ret = wave6_alloc_dma(dev->dev, &dev->temp_vbuf);
	if (ret) {
		dev_err(&pdev->dev, "alloc temp of size %zu failed\n",
			dev->temp_vbuf.size);
		goto err_kfifo_free;
	}

	dev->debugfs = debugfs_lookup(WAVE6_VPU_DEBUGFS_DIR, NULL);
	if (IS_ERR_OR_NULL(dev->debugfs))
		dev->debugfs = debugfs_create_dir(WAVE6_VPU_DEBUGFS_DIR, NULL);

	pm_runtime_enable(&pdev->dev);

	if (dev->res->codec_types & WAVE6_IS_DEC) {
		ret = wave6_vpu_dec_register_device(dev);
		if (ret) {
			dev_err(&pdev->dev, "wave6_vpu_dec_register_device fail: %d\n", ret);
			goto err_temp_vbuf_free;
		}
	}
	if (dev->res->codec_types & WAVE6_IS_ENC) {
		ret = wave6_vpu_enc_register_device(dev);
		if (ret) {
			dev_err(&pdev->dev, "wave6_vpu_enc_register_device fail: %d\n", ret);
			goto err_dec_unreg;
		}
	}

	dev_dbg(&pdev->dev, "Added wave6 driver with caps %s %s\n",
		dev->res->codec_types & WAVE6_IS_ENC ? "'ENCODE'" : "",
		dev->res->codec_types & WAVE6_IS_DEC ? "'DECODE'" : "");

	return 0;

err_dec_unreg:
	if (dev->res->codec_types & WAVE6_IS_DEC)
		wave6_vpu_dec_unregister_device(dev);
err_temp_vbuf_free:
	wave6_free_dma(&dev->temp_vbuf);
err_kfifo_free:
	kfifo_free(&dev->irq_status);
err_m2m_dev_release:
	wave6_vpu_release_m2m_dev(dev);
err_v4l2_unregister:
	v4l2_device_unregister(&dev->v4l2_dev);
err_register_core:
	wave6_vpu_core_unregister_device(dev);
	mutex_destroy(&dev->dev_lock);
	mutex_destroy(&dev->hw_lock);
	mutex_destroy(&dev->pause_lock);

	return ret;
}

static void wave6_vpu_core_remove(struct platform_device *pdev)
{
	struct vpu_core_device *dev = dev_get_drvdata(&pdev->dev);

	pm_runtime_disable(&pdev->dev);

	wave6_vpu_enc_unregister_device(dev);
	wave6_vpu_dec_unregister_device(dev);
	wave6_free_dma(&dev->temp_vbuf);
	kfifo_free(&dev->irq_status);
	wave6_vpu_release_m2m_dev(dev);
	v4l2_device_unregister(&dev->v4l2_dev);
	wave6_vpu_core_unregister_device(dev);
	mutex_destroy(&dev->dev_lock);
	mutex_destroy(&dev->hw_lock);
	mutex_destroy(&dev->pause_lock);
}

static int __maybe_unused wave6_vpu_core_runtime_suspend(struct device *dev)
{
	struct vpu_core_device *core = dev_get_drvdata(dev);
	struct wave6_vpu_device *vpu = dev_get_drvdata(dev->parent);

	if (!core || !vpu || !vpu->ops)
		return -ENODEV;

	call_void_vop(vpu, put_vpu, &core->entity);
	if (core->num_clks)
		clk_bulk_disable_unprepare(core->num_clks, core->clks);

	return 0;
}

static int __maybe_unused wave6_vpu_core_runtime_resume(struct device *dev)
{
	struct vpu_core_device *core = dev_get_drvdata(dev);
	struct wave6_vpu_device *vpu = dev_get_drvdata(dev->parent);
	int ret = 0;

	if (!core || !vpu || !vpu->ops)
		return -ENODEV;

	if (core->num_clks) {
		ret = clk_bulk_prepare_enable(core->num_clks, core->clks);
		if (ret) {
			dev_err(dev, "failed to enable clocks: %d\n", ret);
			return ret;
		}
	}

	ret = call_vop(vpu, get_vpu, &core->entity);
	if (!ret)
		wave6_vpu_wait_activated(core);
	else if (core->num_clks)
		clk_bulk_disable_unprepare(core->num_clks, core->clks);

	return ret;
}

static int __maybe_unused wave6_vpu_core_suspend(struct device *dev)
{
	int ret;

	wave6_vpu_pause(dev, 0);

	ret = pm_runtime_force_suspend(dev);
	if (ret)
		wave6_vpu_pause(dev, 1);

	return ret;
}

static int __maybe_unused wave6_vpu_core_resume(struct device *dev)
{
	int ret;

	ret = pm_runtime_force_resume(dev);
	if (ret)
		return ret;

	wave6_vpu_pause(dev, 1);
	return 0;
}

static const struct dev_pm_ops wave6_vpu_core_pm_ops = {
	SET_RUNTIME_PM_OPS(wave6_vpu_core_runtime_suspend,
			   wave6_vpu_core_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(wave6_vpu_core_suspend,
				wave6_vpu_core_resume)
};

static const struct of_device_id wave6_vpu_core_ids[] = {
	{ .compatible = "nxp,imx95-vpu-core", .data = &wave633c_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, wave6_vpu_core_ids);

static struct platform_driver wave6_vpu_core_driver = {
	.driver = {
		.name = VPU_CORE_PLATFORM_DEVICE_NAME,
		.of_match_table = wave6_vpu_core_ids,
		.pm = &wave6_vpu_core_pm_ops,
	},
	.probe = wave6_vpu_core_probe,
	.remove = wave6_vpu_core_remove,
};

module_platform_driver(wave6_vpu_core_driver);
MODULE_DESCRIPTION("chips&media Wave6 VPU CORE V4L2 driver");
MODULE_LICENSE("Dual BSD/GPL");
