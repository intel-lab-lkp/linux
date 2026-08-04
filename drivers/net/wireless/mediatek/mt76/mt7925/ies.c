// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Copyright (C) 2026 MediaTek Inc. */

#include <linux/ieee80211.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>
#include "ies.h"

u8 mt7928_ht_nsts_from_ies(const struct cfg80211_bss_ies *ies)
{
	const struct ieee80211_ht_cap *ht_cap;
	const struct element *elem;
	u8 nsts = 0, i, max_tx_streams;

	elem = cfg80211_find_elem(WLAN_EID_HT_CAPABILITY,
				  ies->data, ies->len);
	if (!elem || elem->datalen < sizeof(*ht_cap))
		return 0;

	ht_cap = (const void *)elem->data;

	if ((ht_cap->mcs.tx_params & IEEE80211_HT_MCS_TX_RX_DIFF) &&
	    (ht_cap->mcs.tx_params & IEEE80211_HT_MCS_TX_DEFINED))
		max_tx_streams =
			((ht_cap->mcs.tx_params & IEEE80211_HT_MCS_TX_MAX_STREAMS_MASK)
				>> IEEE80211_HT_MCS_TX_MAX_STREAMS_SHIFT) + 1;
	else
		max_tx_streams = IEEE80211_HT_MCS_TX_MAX_STREAMS;

	for (i = 0; i < max_tx_streams; i++) {
		if (ht_cap->mcs.rx_mask[i])
			nsts = i + 1;
	}

	return nsts;
}

u8 mt7928_vht_nsts_from_ies(const struct cfg80211_bss_ies *ies)
{
	const struct ieee80211_vht_cap *vht_cap;
	const struct element *elem;
	u16 tx_mcs_map;
	u8 nsts = 0, i;

	elem = cfg80211_find_elem(WLAN_EID_VHT_CAPABILITY,
				  ies->data, ies->len);
	if (!elem || elem->datalen < sizeof(*vht_cap))
		return 0;

	vht_cap = (const void *)elem->data;
	tx_mcs_map = le16_to_cpu(vht_cap->supp_mcs.tx_mcs_map);

	for (i = 0; i < 8; i++) {
		if (((tx_mcs_map >> (2 * i)) & 0x3) != IEEE80211_VHT_MCS_NOT_SUPPORTED)
			nsts = i + 1;
	}

	return nsts;
}

u8 mt7928_he_nsts_from_ies(const struct cfg80211_bss_ies *ies)
{
	const struct ieee80211_he_cap_elem *he_cap;
	const struct ieee80211_he_mcs_nss_supp *mcs_nss;
	const struct element *elem;
	u8 mcs_nss_size;
	u16 tx_mcs_80;
	u8 nsts = 0, i;

	elem = cfg80211_find_ext_elem(WLAN_EID_EXT_HE_CAPABILITY,
				      ies->data, ies->len);

	if (!elem || elem->datalen < 1 + sizeof(*he_cap))
		return 0;

	he_cap = (const void *)(elem->data + 1);
	mcs_nss_size = ieee80211_he_mcs_nss_size(he_cap);

	if (elem->datalen < 1 + sizeof(*he_cap) + mcs_nss_size)
		return 0;

	mcs_nss = (const void *)(he_cap + 1);
	tx_mcs_80 = le16_to_cpu(mcs_nss->tx_mcs_80);

	for (i = 0; i < 8; i++) {
		if (((tx_mcs_80 >> (2 * i)) & 0x3) != IEEE80211_HE_MCS_NOT_SUPPORTED)
			nsts = i + 1;
	}

	return nsts;
}

