// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CXL memory RAS feature driver.
 *
 * Copyright (c) 2024 HiSilicon Limited.
 *
 *  - Supports functions to configure RAS features of the
 *    CXL memory devices.
 *  - Registers with the EDAC device subsystem driver to expose
 *    the feature's sysfs attributes to the user for configuring
 *    CXL memory RAS feature.
 */

#define pr_fmt(fmt)	"CXL MEM FEAT: " fmt

#include <cxlmem.h>
#include <linux/cleanup.h>
#include <linux/limits.h>
#include <cxl.h>
#include <linux/edac.h>

#define CXL_DEV_NUM_RAS_FEATURES	1
#define CXL_DEV_HOUR_IN_SECS	3600

#define CXL_SCRUB_NAME_LEN	128

/* CXL memory patrol scrub control definitions */
static const uuid_t cxl_patrol_scrub_uuid =
	UUID_INIT(0x96dad7d6, 0xfde8, 0x482b, 0xa7, 0x33, 0x75, 0x77, 0x4e,     \
		  0x06, 0xdb, 0x8a);

/* CXL memory patrol scrub control functions */
struct cxl_patrol_scrub_context {
	u16 get_feat_size;
	u16 set_feat_size;
	u8 get_version;
	u8 set_version;
	u16 set_effects;
	struct cxl_memdev *cxlmd;
	struct cxl_region *cxlr;
};

/**
 * struct cxl_memdev_ps_params - CXL memory patrol scrub parameter data structure.
 * @enable:     [IN & OUT] enable(1)/disable(0) patrol scrub.
 * @scrub_cycle_changeable: [OUT] scrub cycle attribute of patrol scrub is changeable.
 * @scrub_cycle_hrs:    [IN] Requested patrol scrub cycle in hours.
 *                      [OUT] Current patrol scrub cycle in hours.
 * @min_scrub_cycle_hrs:[OUT] minimum patrol scrub cycle in hours supported.
 */
struct cxl_memdev_ps_params {
	bool enable;
	bool scrub_cycle_changeable;
	u16 scrub_cycle_hrs;
	u16 min_scrub_cycle_hrs;
};

enum cxl_scrub_param {
	CXL_PS_PARAM_ENABLE,
	CXL_PS_PARAM_SCRUB_CYCLE,
};

#define	CXL_MEMDEV_PS_SCRUB_CYCLE_CHANGE_CAP_MASK	BIT(0)
#define	CXL_MEMDEV_PS_SCRUB_CYCLE_REALTIME_REPORT_CAP_MASK	BIT(1)
#define	CXL_MEMDEV_PS_CUR_SCRUB_CYCLE_MASK	GENMASK(7, 0)
#define	CXL_MEMDEV_PS_MIN_SCRUB_CYCLE_MASK	GENMASK(15, 8)
#define	CXL_MEMDEV_PS_FLAG_ENABLED_MASK	BIT(0)

struct cxl_memdev_ps_rd_attrs {
	u8 scrub_cycle_cap;
	__le16 scrub_cycle_hrs;
	u8 scrub_flags;
}  __packed;

struct cxl_memdev_ps_wr_attrs {
	u8 scrub_cycle_hrs;
	u8 scrub_flags;
}  __packed;

static int cxl_mem_ps_get_attrs(struct cxl_memdev_state *mds,
				struct cxl_memdev_ps_params *params)
{
	size_t rd_data_size = sizeof(struct cxl_memdev_ps_rd_attrs);
	size_t data_size;
	struct cxl_memdev_ps_rd_attrs *rd_attrs __free(kfree) =
						kmalloc(rd_data_size, GFP_KERNEL);
	if (!rd_attrs)
		return -ENOMEM;

	data_size = cxl_get_feature(mds, cxl_patrol_scrub_uuid,
				    CXL_GET_FEAT_SEL_CURRENT_VALUE,
				    rd_attrs, rd_data_size);
	if (!data_size)
		return -EIO;

