// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Wave6 series multi-standard codec IP - wave6 driver
 *
 * Copyright (C) 2025 CHIPS&MEDIA INC
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/genalloc.h>

#include "wave6-vpuconfig.h"
#include "wave6-regdefine.h"
#include "wave6-vpu.h"

static const struct wave6_vpu_resource wave633c_data = {
	.fw_name = "cnm/wave633c_imx9_codec_fw.bin",
	/* For HEVC, AVC, 4096x4096, 8bit */
	.sram_size = 0x14800,
};

static const char *wave6_vpu_state_name(enum wave6_vpu_state state)
{
	switch (state) {
	case WAVE6_VPU_STATE_OFF:
		return "off";
	case WAVE6_VPU_STATE_PREPARE:
		return "prepare";
	case WAVE6_VPU_STATE_ON:
		return "on";
	case WAVE6_VPU_STATE_SLEEP:
		return "sleep";
	default:
		return "unknown";
	}
}

static bool wave6_vpu_valid_transition(struct wave6_vpu_device *vpu,
				       enum wave6_vpu_state next)
{
	switch (vpu->state) {
	case WAVE6_VPU_STATE_OFF:
		/* to PREPARE: first boot attempt */
		/* to ON: already booted before, skipping boot */
		if (next == WAVE6_VPU_STATE_PREPARE ||
		    next == WAVE6_VPU_STATE_ON)
			return true;
		break;
	case WAVE6_VPU_STATE_PREPARE:
		/* to OFF: boot failed */
		/* to ON: boot successful */
		if (next == WAVE6_VPU_STATE_OFF ||
		    next == WAVE6_VPU_STATE_ON)
			return true;
		break;
	case WAVE6_VPU_STATE_ON:
		/* to OFF: sleep failed */
		/* to SLEEP: sleep successful */
		if (next == WAVE6_VPU_STATE_OFF ||
		    next == WAVE6_VPU_STATE_SLEEP)
			return true;
		break;
	case WAVE6_VPU_STATE_SLEEP:
		/* to OFF: resume failed */
		/* to ON: resume successful */
		if (next == WAVE6_VPU_STATE_OFF ||
		    next == WAVE6_VPU_STATE_ON)
			return true;
		break;
	}

	dev_err(vpu->dev, "invalid transition: %s -> %s\n",
		wave6_vpu_state_name(vpu->state), wave6_vpu_state_name(next));

	return false;
}

static void wave6_vpu_set_state(struct wave6_vpu_device *vpu,
				enum wave6_vpu_state state)
{
	if (!wave6_vpu_valid_transition(vpu, state))
		return;

	dev_dbg(vpu->dev, "set state: %s -> %s\n",
		wave6_vpu_state_name(vpu->state), wave6_vpu_state_name(state));

	vpu->state = state;
}

static int wave6_vpu_wait_busy(struct vpu_core_device *core)
{
	u32 val;

	return read_poll_timeout(wave6_vdi_readl, val, !val,
				 W6_VPU_POLL_DELAY_US, W6_VPU_POLL_TIMEOUT,
				 false, core->reg_base, W6_VPU_BUSY_STATUS);
}

static int wave6_vpu_check_result(struct vpu_core_device *core)
{
	if (wave6_vdi_readl(core->reg_base, W6_RET_SUCCESS))
		return 0;

	return wave6_vdi_readl(core->reg_base, W6_RET_FAIL_REASON);
}

static u32 wave6_vpu_get_code_buf_size(struct wave6_vpu_device *vpu)
{
	return min_t(u32, vpu->code_buf.size, W6_MAX_CODE_BUF_SIZE);
}

static void wave6_vpu_remap_code_buf(struct wave6_vpu_device *vpu)
{
	dma_addr_t code_base = vpu->code_buf.dma_addr;
	u32 i, reg_val;

	for (i = 0; i < wave6_vpu_get_code_buf_size(vpu) / W6_MAX_REMAP_PAGE_SIZE; i++) {
		reg_val = REMAP_CTRL_ON |
			  REMAP_CTRL_INDEX(i) |
			  REMAP_CTRL_PAGE_SIZE_ON |
			  REMAP_CTRL_PAGE_SIZE(W6_MAX_REMAP_PAGE_SIZE);
		wave6_vdi_writel(vpu->reg_base, W6_VPU_REMAP_CTRL_GB, reg_val);
		wave6_vdi_writel(vpu->reg_base, W6_VPU_REMAP_VADDR_GB,
				 i * W6_MAX_REMAP_PAGE_SIZE);
		wave6_vdi_writel(vpu->reg_base, W6_VPU_REMAP_PADDR_GB,
				 code_base + i * W6_MAX_REMAP_PAGE_SIZE);
	}
}

