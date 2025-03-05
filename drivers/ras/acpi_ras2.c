// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ACPI RAS2 memory driver
 *
 * Copyright (c) 2024-2025 HiSilicon Limited.
 *
 */

#define pr_fmt(fmt)	"ACPI RAS2 MEMORY: " fmt

#include <linux/bitfield.h>
#include <linux/edac.h>
#include <linux/platform_device.h>
#include <acpi/ras2.h>

#define RAS2_DEV_NUM_RAS_FEATURES	1

#define RAS2_SUPPORT_HW_PARTOL_SCRUB	BIT(0)
#define RAS2_TYPE_PATROL_SCRUB	0x0000

#define RAS2_GET_PATROL_PARAMETERS	0x01
#define RAS2_START_PATROL_SCRUBBER	0x02
#define RAS2_STOP_PATROL_SCRUBBER	0x03

/*
 * RAS2 patrol scrub
 */
#define RAS2_PS_SC_HRS_IN_MASK	GENMASK(15, 8)
#define RAS2_PS_EN_BACKGROUND	BIT(0)
#define RAS2_PS_SC_HRS_OUT_MASK	GENMASK(7, 0)
#define RAS2_PS_MIN_SC_HRS_OUT_MASK	GENMASK(15, 8)
#define RAS2_PS_MAX_SC_HRS_OUT_MASK	GENMASK(23, 16)
#define RAS2_PS_FLAG_SCRUB_RUNNING	BIT(0)

#define RAS2_SCRUB_NAME_LEN      128
#define RAS2_HOUR_IN_SECS    3600

struct acpi_ras2_ps_shared_mem {
	struct acpi_ras2_shmem common;
	struct acpi_ras2_patrol_scrub_param params;
};

static int ras2_is_patrol_scrub_support(struct ras2_mem_ctx *ras2_ctx)
{
	struct acpi_ras2_shmem __iomem *common = (void *)ras2_ctx->comm_addr;

	guard(mutex)(&ras2_ctx->lock);
	common->set_caps[0] = 0;

	return common->features[0] & RAS2_SUPPORT_HW_PARTOL_SCRUB;
}

static int ras2_update_patrol_scrub_params_cache(struct ras2_mem_ctx *ras2_ctx)
{
	struct acpi_ras2_ps_shared_mem __iomem *ps_sm =
		(void *)ras2_ctx->comm_addr;
	int ret;

	ps_sm->common.set_caps[0] = RAS2_SUPPORT_HW_PARTOL_SCRUB;
	ps_sm->params.cmd = RAS2_GET_PATROL_PARAMETERS;

	ret = ras2_send_pcc_cmd(ras2_ctx, RAS2_PCC_CMD_EXEC);
	if (ret) {
		dev_err(ras2_ctx->dev, "failed to read parameters\n");
		return ret;
	}

	ras2_ctx->min_scrub_cycle = FIELD_GET(RAS2_PS_MIN_SC_HRS_OUT_MASK,
					      ps_sm->params.scrub_params_out);
	ras2_ctx->max_scrub_cycle = FIELD_GET(RAS2_PS_MAX_SC_HRS_OUT_MASK,
					      ps_sm->params.scrub_params_out);
	if (!ras2_ctx->bg_scrub) {
		ras2_ctx->base = ps_sm->params.actl_addr_range[0];
		ras2_ctx->size = ps_sm->params.actl_addr_range[1];
	}

	ras2_ctx->scrub_cycle_hrs = FIELD_GET(RAS2_PS_SC_HRS_OUT_MASK,
					      ps_sm->params.scrub_params_out);

	return 0;
}

/* Context - lock must be held */
static int ras2_get_patrol_scrub_running(struct ras2_mem_ctx *ras2_ctx,
					 bool *running)
{
	struct acpi_ras2_ps_shared_mem __iomem *ps_sm =
		(void *)ras2_ctx->comm_addr;
	int ret;

	ps_sm->common.set_caps[0] = RAS2_SUPPORT_HW_PARTOL_SCRUB;
	ps_sm->params.cmd = RAS2_GET_PATROL_PARAMETERS;

	ret = ras2_send_pcc_cmd(ras2_ctx, RAS2_PCC_CMD_EXEC);
	if (ret) {
		dev_err(ras2_ctx->dev, "failed to read parameters\n");
		return ret;
	}

	*running = ps_sm->params.flags & RAS2_PS_FLAG_SCRUB_RUNNING;

	return 0;
}

static int ras2_hw_scrub_read_min_scrub_cycle(struct device *dev, void *drv_data,
					      u32 *min)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;

	*min = ras2_ctx->min_scrub_cycle * RAS2_HOUR_IN_SECS;

	return 0;
}

static int ras2_hw_scrub_read_max_scrub_cycle(struct device *dev, void *drv_data,
					      u32 *max)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;

	*max = ras2_ctx->max_scrub_cycle * RAS2_HOUR_IN_SECS;

	return 0;
}

