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
#include "core.h"

#define CXL_DEV_NUM_RAS_FEATURES	3
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

/* CXL DDR5 ECS control definitions */
static const uuid_t cxl_ecs_uuid =
	UUID_INIT(0xe5b13f22, 0x2328, 0x4a14, 0xb8, 0xba, 0xb9, 0x69, 0x1e,     \
		  0x89, 0x33, 0x86);

struct cxl_ecs_context {
	u16 num_media_frus;
	u16 get_feat_size;
	u16 set_feat_size;
	u8 get_version;
	u8 set_version;
	u16 set_effects;
	struct cxl_memdev *cxlmd;
};

enum {
	CXL_ECS_PARAM_LOG_ENTRY_TYPE,
	CXL_ECS_PARAM_THRESHOLD,
	CXL_ECS_PARAM_MODE,
	CXL_ECS_PARAM_RESET_COUNTER,
};

#define	CXL_ECS_LOG_ENTRY_TYPE_MASK	GENMASK(1, 0)
#define	CXL_ECS_REALTIME_REPORT_CAP_MASK	BIT(0)
#define	CXL_ECS_THRESHOLD_COUNT_MASK	GENMASK(2, 0)
#define	CXL_ECS_MODE_MASK	BIT(3)
#define	CXL_ECS_RESET_COUNTER_MASK	BIT(4)

static const u16 ecs_supp_threshold[] = { 0, 0, 0, 256, 1024, 4096 };

enum {
	ECS_LOG_ENTRY_TYPE_DRAM = 0x0,
	ECS_LOG_ENTRY_TYPE_MEM_MEDIA_FRU = 0x1,
};

enum {
	ECS_THRESHOLD_256 = 3,
	ECS_THRESHOLD_1024 = 4,
	ECS_THRESHOLD_4096 = 5,
};

enum cxl_ecs_mode {
	ECS_MODE_COUNTS_ROWS = 0,
	ECS_MODE_COUNTS_CODEWORDS = 1,
};

/**
 * struct cxl_ecs_params - CXL memory DDR5 ECS parameter data structure.
 * @log_entry_type: ECS log entry type, per DRAM or per memory media FRU.
 * @threshold: ECS threshold count per GB of memory cells.
 * @mode:	codeword/row count mode
 *		0 : ECS counts rows with errors
 *		1 : ECS counts codeword with errors
 * @reset_counter: [IN] reset ECC counter to default value.
 */
struct cxl_ecs_params {
	u8 log_entry_type;
	u16 threshold;
	enum cxl_ecs_mode mode;
	bool reset_counter;
};

struct cxl_ecs_rd_attrs {
	u8 ecs_log_cap;
	u8 ecs_cap;
	__le16 ecs_config;
	u8 ecs_flags;
}  __packed;

struct cxl_ecs_wr_attrs {
	u8 ecs_log_cap;
	__le16 ecs_config;
}  __packed;

/* CXL DDR5 ECS control functions */
static int cxl_mem_ecs_get_attrs(struct device *dev, void *drv_data, int fru_id,
				 struct cxl_ecs_params *params)
{
	struct cxl_ecs_context *cxl_ecs_ctx = drv_data;
	struct cxl_memdev *cxlmd = cxl_ecs_ctx->cxlmd;
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlds);
	size_t rd_data_size;
	u8 threshold_index;
	size_t data_size;

	rd_data_size = cxl_ecs_ctx->get_feat_size;

	struct cxl_ecs_rd_attrs *rd_attrs __free(kfree) =
					kmalloc(rd_data_size, GFP_KERNEL);
	if (!rd_attrs)
		return -ENOMEM;

	params->log_entry_type = 0;
	params->threshold = 0;
	params->mode = 0;
	data_size = cxl_get_feature(mds, cxl_ecs_uuid,
				    CXL_GET_FEAT_SEL_CURRENT_VALUE,
				    rd_attrs, rd_data_size);
	if (!data_size)
		return -EIO;

	params->log_entry_type = FIELD_GET(CXL_ECS_LOG_ENTRY_TYPE_MASK,
					   rd_attrs[fru_id].ecs_log_cap);
	threshold_index = FIELD_GET(CXL_ECS_THRESHOLD_COUNT_MASK,
				    rd_attrs[fru_id].ecs_config);
	params->threshold = ecs_supp_threshold[threshold_index];
	params->mode = FIELD_GET(CXL_ECS_MODE_MASK,
				 rd_attrs[fru_id].ecs_config);
	return 0;
}