static void wave6_vpu_init_code_buf(struct wave6_vpu_device *vpu)
{
	if (vpu->code_buf.size < W6_CODE_BUF_SIZE) {
		dev_warn(vpu->dev,
			 "code buf size (%zu) is too small\n", vpu->code_buf.size);
		vpu->code_buf.phys_addr = 0;
		vpu->code_buf.size = 0;
		memset(&vpu->code_buf, 0, sizeof(vpu->code_buf));
		return;
	}

	vpu->code_buf.vaddr = devm_memremap(vpu->dev,
					    vpu->code_buf.phys_addr,
					    vpu->code_buf.size,
					    MEMREMAP_WC);
	if (!vpu->code_buf.vaddr) {
		memset(&vpu->code_buf, 0, sizeof(vpu->code_buf));
		return;
	}

	vpu->code_buf.dma_addr = dma_map_resource(vpu->dev,
						  vpu->code_buf.phys_addr,
						  vpu->code_buf.size,
						  DMA_BIDIRECTIONAL,
						  0);
	if (!vpu->code_buf.dma_addr) {
		memset(&vpu->code_buf, 0, sizeof(vpu->code_buf));
		return;
	}
}

static void wave6_vpu_allocate_work_buffers(struct wave6_vpu_device *vpu)
{
	struct vpu_buf *buf;
	int i;

	for (i = 0; i < MAX_NUM_INSTANCE; i++) {
		buf = &vpu->work_buffers[i];
		buf->size = W637DEC_WORKBUF_SIZE_FOR_CQ;

		if (wave6_vdi_alloc_dma(vpu->dev, buf)) {
			dev_warn(vpu->dev, "Failed to allocate work_buffers\n");
			return;
		}

		vpu->work_buffers_alloc++;
	}
}

static void wave6_vpu_free_work_buffers(struct wave6_vpu_device *vpu)
{
	int i;

	for (i = 0; i < vpu->work_buffers_alloc; i++)
		wave6_vdi_free_dma(&vpu->work_buffers[i]);

	vpu->work_buffers_alloc = 0;
	vpu->work_buffers_avail = 0;
}

static void wave6_vpu_init_work_buf(struct wave6_vpu_device *vpu,
				    struct vpu_core_device *core)
{
	int ret;

	lockdep_assert_held(&vpu->lock);

	wave6_vdi_writel(core->reg_base, W6_VPU_BUSY_STATUS, BUSY_STATUS_SET);
	wave6_vdi_writel(core->reg_base, W6_COMMAND, W6_CMD_INIT_WORK_BUF);
	wave6_vdi_writel(core->reg_base, W6_VPU_HOST_INT_REQ, HOST_INT_REQ_ON);

	ret = wave6_vpu_wait_busy(core);
	if (ret) {
		dev_err(vpu->dev, "init work buf failed\n");
		return;
	}

	ret = wave6_vpu_check_result(core);
	if (ret) {
		dev_err(vpu->dev, "init work buf failed, reason 0x%x\n", ret);
		return;
	}

	vpu->work_buffers_avail = vpu->work_buffers_alloc;
}

static int wave6_vpu_init_vpu(struct wave6_vpu_device *vpu,
			      struct vpu_core_device *core)
{
	int ret;

	lockdep_assert_held(&vpu->lock);

	/* try init directly as firmware is running */
	if (wave6_vdi_readl(core->reg_base, W6_VPU_VCPU_CUR_PC))
		goto init_done;

	wave6_vpu_set_state(vpu, WAVE6_VPU_STATE_PREPARE);

	wave6_vpu_remap_code_buf(vpu);

	wave6_vdi_writel(core->reg_base, W6_VPU_BUSY_STATUS, BUSY_STATUS_SET);
	wave6_vdi_writel(core->reg_base, W6_CMD_INIT_VPU_SEC_AXI_BASE_CORE0,
			 vpu->sram_buf.dma_addr);
	wave6_vdi_writel(core->reg_base, W6_CMD_INIT_VPU_SEC_AXI_SIZE_CORE0,
			 vpu->sram_buf.size);
	wave6_vdi_writel(vpu->reg_base, W6_COMMAND_GB, W6_CMD_INIT_VPU);
	wave6_vdi_writel(vpu->reg_base, W6_VPU_REMAP_CORE_START_GB,
			 REMAP_CORE_START_ON);

	ret = wave6_vpu_wait_busy(core);
	if (ret) {
		dev_err(vpu->dev, "init vpu timeout\n");
		wave6_vpu_set_state(vpu, WAVE6_VPU_STATE_OFF);
		return -EINVAL;
	}