static int ras2_hw_scrub_cycle_read(struct device *dev, void *drv_data,
				    u32 *scrub_cycle_secs)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;

	*scrub_cycle_secs = ras2_ctx->scrub_cycle_hrs * RAS2_HOUR_IN_SECS;

	return 0;
}

static int ras2_hw_scrub_cycle_write(struct device *dev, void *drv_data,
				     u32 scrub_cycle_secs)
{
	u8 scrub_cycle_hrs = scrub_cycle_secs / RAS2_HOUR_IN_SECS;
	struct ras2_mem_ctx *ras2_ctx = drv_data;
	bool running;
	int ret;

	guard(mutex)(&ras2_ctx->lock);
	ret = ras2_get_patrol_scrub_running(ras2_ctx, &running);
	if (ret)
		return ret;

	if (running)
		return -EBUSY;

	if (scrub_cycle_hrs < ras2_ctx->min_scrub_cycle ||
	    scrub_cycle_hrs > ras2_ctx->max_scrub_cycle)
		return -EINVAL;

	ras2_ctx->scrub_cycle_hrs = scrub_cycle_hrs;

	return 0;
}

static int ras2_hw_scrub_read_addr(struct device *dev, void *drv_data, u64 *base)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;
	int ret;

	/*
	 * When BG scrubbing is enabled the actual address range is not valid.
	 * Return -EBUSY now unless find out a method to retrieve actual full PA range.
	 */
	if (ras2_ctx->bg_scrub)
		return -EBUSY;

	/*
	 * When demand scrubbing is finished firmware must reset actual
	 * address range to 0. Otherwise userspace assumes demand scrubbing
	 * is in progress.
	 */
	ret = ras2_update_patrol_scrub_params_cache(ras2_ctx);
	if (ret)
		return ret;

	*base = ras2_ctx->base;

	return 0;
}

static int ras2_hw_scrub_read_size(struct device *dev, void *drv_data, u64 *size)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;
	int ret;

	if (ras2_ctx->bg_scrub)
		return -EBUSY;

	ret = ras2_update_patrol_scrub_params_cache(ras2_ctx);
	if (ret)
		return ret;

	*size = ras2_ctx->size;

	return 0;
}

static int ras2_hw_scrub_write_addr(struct device *dev, void *drv_data, u64 base)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;
	struct acpi_ras2_ps_shared_mem __iomem *ps_sm =
		(void *)ras2_ctx->comm_addr;
	bool running;
	int ret;

	guard(mutex)(&ras2_ctx->lock);
	ps_sm->common.set_caps[0] = RAS2_SUPPORT_HW_PARTOL_SCRUB;
	if (ras2_ctx->bg_scrub)
		return -EBUSY;

	if (!base || !ras2_ctx->size) {
		dev_warn(ras2_ctx->dev,
			 "%s: Invalid address range, base=0x%llx "
			 "size=0x%llx\n", __func__,
			 base, ras2_ctx->size);
		return -ERANGE;
	}

	ret = ras2_get_patrol_scrub_running(ras2_ctx, &running);
	if (ret)
		return ret;

	if (running)
		return -EBUSY;

	ps_sm->params.scrub_params_in &= ~RAS2_PS_SC_HRS_IN_MASK;
	ps_sm->params.scrub_params_in |= FIELD_PREP(RAS2_PS_SC_HRS_IN_MASK,
						    ras2_ctx->scrub_cycle_hrs);
	ps_sm->params.req_addr_range[0] = base;
	ps_sm->params.req_addr_range[1] = ras2_ctx->size;
	ps_sm->params.scrub_params_in &= ~RAS2_PS_EN_BACKGROUND;
	ps_sm->params.cmd = RAS2_START_PATROL_SCRUBBER;

	ret = ras2_send_pcc_cmd(ras2_ctx, RAS2_PCC_CMD_EXEC);
	if (ret) {
		dev_err(ras2_ctx->dev, "Failed to start demand scrubbing\n");
		return ret;
	}

	return ras2_update_patrol_scrub_params_cache(ras2_ctx);
}

static int ras2_hw_scrub_write_size(struct device *dev, void *drv_data, u64 size)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;
	bool running;
	int ret;

	guard(mutex)(&ras2_ctx->lock);
	ret = ras2_get_patrol_scrub_running(ras2_ctx, &running);
	if (ret)
		return ret;

	if (running)
		return -EBUSY;

	if (!size) {
		dev_warn(dev, "%s: Invalid address range size=0x%llx\n",
			 __func__, size);
		return -EINVAL;
	}

	ras2_ctx->size = size;

	return 0;
}

