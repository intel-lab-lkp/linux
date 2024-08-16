// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ACPI RAS2 memory driver
 *
 * Copyright (c) 2024 HiSilicon Limited.
 *
 */

#define pr_fmt(fmt)	"MEMORY ACPI RAS2: " fmt

#include <linux/bitfield.h>
#include <linux/edac.h>
#include <linux/platform_device.h>
#include <acpi/ras2_acpi.h>

#define RAS2_DEV_NUM_RAS_FEATURES	1

#define RAS2_SUPPORT_HW_PARTOL_SCRUB	BIT(0)
#define RAS2_TYPE_PATROL_SCRUB	0x0000

#define RAS2_GET_PATROL_PARAMETERS	0x01
#define	RAS2_START_PATROL_SCRUBBER	0x02
#define	RAS2_STOP_PATROL_SCRUBBER	0x03

#define RAS2_PATROL_SCRUB_SCHRS_IN_MASK	GENMASK(15, 8)
#define RAS2_PATROL_SCRUB_EN_BACKGROUND	BIT(0)
#define RAS2_PATROL_SCRUB_SCHRS_OUT_MASK	GENMASK(7, 0)
#define RAS2_PATROL_SCRUB_MIN_SCHRS_OUT_MASK	GENMASK(15, 8)
#define RAS2_PATROL_SCRUB_MAX_SCHRS_OUT_MASK	GENMASK(23, 16)
#define RAS2_PATROL_SCRUB_FLAG_SCRUBBER_RUNNING	BIT(0)

#define RAS2_SCRUB_NAME_LEN      128
#define RAS2_HOUR_IN_SECS    3600

struct acpi_ras2_ps_shared_mem {
	struct acpi_ras2_shared_memory common;
	struct acpi_ras2_patrol_scrub_parameter params;
};

static int ras2_is_patrol_scrub_support(struct ras2_scrub_ctx *ras2_ctx)
{
	struct acpi_ras2_shared_memory __iomem *common = (void *)
				ras2_ctx->pcc_subspace->pcc_comm_addr;

	guard(mutex)(&ras2_ctx->lock);
	common->set_capabilities[0] = 0;

	return common->features[0] & RAS2_SUPPORT_HW_PARTOL_SCRUB;
}

static int ras2_update_patrol_scrub_params_cache(struct ras2_scrub_ctx *ras2_ctx)
{
	struct acpi_ras2_ps_shared_mem __iomem *ps_sm = (void *)
					ras2_ctx->pcc_subspace->pcc_comm_addr;
	int ret;

	ps_sm->common.set_capabilities[0] = RAS2_SUPPORT_HW_PARTOL_SCRUB;
	ps_sm->params.patrol_scrub_command = RAS2_GET_PATROL_PARAMETERS;

	ret = ras2_send_pcc_cmd(ras2_ctx, RAS2_PCC_CMD_EXEC);
	if (ret) {
		dev_err(ras2_ctx->dev, "failed to read parameters\n");
		return ret;
	}

	ras2_ctx->min_scrub_cycle = FIELD_GET(RAS2_PATROL_SCRUB_MIN_SCHRS_OUT_MASK,
					      ps_sm->params.scrub_params_out);
	ras2_ctx->max_scrub_cycle = FIELD_GET(RAS2_PATROL_SCRUB_MAX_SCHRS_OUT_MASK,
					      ps_sm->params.scrub_params_out);
	if (!ras2_ctx->bg) {
		ras2_ctx->base = ps_sm->params.actual_address_range[0];
		ras2_ctx->size = ps_sm->params.actual_address_range[1];
	}
	ras2_ctx->scrub_cycle_hrs = FIELD_GET(RAS2_PATROL_SCRUB_SCHRS_OUT_MASK,
					      ps_sm->params.scrub_params_out);

	return 0;
}