static int cxl_mem_ecs_set_attrs(struct device *dev, void *drv_data, int fru_id,
				 struct cxl_ecs_params *params, u8 param_type)
{
	struct cxl_ecs_context *cxl_ecs_ctx = drv_data;
	struct cxl_memdev *cxlmd = cxl_ecs_ctx->cxlmd;
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlds);
	size_t rd_data_size, wr_data_size;
	u16 num_media_frus, count;
	size_t data_size;
	int ret;

	num_media_frus = cxl_ecs_ctx->num_media_frus;
	rd_data_size = cxl_ecs_ctx->get_feat_size;
	wr_data_size = cxl_ecs_ctx->set_feat_size;
	struct cxl_ecs_rd_attrs *rd_attrs __free(kfree) =
				kmalloc(rd_data_size, GFP_KERNEL);
	if (!rd_attrs)
		return -ENOMEM;

	data_size = cxl_get_feature(mds, cxl_ecs_uuid,
				    CXL_GET_FEAT_SEL_CURRENT_VALUE,
				    rd_attrs, rd_data_size);
	if (!data_size)
		return -EIO;
	struct cxl_ecs_wr_attrs *wr_attrs __free(kfree) =
					kmalloc(wr_data_size, GFP_KERNEL);
	if (!wr_attrs)
		return -ENOMEM;

	/* Fill writable attributes from the current attributes
	 * read for all the media FRUs.
	 */
	for (count = 0; count < num_media_frus; count++) {
		wr_attrs[count].ecs_log_cap = rd_attrs[count].ecs_log_cap;
	wr_attrs[count].ecs_config = rd_attrs[count].ecs_config;
	}

	/* Fill attribute to be set for the media FRU */
	switch (param_type) {
	case CXL_ECS_PARAM_LOG_ENTRY_TYPE:
		if (params->log_entry_type != ECS_LOG_ENTRY_TYPE_DRAM &&
		    params->log_entry_type != ECS_LOG_ENTRY_TYPE_MEM_MEDIA_FRU) {
			dev_err(dev,
				"Invalid CXL ECS scrub log entry type(%d) to set\n",
			       params->log_entry_type);
			dev_err(dev,
				"Log Entry Type 0: per DRAM  1: per Memory Media FRU\n");
			return -EINVAL;
		}
		wr_attrs[fru_id].ecs_log_cap = FIELD_PREP(CXL_ECS_LOG_ENTRY_TYPE_MASK,
							  params->log_entry_type);
		break;
	case CXL_ECS_PARAM_THRESHOLD:
		wr_attrs[fru_id].ecs_config &= ~CXL_ECS_THRESHOLD_COUNT_MASK;
		switch (params->threshold) {
		case 256:
			wr_attrs[fru_id].ecs_config |= FIELD_PREP(CXL_ECS_THRESHOLD_COUNT_MASK,
								  ECS_THRESHOLD_256);
			break;
		case 1024:
			wr_attrs[fru_id].ecs_config |= FIELD_PREP(CXL_ECS_THRESHOLD_COUNT_MASK,
								  ECS_THRESHOLD_1024);
			break;
		case 4096:
			wr_attrs[fru_id].ecs_config |= FIELD_PREP(CXL_ECS_THRESHOLD_COUNT_MASK,
								  ECS_THRESHOLD_4096);
			break;
		default:
			dev_err(dev,
				"Invalid CXL ECS scrub threshold count(%d) to set\n",
				params->threshold);
			dev_err(dev,
				"Supported scrub threshold count: 256,1024,4096\n");
			return -EINVAL;
		}
		break;
	case CXL_ECS_PARAM_MODE:
		if (params->mode != ECS_MODE_COUNTS_ROWS &&
		    params->mode != ECS_MODE_COUNTS_CODEWORDS) {
			dev_err(dev,
				"Invalid CXL ECS scrub mode(%d) to set\n",
				params->mode);
			dev_err(dev,
				"Mode 0: ECS counts rows with errors"
				" 1: ECS counts codewords with errors\n");
			return -EINVAL;
		}
		wr_attrs[fru_id].ecs_config &= ~CXL_ECS_MODE_MASK;
		wr_attrs[fru_id].ecs_config |= FIELD_PREP(CXL_ECS_MODE_MASK,
							  params->mode);
		break;
	case CXL_ECS_PARAM_RESET_COUNTER:
		wr_attrs[fru_id].ecs_config &= ~CXL_ECS_RESET_COUNTER_MASK;
		wr_attrs[fru_id].ecs_config |= FIELD_PREP(CXL_ECS_RESET_COUNTER_MASK,
							  params->reset_counter);
		break;
	default:
		dev_err(dev, "Invalid CXL ECS parameter to set\n");
		return -EINVAL;
	}

	ret = cxl_set_feature(mds, cxl_ecs_uuid, cxl_ecs_ctx->set_version,
			      wr_attrs, wr_data_size,
			      CXL_SET_FEAT_FLAG_DATA_SAVED_ACROSS_RESET);
	if (ret) {
		dev_err(dev, "CXL ECS set feature failed ret=%d\n", ret);
		return ret;
	}

	return 0;
}