	ret = wave6_vpu_check_result(core);
	if (ret) {
		dev_err(vpu->dev, "init vpu fail, reason 0x%x\n", ret);
		wave6_vpu_set_state(vpu, WAVE6_VPU_STATE_OFF);
		return -EIO;
	}

init_done:
	wave6_vpu_init_work_buf(vpu, core);
	wave6_vpu_set_state(vpu, WAVE6_VPU_STATE_ON);

	return 0;
}

static int wave6_vpu_sleep(struct wave6_vpu_device *vpu,
			   struct vpu_core_device *core)
{
	int ret;

	lockdep_assert_held(&vpu->lock);

	if (!wave6_vdi_readl(core->reg_base, W6_VPU_VCPU_CUR_PC)) {
		wave6_vpu_set_state(vpu, WAVE6_VPU_STATE_OFF);
		return 0;
	}

	wave6_vdi_writel(core->reg_base, W6_VPU_BUSY_STATUS, BUSY_STATUS_SET);
	wave6_vdi_writel(core->reg_base, W6_COMMAND, W6_CMD_SLEEP_VPU);
	wave6_vdi_writel(core->reg_base, W6_VPU_HOST_INT_REQ, HOST_INT_REQ_ON);

	ret = wave6_vpu_wait_busy(core);
	if (ret) {
		dev_err(vpu->dev, "sleep vpu timeout\n");
		wave6_vpu_set_state(vpu, WAVE6_VPU_STATE_OFF);
		return -EINVAL;
	}

	ret = wave6_vpu_check_result(core);
	if (ret) {
		dev_err(vpu->dev, "sleep vpu fail, reason 0x%x\n", ret);
		wave6_vpu_set_state(vpu, WAVE6_VPU_STATE_OFF);
		return -EIO;
	}

	wave6_vpu_set_state(vpu, WAVE6_VPU_STATE_SLEEP);

	return 0;
}

static int wave6_vpu_wakeup(struct wave6_vpu_device *vpu,
			    struct vpu_core_device *core)
{
	int ret;

	lockdep_assert_held(&vpu->lock);

	/* try wakeup directly as firmware is running */
	if (wave6_vdi_readl(core->reg_base, W6_VPU_VCPU_CUR_PC))
		goto wakeup_done;

	wave6_vpu_remap_code_buf(vpu);

	wave6_vdi_writel(core->reg_base, W6_VPU_BUSY_STATUS, BUSY_STATUS_SET);
	wave6_vdi_writel(core->reg_base, W6_CMD_INIT_VPU_SEC_AXI_BASE_CORE0,
			 vpu->sram_buf.dma_addr);
	wave6_vdi_writel(core->reg_base, W6_CMD_INIT_VPU_SEC_AXI_SIZE_CORE0,
			 vpu->sram_buf.size);
	wave6_vdi_writel(vpu->reg_base, W6_COMMAND_GB, W6_CMD_WAKEUP_VPU);
	wave6_vdi_writel(vpu->reg_base, W6_VPU_REMAP_CORE_START_GB,
			 REMAP_CORE_START_ON);

	ret = wave6_vpu_wait_busy(core);
	if (ret) {
		dev_err(vpu->dev, "wakeup vpu timeout\n");
		wave6_vpu_set_state(vpu, WAVE6_VPU_STATE_OFF);
		return -EINVAL;
	}

	ret = wave6_vpu_check_result(core);
	if (ret) {
		dev_err(vpu->dev, "wakeup vpu fail, reason 0x%x\n", ret);
		wave6_vpu_set_state(vpu, WAVE6_VPU_STATE_OFF);
		return -EIO;
	}

wakeup_done:
	wave6_vpu_set_state(vpu, WAVE6_VPU_STATE_ON);

	return 0;
}

static int wave6_vpu_try_boot(struct wave6_vpu_device *vpu,
			      struct vpu_core_device *core)
{
	u32 product_code;
	int ret;

	lockdep_assert_held(&vpu->lock);

	if (vpu->state != WAVE6_VPU_STATE_OFF && vpu->state != WAVE6_VPU_STATE_SLEEP)
		return 0;

	product_code = wave6_vdi_readl(core->reg_base, W6_VPU_RET_PRODUCT_CODE);
	if (!PRODUCT_CODE_W_SERIES(product_code)) {
		dev_err(vpu->dev, "unknown product : %08x\n", product_code);
		return -EINVAL;
	}

	if (vpu->state == WAVE6_VPU_STATE_SLEEP) {
		ret = wave6_vpu_wakeup(vpu, core);
		return ret;
	}

