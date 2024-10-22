// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2024 Hisilicon Limited.

#include <linux/delay.h>
#include <drm/drm_device.h>
#include <drm/drm_print.h>
#include "dp_comm.h"
#include "dp_reg.h"
#include "dp_link.h"
#include "dp_aux.h"

const u8 link_rate_map[] = {DP_LINK_BW_1_62, DP_LINK_BW_2_7,
			    DP_LINK_BW_5_4, DP_LINK_BW_8_1};

static int dp_link_training_configure(struct dp_dev *dp)
{
	u8 buf[2];
	int ret;

	/* DP 2 lane */
	dp_write_bits(dp->base + DP_PHYIF_CTRL0, DP_CFG_LANE_DATA_EN,
		      dp->link.cap.lanes == DP_LANE_NUM_2 ? 0x3 : 0x1);
	dp_write_bits(dp->base + DP_DPTX_GCTL0, DP_CFG_PHY_LANE_NUM,
		      dp->link.cap.lanes == DP_LANE_NUM_2 ? 0x1 : 0);

	/* enhanced frame */
	dp_write_bits(dp->base + DP_VIDEO_CTRL, DP_CFG_STREAM_FRAME_MODE, 0x1);

	/* set rate and lane count */
	buf[0] = dp_get_link_rate(dp->link.cap.link_rate);
	buf[1] = DP_LANE_COUNT_ENHANCED_FRAME_EN | dp->link.cap.lanes;
	ret = drm_dp_dpcd_write(&dp->aux, DP_LINK_BW_SET, buf, sizeof(buf));
	if (ret != sizeof(buf)) {
		drm_err(dp->dev, "dp aux write link rate and lanes failed, ret: %d\n", ret);
		return ret;
	}

	/* set 8b/10b and downspread */
	buf[0] = 0x10;
	buf[1] = 0x1;
	ret = drm_dp_dpcd_write(&dp->aux, DP_DOWNSPREAD_CTRL, buf, sizeof(buf));
	if (ret != sizeof(buf))
		drm_err(dp->dev, "dp aux write 8b/10b and downspread failed, ret: %d\n", ret);

	ret = drm_dp_read_dpcd_caps(&dp->aux, dp->dpcd);
	if (ret)
		drm_err(dp->dev, "dp aux read dpcd failed, ret: %d\n", ret);

	return ret;
}

static int dp_link_pattern2dpcd(struct dp_dev *dp, enum dp_pattern_e pattern)
{
	switch (pattern) {
	case DP_PATTERN_NO:
		return DP_TRAINING_PATTERN_DISABLE;
	case DP_PATTERN_TPS1:
		return DP_TRAINING_PATTERN_1;
	case DP_PATTERN_TPS2:
		return DP_TRAINING_PATTERN_2;
	case DP_PATTERN_TPS3:
		return DP_TRAINING_PATTERN_3;
	case DP_PATTERN_TPS4:
		return DP_TRAINING_PATTERN_4;
	default:
		drm_err(dp->dev, "dp link unknown pattern %d\n", pattern);
		return -EINVAL;
	}
}

static int dp_link_set_pattern(struct dp_dev *dp, enum dp_pattern_e pattern)
{
	int ret;
	u8 buf;

	ret = dp_link_pattern2dpcd(dp, pattern);
	if (ret < 0)
		return ret;

	buf = (u8)ret;
	if (pattern != DP_TRAINING_PATTERN_DISABLE && pattern != DP_TRAINING_PATTERN_4) {
		buf |= DP_LINK_SCRAMBLING_DISABLE;
		dp_write_bits(dp->base + DP_PHYIF_CTRL0, DP_CFG_SCRAMBLE_EN, 0x1);
	} else {
		dp_write_bits(dp->base + DP_PHYIF_CTRL0, DP_CFG_SCRAMBLE_EN, 0);
	}

	dp_write_bits(dp->base + DP_PHYIF_CTRL0, DP_CFG_PAT_SEL, pattern);

	ret = drm_dp_dpcd_write(&dp->aux, DP_TRAINING_PATTERN_SET, &buf, sizeof(buf));
	if (ret != sizeof(buf))
		drm_err(dp->dev, "dp aux write training pattern set failed\n");

	return 0;
}