static int cxl_ecs_get_log_entry_type(struct device *dev, void *drv_data,
				      int fru_id, u32 *val)
{
	struct cxl_ecs_params params;
	int ret;

	ret = cxl_mem_ecs_get_attrs(dev, drv_data, fru_id, &params);
	if (ret)
		return ret;

	*val = params.log_entry_type;

	return 0;
}

static int cxl_ecs_set_log_entry_type(struct device *dev, void *drv_data,
				      int fru_id, u32 val)
{
	struct cxl_ecs_params params = {
		.log_entry_type = val,
	};

	return cxl_mem_ecs_set_attrs(dev, drv_data, fru_id,
				     &params, CXL_ECS_PARAM_LOG_ENTRY_TYPE);
}

static int cxl_ecs_get_log_entry_type_per_dram(struct device *dev, void *drv_data,
					       int fru_id, u32 *val)
{
	struct cxl_ecs_params params;
	int ret;

	ret = cxl_mem_ecs_get_attrs(dev, drv_data, fru_id, &params);
	if (ret)
		return ret;

	if (params.log_entry_type == ECS_LOG_ENTRY_TYPE_DRAM)
		*val = 1;
	else
		*val = 0;

	return 0;
}

static int cxl_ecs_get_log_entry_type_per_memory_media(struct device *dev,
						       void *drv_data,
						       int fru_id, u32 *val)
{
	struct cxl_ecs_params params;
	int ret;

	ret = cxl_mem_ecs_get_attrs(dev, drv_data, fru_id, &params);
	if (ret)
		return ret;

	if (params.log_entry_type == ECS_LOG_ENTRY_TYPE_MEM_MEDIA_FRU)
		*val = 1;
	else
		*val = 0;

	return 0;
}

static int cxl_ecs_get_mode(struct device *dev, void *drv_data,
			    int fru_id, u32 *val)
{
	struct cxl_ecs_params params;
	int ret;

	ret = cxl_mem_ecs_get_attrs(dev, drv_data, fru_id, &params);
	if (ret)
		return ret;

	*val = params.mode;

	return 0;
}

static int cxl_ecs_set_mode(struct device *dev, void *drv_data,
			    int fru_id, u32 val)
{
	struct cxl_ecs_params params = {
		.mode = val,
	};

	return cxl_mem_ecs_set_attrs(dev, drv_data, fru_id,
				     &params, CXL_ECS_PARAM_MODE);
}

