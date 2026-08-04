/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (C) 2026 MediaTek Inc. */

#ifndef __MT7925_IES_H
#define __MT7925_IES_H

#include <net/cfg80211.h>
#include <net/mac80211.h>

#define MT_SU_SWM_BFER		BIT(0)
#define MT_SU_SWM_BFEE		BIT(1)
#define MT_SU_SWM_NSTS_MEET	BIT(2)

struct mt7928_suswm_cap {
	bool bfee_en;
	bool bfer_en;
	u8 nsts;
};

u8 mt7928_ht_nsts_from_ies(const struct cfg80211_bss_ies *ies);
u8 mt7928_vht_nsts_from_ies(const struct cfg80211_bss_ies *ies);
u8 mt7928_he_nsts_from_ies(const struct cfg80211_bss_ies *ies);
u8 mt7928_eht_nsts_from_ies(const struct cfg80211_bss_ies *ies);

void mt7928_get_bss_suswm_cap(const struct cfg80211_bss_ies *ies,
			      const struct ieee80211_link_sta *link_sta,
			      const struct ieee80211_supported_band *own_sband,
			      enum nl80211_iftype iftype,
			      struct mt7928_suswm_cap *cap);

#endif /* __MT7925_IES_H */
