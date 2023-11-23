// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cxl_memscrub.c - CXL memory scrub driver
 *
 * Copyright (c) 2023 HiSilicon Limited.
 *
 *  - Provides functions to configure patrol scrub
 *    and DDR5 ECS features of the CXL memory devices.
 *  - Registers with the scrub driver to expose
 *    the sysfs attributes to the user for configuring
 *    the memory patrol scrub and DDR5 ECS features.
 */

#define pr_fmt(fmt)	"CXL_MEM_SCRUB: " fmt

#include <cxlmem.h>
#include <memory/memory-scrub.h>

/* CXL memory scrub feature common definitions */
#define CXL_SCRUB_MAX_ATTRB_RANGE_LENGTH	128
#define CXL_MEMDEV_MAX_NAME_LENGTH	128

static int cxl_mem_get_supported_feature_entry(struct cxl_memdev *cxlmd, const uuid_t *feat_uuid,
					       struct cxl_mbox_supp_feat_entry *feat_entry_out)
{
	struct cxl_mbox_get_supp_feats_out *feats_out __free(kvfree) = NULL;
	struct cxl_mbox_supp_feat_entry *feat_entry;
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlds);
	struct cxl_mbox_get_supp_feats_in pi;
	int feat_index, count;
	int nentries;
	int ret;

	feat_index = 0;
	pi.count = sizeof(struct cxl_mbox_get_supp_feats_out) +
			  sizeof(struct cxl_mbox_supp_feat_entry);
	feats_out = kvmalloc(pi.count, GFP_KERNEL);
	if (!feats_out)
		return -ENOMEM;

	do {
		pi.start_index = feat_index;
		memset(feats_out, 0, pi.count);
		ret = cxl_get_supported_features(mds, &pi, feats_out);
		if (ret)
			return ret;

		nentries = feats_out->entries;
		if (!nentries)
			break;

		/* Check CXL memdev supports the feature */
		feat_entry = (void *)feats_out->feat_entries;
		for (count = 0; count < nentries; count++, feat_entry++) {
			if (uuid_equal(&feat_entry->uuid, feat_uuid)) {
				memcpy(feat_entry_out, feat_entry, sizeof(*feat_entry_out));
				return 0;
			}
		}
		feat_index += nentries;
	} while (nentries);

	return -ENOTSUPP;
}

/* CXL memory patrol scrub control definitions */
#define CXL_MEMDEV_PS_GET_FEAT_VERSION	0x01
#define CXL_MEMDEV_PS_SET_FEAT_VERSION	0x01

#define CXL_PATROL_SCRUB	"cxl_patrol_scrub"

/* The default number of regions for CXL memory device patrol scrubber
 * Patrol scrub is a feature where the device controller scrubs the
 * memory at a regular interval accroding to the CXL specification.
 * Hence the number of memory regions to scrub assosiated to the patrol
 * scrub is 1.
 */
#define CXL_MEMDEV_PATROL_SCRUB_NUM_REGIONS	1

static const uuid_t cxl_patrol_scrub_uuid =
	UUID_INIT(0x96dad7d6, 0xfde8, 0x482b, 0xa7, 0x33, 0x75, 0x77, 0x4e,     \
		  0x06, 0xdb, 0x8a);

/* CXL memory patrol scrub control functions */
struct cxl_patrol_scrub_context {
	struct device *dev;
	u16 get_feat_size;
	u16 set_feat_size;
	bool scrub_cycle_changeable;
};

/**
 * struct cxl_memdev_ps_params - CXL memory patrol scrub parameter data structure.
 * @enable:     [IN] enable(1)/disable(0) patrol scrub.
 * @scrub_cycle_changeable: [OUT] scrub cycle attribute of patrol scrub is changeable.
 * @speed:      [IN] Requested patrol scrub cycle in hours.
 *              [OUT] Current patrol scrub cycle in hours.
 * @min_speed:[OUT] minimum patrol scrub cycle, in hours, supported.
 * @speed_avail:[OUT] Supported patrol scrub cycle in hours.
 */
struct cxl_memdev_ps_params {
	bool enable;
	bool scrub_cycle_changeable;
	u16 speed;
	u16 min_speed;
	char speed_avail[CXL_SCRUB_MAX_ATTRB_RANGE_LENGTH];
};