	ret = wave6_vpu_init_vpu(vpu, core);

	return ret;
}

static int wave6_vpu_get(struct wave6_vpu_device *vpu,
			 struct vpu_core_device *core)
{
	int ret;

	if (WARN_ON(!vpu || !core))
		return -EINVAL;

	guard(mutex)(&vpu->lock);

	if (!vpu->fw_available)
		return -EINVAL;

	/* Only the first core executes boot; others return */
	if (atomic_inc_return(&vpu->core_count) > 1)
		return 0;

	ret = pm_runtime_resume_and_get(vpu->dev);
	if (ret)
		goto error_pm;

	ret = wave6_vpu_try_boot(vpu, core);
	if (ret)
		goto error_boot;

	return 0;

error_boot:
	pm_runtime_put_sync(vpu->dev);
error_pm:
	atomic_dec(&vpu->core_count);

	return ret;
}

static void wave6_vpu_put(struct wave6_vpu_device *vpu,
			  struct vpu_core_device *core)
{
	if (WARN_ON(!vpu || !core))
		return;

	guard(mutex)(&vpu->lock);

	if (!vpu->fw_available)
		return;

	/* Only the last core executes sleep; others return */
	if (atomic_dec_return(&vpu->core_count) > 0)
		return;

	wave6_vpu_sleep(vpu, core);

	if (!pm_runtime_suspended(vpu->dev))
		pm_runtime_put_sync(vpu->dev);
}

static void wave6_vpu_require_work_buffer(struct wave6_vpu_device *vpu,
					  struct vpu_core_device *core)
{
	struct vpu_buf *vb;
	u32 size;

	if (WARN_ON(!vpu || !core))
		return;

	size = wave6_vdi_readl(core->reg_base, W6_CMD_SET_WORK_BUF_SIZE);
	if (!size)
		return;

	if (WARN_ON(size > W637DEC_WORKBUF_SIZE_FOR_CQ))
		goto exit;

	if (WARN_ON(vpu->work_buffers_avail <= 0))
		goto exit;

	vpu->work_buffers_avail--;
	vb = &vpu->work_buffers[vpu->work_buffers_avail];

	wave6_vdi_writel(core->reg_base, W6_CMD_SET_WORK_BUF_ADDR, vb->daddr);

exit:
	wave6_vdi_writel(core->reg_base, W6_CMD_SET_WORK_BUF_SIZE, SET_WORK_BUF_SIZE_ACK);
}

static void wave6_vpu_release(struct wave6_vpu_device *vpu)
{
	guard(mutex)(&vpu->lock);

	vpu->fw_available = false;
	wave6_vpu_free_work_buffers(vpu);
	if (vpu->sram_pool && vpu->sram_buf.vaddr) {
		dma_unmap_resource(vpu->dev,
				   vpu->sram_buf.dma_addr,
				   vpu->sram_buf.size,
				   DMA_BIDIRECTIONAL,
				   0);
		gen_pool_free(vpu->sram_pool,
			      (unsigned long)vpu->sram_buf.vaddr,
			      vpu->sram_buf.size);
	}
	if (vpu->code_buf.dma_addr)
		dma_unmap_resource(vpu->dev,
				   vpu->code_buf.dma_addr,
				   vpu->code_buf.size,
				   DMA_BIDIRECTIONAL,
				   0);
}

static void wave6_vpu_load_firmware(const struct firmware *fw, void *context)
{
	struct wave6_vpu_device *vpu = context;

	guard(mutex)(&vpu->lock);

	if (!fw || !fw->data) {
		dev_err(vpu->dev, "No firmware.\n");
		return;
	}

	if (!vpu->fw_available)
		goto exit;

	if (fw->size + W6_EXTRA_CODE_BUF_SIZE > wave6_vpu_get_code_buf_size(vpu)) {
		dev_err(vpu->dev, "firmware size (%zd > %zd) is too big\n",
			fw->size, vpu->code_buf.size);
		vpu->fw_available = false;
		goto exit;
	}

	memcpy(vpu->code_buf.vaddr, fw->data, fw->size);

	vpu->get_vpu = wave6_vpu_get;
	vpu->put_vpu = wave6_vpu_put;
	vpu->req_work_buffer = wave6_vpu_require_work_buffer;
	of_platform_populate(vpu->dev->of_node, NULL, NULL, vpu->dev);

exit:
	release_firmware(fw);
}