/* Context - lock must be held */
static int ras2_get_patrol_scrub_running(struct ras2_scrub_ctx *ras2_ctx,
					 bool *running)
{
	struct acpi_ras2_ps_shared_mem __iomem *ps_sm = (void *)
					ras2_ctx->pcc_subspace->pcc_comm_addr;
	int ret;

	ps_sm->common.set_capabilities[0] = RAS2_SUPPORT_HW_PARTOL_SCRUB;
	ps_sm->params.patrol_scrub_command = RAS2_GET_PATROL_PARAMETERS;

	ret = ras2_send_pcc_cmd(ras2_ctx, RAS2_PCC_CMD_EXEC);
	if (ret) {
		dev_err(ras2_ctx->dev, "failed to read parameters\n");
		return ret;
	}

	*running = ps_sm->params.flags & RAS2_PATROL_SCRUB_FLAG_SCRUBBER_RUNNING;

	return 0;
}

static int ras2_hw_scrub_read_min_scrub_cycle(struct device *dev, void *drv_data,
					      u32 *min)
{
	struct ras2_scrub_ctx *ras2_ctx = drv_data;

	*min = ras2_ctx->min_scrub_cycle * RAS2_HOUR_IN_SECS;

	return 0;
}

static int ras2_hw_scrub_read_max_scrub_cycle(struct device *dev, void *drv_data,
					      u32 *max)
{
	struct ras2_scrub_ctx *ras2_ctx = drv_data;

	*max = ras2_ctx->max_scrub_cycle * RAS2_HOUR_IN_SECS;

	return 0;
}

static int ras2_hw_scrub_cycle_read(struct device *dev, void *drv_data,
				    u32 *scrub_cycle_secs)
{
	struct ras2_scrub_ctx *ras2_ctx = drv_data;

	*scrub_cycle_secs = ras2_ctx->scrub_cycle_hrs * RAS2_HOUR_IN_SECS;

	return 0;
}

