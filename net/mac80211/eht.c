// SPDX-License-Identifier: GPL-2.0-only
/*
 * EHT handling
 *
 * Copyright(c) 2021-2025 Intel Corporation
 */

#include "driver-ops.h"
#include "ieee80211_i.h"

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

	if (eht_ppe_size)
		memcpy(eht_cap->eht_ppe_thres,
		       &eht_cap_ie_elem->optional[mcs_nss_size],
		       eht_ppe_size);

	eht_cap->has_eht = true;

	link_sta->cur_max_bandwidth = ieee80211_sta_cap_rx_bw(link_sta);
	link_sta->pub->bandwidth = ieee80211_sta_cur_vht_bw(link_sta);

	/*
	 * The MPDU length bits are reserved on all but 2.4 GHz and get set via
	 * VHT (5 GHz) or HE (6 GHz) capabilities.
	 */
	if (sband->band != NL80211_BAND_2GHZ)
		return;

	switch (u8_get_bits(eht_cap->eht_cap_elem.mac_cap_info[0],
			    IEEE80211_EHT_MAC_CAP0_MAX_MPDU_LEN_MASK)) {
	case IEEE80211_EHT_MAC_CAP0_MAX_MPDU_LEN_11454:
		link_sta->pub->agg.max_amsdu_len =
			IEEE80211_MAX_MPDU_LEN_VHT_11454;
		break;
	case IEEE80211_EHT_MAC_CAP0_MAX_MPDU_LEN_7991:
		link_sta->pub->agg.max_amsdu_len =
			IEEE80211_MAX_MPDU_LEN_VHT_7991;
		break;
	case IEEE80211_EHT_MAC_CAP0_MAX_MPDU_LEN_3895:
	default:
		link_sta->pub->agg.max_amsdu_len =
			IEEE80211_MAX_MPDU_LEN_VHT_3895;
		break;
	}

	ieee80211_sta_recalc_aggregates(&link_sta->sta->sta);
}

static void
ieee80211_send_eml_op_mode_notif(struct ieee80211_sub_if_data *sdata,
				 struct ieee80211_mgmt *req, u8 act_len)
{
	int hdr_len = offsetof(struct ieee80211_mgmt, u.action.u.eml_omn);
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_mgmt *mgmt;
	struct sk_buff *skb;

	skb = dev_alloc_skb(local->tx_headroom + hdr_len + act_len);
	if (!skb)
		return;

	skb_reserve(skb, local->tx_headroom);
	mgmt = skb_put_zero(skb, hdr_len);
	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_ACTION);
	memcpy(mgmt->da, req->sa, ETH_ALEN);
	memcpy(mgmt->sa, sdata->vif.addr, ETH_ALEN);
	memcpy(mgmt->bssid, sdata->vif.addr, ETH_ALEN);

	mgmt->u.action.category = WLAN_CATEGORY_PROTECTED_EHT;
	memcpy(&mgmt->u.action.u.eml_omn, &req->u.action.u.eml_omn, act_len);
	mgmt->u.action.u.eml_omn.control &= ~(IEEE80211_EML_CTRL_PARAM_UPDATE |
					      IEEE80211_EML_CTRL_INDEV_COEX_ACT);
	ieee80211_tx_skb(sdata, skb);
}

void ieee80211_rx_eml_op_mode_notif(struct ieee80211_sub_if_data *sdata,
				    struct sk_buff *skb)
{
	int hdr_len = offsetof(struct ieee80211_mgmt, u.action.u.eml_omn);
	enum nl80211_iftype type = ieee80211_vif_type_p2p(&sdata->vif);
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
	const struct wiphy_iftype_ext_capab *ift_ext_capa;
	struct ieee80211_mgmt *mgmt = (void *)skb->data;
	struct ieee80211_local *local = sdata->local;
	u8 control = mgmt->u.action.u.eml_omn.control;
	u8 *ptr = mgmt->u.action.u.eml_omn.variable;
	struct wiphy *wiphy = local->hw.wiphy;
	struct link_sta_info *link_sta;
	__le16 link_bitmap = 0;
	struct sta_info *sta;
	u8 act_len = 3; /* action_code + dialog_token + control */

	if (!ieee80211_vif_is_mld(&sdata->vif))
		return;

	ift_ext_capa = cfg80211_get_iftype_ext_capa(wiphy, type);
	if (!ift_ext_capa)
		return;

	if (!(ift_ext_capa->eml_capabilities & IEEE80211_EML_CAP_EMLSR_SUPP) &&
	    !(ift_ext_capa->eml_capabilities & IEEE80211_EML_CAP_EMLMR_SUPPORT))
		return;

	/* eMLSR and eMLMR can't be enabled at the same time */
	if ((control & IEEE80211_EML_CTRL_EMLSR_MODE) &&
	    (control & IEEE80211_EML_CTRL_EMLMR_MODE))
		return;

	if ((control & IEEE80211_EML_CTRL_EMLSR_MODE) ||
	    (control & IEEE80211_EML_CTRL_EMLMR_MODE)) {
		link_bitmap = get_unaligned((__le16 *)ptr);
		act_len += sizeof(__le16); /* eMLSR/eMLMR link_bitmap */
	}

	if (control & IEEE80211_EML_CTRL_EMLMR_MODE) {
		u8 mcs_map_count = ptr[3];

		if (mcs_map_count > 2)
			return;

		/* mcs_map_count_control and mcs_map_bw */
		act_len += 2 + mcs_map_count;
	}

	if (skb->len < hdr_len + act_len)
		return;

	if (!status->link_valid)
		return;

	sta = sta_info_get_bss(sdata, mgmt->sa);
	if (!sta)
		return;

	link_sta = wiphy_dereference(wiphy, sta->link[status->link_id]);
	if (!link_sta)
		return;

	if (drv_set_eml_op_mode(sdata, link_sta->pub, control, link_bitmap))
		return;

	ieee80211_send_eml_op_mode_notif(sdata, mgmt, act_len);
}
