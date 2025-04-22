// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Wave6 series multi-standard codec IP - wave6 control driver
 *
 * Copyright (C) 2025 CHIPS&MEDIA INC
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/genalloc.h>
#include <linux/thermal.h>
#include <linux/units.h>
#include <linux/pm_opp.h>

#include "wave6-vpuconfig.h"
#include "wave6-regdefine.h"
#include "wave6-vpu.h"

#define VPU_CTRL_PLATFORM_DEVICE_NAME "wave6-vpu-ctrl"

static bool wave6_cooling_disable;
module_param(wave6_cooling_disable, bool, 0644);
MODULE_PARM_DESC(wave6_cooling_disable, "enable or disable cooling");

enum wave6_vpu_state {
	WAVE6_VPU_STATE_OFF,
	WAVE6_VPU_STATE_PREPARE,
	WAVE6_VPU_STATE_ON,
	WAVE6_VPU_STATE_SLEEP
};

struct vpu_ctrl_resource {
	const char *fw_name;
	u32 sram_size;
};

struct vpu_ctrl_buf {
	struct list_head list;
	struct vpu_buf buf;
};

struct vpu_dma_buf {
	size_t size;
	dma_addr_t dma_addr;
	void *vaddr;
	phys_addr_t phys_addr;
};

struct vpu_ctrl {
	struct device *dev;
	void __iomem *reg_base;
	struct clk_bulk_data *clks;
	int num_clks;
	struct vpu_dma_buf boot_mem;
	u32 state;
	struct mutex lock; /* the lock for vpu control device */
	struct list_head entities;
	const struct vpu_ctrl_resource *res;
	struct gen_pool *sram_pool;
	struct vpu_dma_buf sram_buf;
	struct list_head buffers;
	int thermal_event;
	int thermal_max;
	struct thermal_cooling_device *cooling;
	unsigned long *freq_table;
	bool available;
};

static const struct vpu_ctrl_resource wave633c_ctrl_data = {
	.fw_name = "wave633c_codec_fw.bin",
	/* For HEVC, AVC, 4096x4096, 8bit */
	.sram_size = 0x14800,
};

static void wave6_vpu_ctrl_writel(struct device *dev, u32 addr, u32 data)
{
	struct vpu_ctrl *ctrl = dev_get_drvdata(dev);

	writel(data, ctrl->reg_base + addr);
}

static const char *wave6_vpu_ctrl_state_name(u32 state)
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

static void wave6_vpu_ctrl_set_state(struct vpu_ctrl *ctrl, u32 state)
{
	dev_dbg(ctrl->dev, "set state: %s -> %s\n",
		wave6_vpu_ctrl_state_name(ctrl->state), wave6_vpu_ctrl_state_name(state));
	ctrl->state = state;
}

static int wave6_vpu_ctrl_wait_busy(struct wave6_vpu_entity *entity)
{
	u32 val;

	return read_poll_timeout(entity->read_reg, val, !val,
				 W6_VPU_POLL_DELAY_US, W6_VPU_POLL_TIMEOUT,
				 false, entity->dev, W6_VPU_BUSY_STATUS);
}

static int wave6_vpu_ctrl_check_result(struct wave6_vpu_entity *entity)
{
	if (entity->read_reg(entity->dev, W6_RET_SUCCESS))
		return 0;

	return entity->read_reg(entity->dev, W6_RET_FAIL_REASON);
}

static u32 wave6_vpu_ctrl_get_code_buf_size(struct vpu_ctrl *ctrl)
{
	return min_t(u32, ctrl->boot_mem.size, WAVE6_MAX_CODE_BUF_SIZE);
}

static void wave6_vpu_ctrl_remap_code_buffer(struct vpu_ctrl *ctrl)
{
	dma_addr_t code_base = ctrl->boot_mem.dma_addr;
	u32 i, reg_val, remap_size;

	for (i = 0; i < wave6_vpu_ctrl_get_code_buf_size(ctrl) / W6_REMAP_MAX_SIZE; i++) {
		remap_size = (W6_REMAP_MAX_SIZE >> 12) & 0x1ff;
		reg_val = 0x80000000 |
			  (WAVE6_UPPER_PROC_AXI_ID << 20) |
			  (0 << 16) |
			  (i << 12) |
			  BIT(11) |
			  remap_size;
		wave6_vpu_ctrl_writel(ctrl->dev, W6_VPU_REMAP_CTRL_GB, reg_val);
		wave6_vpu_ctrl_writel(ctrl->dev, W6_VPU_REMAP_VADDR_GB, i * W6_REMAP_MAX_SIZE);
		wave6_vpu_ctrl_writel(ctrl->dev, W6_VPU_REMAP_PADDR_GB,
				      code_base + i * W6_REMAP_MAX_SIZE);
	}
}