u8 mt7928_eht_nsts_from_ies(const struct cfg80211_bss_ies *ies)
{
	const struct ieee80211_eht_cap_elem_fixed *eht_cap;
	const struct ieee80211_he_cap_elem *he_cap;
	const struct element *eht_elem, *he_elem;
	const u8 *mcs_nss_data;
	u8 mcs_nss_size;
	u8 nsts = 0, i;

	eht_elem = cfg80211_find_ext_elem(WLAN_EID_EXT_EHT_CAPABILITY,
					  ies->data, ies->len);
	if (!eht_elem || eht_elem->datalen < 1 + sizeof(*eht_cap))
		return 0;

	he_elem = cfg80211_find_ext_elem(WLAN_EID_EXT_HE_CAPABILITY,
					 ies->data, ies->len);
	if (!he_elem || he_elem->datalen < 1 + sizeof(*he_cap))
		return 0;

	eht_cap = (const void *)(eht_elem->data + 1);
	he_cap = (const void *)(he_elem->data + 1);

	mcs_nss_size = ieee80211_eht_mcs_nss_size(he_cap, eht_cap, true);
	if (eht_elem->datalen < 1 + sizeof(*eht_cap) + mcs_nss_size)
		return 0;

	mcs_nss_data = (const u8 *)(eht_cap + 1);

	for (i = 0; i < mcs_nss_size; i++)
		nsts = (mcs_nss_data[i] >> 4) & 0xf;

	return nsts;
}

void mt7928_get_bss_suswm_cap(const struct cfg80211_bss_ies *ies,
			      const struct ieee80211_link_sta *link_sta,
			      const struct ieee80211_supported_band *own_sband,
			      enum nl80211_iftype iftype,
			      struct mt7928_suswm_cap *cap)
{
	const struct ieee80211_sta_ht_cap *ht_cap = &link_sta->ht_cap;
	const struct ieee80211_sta_vht_cap *vht_cap = &link_sta->vht_cap;
	const struct ieee80211_sta_he_cap *he_cap = &link_sta->he_cap;
	const struct ieee80211_sta_eht_cap *eht_cap = &link_sta->eht_cap;
	const struct ieee80211_sta_vht_cap *own_vht_cap = &own_sband->vht_cap;
	const struct ieee80211_sta_he_cap *own_he_cap =
		ieee80211_get_he_iftype_cap(own_sband, iftype);
	const struct ieee80211_sta_eht_cap *own_eht_cap =
		ieee80211_get_eht_iftype_cap(own_sband, iftype);

	cap->bfer_en = false;
	cap->bfee_en = false;
	cap->nsts = 0;

	if (ht_cap->ht_supported)
		cap->nsts = mt7928_ht_nsts_from_ies(ies);

	if (vht_cap->vht_supported) {
		if (vht_cap->cap & IEEE80211_VHT_CAP_SU_BEAMFORMER_CAPABLE)
			cap->bfer_en = true;

		if (own_vht_cap->cap & IEEE80211_VHT_CAP_SU_BEAMFORMEE_CAPABLE)
			cap->bfee_en = true;

		cap->nsts = mt7928_vht_nsts_from_ies(ies);
	}

	if (he_cap->has_he && own_he_cap) {
		if (he_cap->he_cap_elem.phy_cap_info[3] &
		    IEEE80211_HE_PHY_CAP3_SU_BEAMFORMER)
			cap->bfer_en = true;

		if (own_he_cap->he_cap_elem.phy_cap_info[4] &
		    IEEE80211_HE_PHY_CAP4_SU_BEAMFORMEE)
			cap->bfee_en = true;

		cap->nsts = mt7928_he_nsts_from_ies(ies);
	}

	if (eht_cap->has_eht && own_eht_cap) {
		if (eht_cap->eht_cap_elem.phy_cap_info[0] &
		    IEEE80211_EHT_PHY_CAP0_SU_BEAMFORMER)
			cap->bfer_en = true;

		if (own_eht_cap->eht_cap_elem.phy_cap_info[0] &
		    IEEE80211_EHT_PHY_CAP0_SU_BEAMFORMEE)
			cap->bfee_en = true;

		cap->nsts = mt7928_eht_nsts_from_ies(ies);
	}
}
