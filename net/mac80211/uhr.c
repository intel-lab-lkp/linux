// SPDX-License-Identifier: GPL-2.0-only
/*
 * UHR handling
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include "ieee80211_i.h"

void
ieee80211_uhr_ie_to_sta_uhr(struct ieee80211_sub_if_data *sdata,
			    struct ieee80211_supported_band *sband,
			    const struct ieee80211_uhr_cap_elem *uhr_cap_elem,
			    u8 uhr_cap_len,
			    struct link_sta_info *link_sta)
{
	struct ieee80211_sta_uhr_cap *uhr_cap = &link_sta->pub->uhr_cap;
	u8 *pos = (u8 *)uhr_cap_elem;
	u8 uhr_total_size = sizeof(uhr_cap->uhr_cap_elem) -
		sizeof(struct ieee80211_dbe_cap);

	memset(uhr_cap, 0, sizeof(*uhr_cap));

	if (!uhr_cap_elem ||
	    !ieee80211_get_uhr_iftype_cap_vif(sband, &sdata->vif))
		return;

	if (uhr_cap_elem->fixed.mac_cap_info[1] &
	    IEEE80211_UHR_MAC_CAP1_DBE_SUPPORT) {
		uhr_total_size += 1;

		if (le64_to_cpu(uhr_cap_elem->fixed.dbe_cap.dbe_cap_param) &
		    IEEE80211_UHR_DBE_CAP_EHT_MCS_BW_160MHZ_PRES)
			uhr_total_size += 3;

		if (le64_to_cpu(uhr_cap_elem->fixed.dbe_cap.dbe_cap_param) &
		    IEEE80211_UHR_DBE_CAP_EHT_MCS_BW_320MHZ_PRES)
			uhr_total_size += 3;
	}

	if (uhr_cap_len < uhr_total_size)
		return;

	/* Copy the static portion of the UHR capabilities */
	memcpy(&uhr_cap->uhr_cap_elem, pos, sizeof(uhr_cap->uhr_cap_elem));

	uhr_cap->has_uhr = true;
}