static int wave6_vpu_ctrl_init_vpu(struct vpu_ctrl *ctrl,
				   struct wave6_vpu_entity *entity)
{
	int ret;

	entity->write_reg(entity->dev, W6_VPU_BUSY_STATUS, 1);
	entity->write_reg(entity->dev, W6_CMD_INIT_VPU_SEC_AXI_BASE_CORE0,
				       ctrl->sram_buf.dma_addr);
	entity->write_reg(entity->dev, W6_CMD_INIT_VPU_SEC_AXI_SIZE_CORE0,
				       ctrl->sram_buf.size);
	wave6_vpu_ctrl_writel(ctrl->dev, W6_COMMAND_GB, W6_CMD_INIT_VPU);
	wave6_vpu_ctrl_writel(ctrl->dev, W6_VPU_REMAP_CORE_START_GB, 1);

	ret = wave6_vpu_ctrl_wait_busy(entity);
	if (ret) {
		dev_err(ctrl->dev, "init vpu timeout\n");
		return -EINVAL;
	}

	ret = wave6_vpu_ctrl_check_result(entity);
	if (ret) {
		dev_err(ctrl->dev, "init vpu fail, reason 0x%x\n", ret);
		return -EIO;
	}

	return 0;
}

static void wave6_vpu_ctrl_on_boot(struct wave6_vpu_entity *entity)
{
	if (!entity->on_boot)
		return;

	entity->on_boot(entity->dev);
}

static void wave6_vpu_ctrl_clear_firmware_buffers(struct vpu_ctrl *ctrl,
						  struct wave6_vpu_entity *entity)
{
	int ret;

	entity->write_reg(entity->dev, W6_VPU_BUSY_STATUS, 1);
	entity->write_reg(entity->dev, W6_COMMAND, W6_CMD_INIT_WORK_BUF);
	entity->write_reg(entity->dev, W6_VPU_HOST_INT_REQ, 1);

	ret = wave6_vpu_ctrl_wait_busy(entity);
	if (ret) {
		dev_err(ctrl->dev, "set buffer failed\n");
		return;
	}

	ret = wave6_vpu_ctrl_check_result(entity);
	if (ret) {
		dev_err(ctrl->dev, "set buffer failed, reason 0x%x\n", ret);
		return;
	}
}

static int wave6_vpu_ctrl_require_buffer(struct device *dev,
					 struct wave6_vpu_entity *entity)
{
	struct vpu_ctrl *ctrl = dev_get_drvdata(dev);
	struct vpu_ctrl_buf *pbuf;
	u32 size;
	int ret = -ENOMEM;

	if (!ctrl || !entity)
		return -EINVAL;

	size = entity->read_reg(entity->dev, W6_CMD_SET_CTRL_WORK_BUF_SIZE);
	if (!size)
		return 0;

	pbuf = kzalloc(sizeof(*pbuf), GFP_KERNEL);
	if (!pbuf)
		goto exit;

	pbuf->buf.size = size;
	ret = wave6_alloc_dma(ctrl->dev, &pbuf->buf);
	if (ret) {
		kfree(pbuf);
		goto exit;
	}

	list_add_tail(&pbuf->list, &ctrl->buffers);
	entity->write_reg(entity->dev, W6_CMD_SET_CTRL_WORK_BUF_ADDR, pbuf->buf.daddr);
exit:
	entity->write_reg(entity->dev, W6_CMD_SET_CTRL_WORK_BUF_SIZE, 0);
	return ret;
}

static void wave6_vpu_ctrl_clear_buffers(struct vpu_ctrl *ctrl)
{
	struct wave6_vpu_entity *entity;
	struct vpu_ctrl_buf *pbuf, *tmp;

	entity = list_first_entry_or_null(&ctrl->entities,
					  struct wave6_vpu_entity, list);
	if (entity)
		wave6_vpu_ctrl_clear_firmware_buffers(ctrl, entity);

	list_for_each_entry_safe(pbuf, tmp, &ctrl->buffers, list) {
		list_del(&pbuf->list);
		wave6_free_dma(&pbuf->buf);
		kfree(pbuf);
	}
}

