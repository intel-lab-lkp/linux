// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2024 Hisilicon Limited.

#include <linux/delay.h>
#include <drm/drm_device.h>
#include <drm/drm_print.h>
#include "dp_comm.h"
#include "dp_reg.h"
#include "dp_link.h"
#include "dp_aux.h"

static int dp_link_training_configure(struct hibmc_dp_dev *dp)
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
	buf[0] = dp->link.cap.link_rate;
	buf[1] = DPCD_ENHANCED_FRAME_EN | dp->link.cap.lanes;
	ret = dp_aux_write(dp, DPCD_LINK_BW_SET, buf, sizeof(buf));
	if (ret) {
		drm_err(dp->dev, "dp aux write link rate and lanes failed, ret: %d\n", ret);
		return ret;
	}

	/* set 8b/10b and downspread */
	buf[0] = 0x10;
	buf[1] = 0x1;
	ret = dp_aux_write(dp, DPCD_DOWNSPREAD_CTRL, buf, sizeof(buf));
	if (ret)
		drm_err(dp->dev, "dp aux write 8b/10b and downspread failed, ret: %d\n", ret);

	return ret;
}

static int dp_link_pattern2dpcd(struct hibmc_dp_dev *dp, enum dp_pattern_e pattern)
{
	switch (pattern) {
	case DP_PATTERN_NO:
		return DPCD_TRAINING_PATTERN_DISABLE;
	case DP_PATTERN_TPS1:
		return DPCD_TRAINING_PATTERN_1;
	case DP_PATTERN_TPS2:
		return DPCD_TRAINING_PATTERN_2;
	case DP_PATTERN_TPS3:
		return DPCD_TRAINING_PATTERN_3;
	case DP_PATTERN_TPS4:
		return DPCD_TRAINING_PATTERN_4;
	default:
		drm_err(dp->dev, "dp link unknown pattern %d\n", pattern);
		return -EINVAL;
	}
}

static int dp_link_set_pattern(struct hibmc_dp_dev *dp, enum dp_pattern_e pattern)
{
	int ret;
	u8 buf;

	ret = dp_link_pattern2dpcd(dp, pattern);
	if (ret < 0)
		return ret;

	buf = (u8)ret;
	if (pattern != DPCD_TRAINING_PATTERN_DISABLE && pattern != DPCD_TRAINING_PATTERN_4) {
		buf |= DPCD_SCRAMBLING_DISABLE;
		dp_write_bits(dp->base + DP_PHYIF_CTRL0, DP_CFG_SCRAMBLE_EN, 0x1);
	} else {
		dp_write_bits(dp->base + DP_PHYIF_CTRL0, DP_CFG_SCRAMBLE_EN, 0);
	}

	dp_write_bits(dp->base + DP_PHYIF_CTRL0, DP_CFG_PAT_SEL, pattern);

	ret = dp_aux_write(dp, DPCD_TRAINING_PATTERN_SET, &buf, sizeof(buf));
	if (ret)
		drm_err(dp->dev, "dp aux write training pattern set failed\n");

	return ret;
}

static int dp_link_training_cr_pre(struct hibmc_dp_dev *dp)
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
		train_set[i] = DPCD_VOLTAGE_SWING_LEVEL_2 | DPCD_PRE_EMPHASIS_LEVEL_0;

	ret = dp_aux_write(dp, DPCD_TRAINING_LANE0_SET, train_set, dp->link.cap.lanes);
	if (ret)
		drm_err(dp->dev, "dp aux write training lane set failed\n");

	return ret;
}

static bool dp_dpcd_cr_done_check_and_update(u8 lane_status, u8 lane_count,
					     u8 *cr_done_lanes)
{
	bool is_ok = true;
	u8 val;

	*cr_done_lanes = GENMASK(lane_count - 1, 0);

	for (u8 lane = 0; lane < lane_count; lane++) {
		val = lane_status >> (lane * AUX_4_BIT);
		if ((val & DPCD_CR_DONE_BITS) == 0) {
			*cr_done_lanes &= ~(BIT(lane));
			is_ok = false;
		}
	}

	return is_ok;
}

static bool dp_dpcd_eq_is_ok(u8 lane_status, u8 lane_count)
{
	u8 val;

	for (u8 lane = 0; lane < lane_count; lane++) {
		val = (lane_status >> (lane * AUX_4_BIT));
		if ((val & DPCD_EQ_DONE_BITS) != DPCD_EQ_DONE_BITS)
			return false;
	}

	return true;
}