enum {
	CXL_MEMDEV_PS_PARAM_ENABLE = 0,
	CXL_MEMDEV_PS_PARAM_SPEED,
};

#define	CXL_MEMDEV_PS_SCRUB_CYCLE_CHANGE_CAP_MASK	BIT(0)
#define	CXL_MEMDEV_PS_SCRUB_CYCLE_REALTIME_REPORT_CAP_MASK	BIT(1)
#define	CXL_MEMDEV_PS_CUR_SCRUB_CYCLE_MASK	GENMASK(7, 0)
#define	CXL_MEMDEV_PS_MIN_SCRUB_CYCLE_MASK	GENMASK(15, 8)
#define	CXL_MEMDEV_PS_FLAG_ENABLED_MASK	BIT(0)

struct cxl_memdev_ps_feat_read_attrbs {
	u8 scrub_cycle_cap;
	__le16 scrub_cycle;
	u8 scrub_flags;
}  __packed;

struct cxl_memdev_ps_set_feat_pi {
	struct cxl_mbox_set_feat_in pi;
	u8 scrub_cycle_hr;
	u8 scrub_flags;
}  __packed;

static int cxl_mem_ps_get_attrbs(struct device *dev,
				 struct cxl_memdev_ps_params *params)
{
	struct cxl_memdev_ps_feat_read_attrbs *rd_attrbs __free(kvfree) = NULL;
	struct cxl_mbox_get_feat_in pi = {
		.uuid = cxl_patrol_scrub_uuid,
		.offset = 0,
		.count = sizeof(struct cxl_memdev_ps_feat_read_attrbs),
		.selection = CXL_GET_FEAT_SEL_CURRENT_VALUE,
	};
	struct cxl_memdev *cxlmd = to_cxl_memdev(dev);
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlds);
	int ret;

	if (!mds)
		return -EFAULT;

	rd_attrbs = kvmalloc(pi.count, GFP_KERNEL);
	if (!rd_attrbs)
		return -ENOMEM;

	ret = cxl_get_feature(mds, &pi, rd_attrbs);
	if (ret) {
		params->enable = 0;
		params->speed = 0;
		snprintf(params->speed_avail, CXL_SCRUB_MAX_ATTRB_RANGE_LENGTH,
			"Unavailable");
		return ret;
	}
	params->scrub_cycle_changeable = FIELD_GET(CXL_MEMDEV_PS_SCRUB_CYCLE_CHANGE_CAP_MASK,
						   rd_attrbs->scrub_cycle_cap);
	params->enable = FIELD_GET(CXL_MEMDEV_PS_FLAG_ENABLED_MASK,
				   rd_attrbs->scrub_flags);
	params->speed = FIELD_GET(CXL_MEMDEV_PS_CUR_SCRUB_CYCLE_MASK,
				  rd_attrbs->scrub_cycle);
	params->min_speed  = FIELD_GET(CXL_MEMDEV_PS_MIN_SCRUB_CYCLE_MASK,
				       rd_attrbs->scrub_cycle);
	snprintf(params->speed_avail, CXL_SCRUB_MAX_ATTRB_RANGE_LENGTH,
		 "Minimum scrub cycle = %d hour", params->min_speed);

	return 0;
}