static void wave6_vpu_ctrl_boot_done(struct vpu_ctrl *ctrl, int wakeup)
{
	struct wave6_vpu_entity *entity;

	if (ctrl->state == WAVE6_VPU_STATE_ON)
		return;

	if (!wakeup)
		wave6_vpu_ctrl_clear_buffers(ctrl);

	list_for_each_entry(entity, &ctrl->entities, list)
		wave6_vpu_ctrl_on_boot(entity);

	dev_dbg(ctrl->dev, "boot done from %s\n", wakeup ? "wakeup" : "cold boot");

	wave6_vpu_ctrl_set_state(ctrl, WAVE6_VPU_STATE_ON);
}

static bool wave6_vpu_ctrl_find_entity(struct vpu_ctrl *ctrl, struct wave6_vpu_entity *entity)
{
	struct wave6_vpu_entity *tmp;

	list_for_each_entry(tmp, &ctrl->entities, list) {
		if (tmp == entity)
			return true;
	}

	return false;
}

static int wave6_vpu_ctrl_boot(struct vpu_ctrl *ctrl,
			       struct wave6_vpu_entity *entity)
{
	u32 product_code;
	int ret = 0;

	product_code = entity->read_reg(entity->dev, W6_VPU_RET_PRODUCT_VERSION);
	if (!PRODUCT_CODE_W_SERIES(product_code)) {
		dev_err(ctrl->dev, "unknown product : %08x\n", product_code);
		return -EINVAL;
	}

	wave6_vpu_ctrl_remap_code_buffer(ctrl);
	ret = wave6_vpu_ctrl_init_vpu(ctrl, entity);
	if (ret)
		wave6_vpu_ctrl_set_state(ctrl, WAVE6_VPU_STATE_OFF);
	else
		wave6_vpu_ctrl_boot_done(ctrl, 0);

	return ret;
}

static int wave6_vpu_ctrl_sleep(struct vpu_ctrl *ctrl, struct wave6_vpu_entity *entity)
{
	int ret;

	entity->write_reg(entity->dev, W6_VPU_BUSY_STATUS, 1);
	entity->write_reg(entity->dev, W6_CMD_INSTANCE_INFO, 0);
	entity->write_reg(entity->dev, W6_COMMAND, W6_CMD_SLEEP_VPU);
	entity->write_reg(entity->dev, W6_VPU_HOST_INT_REQ, 1);

	ret = wave6_vpu_ctrl_wait_busy(entity);
	if (ret) {
		dev_err(ctrl->dev, "sleep vpu timeout\n");
		wave6_vpu_ctrl_set_state(ctrl, WAVE6_VPU_STATE_OFF);
		return -EINVAL;
	}

	ret = wave6_vpu_ctrl_check_result(entity);
	if (ret) {
		dev_err(ctrl->dev, "sleep vpu fail, reason 0x%x\n", ret);
		wave6_vpu_ctrl_set_state(ctrl, WAVE6_VPU_STATE_OFF);
		return -EIO;
	}

	wave6_vpu_ctrl_set_state(ctrl, WAVE6_VPU_STATE_SLEEP);

	return 0;
}

static int wave6_vpu_ctrl_wakeup(struct vpu_ctrl *ctrl, struct wave6_vpu_entity *entity)
{
	int ret;

	wave6_vpu_ctrl_remap_code_buffer(ctrl);

	entity->write_reg(entity->dev, W6_VPU_BUSY_STATUS, 1);
	entity->write_reg(entity->dev, W6_CMD_INIT_VPU_SEC_AXI_BASE_CORE0,
				       ctrl->sram_buf.dma_addr);
	entity->write_reg(entity->dev, W6_CMD_INIT_VPU_SEC_AXI_SIZE_CORE0,
				       ctrl->sram_buf.size);
	wave6_vpu_ctrl_writel(ctrl->dev, W6_COMMAND_GB, W6_CMD_WAKEUP_VPU);
	wave6_vpu_ctrl_writel(ctrl->dev, W6_VPU_REMAP_CORE_START_GB, 1);

	ret = wave6_vpu_ctrl_wait_busy(entity);
	if (ret) {
		dev_err(ctrl->dev, "wakeup vpu timeout\n");
		return -EINVAL;
	}

	ret = wave6_vpu_ctrl_check_result(entity);
	if (ret) {
		dev_err(ctrl->dev, "wakeup vpu fail, reason 0x%x\n", ret);
		return -EIO;
	}

	wave6_vpu_ctrl_boot_done(ctrl, 1);

	return 0;
}

