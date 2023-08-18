// SPDX-License-Identifier: GPL-2.0
/*
 * Wifi Band Exclusion Interface for WWAN
 * Copyright (C) 2023 Advanced Micro Devices
 *
 */

#include <linux/wbrf.h>
#include <net/cfg80211.h>
#include "ieee80211_i.h"

void ieee80211_check_wbrf_support(struct ieee80211_local *local)
{
	struct wiphy *wiphy = local->hw.wiphy;
	struct device *dev;

	if (!wiphy)
		return;

	dev = wiphy->dev.parent;
	if (!dev)
		return;

	local->wbrf_supported = wbrf_supported_producer(dev);
	dev_dbg(dev, "WBRF is %s supported\n",
		local->wbrf_supported ? "" : "not");
}

static void get_chan_freq_boundary(u32 center_freq,
				   u32 bandwidth,
				   u64 *start,
				   u64 *end)
{
	bandwidth = MHZ_TO_KHZ(bandwidth);
	center_freq = MHZ_TO_KHZ(center_freq);

	*start = center_freq - bandwidth / 2;
	*end = center_freq + bandwidth / 2;

	/* Frequency in HZ is expected */
	*start = KHZ_TO_HZ(*start);
	*end = KHZ_TO_HZ(*end);
}

static void wbrf_get_ranges_from_chandef(struct cfg80211_chan_def *chandef,
					 struct wbrf_ranges_in *ranges_in)
{
	u64 start_freq1, end_freq1;
	u64 start_freq2, end_freq2;
	int bandwidth;

	bandwidth = nl80211_chan_width_to_mhz(chandef->width);

	get_chan_freq_boundary(chandef->center_freq1,
			       bandwidth,
			       &start_freq1,
			       &end_freq1);

	ranges_in->band_list[0].start = start_freq1;
	ranges_in->band_list[0].end = end_freq1;

	if (chandef->width == NL80211_CHAN_WIDTH_80P80) {
		get_chan_freq_boundary(chandef->center_freq2,
				       bandwidth,
				       &start_freq2,
				       &end_freq2);

		ranges_in->band_list[1].start = start_freq2;
		ranges_in->band_list[1].end = end_freq2;
	}
}

void ieee80211_add_wbrf(struct ieee80211_local *local,
			struct cfg80211_chan_def *chandef)
{
	struct wbrf_ranges_in ranges_in = {0};
	struct device *dev;

	if (!local->wbrf_supported)
		return;

	dev = local->hw.wiphy->dev.parent;

	wbrf_get_ranges_from_chandef(chandef, &ranges_in);

	wbrf_add_exclusion(dev, &ranges_in);
}

void ieee80211_remove_wbrf(struct ieee80211_local *local,
			   struct cfg80211_chan_def *chandef)
{
	struct wbrf_ranges_in ranges_in = {0};
	struct device *dev;

	if (!local->wbrf_supported)
		return;

	dev = local->hw.wiphy->dev.parent;

	wbrf_get_ranges_from_chandef(chandef, &ranges_in);

	wbrf_remove_exclusion(dev, &ranges_in);
}