static int cxl_mem_ps_set_attrbs(struct device *dev,
				 struct cxl_memdev_ps_params *params, u8 param_type)
{
	struct cxl_memdev_ps_set_feat_pi set_pi = {
		.pi.uuid = cxl_patrol_scrub_uuid,
		.pi.flags = CXL_SET_FEAT_FLAG_MOD_VALUE_SAVED_ACROSS_RESET |
			    CXL_SET_FEAT_FLAG_FULL_DATA_TRANSFER,
		.pi.offset = 0,
		.pi.version = CXL_MEMDEV_PS_SET_FEAT_VERSION,
	};
	struct cxl_memdev *cxlmd = to_cxl_memdev(dev);
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlds);
	struct cxl_memdev_ps_params rd_params;
	int ret;

	if (!mds)
		return -EFAULT;

	ret = cxl_mem_ps_get_attrbs(dev, &rd_params);
	if (ret) {
		dev_err(dev, "Get cxlmemdev patrol scrub params fail ret=%d\n",
			ret);
		return ret;
	}

	switch (param_type) {
	case CXL_MEMDEV_PS_PARAM_ENABLE:
		set_pi.scrub_flags = FIELD_PREP(CXL_MEMDEV_PS_FLAG_ENABLED_MASK,
						   params->enable);
		set_pi.scrub_cycle_hr = FIELD_PREP(CXL_MEMDEV_PS_CUR_SCRUB_CYCLE_MASK,
						      rd_params.speed);
		break;
	case CXL_MEMDEV_PS_PARAM_SPEED:
		if (params->speed < rd_params.min_speed) {
			dev_err(dev, "Invalid CXL patrol scrub cycle(%d) to set\n",
				params->speed);
			dev_err(dev, "Minimum supported CXL patrol scrub cycle in hour %d\n",
			       params->min_speed);
			return -EINVAL;
		}
		set_pi.scrub_cycle_hr = FIELD_PREP(CXL_MEMDEV_PS_CUR_SCRUB_CYCLE_MASK,
						      params->speed);
		set_pi.scrub_flags = FIELD_PREP(CXL_MEMDEV_PS_FLAG_ENABLED_MASK,
						   rd_params.enable);
		break;
	default:
		dev_err(dev, "Invalid CXL patrol scrub parameter to set\n");
		return -EINVAL;
	}

	ret = cxl_set_feature(mds, &set_pi, sizeof(set_pi));
	if (ret) {
		dev_err(dev, "CXL patrol scrub set feature fail ret=%d\n",
			ret);
		return ret;
	}

	/* Verify attribute set successfully */
	if (param_type == CXL_MEMDEV_PS_PARAM_SPEED) {
		ret = cxl_mem_ps_get_attrbs(dev, &rd_params);
		if (ret) {
			dev_err(dev, "Get cxlmemdev patrol scrub params fail ret=%d\n", ret);
			return ret;
		}
		if (rd_params.speed != params->speed)
			return -EFAULT;
	}

	return 0;
}

static int cxl_mem_ps_enable_write(struct device *dev, long val)
{
	struct cxl_memdev_ps_params params;
	int ret;

	params.enable = val;
	ret = cxl_mem_ps_set_attrbs(dev, &params, CXL_MEMDEV_PS_PARAM_ENABLE);
	if (ret) {
		dev_err(dev, "CXL patrol scrub enable fail, enable=%d ret=%d\n",
		       params.enable, ret);
		return ret;
	}

	return 0;
}

static int cxl_mem_ps_speed_read(struct device *dev, u64 *val)
{
	struct cxl_memdev_ps_params params;
	int ret;

	ret = cxl_mem_ps_get_attrbs(dev, &params);
	if (ret) {
		dev_err(dev, "Get CXL patrol scrub params fail ret=%d\n",
			ret);
		return ret;
	}
	*val = params.speed;

	return 0;
}

static int cxl_mem_ps_speed_write(struct device *dev, long val)
{
	struct cxl_memdev_ps_params params;
	int ret;

	params.speed = val;
	ret = cxl_mem_ps_set_attrbs(dev, &params, CXL_MEMDEV_PS_PARAM_SPEED);
	if (ret) {
		dev_err(dev, "Set CXL patrol scrub params for speed fail ret=%d\n",
			ret);
		return ret;
	}

	return 0;
}

static int cxl_mem_ps_speed_available_read(struct device *dev, char *buf)
{
	struct cxl_memdev_ps_params params;
	int ret;

	ret = cxl_mem_ps_get_attrbs(dev, &params);
	if (ret) {
		dev_err(dev, "Get CXL patrol scrub params fail ret=%d\n",
			ret);
		return ret;
	}

	sysfs_emit(buf, "%s\n", params.speed_avail);

	return 0;
}

/**
 * cxl_mem_patrol_scrub_is_visible() - Callback to return attribute visibility
 * @drv_data: Pointer to driver-private data structure passed
 *	      as argument to devm_scrub_device_register().
 * @attr: Scrub attribute
 * @region_id: ID of the memory region
 *
 * Returns: 0 on success, an error otherwise
 */
static umode_t cxl_mem_patrol_scrub_is_visible(const void *drv_data,
					       u32 attr, int region_id)
{
	const struct cxl_patrol_scrub_context *cxl_ps_ctx = drv_data;

	if (attr == scrub_speed_available ||
	    attr == scrub_speed) {
		if (!cxl_ps_ctx->scrub_cycle_changeable)
			return 0;
	}

	switch (attr) {
	case scrub_speed_available:
		return 0444;
	case scrub_enable:
		return 0200;
	case scrub_speed:
		return 0644;
	default:
		return 0;
	}
}