static int dp_link_training_cr_pre(struct dp_dev *dp)
{
	u8 *train_set = dp->link.train_set;
	int ret;
	u8 i;

	ret = dp_link_training_configure(dp);
	if (ret)
		return ret;

	ret = dp_link_set_pattern(dp, DP_PATTERN_TPS1);
	if (ret)
		return ret;

	for (i = 0; i < dp->link.cap.lanes; i++)
		train_set[i] = DP_TRAIN_VOLTAGE_SWING_LEVEL_2;

	ret = drm_dp_dpcd_write(&dp->aux, DP_TRAINING_LANE0_SET, train_set, dp->link.cap.lanes);
	if (ret != dp->link.cap.lanes)
		drm_err(dp->dev, "dp aux write training lane set failed\n");

	return 0;
}

static bool dp_link_get_adjust_train(struct dp_dev *dp, u8 lane_status[DP_LINK_STATUS_SIZE])
{
	u8 pre_emph[DP_LANE_NUM_MAX] = {0};
	u8 voltage[DP_LANE_NUM_MAX] = {0};
	bool changed = false;
	u8 train_set;
	u8 lane;

	/* not support level 3 */
	for (lane = 0; lane < dp->link.cap.lanes; lane++) {
		voltage[lane] = drm_dp_get_adjust_request_voltage(lane_status, lane);
		pre_emph[lane] = drm_dp_get_adjust_request_pre_emphasis(lane_status, lane);
	}

	for (lane = 0; lane < dp->link.cap.lanes; lane++) {
		train_set = voltage[lane] | pre_emph[lane];
		if (dp->link.train_set[lane] != train_set) {
			changed = true;
			dp->link.train_set[lane] = train_set;
		}
	}

	return changed;
}

u8 dp_get_link_rate(u8 index)
{
	return link_rate_map[index];
}

static int dp_link_reduce_rate(struct dp_dev *dp)
{
	if (dp->link.cap.link_rate > 0) {
		dp->link.cap.link_rate--;
		return 0;
	}

	drm_err(dp->dev, "dp link training reduce rate failed, already lowest rate\n");

	return -EFAULT;
}

static int dp_link_reduce_lane(struct dp_dev *dp)
{
	if (dp->link.cap.lanes == DP_LANE_NUM_1) {
		drm_err(dp->dev, "dp link training reduce lane failed, already reach minimum\n");
		return -EFAULT;
	}

	/* currently only 1 lane */
	dp->link.cap.lanes = DP_LANE_NUM_1;

	return 0;
}

static int dp_link_training_cr(struct dp_dev *dp)
{
	u8 lane_status[DP_LINK_STATUS_SIZE] = {0};
	bool level_changed;
	u32 voltage_tries;
	u32 cr_tries;
	u32 max_cr;
	int ret;

	/*
	 * DP 1.4 spec define 10 for maxtries value, for pre DP 1.4 version set a limit of 80
	 * (4 voltage levels x 4 preemphasis levels x 5 identical voltage retries)
	 */
	max_cr = dp->link.cap.rx_dpcd_revision >= DPCD_REVISION_14 ? 10 : 80;

	voltage_tries = 1;
	for (cr_tries = 0; cr_tries < max_cr; cr_tries++) {
		drm_dp_link_train_clock_recovery_delay(&dp->aux, dp->dpcd);

		ret = drm_dp_dpcd_read_link_status(&dp->aux, lane_status);
		if (ret != DP_LINK_STATUS_SIZE) {
			drm_err(dp->dev, "Get lane status failed\n");
			return ret;
		}

		if (drm_dp_clock_recovery_ok(lane_status, dp->link.cap.lanes)) {
			drm_info(dp->dev, "dp link training cr done\n");
			dp->link.status.clock_recovered = true;
			return 0;
		}

		if (voltage_tries == 5) {
			drm_info(dp->dev, "same voltage tries 5 times\n");
			dp->link.status.clock_recovered = false;
			return 0;
		}

		level_changed = dp_link_get_adjust_train(dp, lane_status);
		ret = drm_dp_dpcd_write(&dp->aux, DP_TRAINING_LANE0_SET, dp->link.train_set,
					dp->link.cap.lanes);
		if (ret != dp->link.cap.lanes) {
			drm_err(dp->dev, "Update link training failed\n");
			return ret;
		}

		voltage_tries = level_changed ? 1 : voltage_tries + 1;
	}

	drm_err(dp->dev, "dp link training clock recovery %u timers failed\n", max_cr);
	dp->link.status.clock_recovered = false;

	return 0;
}