	params->scrub_cycle_changeable = FIELD_GET(CXL_MEMDEV_PS_SCRUB_CYCLE_CHANGE_CAP_MASK,
						   rd_attrs->scrub_cycle_cap);
	params->enable = FIELD_GET(CXL_MEMDEV_PS_FLAG_ENABLED_MASK,
				   rd_attrs->scrub_flags);
	params->scrub_cycle_hrs = FIELD_GET(CXL_MEMDEV_PS_CUR_SCRUB_CYCLE_MASK,
					    rd_attrs->scrub_cycle_hrs);
	params->min_scrub_cycle_hrs = FIELD_GET(CXL_MEMDEV_PS_MIN_SCRUB_CYCLE_MASK,
						rd_attrs->scrub_cycle_hrs);

	return 0;
}

static int cxl_ps_get_attrs(struct device *dev, void *drv_data,
			    struct cxl_memdev_ps_params *params)
{
	struct cxl_patrol_scrub_context *cxl_ps_ctx = drv_data;
	struct cxl_memdev *cxlmd;
	struct cxl_dev_state *cxlds;
	struct cxl_memdev_state *mds;
	u16 min_scrub_cycle = 0;
	int i, ret;

	if (cxl_ps_ctx->cxlr) {
		struct cxl_region *cxlr = cxl_ps_ctx->cxlr;
		struct cxl_region_params *p = &cxlr->params;

		for (i = p->interleave_ways - 1; i >= 0; i--) {
			struct cxl_endpoint_decoder *cxled = p->targets[i];

			cxlmd = cxled_to_memdev(cxled);
			cxlds = cxlmd->cxlds;
			mds = to_cxl_memdev_state(cxlds);
			ret = cxl_mem_ps_get_attrs(mds, params);
			if (ret)
				return ret;

			if (params->min_scrub_cycle_hrs > min_scrub_cycle)
				min_scrub_cycle = params->min_scrub_cycle_hrs;
		}
		params->min_scrub_cycle_hrs = min_scrub_cycle;
		return 0;
	}
	cxlmd = cxl_ps_ctx->cxlmd;
	cxlds = cxlmd->cxlds;
	mds = to_cxl_memdev_state(cxlds);

	return cxl_mem_ps_get_attrs(mds, params);
}

static int cxl_mem_ps_set_attrs(struct device *dev, void *drv_data,
				struct cxl_memdev_state *mds,
				struct cxl_memdev_ps_params *params,
				enum cxl_scrub_param param_type)
{
	struct cxl_patrol_scrub_context *cxl_ps_ctx = drv_data;
	struct cxl_memdev_ps_wr_attrs wr_attrs;
	struct cxl_memdev_ps_params rd_params;
	int ret;

	ret = cxl_mem_ps_get_attrs(mds, &rd_params);
	if (ret) {
		dev_err(dev, "Get cxlmemdev patrol scrub params failed ret=%d\n",
			ret);
		return ret;
	}

	switch (param_type) {
	case CXL_PS_PARAM_ENABLE:
		wr_attrs.scrub_flags = FIELD_PREP(CXL_MEMDEV_PS_FLAG_ENABLED_MASK,
						  params->enable);
		wr_attrs.scrub_cycle_hrs = FIELD_PREP(CXL_MEMDEV_PS_CUR_SCRUB_CYCLE_MASK,
						      rd_params.scrub_cycle_hrs);
		break;
	case CXL_PS_PARAM_SCRUB_CYCLE:
		if (params->scrub_cycle_hrs < rd_params.min_scrub_cycle_hrs) {
			dev_err(dev, "Invalid CXL patrol scrub cycle(%d) to set\n",
				params->scrub_cycle_hrs);
			dev_err(dev, "Minimum supported CXL patrol scrub cycle in hour %d\n",
				params->min_scrub_cycle_hrs);
			return -EINVAL;
		}
		wr_attrs.scrub_cycle_hrs = FIELD_PREP(CXL_MEMDEV_PS_CUR_SCRUB_CYCLE_MASK,
						      params->scrub_cycle_hrs);
		wr_attrs.scrub_flags = FIELD_PREP(CXL_MEMDEV_PS_FLAG_ENABLED_MASK,
						  rd_params.enable);
		break;
	}