static int wave6_vpu_ctrl_try_boot(struct vpu_ctrl *ctrl, struct wave6_vpu_entity *entity)
{
	int ret;

	if (ctrl->state != WAVE6_VPU_STATE_OFF && ctrl->state != WAVE6_VPU_STATE_SLEEP)
		return 0;

	if (entity->read_reg(entity->dev, W6_VPU_VCPU_CUR_PC)) {
		dev_dbg(ctrl->dev, "try boot directly as firmware is running\n");
		wave6_vpu_ctrl_boot_done(ctrl, ctrl->state == WAVE6_VPU_STATE_SLEEP);
		return 0;
	}

	if (ctrl->state == WAVE6_VPU_STATE_SLEEP) {
		ret = wave6_vpu_ctrl_wakeup(ctrl, entity);
		return ret;
	}

	wave6_vpu_ctrl_set_state(ctrl, WAVE6_VPU_STATE_PREPARE);
	return wave6_vpu_ctrl_boot(ctrl, entity);
}

static int wave6_vpu_ctrl_resume_and_get(struct device *dev,
					 struct wave6_vpu_entity *entity)
{
	struct vpu_ctrl *ctrl = dev_get_drvdata(dev);
	bool boot;
	int ret;

	if (!ctrl || !ctrl->available)
		return -EINVAL;

	if (!entity || !entity->dev || !entity->read_reg || !entity->write_reg)
		return -EINVAL;

	if (wave6_vpu_ctrl_find_entity(ctrl, entity))
		return 0;

	ret = pm_runtime_resume_and_get(ctrl->dev);
	if (ret) {
		dev_err(dev, "pm runtime resume fail, ret = %d\n", ret);
		return ret;
	}

	boot = list_empty(&ctrl->entities) ? true : false;
	list_add_tail(&entity->list, &ctrl->entities);
	if (boot)
		ret = wave6_vpu_ctrl_try_boot(ctrl, entity);
	else if (ctrl->state == WAVE6_VPU_STATE_ON)
		wave6_vpu_ctrl_on_boot(entity);

	if (ret)
		pm_runtime_put_sync(ctrl->dev);

	return ret;
}

static void wave6_vpu_ctrl_put_sync(struct device *dev,
				    struct wave6_vpu_entity *entity)
{
	struct vpu_ctrl *ctrl = dev_get_drvdata(dev);

	if (!ctrl || !ctrl->available)
		return;

	if (!wave6_vpu_ctrl_find_entity(ctrl, entity))
		return;

	list_del_init(&entity->list);
	if (list_empty(&ctrl->entities)) {
		if (!entity->read_reg(entity->dev, W6_VPU_VCPU_CUR_PC))
			wave6_vpu_ctrl_set_state(ctrl, WAVE6_VPU_STATE_OFF);
		else
			wave6_vpu_ctrl_sleep(ctrl, entity);
	}

	if (!pm_runtime_suspended(ctrl->dev))
		pm_runtime_put_sync(ctrl->dev);
}

static const struct wave6_vpu_ctrl_ops wave6_vpu_ctrl_ops = {
	.get_ctrl = wave6_vpu_ctrl_resume_and_get,
	.put_ctrl = wave6_vpu_ctrl_put_sync,
	.require_work_buffer = wave6_vpu_ctrl_require_buffer,
};

static void wave6_vpu_ctrl_init_reserved_boot_region(struct vpu_ctrl *ctrl)
{
	if (ctrl->boot_mem.size < WAVE6_CODE_BUF_SIZE) {
		dev_warn(ctrl->dev, "boot memory size (%zu) is too small\n", ctrl->boot_mem.size);
		ctrl->boot_mem.phys_addr = 0;
		ctrl->boot_mem.size = 0;
		memset(&ctrl->boot_mem, 0, sizeof(ctrl->boot_mem));
		return;
	}

	ctrl->boot_mem.vaddr = devm_memremap(ctrl->dev,
					     ctrl->boot_mem.phys_addr,
					     ctrl->boot_mem.size,
					     MEMREMAP_WC);
	if (!ctrl->boot_mem.vaddr) {
		memset(&ctrl->boot_mem, 0, sizeof(ctrl->boot_mem));
		return;
	}

	ctrl->boot_mem.dma_addr = dma_map_resource(ctrl->dev,
						   ctrl->boot_mem.phys_addr,
						   ctrl->boot_mem.size,
						   DMA_BIDIRECTIONAL,
						   0);
	if (!ctrl->boot_mem.dma_addr) {
		memset(&ctrl->boot_mem, 0, sizeof(ctrl->boot_mem));
		return;
	}
}