/**
 * cxl_mem_patrol_scrub_read() - Read callback for data attributes
 * @dev: Pointer to scrub device
 * @attr: Scrub attribute
 * @region_id: ID of the memory region
 * @val: Pointer to the returned data
 *
 * Returns: 0 on success, an error otherwise
 */
static int cxl_mem_patrol_scrub_read(struct device *dev, u32 attr,
				     int region_id, u64 *val)
{

	switch (attr) {
	case scrub_speed:
		return cxl_mem_ps_speed_read(dev->parent, val);
	default:
		return -ENOTSUPP;
	}
}

/**
 * cxl_mem_patrol_scrub_write() - Write callback for data attributes
 * @dev: Pointer to scrub device
 * @attr: Scrub attribute
 * @region_id: ID of the memory region
 * @val: Value to write
 *
 * Returns: 0 on success, an error otherwise
 */
static int cxl_mem_patrol_scrub_write(struct device *dev, u32 attr,
				      int region_id, u64 val)
{
	switch (attr) {
	case scrub_enable:
		return cxl_mem_ps_enable_write(dev->parent, val);
	case scrub_speed:
		return cxl_mem_ps_speed_write(dev->parent, val);
	default:
		return -ENOTSUPP;
	}
}

/**
 * cxl_mem_patrol_scrub_read_strings() - Read callback for string attributes
 * @dev: Pointer to scrub device
 * @attr: Scrub attribute
 * @region_id: ID of the memory region
 * @buf: Pointer to the buffer for copying returned string
 *
 * Returns: 0 on success, an error otherwise
 */
static int cxl_mem_patrol_scrub_read_strings(struct device *dev, u32 attr,
					     int region_id, char *buf)
{
	switch (attr) {
	case scrub_speed_available:
		return cxl_mem_ps_speed_available_read(dev->parent, buf);
	default:
		return -ENOTSUPP;
	}
}

static const struct scrub_ops cxl_ps_scrub_ops = {
	.is_visible = cxl_mem_patrol_scrub_is_visible,
	.read = cxl_mem_patrol_scrub_read,
	.write = cxl_mem_patrol_scrub_write,
	.read_string = cxl_mem_patrol_scrub_read_strings,
};

int cxl_mem_patrol_scrub_init(struct cxl_memdev *cxlmd)
{
	char scrub_name[CXL_MEMDEV_MAX_NAME_LENGTH];
	struct cxl_patrol_scrub_context *cxl_ps_ctx;
	struct cxl_mbox_supp_feat_entry feat_entry;
	struct cxl_memdev_ps_params params;
	struct device *cxl_scrub_dev;
	int ret;

	ret = cxl_mem_get_supported_feature_entry(cxlmd, &cxl_patrol_scrub_uuid,
						  &feat_entry);
	if (ret < 0)
		return ret;

	if (!(feat_entry.attrb_flags & CXL_FEAT_ENTRY_FLAG_CHANGABLE))
		return -ENOTSUPP;

	cxl_ps_ctx = devm_kzalloc(&cxlmd->dev, sizeof(*cxl_ps_ctx), GFP_KERNEL);
	if (!cxl_ps_ctx)
		return -ENOMEM;

	cxl_ps_ctx->get_feat_size = feat_entry.get_feat_size;
	cxl_ps_ctx->set_feat_size = feat_entry.set_feat_size;
	ret = cxl_mem_ps_get_attrbs(&cxlmd->dev, &params);
	if (ret) {
		dev_err(&cxlmd->dev, "Get CXL patrol scrub params fail ret=%d\n",
			ret);
		return ret;
	}
	cxl_ps_ctx->scrub_cycle_changeable =  params.scrub_cycle_changeable;

	snprintf(scrub_name, sizeof(scrub_name), "%s_%s",
		 CXL_PATROL_SCRUB, dev_name(&cxlmd->dev));
	cxl_scrub_dev = devm_scrub_device_register(&cxlmd->dev, scrub_name,
						   cxl_ps_ctx, &cxl_ps_scrub_ops,
						   CXL_MEMDEV_PATROL_SCRUB_NUM_REGIONS);
	if (IS_ERR(cxl_scrub_dev))
		return PTR_ERR(cxl_scrub_dev);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_mem_patrol_scrub_init, CXL);