	ret = cxl_set_feature(mds, cxl_patrol_scrub_uuid, cxl_ps_ctx->set_version,
			      &wr_attrs, sizeof(wr_attrs),
			      CXL_SET_FEAT_FLAG_DATA_SAVED_ACROSS_RESET);
	if (ret) {
		dev_err(dev, "CXL patrol scrub set feature failed ret=%d\n", ret);
		return ret;
	}

	return 0;
}

static int cxl_ps_set_attrs(struct device *dev, void *drv_data,
			    struct cxl_memdev_ps_params *params,
			    enum cxl_scrub_param param_type)
{
	struct cxl_patrol_scrub_context *cxl_ps_ctx = drv_data;
	struct cxl_memdev *cxlmd;
	struct cxl_dev_state *cxlds;
	struct cxl_memdev_state *mds;
	int ret, i;

	if (cxl_ps_ctx->cxlr) {
		struct cxl_region *cxlr = cxl_ps_ctx->cxlr;
		struct cxl_region_params *p = &cxlr->params;

		for (i = p->interleave_ways - 1; i >= 0; i--) {
			struct cxl_endpoint_decoder *cxled = p->targets[i];

			cxlmd = cxled_to_memdev(cxled);
			cxlds = cxlmd->cxlds;
			mds = to_cxl_memdev_state(cxlds);
			ret = cxl_mem_ps_set_attrs(dev, drv_data, mds,
						   params, param_type);
			if (ret)
				return ret;
		}
	} else {
		cxlmd = cxl_ps_ctx->cxlmd;
		cxlds = cxlmd->cxlds;
		mds = to_cxl_memdev_state(cxlds);

		return cxl_mem_ps_set_attrs(dev, drv_data, mds, params, param_type);
	}

	return 0;
}

static int cxl_patrol_scrub_get_enabled_bg(struct device *dev, void *drv_data, bool *enabled)
{
	struct cxl_memdev_ps_params params;
	int ret;

	ret = cxl_ps_get_attrs(dev, drv_data, &params);
	if (ret)
		return ret;

	*enabled = params.enable;

	return 0;
}

static int cxl_patrol_scrub_set_enabled_bg(struct device *dev, void *drv_data, bool enable)
{
	struct cxl_memdev_ps_params params = {
		.enable = enable,
	};

	return cxl_ps_set_attrs(dev, drv_data, &params, CXL_PS_PARAM_ENABLE);
}

static int cxl_patrol_scrub_read_min_scrub_cycle(struct device *dev, void *drv_data,
						 u32 *min)
{
	struct cxl_memdev_ps_params params;
	int ret;

	ret = cxl_ps_get_attrs(dev, drv_data, &params);
	if (ret)
		return ret;
	*min = params.min_scrub_cycle_hrs * CXL_DEV_HOUR_IN_SECS;

	return 0;
}

static int cxl_patrol_scrub_read_max_scrub_cycle(struct device *dev, void *drv_data,
						 u32 *max)
{
	*max = U8_MAX * CXL_DEV_HOUR_IN_SECS; /* Max set by register size */

	return 0;
}

static int cxl_patrol_scrub_read_scrub_cycle(struct device *dev, void *drv_data,
					     u32 *scrub_cycle_secs)
{
	struct cxl_memdev_ps_params params;
	int ret;

	ret = cxl_ps_get_attrs(dev, drv_data, &params);
	if (ret)
		return ret;

	*scrub_cycle_secs = params.scrub_cycle_hrs * CXL_DEV_HOUR_IN_SECS;

	return 0;
}