static int wave6_vpu_ctrl_thermal_update(struct device *dev, int state)
{
	struct vpu_ctrl *ctrl = dev_get_drvdata(dev);
	unsigned long new_clock_rate;
	int ret;

	if (wave6_cooling_disable || state > ctrl->thermal_max || !ctrl->cooling)
		return 0;

	new_clock_rate = DIV_ROUND_UP(ctrl->freq_table[state], HZ_PER_KHZ);
	dev_dbg(dev, "receive cooling set state: %d, new clock rate %ld\n",
		state, new_clock_rate);

	ret = dev_pm_genpd_set_performance_state(ctrl->dev, new_clock_rate);
	if (ret && !((ret == -ENODEV) || (ret == -EOPNOTSUPP))) {
		dev_err(dev, "failed to set perf to %lu (ret = %d)\n", new_clock_rate, ret);
		return ret;
	}

	return 0;
}

static int wave6_cooling_get_max_state(struct thermal_cooling_device *cdev,
				       unsigned long *state)
{
	struct vpu_ctrl *ctrl = cdev->devdata;

	*state = ctrl->thermal_max;

	return 0;
}

static int wave6_cooling_get_cur_state(struct thermal_cooling_device *cdev,
				       unsigned long *state)
{
	struct vpu_ctrl *ctrl = cdev->devdata;

	*state = ctrl->thermal_event;

	return 0;
}

static int wave6_cooling_set_cur_state(struct thermal_cooling_device *cdev,
				       unsigned long state)
{
	struct vpu_ctrl *ctrl = cdev->devdata;
	struct wave6_vpu_entity *entity;

	ctrl->thermal_event = state;

	list_for_each_entry(entity, &ctrl->entities, list) {
		if (entity->pause)
			entity->pause(entity->dev, 0);
	}

	wave6_vpu_ctrl_thermal_update(ctrl->dev, state);

	list_for_each_entry(entity, &ctrl->entities, list) {
		if (entity->pause)
			entity->pause(entity->dev, 1);
	}

	return 0;
}

static struct thermal_cooling_device_ops wave6_cooling_ops = {
	.get_max_state = wave6_cooling_get_max_state,
	.get_cur_state = wave6_cooling_get_cur_state,
	.set_cur_state = wave6_cooling_set_cur_state,
};

static void wave6_cooling_remove(struct vpu_ctrl *ctrl)
{
	thermal_cooling_device_unregister(ctrl->cooling);

	kfree(ctrl->freq_table);
	ctrl->freq_table = NULL;
}

static void wave6_cooling_init(struct vpu_ctrl *ctrl)
{
	int i;
	int num_opps;
	unsigned long freq;

	num_opps = dev_pm_opp_get_opp_count(ctrl->dev);
	if (num_opps <= 0) {
		dev_err(ctrl->dev, "fail to get pm opp count, ret = %d\n", num_opps);
		goto error;
	}

	ctrl->freq_table = kcalloc(num_opps, sizeof(*ctrl->freq_table), GFP_KERNEL);
	if (!ctrl->freq_table)
		goto error;

	for (i = 0, freq = ULONG_MAX; i < num_opps; i++, freq--) {
		struct dev_pm_opp *opp;

		opp = dev_pm_opp_find_freq_floor(ctrl->dev, &freq);
		if (IS_ERR(opp))
			break;

		dev_pm_opp_put(opp);

		dev_dbg(ctrl->dev, "[%d] = %ld\n", i, freq);
		if (freq < 100 * HZ_PER_MHZ)
			break;

		ctrl->freq_table[i] = freq;
		ctrl->thermal_max = i;
	}

	if (!ctrl->thermal_max)
		goto error;

	ctrl->thermal_event = 0;
	ctrl->cooling = thermal_of_cooling_device_register(ctrl->dev->of_node,
							   (char *)dev_name(ctrl->dev),
							   ctrl,
							   &wave6_cooling_ops);
	if (IS_ERR(ctrl->cooling)) {
		dev_err(ctrl->dev, "register cooling device failed\n");
		goto error;
	}

	return;
error:
	wave6_cooling_remove(ctrl);
}

