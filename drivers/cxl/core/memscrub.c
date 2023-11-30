// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cxl_memscrub.c - CXL memory scrub driver
 *
 * Copyright (c) 2023 HiSilicon Limited.
 *
 *  - Provides functions to configure patrol scrub
 *    feature of the CXL memory devices.
 */

#define pr_fmt(fmt)	"CXL_MEM_SCRUB: " fmt

#include <cxlmem.h>

/* CXL memory scrub feature common definitions */
#define CXL_SCRUB_MAX_ATTRB_RANGE_LENGTH	128

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

static int __maybe_unused
cxl_mem_ps_set_attrbs(struct device *dev, struct cxl_memdev_ps_params *params,
		      u8 param_type)
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

int cxl_mem_patrol_scrub_init(struct cxl_memdev *cxlmd)
{
	struct cxl_patrol_scrub_context *cxl_ps_ctx;
	struct cxl_mbox_supp_feat_entry feat_entry;
	struct cxl_memdev_ps_params params;
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

	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_mem_patrol_scrub_init, CXL);