static int cxl_patrol_scrub_write_scrub_cycle(struct device *dev, void *drv_data,
					      u32 scrub_cycle_secs)
{
	struct cxl_memdev_ps_params params = {
		.scrub_cycle_hrs = scrub_cycle_secs / CXL_DEV_HOUR_IN_SECS,
	};

	return cxl_ps_set_attrs(dev, drv_data, &params, CXL_PS_PARAM_SCRUB_CYCLE);
}

static const struct edac_scrub_ops cxl_ps_scrub_ops = {
	.get_enabled_bg = cxl_patrol_scrub_get_enabled_bg,
	.set_enabled_bg = cxl_patrol_scrub_set_enabled_bg,
	.min_cycle_read = cxl_patrol_scrub_read_min_scrub_cycle,
	.max_cycle_read = cxl_patrol_scrub_read_max_scrub_cycle,
	.cycle_duration_read = cxl_patrol_scrub_read_scrub_cycle,
	.cycle_duration_write = cxl_patrol_scrub_write_scrub_cycle,
};

int cxl_mem_ras_features_init(struct cxl_memdev *cxlmd, struct cxl_region *cxlr)
{
	struct edac_dev_feature ras_features[CXL_DEV_NUM_RAS_FEATURES];
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlds);
	struct cxl_patrol_scrub_context *cxl_ps_ctx;
	struct cxl_mbox_supp_feat_entry feat_entry;
	char cxl_dev_name[CXL_SCRUB_NAME_LEN];
	int rc, i, num_ras_features = 0;

	if (cxlr) {
		struct cxl_region_params *p = &cxlr->params;

		for (i = p->interleave_ways - 1; i >= 0; i--) {
			struct cxl_endpoint_decoder *cxled = p->targets[i];

			cxlmd = cxled_to_memdev(cxled);
			memset(&feat_entry, 0, sizeof(feat_entry));
			rc = cxl_get_supported_feature_entry(mds, &cxl_patrol_scrub_uuid,
							     &feat_entry);
			if (rc < 0)
				return rc;
			if (!(feat_entry.attr_flags & CXL_FEAT_ENTRY_FLAG_CHANGABLE))
				return -EOPNOTSUPP;
		}
	} else {
		rc = cxl_get_supported_feature_entry(mds, &cxl_patrol_scrub_uuid,
						     &feat_entry);
		if (rc < 0)
			return rc;

		if (!(feat_entry.attr_flags & CXL_FEAT_ENTRY_FLAG_CHANGABLE))
			return -EOPNOTSUPP;
	}

	cxl_ps_ctx = devm_kzalloc(&cxlmd->dev, sizeof(*cxl_ps_ctx), GFP_KERNEL);
	if (!cxl_ps_ctx)
		return -ENOMEM;

	*cxl_ps_ctx = (struct cxl_patrol_scrub_context) {
		.get_feat_size = feat_entry.get_size,
		.set_feat_size = feat_entry.set_size,
		.get_version = feat_entry.get_version,
		.set_version = feat_entry.set_version,
		.set_effects = feat_entry.set_effects,
	};
	if (cxlr) {
		snprintf(cxl_dev_name, sizeof(cxl_dev_name),
			 "cxl_region%d", cxlr->id);
		cxl_ps_ctx->cxlr = cxlr;
	} else {
		snprintf(cxl_dev_name, sizeof(cxl_dev_name),
			 "%s_%s", "cxl", dev_name(&cxlmd->dev));
		cxl_ps_ctx->cxlmd = cxlmd;
	}

	ras_features[num_ras_features].feat = RAS_FEAT_SCRUB;
	ras_features[num_ras_features].scrub_ops = &cxl_ps_scrub_ops;
	ras_features[num_ras_features].scrub_ctx = cxl_ps_ctx;
	num_ras_features++;

	return edac_dev_register(&cxlmd->dev, cxl_dev_name, NULL,
				 num_ras_features, ras_features);
}
EXPORT_SYMBOL_NS_GPL(cxl_mem_ras_features_init, CXL);