static int ras2_hw_scrub_set_enabled_bg(struct device *dev, void *drv_data, bool enable)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;
	struct acpi_ras2_ps_shared_mem __iomem *ps_sm =
		(void *)ras2_ctx->comm_addr;
	bool running;
	int ret;

	guard(mutex)(&ras2_ctx->lock);
	ps_sm->common.set_caps[0] = RAS2_SUPPORT_HW_PARTOL_SCRUB;
	ret = ras2_get_patrol_scrub_running(ras2_ctx, &running);
	if (ret)
		return ret;

	if (enable) {
		if (ras2_ctx->bg_scrub || running)
			return -EBUSY;
		ps_sm->params.req_addr_range[0] = 0;
		ps_sm->params.req_addr_range[1] = 0;
		ps_sm->params.scrub_params_in &= ~RAS2_PS_SC_HRS_IN_MASK;
		ps_sm->params.scrub_params_in |= FIELD_PREP(RAS2_PS_SC_HRS_IN_MASK,
							    ras2_ctx->scrub_cycle_hrs);
		ps_sm->params.cmd = RAS2_START_PATROL_SCRUBBER;
	} else {
		if (!ras2_ctx->bg_scrub)
			return -EPERM;
		ps_sm->params.cmd = RAS2_STOP_PATROL_SCRUBBER;
	}

	ps_sm->params.scrub_params_in &= ~RAS2_PS_EN_BACKGROUND;
	ps_sm->params.scrub_params_in |= FIELD_PREP(RAS2_PS_EN_BACKGROUND,
						    enable);
	ret = ras2_send_pcc_cmd(ras2_ctx, RAS2_PCC_CMD_EXEC);
	if (ret) {
		dev_err(ras2_ctx->dev, "Failed to %s background scrubbing\n",
			str_enable_disable(enable));
		return ret;
	}

	if (enable) {
		ras2_ctx->bg_scrub = true;
		/* Update the cache to account for rounding of supplied parameters and similar */
		ret = ras2_update_patrol_scrub_params_cache(ras2_ctx);
	} else {
		ret = ras2_update_patrol_scrub_params_cache(ras2_ctx);
		ras2_ctx->bg_scrub = false;
	}

	return ret;
}

static int ras2_hw_scrub_get_enabled_bg(struct device *dev, void *drv_data, bool *enabled)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;

	*enabled = ras2_ctx->bg_scrub;

	return 0;
}

static const struct edac_scrub_ops ras2_scrub_ops = {
	.read_addr = ras2_hw_scrub_read_addr,
	.read_size = ras2_hw_scrub_read_size,
	.write_addr = ras2_hw_scrub_write_addr,
	.write_size = ras2_hw_scrub_write_size,
	.get_enabled_bg = ras2_hw_scrub_get_enabled_bg,
	.set_enabled_bg = ras2_hw_scrub_set_enabled_bg,
	.get_min_cycle = ras2_hw_scrub_read_min_scrub_cycle,
	.get_max_cycle = ras2_hw_scrub_read_max_scrub_cycle,
	.get_cycle_duration = ras2_hw_scrub_cycle_read,
	.set_cycle_duration = ras2_hw_scrub_cycle_write,
};

static int ras2_probe(struct auxiliary_device *auxdev,
		      const struct auxiliary_device_id *id)
{
	struct ras2_mem_ctx *ras2_ctx = container_of(auxdev, struct ras2_mem_ctx, adev);
	struct edac_dev_feature ras_features[RAS2_DEV_NUM_RAS_FEATURES];
	char scrub_name[RAS2_SCRUB_NAME_LEN];
	int num_ras_features = 0;
	int ret;

	if (!ras2_is_patrol_scrub_support(ras2_ctx))
		return -EOPNOTSUPP;

	ret = ras2_update_patrol_scrub_params_cache(ras2_ctx);
	if (ret)
		return ret;

	snprintf(scrub_name, sizeof(scrub_name), "acpi_ras_mem%d",
		 ras2_ctx->id);

	ras_features[num_ras_features].ft_type = RAS_FEAT_SCRUB;
	ras_features[num_ras_features].instance = ras2_ctx->instance;
	ras_features[num_ras_features].scrub_ops = &ras2_scrub_ops;
	ras_features[num_ras_features].ctx = ras2_ctx;
	num_ras_features++;

	return edac_dev_register(&auxdev->dev, scrub_name, NULL,
				 num_ras_features, ras_features);
}

static const struct auxiliary_device_id ras2_mem_dev_id_table[] = {
	{ .name = RAS2_AUX_DEV_NAME "." RAS2_MEM_DEV_ID_NAME, },
	{ }
};

MODULE_DEVICE_TABLE(auxiliary, ras2_mem_dev_id_table);

static struct auxiliary_driver ras2_mem_driver = {
	.name = RAS2_MEM_DEV_ID_NAME,
	.probe = ras2_probe,
	.id_table = ras2_mem_dev_id_table,
};
module_auxiliary_driver(ras2_mem_driver);

MODULE_IMPORT_NS("ACPI_RAS2");
MODULE_DESCRIPTION("ACPI RAS2 memory driver");
MODULE_LICENSE("GPL");