static int wave6_vpu_probe(struct platform_device *pdev)
{
	struct device_node *np;
	struct wave6_vpu_device *vpu;
	const struct wave6_vpu_resource *res;
	int ret;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to set DMA mask: %d\n", ret);
		return ret;
	}

	res = of_device_get_match_data(&pdev->dev);
	if (!res)
		return -ENODEV;

	vpu = devm_kzalloc(&pdev->dev, sizeof(*vpu), GFP_KERNEL);
	if (!vpu)
		return -ENOMEM;

	ret = devm_mutex_init(&pdev->dev, &vpu->lock);
	if (ret)
		return ret;

	atomic_set(&vpu->core_count, 0);
	dev_set_drvdata(&pdev->dev, vpu);
	vpu->dev = &pdev->dev;
	vpu->res = res;
	vpu->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vpu->reg_base))
		return PTR_ERR(vpu->reg_base);

	ret = devm_clk_bulk_get_all(&pdev->dev, &vpu->clks);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "failed to get clocks\n");

	vpu->num_clks = ret;

	np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (np) {
		struct resource mem;

		ret = of_address_to_resource(np, 0, &mem);
		of_node_put(np);
		if (!ret) {
			vpu->code_buf.phys_addr = mem.start;
			vpu->code_buf.size = resource_size(&mem);
			wave6_vpu_init_code_buf(vpu);
		} else {
			dev_warn(&pdev->dev, "memory-region is not available.\n");
		}
	}

	vpu->sram_pool = of_gen_pool_get(pdev->dev.of_node, "sram", 0);
	if (vpu->sram_pool) {
		vpu->sram_buf.size = vpu->res->sram_size;
		vpu->sram_buf.vaddr = gen_pool_dma_alloc(vpu->sram_pool,
							 vpu->sram_buf.size,
							 &vpu->sram_buf.phys_addr);
		if (!vpu->sram_buf.vaddr)
			vpu->sram_buf.size = 0;
		else
			vpu->sram_buf.dma_addr = dma_map_resource(&pdev->dev,
								  vpu->sram_buf.phys_addr,
								  vpu->sram_buf.size,
								  DMA_BIDIRECTIONAL,
								  0);
	}

	vpu->thermal.dev = &pdev->dev;
	ret = wave6_vpu_cooling_init(&vpu->thermal);
	if (ret)
		dev_err(&pdev->dev, "failed to initialize thermal cooling, ret = %d\n", ret);

	wave6_vpu_allocate_work_buffers(vpu);

	pm_runtime_enable(&pdev->dev);
	vpu->fw_available = true;

	ret = firmware_request_nowait_nowarn(THIS_MODULE,
					     vpu->res->fw_name,
					     &pdev->dev,
					     GFP_KERNEL,
					     vpu,
					     wave6_vpu_load_firmware);
	if (ret) {
		dev_err(&pdev->dev, "request firmware fail, ret = %d\n", ret);
		goto error;
	}

	return 0;

error:
	wave6_vpu_release(vpu);
	wave6_vpu_cooling_remove(&vpu->thermal);
	pm_runtime_disable(&pdev->dev);

	return ret;
}

static void wave6_vpu_remove(struct platform_device *pdev)
{
	struct wave6_vpu_device *vpu = dev_get_drvdata(&pdev->dev);

	wave6_vpu_release(vpu);
	wave6_vpu_cooling_remove(&vpu->thermal);
	of_platform_depopulate(vpu->dev);
	pm_runtime_disable(vpu->dev);
}

static int __maybe_unused wave6_vpu_runtime_suspend(struct device *dev)
{
	struct wave6_vpu_device *vpu = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(vpu->num_clks, vpu->clks);

	return 0;
}

static int __maybe_unused wave6_vpu_runtime_resume(struct device *dev)
{
	struct wave6_vpu_device *vpu = dev_get_drvdata(dev);

	return clk_bulk_prepare_enable(vpu->num_clks, vpu->clks);
}

static const struct dev_pm_ops wave6_vpu_pm_ops = {
	SET_RUNTIME_PM_OPS(wave6_vpu_runtime_suspend,
			   wave6_vpu_runtime_resume, NULL)
};

static const struct of_device_id wave6_vpu_ids[] = {
	{ .compatible = "nxp,imx95-vpu", .data = &wave633c_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, wave6_vpu_ids);

static struct platform_driver wave6_vpu_driver = {
	.driver = {
		.name = WAVE6_VPU_PLATFORM_DRIVER_NAME,
		.of_match_table = wave6_vpu_ids,
		.pm = &wave6_vpu_pm_ops,
	},
	.probe = wave6_vpu_probe,
	.remove = wave6_vpu_remove,
};

module_platform_driver(wave6_vpu_driver);
MODULE_DESCRIPTION("chips&media Wave6 VPU driver");
MODULE_LICENSE("Dual BSD/GPL");