static int cxl_ecs_get_mode_counts_rows(struct device *dev, void *drv_data,
					int fru_id, u32 *val)
{
	struct cxl_ecs_params params;
	int ret;

	ret = cxl_mem_ecs_get_attrs(dev, drv_data, fru_id, &params);
	if (ret)
		return ret;

	if (params.mode == ECS_MODE_COUNTS_ROWS)
		*val = 1;
	else
		*val = 0;

	return 0;
}

static int cxl_ecs_get_mode_counts_codewords(struct device *dev, void *drv_data,
					     int fru_id, u32 *val)
{
	struct cxl_ecs_params params;
	int ret;

	ret = cxl_mem_ecs_get_attrs(dev, drv_data, fru_id, &params);
	if (ret)
		return ret;

	if (params.mode == ECS_MODE_COUNTS_CODEWORDS)
		*val = 1;
	else
		*val = 0;

	return 0;
}

static int cxl_ecs_reset(struct device *dev, void *drv_data, int fru_id, u32 val)
{
	struct cxl_ecs_params params = {
		.reset_counter = val,
	};

	return cxl_mem_ecs_set_attrs(dev, drv_data, fru_id,
				     &params, CXL_ECS_PARAM_RESET_COUNTER);
}

static int cxl_ecs_get_threshold(struct device *dev, void *drv_data,
				 int fru_id, u32 *val)
{
	struct cxl_ecs_params params;
	int ret;

	ret = cxl_mem_ecs_get_attrs(dev, drv_data, fru_id, &params);
	if (ret)
		return ret;

	*val = params.threshold;

	return 0;
}

static int cxl_ecs_set_threshold(struct device *dev, void *drv_data,
				 int fru_id, u32 val)
{
	struct cxl_ecs_params params = {
		.threshold = val,
	};

	return cxl_mem_ecs_set_attrs(dev, drv_data, fru_id,
				     &params, CXL_ECS_PARAM_THRESHOLD);
}

static const struct edac_ecs_ops cxl_ecs_ops = {
	.get_log_entry_type = cxl_ecs_get_log_entry_type,
	.set_log_entry_type = cxl_ecs_set_log_entry_type,
	.get_log_entry_type_per_dram = cxl_ecs_get_log_entry_type_per_dram,
	.get_log_entry_type_per_memory_media =
				cxl_ecs_get_log_entry_type_per_memory_media,
	.get_mode = cxl_ecs_get_mode,
	.set_mode = cxl_ecs_set_mode,
	.get_mode_counts_codewords = cxl_ecs_get_mode_counts_codewords,
	.get_mode_counts_rows = cxl_ecs_get_mode_counts_rows,
	.reset = cxl_ecs_reset,
	.get_threshold = cxl_ecs_get_threshold,
	.set_threshold = cxl_ecs_set_threshold,
};

/* CXL memory soft PPR & hard PPR control definitions */
static const uuid_t cxl_sppr_uuid =
	UUID_INIT(0x892ba475, 0xfad8, 0x474e, 0x9d, 0x3e, 0x69, 0x2c, 0x91,     \
		  0x75, 0x68, 0xbb);

static const uuid_t cxl_hppr_uuid =
	UUID_INIT(0x80ea4521, 0x786f, 0x4127, 0xaf, 0xb1, 0xec, 0x74, 0x59,     \
		  0xfb, 0x0e, 0x24);

struct cxl_ppr_context {
	uuid_t ppr_uuid;
	u8 instance;
	u16 get_feat_size;
	u16 set_feat_size;
	u8 get_version;
	u8 set_version;
	u16 set_effects;
	struct cxl_memdev *cxlmd;
	enum edac_ppr_type ppr_type;
	u64 dpa;
	u32 nibble_mask;
};

/**
 * struct cxl_memdev_ppr_params - CXL memory PPR parameter data structure.
 * @op_class[OUT]: PPR operation class.
 * @op_subclass[OUT]: PPR operation subclass.
 * @dpa_support[OUT]: device physical address for PPR support.
 * @media_accessible[OUT]: memory media is accessible or not during PPR operation.
 * @data_retained[OUT]: data is retained or not during PPR operation.
 * @dpa:[IN]: device physical address.
 */
