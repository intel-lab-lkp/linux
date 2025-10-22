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
#include <linux/iopoll.h>
#include "wave6-vpu-core.h"
#include "wave6-regdefine.h"
#include "wave6-vpuconfig.h"
#include "wave6-hw.h"
#include "wave6-vpu-dbg.h"

#define CREATE_TRACE_POINTS
#include "wave6-trace.h"

#define WAVE6_VPU_CORE_PLATFORM_DRIVER_NAME "wave6-vpu-core"
#define WAVE6_VPU_DEBUGFS_DIR "wave6"

#define WAVE6_IS_ENC BIT(0)
#define WAVE6_IS_DEC BIT(1)

static const struct wave6_vpu_core_resource wave633c_core_data = {
	.codec_types = WAVE6_IS_ENC | WAVE6_IS_DEC,
	.compatible_fw_version = 0x2010000,
};

static irqreturn_t wave6_vpu_core_irq(int irq, void *dev_id)
{
	struct vpu_core_device *core = dev_id;
	struct vpu_irq irq_info;

	if (!vpu_read_reg(core, W6_VPU_VPU_INT_STS))
		return IRQ_NONE;

	irq_info.status = vpu_read_reg(core, W6_VPU_VINT_REASON);
	irq_info.inst_idc = vpu_read_reg(core, W6_RET_INT_INSTANCE_INFO);

	vpu_write_reg(core, W6_RET_INT_INSTANCE_INFO, INT_INSTANCE_INFO_CLEAR);
	vpu_write_reg(core, W6_VPU_VINT_REASON_CLEAR, irq_info.status);
	vpu_write_reg(core, W6_VPU_VINT_CLEAR, VINT_CLEAR);

	trace_wave6_vpu_irq(core, irq_info.status, irq_info.inst_idc);

	if (irq_info.status & BIT(W6_INT_BIT_REQ_WORK_BUF)) {
		if (core->vpu)
			core->vpu->req_work_buffer(core->vpu, core);

		return IRQ_HANDLED;
	}

	kfifo_in(&core->irq_fifo, &irq_info, sizeof(struct vpu_irq));

	return IRQ_WAKE_THREAD;
}

static struct vpu_instance *wave6_vpu_core_get_instance(struct vpu_core_device *core,
							u32 inst_idc)
{
	struct vpu_instance *inst;

	guard(spinlock)(&core->inst_lock);

	list_for_each_entry(inst, &core->instances, list) {
		if (BIT(inst->id) & inst_idc)
			return inst;
	}

	return NULL;
}

static irqreturn_t wave6_vpu_core_irq_thread(int irq, void *dev_id)
{
	struct vpu_core_device *core = dev_id;
	struct vpu_instance *inst;
	struct vpu_irq irq_info;

	while (kfifo_len(&core->irq_fifo)) {
		bool error = false;

		if (!kfifo_out(&core->irq_fifo, &irq_info, sizeof(struct vpu_irq)))
			break;

		inst = wave6_vpu_core_get_instance(core, irq_info.inst_idc);
		if (!inst)
			break;

		if ((irq_info.status & BIT(W6_INT_BIT_INIT_SEQ)) ||
		    (irq_info.status & BIT(W6_INT_BIT_ENC_SET_PARAM))) {
			complete(&inst->irq_done);
			continue;
		}

		if (irq_info.status & BIT(W6_INT_BIT_BSBUF_ERROR))
			error = true;

		if (inst->ops && inst->ops->finish_process)
			inst->ops->finish_process(inst, error);
	}

	return IRQ_HANDLED;
}

static void wave6_vpu_core_check_state(struct vpu_core_device *core)
{
	u32 val;
	int ret;

	guard(mutex)(&core->hw_lock);

	ret = read_poll_timeout(vpu_read_reg, val, val != 0,
				W6_VPU_POLL_DELAY_US, W6_VPU_POLL_TIMEOUT,
				false, core, W6_VPU_VCPU_CUR_PC);
	if (ret)
		return;

	wave6_vpu_enable_interrupt(core);
	ret = wave6_vpu_get_version(core);
	if (ret) {
		dev_err(core->dev, "wave6_vpu_get_version fail\n");
		return;
	}

	dev_dbg(core->dev, "product 0x%x, fw_ver %d.%d.%d(r%d), hw_ver 0x%x\n",
		core->attr.product_code,
		FW_VERSION_MAJOR(core->attr.fw_version),
		FW_VERSION_MINOR(core->attr.fw_version),
		FW_VERSION_REL(core->attr.fw_version),
		core->attr.fw_revision,
		core->attr.hw_version);

	if (core->attr.fw_version < core->res->compatible_fw_version)
		dev_err(core->dev, "fw version is too low (< v%d.%d.%d)\n",
			FW_VERSION_MAJOR(core->res->compatible_fw_version),
			FW_VERSION_MINOR(core->res->compatible_fw_version),
			FW_VERSION_REL(core->res->compatible_fw_version));
}