static int dp_link_training_channel_eq(struct dp_dev *dp)
{
	u8 lane_status[DP_LINK_STATUS_SIZE] = {0};
	enum dp_pattern_e tps;
	u8 eq_tries;
	int ret;

	if (dp->link.cap.is_tps4)
		tps = DP_PATTERN_TPS4;
	else if (dp->link.cap.is_tps3)
		tps = DP_PATTERN_TPS3;
	else
		tps = DP_PATTERN_TPS2;

	ret = dp_link_set_pattern(dp, tps);
	if (ret)
		return ret;

	for (eq_tries = 0; eq_tries < EQ_MAX_RETRY; eq_tries++) {
		drm_dp_link_train_channel_eq_delay(&dp->aux, dp->dpcd);

		ret = drm_dp_dpcd_read_link_status(&dp->aux, lane_status);
		if (ret != DP_LINK_STATUS_SIZE) {
			drm_err(dp->dev, "get lane status failed\n");
			break;
		}

		if (!drm_dp_clock_recovery_ok(lane_status, dp->link.cap.lanes)) {
			drm_info(dp->dev, "clock recovery check failed\n");
			drm_info(dp->dev, "cannot continue channel equalization\n");
			dp->link.status.clock_recovered = false;
			break;
		}

		if (drm_dp_channel_eq_ok(lane_status, dp->link.cap.lanes)) {
			dp->link.status.channel_equalized = true;
			drm_info(dp->dev, "dp link training eq done\n");
			break;
		}

		dp_link_get_adjust_train(dp, lane_status);
		ret = drm_dp_dpcd_write(&dp->aux, DP_TRAINING_LANE0_SET,
					dp->link.train_set, dp->link.cap.lanes);
		if (ret != dp->link.cap.lanes) {
			drm_err(dp->dev, "Update link training failed\n");
			break;
		}
	}

	if (eq_tries == EQ_MAX_RETRY)
		drm_err(dp->dev, "channel equalization failed %u times\n", eq_tries);

	dp_link_set_pattern(dp, DP_PATTERN_NO);

	return ret < 0 ? ret : 0;
}

static int dp_link_downgrade_training_cr(struct dp_dev *dp)
{
	if (dp_link_reduce_rate(dp))
		return dp_link_reduce_lane(dp);

	return 0;
}

static int dp_link_downgrade_training_eq(struct dp_dev *dp)
{
	if ((dp->link.status.clock_recovered && !dp->link.status.channel_equalized)) {
		if (!dp_link_reduce_lane(dp))
			return 0;
	}

	return dp_link_reduce_rate(dp);
}

int dp_link_training(struct dp_dev *dp)
{
	struct hibmc_dp_link *link = &dp->link;
	int ret;

	while (true) {
		ret = dp_link_training_cr_pre(dp);
		if (ret)
			goto err;

		ret = dp_link_training_cr(dp);
		if (ret)
			goto err;

		if (!link->status.clock_recovered) {
			ret = dp_link_downgrade_training_cr(dp);
			if (ret)
				goto err;
			continue;
		}

		ret = dp_link_training_channel_eq(dp);
		if (ret)
			goto err;

		if (!link->status.channel_equalized) {
			ret = dp_link_downgrade_training_eq(dp);
			if (ret)
				goto err;
			continue;
		}

		return 0;
	}

err:
	dp_link_set_pattern(dp, DP_PATTERN_NO);

	return ret;
}
