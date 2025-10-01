// SPDX-License-Identifier: MIT
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * Based on Nouveau DP code:
 * Copyright 2009 Red Hat Inc.
 */

#include <drm/drm_connector.h>
#include <drm/drm_print.h>
#include <drm/display/drm_dp_connector_helper.h>
#include <drm/display/drm_dp_helper.h>

static void drm_connector_dp_init_lttpr_caps(struct drm_connector *connector)
{
	struct drm_dp_aux *aux = connector->dp.aux;
	u8 *lttpr_caps = connector->dp.lttpr_caps;
	u8 dpcd[DP_RECEIVER_CAP_SIZE];
	int ret, nr;

	if (connector->dp.caps.forbid_lttpr_init)
		return;

	/*
	 * First access should be to the
	 * DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV,
	 * otherwise LTTPRs might be not initialized correctly.
	 */
	ret = drm_dp_dpcd_probe(aux, DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV);
	if (ret)
		goto err;

	ret = drm_dp_read_dpcd_caps(aux, dpcd);
	if (ret)
		goto err;

	ret = drm_dp_read_lttpr_common_caps(aux, dpcd, lttpr_caps);
	if (ret)
		goto err;

	/* FIXME: don't attempt switching LTTPR mode on active link */
	nr = drm_dp_lttpr_count(lttpr_caps);
	ret = drm_dp_lttpr_init(aux, nr);
	if (ret)
		goto err;

	connector->dp.lttpr_count = nr;

	return;

err:
	memset(lttpr_caps, 0, DP_LTTPR_COMMON_CAP_SIZE);
	connector->dp.lttpr_count = 0;
}

enum drm_connector_status drm_atomic_helper_connector_dp_detect(struct drm_connector *connector)
{
	struct drm_dp_aux *aux = connector->dp.aux;
	u8 *dpcd = connector->dp.dpcd;
	struct drm_dp_desc desc;
	int ret;

	drm_connector_dp_init_lttpr_caps(connector);

	ret = drm_dp_read_dpcd_caps(aux, dpcd);
	if (ret)
		return connector_status_disconnected;

	if (connector->connector_type == DRM_MODE_CONNECTOR_eDP) {
		u8 value;

		ret = drm_dp_dpcd_read_byte(aux, DP_EDP_DPCD_REV, &value);
		if (ret < 0)
			return connector_status_disconnected;

		connector->dp.edp = value;
	}

	ret = drm_dp_read_desc(aux, &desc, drm_dp_is_branch(dpcd));
	if (ret < 0)
		return connector_status_disconnected;

	if (drm_dp_read_sink_count_cap(connector, dpcd, &desc)) {
		ret = drm_dp_read_sink_count(aux);
		if (ret < 0)
			return connector_status_disconnected;

		/* No sink devices */
		if (!ret)
			return connector_status_disconnected;
	}

	return connector_status_connected;
}
EXPORT_SYMBOL(drm_atomic_helper_connector_dp_detect);

static bool drm_connector_dp_check_rate(struct drm_connector *connector,
					u32 rate)
{
	for (int j = 0; j < connector->dp.caps.num_supported_rates; j++)
		if (connector->dp.caps.supported_rates[j] == rate)
			return true;

	return false;
}

void drm_atomic_helper_connector_dp_hotplug(struct drm_connector *connector,
					    enum drm_connector_status status)
{
	struct drm_dp_aux *aux = connector->dp.aux;
	u8 *lttpr_caps = connector->dp.lttpr_caps;
	u8 *dpcd = connector->dp.dpcd;
	u32 lane_count;
	int ret;

	connector->dp.rate_count = 0;
	if (connector->connector_type == DRM_MODE_CONNECTOR_eDP &&
	    connector->dp.edp >= DP_EDP_14) {
		__le16 rates[DP_MAX_SUPPORTED_RATES];
		int num_rates;

		ret = drm_dp_dpcd_read_data(aux, DP_SUPPORTED_LINK_RATES,
					    rates, sizeof(rates));
		if (ret)
			rates[0] = 0;

		for (num_rates = 0;
		     num_rates < DP_MAX_SUPPORTED_RATES && rates[num_rates] != 0;
		     num_rates++)
			;

		for (int i = num_rates; i > 0; i--) {
			u32 rate = (le16_to_cpu(rates[i - 1]) * 200) / 10;

			if (!rate)
				break;

			if (!drm_connector_dp_check_rate(connector, rate))
				continue;

			connector->dp.rate[connector->dp.rate_count].dpcd = i - 1;
			connector->dp.rate[connector->dp.rate_count].rate = rate;
			connector->dp.rate_count++;
		}
	}

	if (!connector->dp.rate_count) {
		const u32 rates[] = { 810000, 540000, 270000, 162000 };
		u32 max_rate = dpcd[DP_MAX_LINK_RATE] * 27000;

		if (connector->dp.lttpr_count) {
			int rate = drm_dp_lttpr_max_link_rate(connector->dp.lttpr_caps);

			if (rate && rate < max_rate)
				max_rate = rate;
		}

		for (int i = 0; i < ARRAY_SIZE(rates); i++) {
			u32 rate = rates[i];

			if (rate > max_rate)
				continue;

			if (!drm_connector_dp_check_rate(connector, rate))
				continue;

			connector->dp.rate[connector->dp.rate_count].dpcd = -1;
			connector->dp.rate[connector->dp.rate_count].rate = rate;
			connector->dp.rate_count++;
		}
	}

	lane_count = dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK;
	if (connector->dp.lttpr_count) {
		unsigned int lttpr_lane_count = drm_dp_lttpr_max_lane_count(lttpr_caps);

		if (lttpr_lane_count)
			lane_count = min(lane_count, lttpr_lane_count);
	}

	connector->dp.dprx_lanes = lane_count;

}
EXPORT_SYMBOL(drm_atomic_helper_connector_dp_hotplug);