struct cxl_memdev_ppr_params {
	u8 op_class;
	u8 op_subclass;
	bool dpa_support;
	bool media_accessible;
	bool data_retained;
	u64 dpa;
};

enum cxl_ppr_param {
	CXL_PPR_PARAM_DO_PPR,
};

#define	CXL_MEMDEV_PPR_QUERY_RESOURCE_FLAG BIT(0)

#define CXL_MEMDEV_PPR_DEVICE_INITIATED_MASK BIT(0)
#define CXL_MEMDEV_PPR_FLAG_DPA_SUPPORT_MASK BIT(0)
#define CXL_MEMDEV_PPR_FLAG_NIBBLE_SUPPORT_MASK BIT(1)
#define CXL_MEMDEV_PPR_FLAG_MEM_SPARING_EV_REC_SUPPORT_MASK BIT(2)

#define CXL_MEMDEV_PPR_RESTRICTION_FLAG_MEDIA_ACCESSIBLE_MASK BIT(0)
#define CXL_MEMDEV_PPR_RESTRICTION_FLAG_DATA_RETAINED_MASK BIT(2)

#define CXL_MEMDEV_PPR_SPARING_EV_REC_EN_MASK BIT(0)

struct cxl_memdev_ppr_rd_attrs {
	u8 max_op_latency;
	__le16 op_cap;
	__le16 op_mode;
	u8 op_class;
	u8 op_subclass;
	u8 rsvd[9];
	u8 ppr_flags;
	__le16 restriction_flags;
	u8 ppr_op_mode;
}  __packed;

struct cxl_memdev_ppr_wr_attrs {
	__le16 op_mode;
	u8 ppr_op_mode;
}  __packed;

struct cxl_memdev_ppr_maintenance_attrs {
	u8 flags;
	__le64 dpa;
	u8 nibble_mask[3];
}  __packed;

static int cxl_mem_ppr_get_attrs(struct device *dev, void *drv_data,
				 struct cxl_memdev_ppr_params *params)
{
	struct cxl_ppr_context *cxl_ppr_ctx = drv_data;
	struct cxl_memdev *cxlmd = cxl_ppr_ctx->cxlmd;
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlds);
	size_t rd_data_size = sizeof(struct cxl_memdev_ppr_rd_attrs);
	size_t data_size;
	struct cxl_memdev_ppr_rd_attrs *rd_attrs __free(kfree) =
				kmalloc(rd_data_size, GFP_KERNEL);
	if (!rd_attrs)
		return -ENOMEM;

	data_size = cxl_get_feature(mds, cxl_ppr_ctx->ppr_uuid,
				    CXL_GET_FEAT_SEL_CURRENT_VALUE,
				    rd_attrs, rd_data_size);
	if (!data_size)
		return -EIO;

	params->op_class = rd_attrs->op_class;
	params->op_subclass = rd_attrs->op_subclass;
	params->dpa_support = FIELD_GET(CXL_MEMDEV_PPR_FLAG_DPA_SUPPORT_MASK,
					rd_attrs->ppr_flags);
	params->media_accessible = FIELD_GET(CXL_MEMDEV_PPR_RESTRICTION_FLAG_MEDIA_ACCESSIBLE_MASK,
					     rd_attrs->restriction_flags) ^ 1;
	params->data_retained = FIELD_GET(CXL_MEMDEV_PPR_RESTRICTION_FLAG_DATA_RETAINED_MASK,
					  rd_attrs->restriction_flags) ^ 1;

	return 0;
}