static int wave6_vpu_ctrl_register_device(struct vpu_ctrl *ctrl)
{
	struct wave6_vpu_device *vpu = dev_get_drvdata(ctrl->dev->parent);

	if (!vpu || !vpu->ops)
		return -EPROBE_DEFER;

	return call_vop(vpu, reg_ctrl, ctrl->dev, &wave6_vpu_ctrl_ops);
}

static void wave6_vpu_ctrl_unregister_device(struct vpu_ctrl *ctrl)
{
	struct wave6_vpu_device *vpu = dev_get_drvdata(ctrl->dev->parent);

	if (!vpu || !vpu->ops)
		return;

	call_void_vop(vpu, unreg_ctrl, ctrl->dev);
}

static void wave6_vpu_ctrl_load_firmware(const struct firmware *fw, void *context)
{
	struct vpu_ctrl *ctrl = context;
	int ret;

	mutex_lock(&ctrl->lock);

	if (!fw || !fw->data) {
		dev_err(ctrl->dev, "No firmware.\n");
		return;
	}

	if (!ctrl->available)
		goto exit;

	if (fw->size + WAVE6_EXTRA_CODE_BUF_SIZE > wave6_vpu_ctrl_get_code_buf_size(ctrl)) {
		dev_err(ctrl->dev, "firmware size (%ld > %zd) is too big\n",
			fw->size, ctrl->boot_mem.size);
		ctrl->available = false;
		goto exit;
	}

	memcpy(ctrl->boot_mem.vaddr, fw->data, fw->size);

	ret = wave6_vpu_ctrl_register_device(ctrl);
	if (ret) {
		dev_err(ctrl->dev, "register vpu ctrl fail, ret = %d\n", ret);
		ctrl->available = false;
		goto exit;
	}

exit:
	mutex_unlock(&ctrl->lock);
	release_firmware(fw);
}

static int wave6_vpu_ctrl_probe(struct platform_device *pdev)
{
	struct vpu_ctrl *ctrl;
	struct device_node *np;
	const struct vpu_ctrl_resource *res;
	int ret;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret < 0) {
		dev_err(&pdev->dev, "dma_set_mask_and_coherent failed: %d\n", ret);
		return ret;
	}

	res = of_device_get_match_data(&pdev->dev);
	if (!res)
		return -ENODEV;

	ctrl = devm_kzalloc(&pdev->dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	mutex_init(&ctrl->lock);
	INIT_LIST_HEAD(&ctrl->entities);
	INIT_LIST_HEAD(&ctrl->buffers);
	dev_set_drvdata(&pdev->dev, ctrl);
	ctrl->dev = &pdev->dev;
	ctrl->res = res;
	ctrl->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ctrl->reg_base))
		return PTR_ERR(ctrl->reg_base);

	ret = devm_clk_bulk_get_all(&pdev->dev, &ctrl->clks);
	if (ret < 0) {
		dev_warn(&pdev->dev, "unable to get clocks: %d\n", ret);
		ret = 0;
	}
	ctrl->num_clks = ret;

	np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (np) {
		struct resource mem;

		ret = of_address_to_resource(np, 0, &mem);
		of_node_put(np);
		if (!ret) {
			ctrl->boot_mem.phys_addr = mem.start;
			ctrl->boot_mem.size = resource_size(&mem);
			wave6_vpu_ctrl_init_reserved_boot_region(ctrl);
		} else {
			dev_warn(&pdev->dev, "boot resource is not available.\n");
		}
	}

	ctrl->sram_pool = of_gen_pool_get(pdev->dev.of_node, "sram", 0);
	if (ctrl->sram_pool) {
		ctrl->sram_buf.size = ctrl->res->sram_size;
		ctrl->sram_buf.vaddr = gen_pool_dma_alloc(ctrl->sram_pool,
							  ctrl->sram_buf.size,
							  &ctrl->sram_buf.phys_addr);
		if (!ctrl->sram_buf.vaddr)
			ctrl->sram_buf.size = 0;
		else
			ctrl->sram_buf.dma_addr = dma_map_resource(&pdev->dev,
								   ctrl->sram_buf.phys_addr,
								   ctrl->sram_buf.size,
								   DMA_BIDIRECTIONAL,
								   0);
	}

	wave6_cooling_init(ctrl);

	pm_runtime_enable(&pdev->dev);
	ctrl->available = true;

	ret = firmware_request_nowait_nowarn(THIS_MODULE,
					     ctrl->res->fw_name,
					     &pdev->dev,
					     GFP_KERNEL,
					     ctrl,
					     wave6_vpu_ctrl_load_firmware);
	if (ret) {
		dev_err(&pdev->dev, "request firmware fail, ret = %d\n", ret);
		goto error;
	}

	return 0;