static int ras2_hw_scrub_cycle_write(struct device *dev, void *drv_data,
				     u32 scrub_cycle_secs)
{
	u8 scrub_cycle_hrs = scrub_cycle_secs / RAS2_HOUR_IN_SECS;
	struct ras2_scrub_ctx *ras2_ctx = drv_data;
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

static int ras2_hw_scrub_read_range(struct device *dev, void *drv_data, u64 *base, u64 *size)
{
	struct ras2_scrub_ctx *ras2_ctx = drv_data;

	/*
	 * When BG scrubbing is enabled the actual address range is not valid.
	 * Return -EBUSY now unless findout a method to retrieve actual full PA range.
	 */
	if (ras2_ctx->bg)
		return -EBUSY;

	*base = ras2_ctx->base;
	*size = ras2_ctx->size;

	return 0;
}

static int ras2_hw_scrub_write_range(struct device *dev, void *drv_data, u64 base, u64 size)
{
	struct ras2_scrub_ctx *ras2_ctx = drv_data;
	bool running;
	int ret;

	guard(mutex)(&ras2_ctx->lock);
	ret = ras2_get_patrol_scrub_running(ras2_ctx, &running);
	if (ret)
		return ret;

	if (running)
		return -EBUSY;

	if (!base || !size) {
		dev_warn(dev, "%s: Invalid address range, base=0x%llx size=0x%llx\n",
			 __func__, base, size);
		return -EINVAL;
	}

	ras2_ctx->base = base;
	ras2_ctx->size = size;

	return 0;
}

static int ras2_hw_scrub_set_enabled_bg(struct device *dev, void *drv_data, bool enable)
{
	struct ras2_scrub_ctx *ras2_ctx = drv_data;
	struct acpi_ras2_ps_shared_mem __iomem *ps_sm = (void *)
					ras2_ctx->pcc_subspace->pcc_comm_addr;
	bool running;
	int ret;

	guard(mutex)(&ras2_ctx->lock);
	ps_sm->common.set_capabilities[0] = RAS2_SUPPORT_HW_PARTOL_SCRUB;
	ret = ras2_get_patrol_scrub_running(ras2_ctx, &running);
	if (ret)
		return ret;
	if (enable) {
		if (ras2_ctx->bg || running)
			return -EBUSY;
		ps_sm->params.requested_address_range[0] = 0;
		ps_sm->params.requested_address_range[1] = 0;
		ps_sm->params.scrub_params_in &= ~RAS2_PATROL_SCRUB_SCHRS_IN_MASK;
		ps_sm->params.scrub_params_in |= FIELD_PREP(RAS2_PATROL_SCRUB_SCHRS_IN_MASK,
							    ras2_ctx->scrub_cycle_hrs);
		ps_sm->params.patrol_scrub_command = RAS2_START_PATROL_SCRUBBER;
	} else {
		if (!ras2_ctx->bg)
			return -EPERM;
		if (!ras2_ctx->bg && running)
			return -EBUSY;
		ps_sm->params.patrol_scrub_command = RAS2_STOP_PATROL_SCRUBBER;
	}
	ps_sm->params.scrub_params_in &= ~RAS2_PATROL_SCRUB_EN_BACKGROUND;
	ps_sm->params.scrub_params_in |= FIELD_PREP(RAS2_PATROL_SCRUB_EN_BACKGROUND,
						    enable);
	ret = ras2_send_pcc_cmd(ras2_ctx, RAS2_PCC_CMD_EXEC);
	if (ret) {
		dev_err(ras2_ctx->dev, "Failed to %s background scrubbing\n",
			enable ? "enable" : "disable");
		return ret;
	}
	if (enable) {
		ras2_ctx->bg = true;
		/* Update the cache to account for rounding of supplied parameters and similar */
		ret = ras2_update_patrol_scrub_params_cache(ras2_ctx);
	} else {
		ret = ras2_update_patrol_scrub_params_cache(ras2_ctx);
		ras2_ctx->bg = false;
	}

	return ret;
}

static int ras2_hw_scrub_get_enabled_bg(struct device *dev, void *drv_data, bool *enabled)
{
	struct ras2_scrub_ctx *ras2_ctx = drv_data;

	*enabled = ras2_ctx->bg;

	return 0;
}

static int ras2_hw_scrub_set_enabled_od(struct device *dev, void *drv_data, bool enable)
{
	struct ras2_scrub_ctx *ras2_ctx = drv_data;
	struct acpi_ras2_ps_shared_mem __iomem *ps_sm = (void *)
					ras2_ctx->pcc_subspace->pcc_comm_addr;
	bool running;
	int ret;

	guard(mutex)(&ras2_ctx->lock);
	ps_sm->common.set_capabilities[0] = RAS2_SUPPORT_HW_PARTOL_SCRUB;
	if (ras2_ctx->bg)
		return -EBUSY;
	ret = ras2_get_patrol_scrub_running(ras2_ctx, &running);
	if (ret)
		return ret;
	if (enable) {
		if (!ras2_ctx->base || !ras2_ctx->size) {
			dev_warn(ras2_ctx->dev,
				 "%s: Invalid address range, base=0x%llx "
				 "size=0x%llx\n", __func__,
				 ras2_ctx->base, ras2_ctx->size);
			return -ERANGE;
		}
		if (running)
			return -EBUSY;
		ps_sm->params.scrub_params_in &= ~RAS2_PATROL_SCRUB_SCHRS_IN_MASK;
		ps_sm->params.scrub_params_in |= FIELD_PREP(RAS2_PATROL_SCRUB_SCHRS_IN_MASK,
							    ras2_ctx->scrub_cycle_hrs);
		ps_sm->params.requested_address_range[0] = ras2_ctx->base;
		ps_sm->params.requested_address_range[1] = ras2_ctx->size;
		ps_sm->params.scrub_params_in &= ~RAS2_PATROL_SCRUB_EN_BACKGROUND;
		ps_sm->params.patrol_scrub_command = RAS2_START_PATROL_SCRUBBER;
	} else {
		if (!running)
			return 0;
		ps_sm->params.patrol_scrub_command = RAS2_STOP_PATROL_SCRUBBER;
	}

	ret = ras2_send_pcc_cmd(ras2_ctx, RAS2_PCC_CMD_EXEC);
	if (ret) {
		dev_err(ras2_ctx->dev, "Failed to %s demand scrubbing\n",
			enable ? "enable" : "disable");
		return ret;
	}

	return ras2_update_patrol_scrub_params_cache(ras2_ctx);
}

static int ras2_hw_scrub_get_enabled_od(struct device *dev, void *drv_data, bool *enabled)
{
	struct ras2_scrub_ctx *ras2_ctx = drv_data;

	guard(mutex)(&ras2_ctx->lock);
	if (ras2_ctx->bg) {
		*enabled = false;
		return 0;
	}

	return ras2_get_patrol_scrub_running(ras2_ctx, enabled);
}

static const struct edac_scrub_ops ras2_scrub_ops = {
	.read_range = ras2_hw_scrub_read_range,
	.write_range = ras2_hw_scrub_write_range,
	.get_enabled_bg = ras2_hw_scrub_get_enabled_bg,
	.set_enabled_bg = ras2_hw_scrub_set_enabled_bg,
	.get_enabled_od = ras2_hw_scrub_get_enabled_od,
	.set_enabled_od = ras2_hw_scrub_set_enabled_od,
	.min_cycle_read = ras2_hw_scrub_read_min_scrub_cycle,
	.max_cycle_read = ras2_hw_scrub_read_max_scrub_cycle,
	.cycle_duration_read = ras2_hw_scrub_cycle_read,
	.cycle_duration_write = ras2_hw_scrub_cycle_write,
};

static DEFINE_IDA(ras2_ida);

static void ida_release(void *ctx)
{
	struct ras2_scrub_ctx *ras2_ctx = ctx;

	ida_free(&ras2_ida, ras2_ctx->id);
}

static int ras2_probe(struct platform_device *pdev)
{
	struct edac_dev_feature ras_features[RAS2_DEV_NUM_RAS_FEATURES];
	char scrub_name[RAS2_SCRUB_NAME_LEN];
	struct ras2_scrub_ctx *ras2_ctx;
	int num_ras_features = 0;
	int ret, id;

	/* RAS2 PCC Channel and Scrub specific context */
	ras2_ctx = devm_kzalloc(&pdev->dev, sizeof(*ras2_ctx), GFP_KERNEL);
	if (!ras2_ctx)
		return -ENOMEM;

	ras2_ctx->dev = &pdev->dev;
	mutex_init(&ras2_ctx->lock);

	ret = devm_ras2_register_pcc_channel(&pdev->dev, ras2_ctx,
					     *((int *)dev_get_platdata(&pdev->dev)));
	if (ret < 0) {
		dev_dbg(ras2_ctx->dev,
			"failed to register pcc channel ret=%d\n", ret);
		return ret;
	}
	if (!ras2_is_patrol_scrub_support(ras2_ctx))
		return -EOPNOTSUPP;

	ret = ras2_update_patrol_scrub_params_cache(ras2_ctx);
	if (ret)
		return ret;

	id = ida_alloc(&ras2_ida, GFP_KERNEL);
	if (id < 0)
		return id;

	ras2_ctx->id = id;

	ret = devm_add_action_or_reset(&pdev->dev, ida_release, ras2_ctx);
	if (ret < 0)
		return ret;

	snprintf(scrub_name, sizeof(scrub_name), "acpi_ras2_mem%d",
		 ras2_ctx->id);

	ras_features[num_ras_features].feat = RAS_FEAT_SCRUB;
	ras_features[num_ras_features].scrub_ops = &ras2_scrub_ops;
	ras_features[num_ras_features].scrub_ctx = ras2_ctx;
	num_ras_features++;

	return edac_dev_register(&pdev->dev, scrub_name, NULL,
				 num_ras_features, ras_features);
}

static const struct platform_device_id ras2_id_table[] = {
	{ .name = "acpi_ras2", },
	{ }
};
MODULE_DEVICE_TABLE(platform, ras2_id_table);

static struct platform_driver ras2_driver = {
	.probe = ras2_probe,
	.driver = {
		.name = "acpi_ras2",
	},
	.id_table = ras2_id_table,
};
module_driver(ras2_driver, platform_driver_register, platform_driver_unregister);

MODULE_IMPORT_NS(ACPI_RAS2);
MODULE_DESCRIPTION("ACPI RAS2 memory driver");
MODULE_LICENSE("GPL");