static int cxl_mem_ppr_set_attrs(struct device *dev, void *drv_data,
				 struct cxl_memdev_ppr_params *params,
				 enum cxl_ppr_param param_type)
{
	struct cxl_memdev_ppr_maintenance_attrs maintenance_attrs;
	struct cxl_ppr_context *cxl_ppr_ctx = drv_data;
	struct cxl_memdev *cxlmd = cxl_ppr_ctx->cxlmd;
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlds);
	struct cxl_memdev_ppr_params rd_params;
	struct cxl_region *cxlr;
	int ret;

	ret = cxl_mem_ppr_get_attrs(dev, drv_data, &rd_params);
	if (ret) {
		dev_err(dev, "Get cxlmemdev PPR params failed ret=%d\n",
			ret);
		return ret;
	}

	switch (param_type) {
	case CXL_PPR_PARAM_DO_PPR:
		ret = down_read_interruptible(&cxl_region_rwsem);
		if (ret)
			return ret;
		if (!rd_params.media_accessible || !rd_params.data_retained) {
			/* Check if DPA is mapped */
			ret = down_read_interruptible(&cxl_dpa_rwsem);
			if (ret) {
				up_read(&cxl_region_rwsem);
				return ret;
			}

			cxlr = cxl_dpa_to_region(cxlmd, cxl_ppr_ctx->dpa);
			up_read(&cxl_dpa_rwsem);
			if (cxlr) {
				dev_err(dev, "CXL can't do PPR as DPA is mapped\n");
				up_read(&cxl_region_rwsem);
				return -EBUSY;
			}
		}
		maintenance_attrs.flags = CXL_MEMDEV_PPR_QUERY_RESOURCE_FLAG;
		maintenance_attrs.dpa = params->dpa;
		/* May need to get the nibble mask from the CXL dram error record via
		 * trace dram event. Presently all nibble masks bits set to 1.
		 */
		maintenance_attrs.nibble_mask[0] = 0xFF;
		maintenance_attrs.nibble_mask[1] = 0xFF;
		maintenance_attrs.nibble_mask[2] = 0xFF;
		ret = cxl_do_maintenance(mds, rd_params.op_class, rd_params.op_subclass,
					 &maintenance_attrs, sizeof(maintenance_attrs));
		if (ret) {
			dev_err(dev, "CXL do PPR maintenance failed ret=%d\n", ret);
			up_read(&cxl_region_rwsem);
			return ret;
		}
		up_read(&cxl_region_rwsem);
		return 0;
	default:
		return -EINVAL;
	}
}

static int cxl_ppr_get_persist_mode_avail(struct device *dev, void *drv_data,
					  char *buf)
{
	return sysfs_emit(buf, "Soft PPR Hard PPR\n");
}

static int cxl_ppr_get_persist_mode(struct device *dev, void *drv_data,
				    u32 *persist_mode)
{
	struct cxl_ppr_context *cxl_ppr_ctx = drv_data;

	*persist_mode = cxl_ppr_ctx->ppr_type;

	return 0;
}

static int cxl_ppr_get_dpa_support(struct device *dev, void *drv_data,
				   u32 *dpa_support)
{
	struct cxl_memdev_ppr_params params;
	int ret;

	ret = cxl_mem_ppr_get_attrs(dev, drv_data, &params);
	if (ret)
		return ret;

	*dpa_support = params.dpa_support;

	return 0;
}

static int cxl_get_ppr_safe_when_in_use(struct device *dev, void *drv_data,
					u32 *safe)
{
	struct cxl_memdev_ppr_params params;
	int ret;

	ret = cxl_mem_ppr_get_attrs(dev, drv_data, &params);
	if (ret)
		return ret;

	*safe = params.media_accessible & params.data_retained;

	return 0;
}

static int cxl_do_ppr(struct device *dev, void *drv_data, bool hpa, u64 pa)
{
	struct cxl_memdev_ppr_params params = {
		.dpa = pa,
	};

	/* CXL mem perform PPR, need support for HPA? */
	if (hpa)
		return -EOPNOTSUPP;

	return cxl_mem_ppr_set_attrs(dev, drv_data, &params,
				     CXL_PPR_PARAM_DO_PPR);
}

static const struct edac_ppr_ops cxl_sppr_ops = {
	.get_persist_mode_avail = cxl_ppr_get_persist_mode_avail,
	.get_persist_mode = cxl_ppr_get_persist_mode,
	.get_dpa_support = cxl_ppr_get_dpa_support,
	.get_ppr_safe_when_in_use = cxl_get_ppr_safe_when_in_use,
	.do_ppr = cxl_do_ppr,
};