void wave6_vpu_core_activate(struct vpu_core_device *core)
{
	core->active = true;
}

static void wave6_vpu_core_wait_activated(struct vpu_core_device *core)
{
	if (core->active)
		wave6_vpu_core_check_state(core);
}

static int wave6_vpu_core_probe(struct platform_device *pdev)
{
	struct vpu_core_device *core;
	const struct wave6_vpu_core_resource *res;
	int ret;
	int irq;

	res = device_get_match_data(&pdev->dev);
	if (!res)
		return -ENODEV;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to set DMA mask: %d\n", ret);
		return ret;
	}

	core = devm_kzalloc(&pdev->dev, sizeof(*core), GFP_KERNEL);
	if (!core)
		return -ENOMEM;

	ret = devm_mutex_init(&pdev->dev, &core->dev_lock);
	if (ret)
		return ret;

	ret = devm_mutex_init(&pdev->dev, &core->hw_lock);
	if (ret)
		return ret;

	spin_lock_init(&core->inst_lock);
	INIT_LIST_HEAD(&core->instances);
	dev_set_drvdata(&pdev->dev, core);
	core->dev = &pdev->dev;
	core->res = res;

	if (pdev->dev.parent->driver && pdev->dev.parent->driver->name &&
	    !strcmp(pdev->dev.parent->driver->name, WAVE6_VPU_PLATFORM_DRIVER_NAME))
		core->vpu = dev_get_drvdata(pdev->dev.parent);

	core->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(core->reg_base))
		return PTR_ERR(core->reg_base);

	ret = devm_clk_bulk_get_all(&pdev->dev, &core->clks);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "failed to get clocks\n");

	core->num_clks = ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_threaded_irq(&pdev->dev, irq,
					wave6_vpu_core_irq,
					wave6_vpu_core_irq_thread,
					0, "vpu_irq", core);
	if (ret) {
		dev_err(&pdev->dev, "failed to request IRQ: %d\n", ret);
		return ret;
	}

	ret = v4l2_device_register(&pdev->dev, &core->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to register v4l2_dev: %d\n", ret);
		return ret;
	}

	ret = wave6_vpu_init_m2m_dev(core);
	if (ret)
		goto err_v4l2_unregister;

	ret = kfifo_alloc(&core->irq_fifo,
			  MAX_NUM_INSTANCE * sizeof(struct vpu_irq),
			  GFP_KERNEL);
	if (ret) {
		dev_err(&pdev->dev, "failed to allocate fifo\n");
		goto err_m2m_dev_release;
	}

	core->temp_vbuf.size = ALIGN(W6_TEMPBUF_SIZE, 4096);
	ret = wave6_vdi_alloc_dma(core->dev, &core->temp_vbuf);
	if (ret) {
		dev_err(&pdev->dev, "failed to allocate temp_vbuf: %d\n", ret);
		goto err_kfifo_free;
	}

	core->debugfs = debugfs_lookup(WAVE6_VPU_DEBUGFS_DIR, NULL);
	if (IS_ERR_OR_NULL(core->debugfs))
		core->debugfs = debugfs_create_dir(WAVE6_VPU_DEBUGFS_DIR, NULL);

	pm_runtime_enable(&pdev->dev);

	if (core->res->codec_types & WAVE6_IS_DEC) {
		ret = wave6_vpu_dec_register_device(core);
		if (ret) {
			dev_err(&pdev->dev,
				"failed to register video_dev_dec: %d\n", ret);
			goto err_temp_vbuf_free;
		}
	}
	if (core->res->codec_types & WAVE6_IS_ENC) {
		ret = wave6_vpu_enc_register_device(core);
		if (ret) {
			dev_err(&pdev->dev,
				"failed to register video_dev_enc: %d\n", ret);
			goto err_dec_unreg;
		}
	}

	dev_dbg(&pdev->dev, "Added wave6 driver with caps %s %s\n",
		core->res->codec_types & WAVE6_IS_ENC ? "'ENCODE'" : "",
		core->res->codec_types & WAVE6_IS_DEC ? "'DECODE'" : "");

	return 0;