/* CXL DDR5 ECS control definitions */
#define CXL_MEMDEV_ECS_GET_FEAT_VERSION	0x01
#define CXL_MEMDEV_ECS_SET_FEAT_VERSION	0x01

static const uuid_t cxl_ecs_uuid =
	UUID_INIT(0xe5b13f22, 0x2328, 0x4a14, 0xb8, 0xba, 0xb9, 0x69, 0x1e,     \
		  0x89, 0x33, 0x86);

struct cxl_ecs_context {
	struct device *dev;
	u16 nregions;
	u16 get_feat_size;
	u16 set_feat_size;
};

/**
 * struct cxl_memdev_ecs_params - CXL memory DDR5 ECS parameter data structure.
 * @log_entry_type: ECS log entry type, per DRAM or per memory media FRU.
 * @threshold: ECS threshold count per GB of memory cells.
 * @mode:	codeword/row count mode
 *		0 : ECS counts rows with errors
 *		1 : ECS counts codeword with errors
 * @reset_counter: [IN] reset ECC counter to default value.
 */
struct cxl_memdev_ecs_params {
	u8 log_entry_type;
	u16 threshold;
	u8 mode;
	bool reset_counter;
};

enum {
	CXL_MEMDEV_ECS_PARAM_LOG_ENTRY_TYPE = 0,
	CXL_MEMDEV_ECS_PARAM_THRESHOLD,
	CXL_MEMDEV_ECS_PARAM_MODE,
	CXL_MEMDEV_ECS_PARAM_RESET_COUNTER,
};

#define	CXL_MEMDEV_ECS_LOG_ENTRY_TYPE_MASK	GENMASK(1, 0)
#define	CXL_MEMDEV_ECS_REALTIME_REPORT_CAP_MASK	BIT(0)
#define	CXL_MEMDEV_ECS_THRESHOLD_COUNT_MASK	GENMASK(2, 0)
#define	CXL_MEMDEV_ECS_MODE_MASK	BIT(3)
#define	CXL_MEMDEV_ECS_RESET_COUNTER_MASK	BIT(4)

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

enum {
	ECS_MODE_COUNTS_ROWS = 0,
	ECS_MODE_COUNTS_CODEWORDS = 1,
};

struct cxl_memdev_ecs_feat_read_attrbs {
	u8 ecs_log_cap;
	u8 ecs_cap;
	__le16 ecs_config;
	u8 ecs_flags;
}  __packed;

struct cxl_memdev_ecs_set_feat_pi {
	struct cxl_mbox_set_feat_in pi;
	struct cxl_memdev_ecs_feat_wr_attrbs {
		u8 ecs_log_cap;
		__le16 ecs_config;
	} __packed wr_attrbs[];
}  __packed;

/* CXL DDR5 ECS control functions */
static int cxl_mem_ecs_get_attrbs(struct device *dev, int fru_id,
				  struct cxl_memdev_ecs_params *params)
{
	struct cxl_memdev_ecs_feat_read_attrbs *rd_attrbs __free(kvfree) = NULL;
	struct cxl_memdev *cxlmd = to_cxl_memdev(dev->parent);
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlds);
	struct cxl_mbox_get_feat_in pi = {
		.uuid = cxl_ecs_uuid,
		.offset = 0,
		.selection = CXL_GET_FEAT_SEL_CURRENT_VALUE,
	};
	struct cxl_ecs_context *cxl_ecs_ctx;
	u8 threshold_index;
	int ret;

	if (!mds)
		return -EFAULT;
	cxl_ecs_ctx = dev_get_drvdata(dev);

	pi.count = cxl_ecs_ctx->get_feat_size;
	rd_attrbs = kvmalloc(pi.count, GFP_KERNEL);
	if (!rd_attrbs)
		return -ENOMEM;

	ret = cxl_get_feature(mds, &pi, rd_attrbs);
	if (ret) {
		params->log_entry_type = 0;
		params->threshold = 0;
		params->mode = 0;
		return ret;
	}
	params->log_entry_type = FIELD_GET(CXL_MEMDEV_ECS_LOG_ENTRY_TYPE_MASK,
					   rd_attrbs[fru_id].ecs_log_cap);
	threshold_index = FIELD_GET(CXL_MEMDEV_ECS_THRESHOLD_COUNT_MASK,
				    rd_attrbs[fru_id].ecs_config);
	params->threshold = ecs_supp_threshold[threshold_index];
	params->mode = FIELD_GET(CXL_MEMDEV_ECS_MODE_MASK,
				 rd_attrbs[fru_id].ecs_config);

	return 0;
}

