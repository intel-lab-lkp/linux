// SPDX-License-Identifier: GPL-2.0-only
/*
 * EHT handling
 *
 * Copyright(c) 2021-2023 Intel Corporation
 */

#include "ieee80211_i.h"

static void
ieee80211_eht_override_peer_capabilities(u8 *he_info, u8 *eht_info,
					 const u8 *mcs_set)
{
	u8 offset_320mhz_set = 3;

	if (((*he_info) &
	     (IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G |
	      IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_80PLUS80_MHZ_IN_5G))) {
		/* for 160 MHz bandwidth, if none of the MCS-NSS set
		 * has a minimum NSS of 1 for both Rx and Tx, disable
		 * support for 160 MHz bandwidth by resetting
		 * corresponding flag bits of HE capabilities IE
		 */
		if (((mcs_set[3] & 0x0F) == 0x00 || (mcs_set[3] & 0xF0) == 0x00) &&
		    ((mcs_set[4] & 0x0F) == 0x00 || (mcs_set[4] & 0xF0) == 0x00) &&
		    ((mcs_set[5] & 0x0F) == 0x00 || (mcs_set[5] & 0xF0) == 0x00)) {
			(*he_info) &=
			    ~IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G;
			(*he_info) &=
			    ~IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_80PLUS80_MHZ_IN_5G;
		}
		offset_320mhz_set += 3;
	}

	/* for 320 MHz bandwidth, if none of the MCS-NSS set
	 * has a minimum NSS of 1 for both Rx and Tx, disable
	 * support for 320 MHz bandwidth by resetting
	 * corresponding flag bit of EHT capabilities IE
	 */
	if (((*eht_info) & IEEE80211_EHT_PHY_CAP0_320MHZ_IN_6GHZ) &&
	    ((mcs_set[offset_320mhz_set] & 0x0F) == 0x00 ||
	     (mcs_set[offset_320mhz_set] & 0xF0) == 0x00) &&
	    ((mcs_set[offset_320mhz_set + 1] & 0x0F) == 0x00 ||
	     (mcs_set[offset_320mhz_set + 1] & 0xF0) == 0x00) &&
	    ((mcs_set[offset_320mhz_set + 2] & 0x0F) == 0x00 ||
	     (mcs_set[offset_320mhz_set + 2] & 0xF0) == 0x00))
		(*eht_info) &= ~IEEE80211_EHT_PHY_CAP0_320MHZ_IN_6GHZ;
}

void
ieee80211_eht_cap_ie_to_sta_eht_cap(struct ieee80211_sub_if_data *sdata,
				    struct ieee80211_supported_band *sband,
				    const u8 *he_cap_ie, u8 he_cap_len,
				    const struct ieee80211_eht_cap_elem *eht_cap_ie_elem,
				    u8 eht_cap_len,
				    struct link_sta_info *link_sta)
{
	struct ieee80211_sta_eht_cap *eht_cap = &link_sta->pub->eht_cap;
	struct ieee80211_he_cap_elem *he_cap_ie_elem = (void *)he_cap_ie;
	u8 eht_ppe_size = 0;
	u8 mcs_nss_size;
	u8 eht_total_size = sizeof(eht_cap->eht_cap_elem);
	u8 *pos = (u8 *)eht_cap_ie_elem;

	memset(eht_cap, 0, sizeof(*eht_cap));

	if (!eht_cap_ie_elem ||
	    !ieee80211_get_eht_iftype_cap_vif(sband, &sdata->vif))
		return;

	mcs_nss_size = ieee80211_eht_mcs_nss_size(he_cap_ie_elem,
						  &eht_cap_ie_elem->fixed,
						  sdata->vif.type ==
							NL80211_IFTYPE_STATION);

	eht_total_size += mcs_nss_size;

	/* Calculate the PPE thresholds length only if the header is present */
	if (eht_cap_ie_elem->fixed.phy_cap_info[5] &
			IEEE80211_EHT_PHY_CAP5_PPE_THRESHOLD_PRESENT) {
		u16 eht_ppe_hdr;

		if (eht_cap_len < eht_total_size + sizeof(u16))
			return;

		eht_ppe_hdr = get_unaligned_le16(eht_cap_ie_elem->optional + mcs_nss_size);
		eht_ppe_size =
			ieee80211_eht_ppe_size(eht_ppe_hdr,
					       eht_cap_ie_elem->fixed.phy_cap_info);
		eht_total_size += eht_ppe_size;

		/* we calculate as if NSS > 8 are valid, but don't handle that */
		if (eht_ppe_size > sizeof(eht_cap->eht_ppe_thres))
			return;
	}

	if (eht_cap_len < eht_total_size)
		return;

	/* Copy the static portion of the EHT capabilities */
	memcpy(&eht_cap->eht_cap_elem, pos, sizeof(eht_cap->eht_cap_elem));
	pos += sizeof(eht_cap->eht_cap_elem);

	/* Copy MCS/NSS which depends on the peer capabilities */
	memset(&eht_cap->eht_mcs_nss_supp, 0,
	       sizeof(eht_cap->eht_mcs_nss_supp));
	memcpy(&eht_cap->eht_mcs_nss_supp, pos, mcs_nss_size);
	/* Override station bandwidth capabilities
	 * if bandwidth is unsupported in MCS-NSS set
	 */
	ieee80211_eht_override_peer_capabilities
			(&link_sta->pub->he_cap.he_cap_elem.phy_cap_info[0],
			 &eht_cap->eht_cap_elem.phy_cap_info[0],
			 eht_cap_ie_elem->optional);

	if (eht_ppe_size)
		memcpy(eht_cap->eht_ppe_thres,
		       &eht_cap_ie_elem->optional[mcs_nss_size],
		       eht_ppe_size);

	eht_cap->has_eht = true;

	link_sta->cur_max_bandwidth = ieee80211_sta_cap_rx_bw(link_sta);
	link_sta->pub->bandwidth = ieee80211_sta_cur_vht_bw(link_sta);
}