err_dec_unreg:
	if (core->res->codec_types & WAVE6_IS_DEC)
		wave6_vpu_dec_unregister_device(core);
err_temp_vbuf_free:
	wave6_vdi_free_dma(&core->temp_vbuf);
err_kfifo_free:
	kfifo_free(&core->irq_fifo);
err_m2m_dev_release:
	wave6_vpu_release_m2m_dev(core);
err_v4l2_unregister:
	v4l2_device_unregister(&core->v4l2_dev);

	return ret;
}

static void wave6_vpu_core_remove(struct platform_device *pdev)
{
	struct vpu_core_device *core = dev_get_drvdata(&pdev->dev);

	pm_runtime_disable(&pdev->dev);

	wave6_vpu_enc_unregister_device(core);
	wave6_vpu_dec_unregister_device(core);
	wave6_vdi_free_dma(&core->temp_vbuf);
	kfifo_free(&core->irq_fifo);
	wave6_vpu_release_m2m_dev(core);
	v4l2_device_unregister(&core->v4l2_dev);
}

static int __maybe_unused wave6_vpu_core_runtime_suspend(struct device *dev)
{
	struct vpu_core_device *core = dev_get_drvdata(dev);

	if (WARN_ON(!core))
		return -ENODEV;

	/*
	 * Only call parent VPU put_vpu if the core has a parent and is active.
	 * - core->vpu: prevent access in core without parent VPU.
	 * - core->active: execute sleep only after m2m streaming is started.
	 */
	if (core->vpu && core->active)
		core->vpu->put_vpu(core->vpu, core);

	if (core->num_clks)
		clk_bulk_disable_unprepare(core->num_clks, core->clks);

	return 0;
}

static int __maybe_unused wave6_vpu_core_runtime_resume(struct device *dev)
{
	struct vpu_core_device *core = dev_get_drvdata(dev);
	int ret = 0;

	if (WARN_ON(!core))
		return -ENODEV;

	if (core->num_clks) {
		ret = clk_bulk_prepare_enable(core->num_clks, core->clks);
		if (ret) {
			dev_err(dev, "failed to enable clocks: %d\n", ret);
			return ret;
		}
	}

	/*
	 * Only call parent VPU get_vpu if the core has a parent and is active.
	 * - core->vpu: prevent access in core without parent VPU.
	 * - core->active: execute boot only after m2m streaming is started.
	 */
	if (core->vpu && core->active)
		ret = core->vpu->get_vpu(core->vpu, core);

	if (!ret)
		wave6_vpu_core_wait_activated(core);
	else if (core->num_clks)
		clk_bulk_disable_unprepare(core->num_clks, core->clks);

	return ret;
}

static int __maybe_unused wave6_vpu_core_suspend(struct device *dev)
{
	struct vpu_core_device *core = dev_get_drvdata(dev);
	int ret;

	v4l2_m2m_suspend(core->m2m_dev);

	ret = pm_runtime_force_suspend(dev);
	if (ret)
		v4l2_m2m_resume(core->m2m_dev);

	return ret;
}

static int __maybe_unused wave6_vpu_core_resume(struct device *dev)
{
	struct vpu_core_device *core = dev_get_drvdata(dev);
	int ret;

	ret = pm_runtime_force_resume(dev);
	if (ret)
		return ret;

	v4l2_m2m_resume(core->m2m_dev);

	return 0;
}

static const struct dev_pm_ops wave6_vpu_core_pm_ops = {
	SET_RUNTIME_PM_OPS(wave6_vpu_core_runtime_suspend,
			   wave6_vpu_core_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(wave6_vpu_core_suspend,
				wave6_vpu_core_resume)
};

static const struct of_device_id wave6_vpu_core_ids[] = {
	{ .compatible = "nxp,imx95-vpu-core", .data = &wave633c_core_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, wave6_vpu_core_ids);

static struct platform_driver wave6_vpu_core_driver = {
	.driver = {
		.name = WAVE6_VPU_CORE_PLATFORM_DRIVER_NAME,
		.of_match_table = wave6_vpu_core_ids,
		.pm = &wave6_vpu_core_pm_ops,
	},
	.probe = wave6_vpu_core_probe,
	.remove = wave6_vpu_core_remove,
};

module_platform_driver(wave6_vpu_core_driver);
MODULE_DESCRIPTION("chips&media Wave6 VPU CORE V4L2 driver");
MODULE_LICENSE("Dual BSD/GPL");