static int __maybe_unused
cxl_mem_ecs_set_attrbs(struct device *dev, int fru_id,
		       struct cxl_memdev_ecs_params *params, u8 param_type)
{
	struct cxl_memdev_ecs_feat_read_attrbs *rd_attrbs __free(kvfree) = NULL;
	struct cxl_memdev_ecs_set_feat_pi *set_pi __free(kvfree) = NULL;
	struct cxl_memdev *cxlmd = to_cxl_memdev(dev->parent);
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlds);
	struct cxl_mbox_get_feat_in pi = {
		.uuid = cxl_ecs_uuid,
		.offset = 0,
		.selection = CXL_GET_FEAT_SEL_CURRENT_VALUE,
	};
	struct cxl_memdev_ecs_feat_wr_attrbs *wr_attrbs;
	struct cxl_memdev_ecs_params rd_params;
	struct cxl_ecs_context *cxl_ecs_ctx;
	u16 nmedia_frus, count;
	u32 set_pi_size;
	int ret;

	if (!mds)
		return -EFAULT;

	cxl_ecs_ctx = dev_get_drvdata(dev);
	nmedia_frus = cxl_ecs_ctx->nregions;

	rd_attrbs = kvmalloc(cxl_ecs_ctx->get_feat_size, GFP_KERNEL);
	if (!rd_attrbs)
		return -ENOMEM;

	pi.count = cxl_ecs_ctx->get_feat_size;
	ret = cxl_get_feature(mds, &pi, rd_attrbs);
	if (ret)
		return ret;
	set_pi_size = sizeof(struct cxl_mbox_set_feat_in) +
				cxl_ecs_ctx->set_feat_size;
	set_pi = kvmalloc(set_pi_size, GFP_KERNEL);
	if (!set_pi)
		return -ENOMEM;

	set_pi->pi.uuid = cxl_ecs_uuid;
	set_pi->pi.flags = CXL_SET_FEAT_FLAG_MOD_VALUE_SAVED_ACROSS_RESET |
				CXL_SET_FEAT_FLAG_FULL_DATA_TRANSFER;
	set_pi->pi.offset = 0;
	set_pi->pi.version = CXL_MEMDEV_ECS_SET_FEAT_VERSION;
	/* Fill writable attributes from the current attributes read for all the media FRUs */
	wr_attrbs = set_pi->wr_attrbs;
	for (count = 0; count < nmedia_frus; count++) {
		wr_attrbs[count].ecs_log_cap = rd_attrbs[count].ecs_log_cap;
		wr_attrbs[count].ecs_config = rd_attrbs[count].ecs_config;
	}

	/* Fill attribute to be set for the media FRU */
	switch (param_type) {
	case CXL_MEMDEV_ECS_PARAM_LOG_ENTRY_TYPE:
		if (params->log_entry_type != ECS_LOG_ENTRY_TYPE_DRAM &&
		    params->log_entry_type != ECS_LOG_ENTRY_TYPE_MEM_MEDIA_FRU) {
			dev_err(dev->parent,
				"Invalid CXL ECS scrub log entry type(%d) to set\n",
			       params->log_entry_type);
			dev_err(dev->parent,
				"Log Entry Type 0: per DRAM  1: per Memory Media FRU\n");
			return -EINVAL;
		}
		wr_attrbs[fru_id].ecs_log_cap = FIELD_PREP(CXL_MEMDEV_ECS_LOG_ENTRY_TYPE_MASK,
							   params->log_entry_type);
		break;
	case CXL_MEMDEV_ECS_PARAM_THRESHOLD:
		wr_attrbs[fru_id].ecs_config &= ~CXL_MEMDEV_ECS_THRESHOLD_COUNT_MASK;
		switch (params->threshold) {
		case 256:
			wr_attrbs[fru_id].ecs_config |= FIELD_PREP(
						CXL_MEMDEV_ECS_THRESHOLD_COUNT_MASK,
						ECS_THRESHOLD_256);
			break;
		case 1024:
			wr_attrbs[fru_id].ecs_config |= FIELD_PREP(
						CXL_MEMDEV_ECS_THRESHOLD_COUNT_MASK,
						ECS_THRESHOLD_1024);
			break;
		case 4096:
			wr_attrbs[fru_id].ecs_config |= FIELD_PREP(
						CXL_MEMDEV_ECS_THRESHOLD_COUNT_MASK,
						ECS_THRESHOLD_4096);
			break;
		default:
			dev_err(dev->parent,
				"Invalid CXL ECS scrub threshold count(%d) to set\n",
				params->threshold);
			dev_err(dev->parent,
				"Supported scrub threshold count: 256,1024,4096\n");
			return -EINVAL;
		}
		break;
	case CXL_MEMDEV_ECS_PARAM_MODE:
		if (params->mode != ECS_MODE_COUNTS_ROWS &&
		    params->mode != ECS_MODE_COUNTS_CODEWORDS) {
			dev_err(dev->parent,
				"Invalid CXL ECS scrub mode(%d) to set\n",
				params->mode);
			dev_err(dev->parent,
				"Mode 0: ECS counts rows with errors"
				" 1: ECS counts codewords with errors\n");
			return -EINVAL;
		}
		wr_attrbs[fru_id].ecs_config &= ~CXL_MEMDEV_ECS_MODE_MASK;
		wr_attrbs[fru_id].ecs_config |= FIELD_PREP(CXL_MEMDEV_ECS_MODE_MASK,
							  params->mode);
		break;
	case CXL_MEMDEV_ECS_PARAM_RESET_COUNTER:
		wr_attrbs[fru_id].ecs_config &= ~CXL_MEMDEV_ECS_RESET_COUNTER_MASK;
		wr_attrbs[fru_id].ecs_config |= FIELD_PREP(CXL_MEMDEV_ECS_RESET_COUNTER_MASK,
							   params->reset_counter);
		break;
	default:
		dev_err(dev->parent, "Invalid CXL ECS parameter to set\n");
		return -EINVAL;
	}
	ret = cxl_set_feature(mds, set_pi, set_pi_size);
	if (ret) {
		dev_err(dev->parent, "CXL ECS set feature fail ret=%d\n", ret);
		return ret;
	}

	/* Verify attribute is set successfully */
	ret = cxl_mem_ecs_get_attrbs(dev, fru_id, &rd_params);
	if (ret) {
		dev_err(dev->parent, "Get cxlmemdev ECS params fail ret=%d\n", ret);
		return ret;
	}
	switch (param_type) {
	case CXL_MEMDEV_ECS_PARAM_LOG_ENTRY_TYPE:
		if (rd_params.log_entry_type != params->log_entry_type)
			return -EFAULT;
		break;
	case CXL_MEMDEV_ECS_PARAM_THRESHOLD:
		if (rd_params.threshold != params->threshold)
			return -EFAULT;
		break;
	case CXL_MEMDEV_ECS_PARAM_MODE:
		if (rd_params.mode != params->mode)
			return -EFAULT;
		break;
	}

	return 0;
}

int cxl_mem_ddr5_ecs_init(struct cxl_memdev *cxlmd)
{
	struct cxl_mbox_supp_feat_entry feat_entry;
	struct cxl_ecs_context *cxl_ecs_ctx;
	int nmedia_frus;
	int ret;

	ret = cxl_mem_get_supported_feature_entry(cxlmd, &cxl_ecs_uuid, &feat_entry);
	if (ret < 0)
		return ret;

	if (!(feat_entry.attrb_flags & CXL_FEAT_ENTRY_FLAG_CHANGABLE))
		return -ENOTSUPP;
	nmedia_frus = feat_entry.get_feat_size/
				sizeof(struct cxl_memdev_ecs_feat_read_attrbs);
	if (nmedia_frus) {
		cxl_ecs_ctx = devm_kzalloc(&cxlmd->dev, sizeof(*cxl_ecs_ctx), GFP_KERNEL);
		if (!cxl_ecs_ctx)
			return -ENOMEM;

		cxl_ecs_ctx->nregions = nmedia_frus;
		cxl_ecs_ctx->get_feat_size = feat_entry.get_feat_size;
		cxl_ecs_ctx->set_feat_size = feat_entry.set_feat_size;
	}

	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_mem_ddr5_ecs_init, CXL);