static bool dp_link_get_adjust_train(struct hibmc_dp_dev *dp, u8 lane_status)
{
	u8 pre_emph[DP_LANE_NUM_MAX] = {0};
	u8 voltage[DP_LANE_NUM_MAX] = {0};
	bool changed = false;
	u8 train_set;
	u8 lane;

	/* not support level 3 */
	for (lane = 0; lane < dp->link.cap.lanes; lane++) {
		voltage[lane] = (lane_status & (DPCD_VOLTAGE_SWING_LANE_0 << (AUX_4_BIT * lane)))
			  << DPCD_VOLTAGE_SWING_SET_S;
		pre_emph[lane] = (lane_status & (DPCD_PRE_EMPHASIS_LANE_0 << (AUX_4_BIT * lane)))
			   << DPCD_PRE_EMPHASIS_SET_S;
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

static int dp_link_reduce_rate(struct hibmc_dp_dev *dp)
{
	u8 link_rate_map[DP_LINK_RATE_NUM] = {DP_LINK_RATE_0, DP_LINK_RATE_1,
					      DP_LINK_RATE_2, DP_LINK_RATE_3};

	for (u8 i = 0; i < DP_LINK_RATE_NUM; i++) {
		if (link_rate_map[i] == dp->link.cap.link_rate) {
			if (i == 0) {
				drm_err(dp->dev, "dp link training reduce rate failed, already lowest rate\n");
				return -EFAULT;
			}
			dp->link.cap.link_rate = link_rate_map[i - 1];
			return 0;
		}
	}

	drm_err(dp->dev, "dp link training reduce rate failed, rate match failed\n");
	return -EFAULT;
}

static int dp_link_reduce_lane(struct hibmc_dp_dev *dp)
{
	/* currently only 1 lane */
	dp->link.cap.lanes = DP_LANE_NUM_1;

	return 0;
}

static int dp_link_training_cr(struct hibmc_dp_dev *dp)
{
	u8 lane_status[DP_LANE_STATUS_SIZE] = {0};
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
		msleep(50);

		ret = dp_aux_read(dp, DPCD_LANE0_1_STATUS, lane_status, DP_LANE_STATUS_SIZE);
		if (ret) {
			drm_err(dp->dev, "Get lane status failed\n");
			return ret;
		}

		ret = dp_dpcd_cr_done_check_and_update(lane_status[0], dp->link.cap.lanes,
						       &dp->link.status.cr_done_lanes);
		if (ret) {
			drm_info(dp->dev, "dp link training cr done\n");
			dp->link.status.clock_recovered = true;
			return 0;
		}

		if (voltage_tries == 5) {
			drm_info(dp->dev, "same voltage tries 5 times\n");
			dp->link.status.clock_recovered = false;
			return 0;
		}

		ret = dp_aux_read(dp, DPCD_ADJUST_REQUEST_LANE0_1, lane_status,
				  DP_LANE_STATUS_SIZE);
		if (ret) {
			drm_err(dp->dev, "Get adjust status failed\n");
			return ret;
		}

		level_changed = dp_link_get_adjust_train(dp, lane_status[0]);
		ret = dp_aux_write(dp, DPCD_TRAINING_LANE0_SET, dp->link.train_set,
				   dp->link.cap.lanes);
		if (ret) {
			drm_err(dp->dev, "Update link training failed\n");
			return ret;
		}

		voltage_tries = level_changed ? 1 : voltage_tries + 1;
	}

	drm_err(dp->dev, "dp link training clock recovery %u timers failed\n", max_cr);
	dp->link.status.clock_recovered = false;

	return 0;
}

static int dp_link_training_channel_eq(struct hibmc_dp_dev *dp)
{
	u8 lane_status[DP_LANE_STATUS_SIZE] = {0};
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
		msleep(50);

		ret = dp_aux_read(dp, DPCD_LANE0_1_STATUS, lane_status, DP_LANE_STATUS_SIZE);
		if (ret) {
			drm_err(dp->dev, "get lane status failed\n");
			break;
		}

		ret = dp_dpcd_cr_done_check_and_update(lane_status[0], dp->link.cap.lanes,
						       &dp->link.status.cr_done_lanes);
		if (!ret) {
			drm_info(dp->dev, "clock recovery check failed\n");
			drm_info(dp->dev, "cannot continue channel equalization\n");
			dp->link.status.clock_recovered = false;
			break;
		}

		ret = dp_dpcd_eq_is_ok(lane_status[0], dp->link.cap.lanes);
		if (ret) {
			dp->link.status.channel_equalized = true;
			drm_info(dp->dev, "dp link training eq done\n");
			break;
		}

		ret = dp_aux_read(dp, DPCD_ADJUST_REQUEST_LANE0_1,
				  lane_status, DP_LANE_STATUS_SIZE);
		if (ret) {
			drm_err(dp->dev, "Get adjust status failed\n");
			return ret;
		}

		dp_link_get_adjust_train(dp, lane_status[0]);

		ret = dp_aux_write(dp, DPCD_TRAINING_LANE0_SET,
				   dp->link.train_set, dp->link.cap.lanes);
		if (ret) {
			drm_err(dp->dev, "Update link training failed\n");
			break;
		}
	}

	if (eq_tries == EQ_MAX_RETRY)
		drm_err(dp->dev, "channel equalization failed %u times\n", eq_tries);

	dp_link_set_pattern(dp, DP_PATTERN_NO);

	return ret;
}

static int dp_link_downgrade_training_cr(struct hibmc_dp_dev *dp)
{
	if (dp_link_reduce_rate(dp))
		return dp_link_reduce_lane(dp);

	return 0;
}

static int dp_link_downgrade_training_eq(struct hibmc_dp_dev *dp)
{
	if ((!dp->link.status.clock_recovered && dp->link.status.cr_done_lanes != 0) ||
	    (dp->link.status.clock_recovered && !dp->link.status.channel_equalized)) {
		if (!dp_link_reduce_lane(dp))
			return 0;
	}

	return dp_link_reduce_rate(dp);
}

int dp_link_training(struct hibmc_dp_dev *dp)
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