int cxl_mem_ras_features_init(struct cxl_memdev *cxlmd, struct cxl_region *cxlr)
{
	struct edac_dev_feature ras_features[CXL_DEV_NUM_RAS_FEATURES];
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlds);
	struct cxl_patrol_scrub_context *cxl_ps_ctx;
	struct cxl_mbox_supp_feat_entry feat_entry;
	char cxl_dev_name[CXL_SCRUB_NAME_LEN];
	struct cxl_ecs_context *cxl_ecs_ctx;
	struct cxl_ppr_context *cxl_sppr_ctx;
	int rc, i, num_ras_features = 0;
	int num_media_frus;
	u8 ppr_inst = 0;

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

	if (!cxlr) {
		rc = cxl_get_supported_feature_entry(mds, &cxl_ecs_uuid,
						     &feat_entry);
		if (rc < 0)
			goto feat_register;

		if (!(feat_entry.attr_flags & CXL_FEAT_ENTRY_FLAG_CHANGABLE))
			goto feat_register;
		num_media_frus = feat_entry.get_size /
					sizeof(struct cxl_ecs_rd_attrs);
		if (!num_media_frus)
			goto feat_register;

		cxl_ecs_ctx = devm_kzalloc(&cxlmd->dev, sizeof(*cxl_ecs_ctx),
					   GFP_KERNEL);
		if (!cxl_ecs_ctx)
			goto feat_register;
		*cxl_ecs_ctx = (struct cxl_ecs_context) {
			.get_feat_size = feat_entry.get_size,
			.set_feat_size = feat_entry.set_size,
			.get_version = feat_entry.get_version,
			.set_version = feat_entry.set_version,
			.set_effects = feat_entry.set_effects,
			.num_media_frus = num_media_frus,
			.cxlmd = cxlmd,
		};

		ras_features[num_ras_features].feat = RAS_FEAT_ECS;
		ras_features[num_ras_features].ecs_ops = &cxl_ecs_ops;
		ras_features[num_ras_features].ecs_ctx = cxl_ecs_ctx;
		ras_features[num_ras_features].ecs_info.num_media_frus =
								num_media_frus;
		num_ras_features++;

		/* CXL sPPR */
		rc = cxl_get_supported_feature_entry(mds, &cxl_sppr_uuid,
						     &feat_entry);
		if (rc < 0)
			goto feat_register;

		if (!(feat_entry.attr_flags & CXL_FEAT_ENTRY_FLAG_CHANGABLE))
			goto feat_register;

		cxl_sppr_ctx = devm_kzalloc(&cxlmd->dev, sizeof(*cxl_sppr_ctx),
					    GFP_KERNEL);
		if (!cxl_sppr_ctx)
			goto feat_register;
		*cxl_sppr_ctx = (struct cxl_ppr_context) {
			.ppr_uuid = cxl_sppr_uuid,
			.get_feat_size = feat_entry.get_size,
			.set_feat_size = feat_entry.set_size,
			.get_version = feat_entry.get_version,
			.set_version = feat_entry.set_version,
			.set_effects = feat_entry.set_effects,
			.cxlmd = cxlmd,
			.ppr_type = EDAC_TYPE_SPPR,
			.instance = ppr_inst++,
		};

		ras_features[num_ras_features].feat = RAS_FEAT_PPR;
		ras_features[num_ras_features].instance = cxl_sppr_ctx->instance;
		ras_features[num_ras_features].ppr_ops = &cxl_sppr_ops;
		ras_features[num_ras_features].ppr_ctx = cxl_sppr_ctx;
		num_ras_features++;
	}

feat_register:
	return edac_dev_register(&cxlmd->dev, cxl_dev_name, NULL,
				 num_ras_features, ras_features);
}
EXPORT_SYMBOL_NS_GPL(cxl_mem_ras_features_init, CXL);