error:
	pm_runtime_disable(&pdev->dev);
	wave6_cooling_remove(ctrl);
	if (ctrl->sram_pool && ctrl->sram_buf.vaddr) {
		dma_unmap_resource(&pdev->dev,
				   ctrl->sram_buf.dma_addr,
				   ctrl->sram_buf.size,
				   DMA_BIDIRECTIONAL,
				   0);
		gen_pool_free(ctrl->sram_pool,
			      (unsigned long)ctrl->sram_buf.vaddr,
			      ctrl->sram_buf.size);
	}
	if (ctrl->boot_mem.dma_addr)
		dma_unmap_resource(&pdev->dev,
				   ctrl->boot_mem.dma_addr,
				   ctrl->boot_mem.size,
				   DMA_BIDIRECTIONAL,
				   0);
	mutex_destroy(&ctrl->lock);

	return ret;
}

static void wave6_vpu_ctrl_remove(struct platform_device *pdev)
{
	struct vpu_ctrl *ctrl = dev_get_drvdata(&pdev->dev);

	mutex_lock(&ctrl->lock);

	ctrl->available = false;
	wave6_vpu_ctrl_clear_buffers(ctrl);
	wave6_vpu_ctrl_unregister_device(ctrl);

	mutex_unlock(&ctrl->lock);

	pm_runtime_disable(&pdev->dev);
	wave6_cooling_remove(ctrl);
	if (ctrl->sram_pool && ctrl->sram_buf.vaddr) {
		dma_unmap_resource(&pdev->dev,
				   ctrl->sram_buf.dma_addr,
				   ctrl->sram_buf.size,
				   DMA_BIDIRECTIONAL,
				   0);
		gen_pool_free(ctrl->sram_pool,
			      (unsigned long)ctrl->sram_buf.vaddr,
			      ctrl->sram_buf.size);
	}
	if (ctrl->boot_mem.dma_addr)
		dma_unmap_resource(&pdev->dev,
				   ctrl->boot_mem.dma_addr,
				   ctrl->boot_mem.size,
				   DMA_BIDIRECTIONAL,
				   0);
	mutex_destroy(&ctrl->lock);
}

static int __maybe_unused wave6_vpu_ctrl_runtime_suspend(struct device *dev)
{
	struct vpu_ctrl *ctrl = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(ctrl->num_clks, ctrl->clks);
	return 0;
}

static int __maybe_unused wave6_vpu_ctrl_runtime_resume(struct device *dev)
{
	struct vpu_ctrl *ctrl = dev_get_drvdata(dev);

	return clk_bulk_prepare_enable(ctrl->num_clks, ctrl->clks);
}

static const struct dev_pm_ops wave6_vpu_ctrl_pm_ops = {
	SET_RUNTIME_PM_OPS(wave6_vpu_ctrl_runtime_suspend,
			   wave6_vpu_ctrl_runtime_resume, NULL)
};

static const struct of_device_id wave6_vpu_ctrl_ids[] = {
	{ .compatible = "nxp,imx95-vpu-ctrl", .data = &wave633c_ctrl_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, wave6_vpu_ctrl_ids);

static struct platform_driver wave6_vpu_ctrl_driver = {
	.driver = {
		.name = VPU_CTRL_PLATFORM_DEVICE_NAME,
		.of_match_table = wave6_vpu_ctrl_ids,
		.pm = &wave6_vpu_ctrl_pm_ops,
	},
	.probe = wave6_vpu_ctrl_probe,
	.remove = wave6_vpu_ctrl_remove,
};

module_platform_driver(wave6_vpu_ctrl_driver);
MODULE_DESCRIPTION("chips&media Wave6 VPU CTRL driver");
MODULE_LICENSE("Dual BSD/GPL");
