// SPDX-License-Identifier: GPL-2.0-only
/*
 * spectrum management
 *
 * Copyright 2003, Jouni Malinen <jkmaline@cc.hut.fi>
 * Copyright 2002-2005, Instant802 Networks, Inc.
 * Copyright 2005-2006, Devicescape Software, Inc.
 * Copyright 2006-2007  Jiri Benc <jbenc@suse.cz>
 * Copyright 2007, Michael Wu <flamingice@sourmilk.net>
 * Copyright 2007-2008, Intel Corporation
 * Copyright 2008, Johannes Berg <johannes@sipsolutions.net>
 * Copyright (C) 2018, 2020, 2022-2023 Intel Corporation
 */

#include <linux/ieee80211.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>
#include "ieee80211_i.h"
#include "sta_info.h"
#include "wme.h"

static inline void
op_class_to_6ghz_he_eht_oper(u8 op_class, struct ieee80211_channel *chan,
			     struct ieee80211_he_operation *he_oper,
			     struct ieee80211_eht_operation *eht_oper)
{
	u8 new_chan_width;
	u32 he_oper_params, center_freq1 = 0, center_freq2 = 0;
	struct ieee80211_he_6ghz_oper *he_6ghz_oper;
	struct ieee80211_eht_operation_info *eht_oper_info;

	new_chan_width = ieee80211_operating_class_to_chan_width(op_class);
	if (!ieee80211_operating_class_to_center_freq(op_class, chan,
						      &center_freq1,
						      &center_freq2)) {
		new_chan_width = NL80211_CHAN_WIDTH_20;
		center_freq1 = chan->center_freq;
	}

	he_oper_params =
		u32_encode_bits(1, IEEE80211_HE_OPERATION_6GHZ_OP_INFO);
	he_oper->he_oper_params = cpu_to_le32(he_oper_params);

	he_6ghz_oper = (struct ieee80211_he_6ghz_oper *)he_oper->optional;
	he_6ghz_oper->primary =
		ieee80211_frequency_to_channel(chan->center_freq);
	he_6ghz_oper->ccfs0 = ieee80211_frequency_to_channel(center_freq1);
	he_6ghz_oper->ccfs1 = center_freq2 ?
		ieee80211_frequency_to_channel(center_freq2) : 0;

	switch (new_chan_width) {
	case NL80211_CHAN_WIDTH_320:
		/* Cannot derive center frequency of 320 MHZ from op_class
		 * since it might be 320 Mhz-1 or 320 Mhz-2.
		 */
		WARN_ON(1);
		break;
	case NL80211_CHAN_WIDTH_160:
		he_6ghz_oper->ccfs1 = he_6ghz_oper->ccfs0;
		he_6ghz_oper->ccfs0 += chan->center_freq < center_freq1 ? -8 : 8;
		fallthrough;
	case NL80211_CHAN_WIDTH_80P80:
		he_6ghz_oper->control =
			IEEE80211_HE_6GHZ_OPER_CTRL_CHANWIDTH_160MHZ;
		break;
	case NL80211_CHAN_WIDTH_80:
		he_6ghz_oper->control =
			IEEE80211_HE_6GHZ_OPER_CTRL_CHANWIDTH_80MHZ;
		break;
	case NL80211_CHAN_WIDTH_40:
		he_6ghz_oper->control =
			IEEE80211_HE_6GHZ_OPER_CTRL_CHANWIDTH_40MHZ;
		break;
	default:
		he_6ghz_oper->control =
			IEEE80211_HE_6GHZ_OPER_CTRL_CHANWIDTH_20MHZ;
		break;
	}

	eht_oper->params = IEEE80211_EHT_OPER_INFO_PRESENT;

	eht_oper_info =
		(struct ieee80211_eht_operation_info *)eht_oper->optional;
	eht_oper_info->control = he_6ghz_oper->control;
	eht_oper_info->ccfs0 = he_6ghz_oper->ccfs0;
	eht_oper_info->ccfs1 = he_6ghz_oper->ccfs1;
}

static inline void
wbcs_ie_to_6ghz_he_eht_oper(const struct ieee80211_wide_bw_chansw_ie *wbcs_ie,
			    u8 new_chan_no,
			    struct ieee80211_he_operation *he_oper,
			    struct ieee80211_eht_operation *eht_oper)
{
	u32 he_oper_params;
	struct ieee80211_he_6ghz_oper *he_6ghz_oper;
	struct ieee80211_eht_operation_info *eht_oper_info;
	bool fallback_20mhz;
	u8 ccfs_diff;

	he_oper_params =
		u32_encode_bits(1, IEEE80211_HE_OPERATION_6GHZ_OP_INFO);
	he_oper->he_oper_params = cpu_to_le32(he_oper_params);

	he_6ghz_oper = (struct ieee80211_he_6ghz_oper *)he_oper->optional;
	he_6ghz_oper->primary = new_chan_no;

	/* The Wide Bandwidth Channel Switch IE in a 6 GHz BSS migth be
	 * deprecated VHT operation, VHT operation (IEEE 802.11-2020 9.4.2.160)
	 * or HE operation (IEEE P80211be D3.2 9.4.2.159).
	 * Check the combination of width, ccfs0 and ccfs1 to build the correct
	 * HE/EHT operation.
	 */
	he_6ghz_oper->ccfs0 = wbcs_ie->new_center_freq_seg0;
	he_6ghz_oper->ccfs1 = wbcs_ie->new_center_freq_seg1;
	switch (wbcs_ie->new_channel_width) {
	case 0:
		/* Must be [deprecated] VHT operation with 40 MHz bandwidth */
		he_6ghz_oper->control =
			IEEE80211_HE_6GHZ_OPER_CTRL_CHANWIDTH_40MHZ;
		break;
	case 1:
		if (he_6ghz_oper->ccfs1) {
			/* VHT operation with 160/80P80 MHz bandwidth */
			he_6ghz_oper->control =
				IEEE80211_HE_6GHZ_OPER_CTRL_CHANWIDTH_160MHZ;
		} else if ((he_6ghz_oper->ccfs0 - 7) % 16 == 0) {
			/* [deprecated] VHT operation with 80 MHz bandwidth */
			he_6ghz_oper->control =
				IEEE80211_HE_6GHZ_OPER_CTRL_CHANWIDTH_80MHZ;
		} else {
			/* HE operation with 40 MHz bandwidth */
			he_6ghz_oper->control =
				IEEE80211_HE_6GHZ_OPER_CTRL_CHANWIDTH_40MHZ;
		}
		break;
	case 2:
		if ((he_6ghz_oper->ccfs0 - 15) % 32 == 0) {
			/* deprecated VHT operation with 160 MHz bandwidth */
			he_6ghz_oper->control =
				IEEE80211_HE_6GHZ_OPER_CTRL_CHANWIDTH_160MHZ;
			he_6ghz_oper->ccfs1 = he_6ghz_oper->ccfs0;
			he_6ghz_oper->ccfs0 +=
				new_chan_no < he_6ghz_oper->ccfs0 ? -8 : 8;
		} else {
			/* HE operation with 80 MHz bandwidth */
			he_6ghz_oper->control =
				IEEE80211_HE_6GHZ_OPER_CTRL_CHANWIDTH_80MHZ;
		}
		break;
	case 3:
		/* Can be
		 * 1. deprecated VHT operation with 80P80 MHz bandwidth
		 * 2. HE operation with 160/80P80 MHz bandwidth
		 */
		he_6ghz_oper->control =
			IEEE80211_HE_6GHZ_OPER_CTRL_CHANWIDTH_160MHZ;
		break;
	case 4:
		/* 320 MHz bandwidth
		 * TODO channel switch to 320 MHz bandwidth should be indiated
		 * by Bandwidth Indication IE (IEEE P80211be D3.2 9.4.2.159)
		 */
		he_6ghz_oper->control = IEEE80211_EHT_OPER_CHAN_WIDTH_320MHZ;
		break;
	default:
		/* Ignore invalid width */
		break;
	}

	/* Validate the relationship between new channel width and center frequency
	 * segments, and fallback to 20 MHz if the relationship is wrong.
	 */
	fallback_20mhz = false;
	switch (he_6ghz_oper->control) {
	case IEEE80211_HE_6GHZ_OPER_CTRL_CHANWIDTH_40MHZ:
		if ((he_6ghz_oper->ccfs0 - 3) % 8 != 0)
			fallback_20mhz = true;
		break;
	case IEEE80211_HE_6GHZ_OPER_CTRL_CHANWIDTH_80MHZ:
		if ((he_6ghz_oper->ccfs0 - 7) % 16 != 0)
			fallback_20mhz = true;
		break;
	case IEEE80211_HE_6GHZ_OPER_CTRL_CHANWIDTH_160MHZ:
		ccfs_diff = abs(he_6ghz_oper->ccfs1 - he_6ghz_oper->ccfs0);
		if ((ccfs_diff == 8 && (he_6ghz_oper->ccfs1 - 15) % 32 != 0) ||
		    (ccfs_diff > 16 && ((he_6ghz_oper->ccfs0 - 7) % 16 != 0 ||
		    (he_6ghz_oper->ccfs1 - 7) % 16 != 0)))
			fallback_20mhz = true;
		break;
	case IEEE80211_EHT_OPER_CHAN_WIDTH_320MHZ:
		if ((he_6ghz_oper->ccfs1 - 31) % 32 != 0)
			fallback_20mhz = true;
		break;
	}

	if (fallback_20mhz) {
		he_6ghz_oper->control =
			IEEE80211_HE_6GHZ_OPER_CTRL_CHANWIDTH_20MHZ;
		he_6ghz_oper->ccfs0 = he_6ghz_oper->primary;
		he_6ghz_oper->ccfs1 = 0;
	}

	eht_oper->params = IEEE80211_EHT_OPER_INFO_PRESENT;
	eht_oper_info =
		(struct ieee80211_eht_operation_info *)eht_oper->optional;
	eht_oper_info->control = he_6ghz_oper->control;
	eht_oper_info->ccfs0 = he_6ghz_oper->ccfs0;
	eht_oper_info->ccfs1 = he_6ghz_oper->ccfs1;
}

static inline void
op_class_to_ht_vht_oper(u8 op_class, struct ieee80211_channel *chan,
			struct ieee80211_ht_operation *ht_oper,
			struct ieee80211_vht_operation *vht_oper)
{
	u8 new_chan_width;
	u32 center_freq1 = 0, center_freq2 = 0;

	new_chan_width = ieee80211_operating_class_to_chan_width(op_class);
	if (!ieee80211_operating_class_to_center_freq(op_class, chan,
						      &center_freq1,
						      &center_freq2)) {
		new_chan_width = NL80211_CHAN_WIDTH_20;
		center_freq1 = chan->center_freq;
	}

	vht_oper->center_freq_seg0_idx =
		ieee80211_frequency_to_channel(center_freq1);
	vht_oper->center_freq_seg1_idx = center_freq2 ?
		ieee80211_frequency_to_channel(center_freq2) : 0;

	ht_oper->ht_param = (chan->center_freq / 20) & 1 ?
				IEEE80211_HT_PARAM_CHA_SEC_ABOVE :
				IEEE80211_HT_PARAM_CHA_SEC_BELOW;

	switch (new_chan_width) {
	case NL80211_CHAN_WIDTH_320:
		WARN_ON(1);
		break;
	case NL80211_CHAN_WIDTH_160:
		vht_oper->chan_width = IEEE80211_VHT_CHANWIDTH_80MHZ;
		vht_oper->center_freq_seg1_idx = vht_oper->center_freq_seg0_idx;
		vht_oper->center_freq_seg0_idx +=
			chan->center_freq < center_freq1 ? -8 : 8;
		break;
	case NL80211_CHAN_WIDTH_80P80:
		vht_oper->chan_width = IEEE80211_VHT_CHANWIDTH_80MHZ;
		break;
	case NL80211_CHAN_WIDTH_80:
		vht_oper->chan_width = IEEE80211_VHT_CHANWIDTH_80MHZ;
		break;
	default:
		vht_oper->chan_width = IEEE80211_VHT_CHANWIDTH_USE_HT;
		if (chan->center_freq != center_freq1)
			ht_oper->ht_param = chan->center_freq > center_freq1 ?
				IEEE80211_HT_PARAM_CHA_SEC_BELOW :
				IEEE80211_HT_PARAM_CHA_SEC_ABOVE;
		else
			ht_oper->ht_param = IEEE80211_HT_PARAM_CHA_SEC_NONE;
	}

	ht_oper->operation_mode =
		cpu_to_le16(vht_oper->center_freq_seg1_idx <<
				IEEE80211_HT_OP_MODE_CCFS2_SHIFT);
}

static inline void
wbcs_ie_to_ht_vht_oper(struct ieee80211_channel *chan,
		       const struct ieee80211_wide_bw_chansw_ie *wbcs_ie,
		       struct ieee80211_ht_operation *ht_oper,
		       struct ieee80211_vht_operation *vht_oper)
{
	u8 new_chan_width, new_ccfs0, new_ccfs1;
	bool fallback_20mhz;

	new_chan_width = wbcs_ie->new_channel_width;
	new_ccfs0 = wbcs_ie->new_center_freq_seg0;
	new_ccfs1 = wbcs_ie->new_center_freq_seg1;

	/* Validate the relationship between new channel width and center frequency
	 * segments, and fallback to 20 MHz if the relationship is wrong.
	 */
	fallback_20mhz = false;
	switch (new_chan_width) {
	case IEEE80211_VHT_CHANWIDTH_USE_HT:
		/* If the wide bandwidth channel switch IE is presented,
		 * the new channel width is at least 40 MHz.
		 */
		if (!new_ccfs1) {
			new_ccfs0 -= (new_ccfs0 >= 149) ? 151 : 38;
			if (new_ccfs0 % 8 != 0)
				fallback_20mhz = true;
		} else {
			fallback_20mhz = true;
		}
		break;
	case IEEE80211_VHT_CHANWIDTH_80MHZ:
		if (!new_ccfs1) {
			new_ccfs0 -= (new_ccfs0 >= 149) ? 155 : 42;
			if (new_ccfs0 % 16 != 0)
				fallback_20mhz = true;
			break;
		} else if (abs(new_ccfs1 - new_ccfs0) == 8) {
			new_ccfs0 = new_ccfs1;
			new_ccfs1 = 0;
		}
		fallthrough;
	case IEEE80211_VHT_CHANWIDTH_160MHZ:
		if (!new_ccfs1) {
			if (new_ccfs0 != 50 && new_ccfs0 != 114 && new_ccfs0 != 163)
				fallback_20mhz = true;
			break;
		}
		fallthrough;
	case IEEE80211_VHT_CHANWIDTH_80P80MHZ:
		new_ccfs0 -= (new_ccfs0 >= 149) ? 155 : 42;
		new_ccfs1 -= (new_ccfs1 >= 149) ? 155 : 42;
		if (new_ccfs0 % 16 != 0 || new_ccfs1 % 16 != 0)
			fallback_20mhz = true;
		break;
	default:
		fallback_20mhz = true;
	}

	if (fallback_20mhz) {
		ht_oper->ht_param = IEEE80211_HT_PARAM_CHA_SEC_NONE;

		vht_oper->chan_width = IEEE80211_VHT_CHANWIDTH_USE_HT;
		vht_oper->center_freq_seg0_idx =
			ieee80211_frequency_to_channel(chan->center_freq);
		vht_oper->center_freq_seg1_idx = 0;

	} else {
		ht_oper->ht_param = (chan->center_freq / 20) & 1 ?
					IEEE80211_HT_PARAM_CHA_SEC_ABOVE :
					IEEE80211_HT_PARAM_CHA_SEC_BELOW;

		vht_oper->chan_width = new_chan_width;
		vht_oper->center_freq_seg0_idx = wbcs_ie->new_center_freq_seg0;
		vht_oper->center_freq_seg1_idx = wbcs_ie->new_center_freq_seg1;
	}

	ht_oper->operation_mode = cpu_to_le16(vht_oper->center_freq_seg1_idx <<
					      IEEE80211_HT_OP_MODE_CCFS2_SHIFT);
}

int ieee80211_parse_ch_switch_ie(struct ieee80211_sub_if_data *sdata,
				 struct ieee802_11_elems *elems,
				 enum nl80211_band current_band,
				 u32 vht_cap_info,
				 ieee80211_conn_flags_t conn_flags, u8 *bssid,
				 struct ieee80211_csa_ie *csa_ie)
{
	struct ieee80211_supported_band *sband;
	enum nl80211_band new_band = current_band;
	int new_freq, size;
	u8 new_chan_no = 0, new_op_class = 0;
	struct ieee80211_channel *new_chan;
	struct cfg80211_chan_def new_chandef = {};
	const struct ieee80211_sec_chan_offs_ie *sec_chan_offs;
	const struct ieee80211_wide_bw_chansw_ie *wide_bw_chansw_ie;
	const struct ieee80211_bandwidth_indication *bwi;
	const struct ieee80211_ext_chansw_ie *ext_chansw_ie;
	struct ieee80211_ht_operation *ht_oper;
	struct ieee80211_vht_operation *vht_oper;
	struct ieee80211_he_operation *he_oper;
	struct ieee80211_eht_operation *eht_oper;
	struct ieee80211_sta_ht_cap sta_ht_cap;
	int secondary_channel_offset = -1;

	memset(csa_ie, 0, sizeof(*csa_ie));

	sec_chan_offs = elems->sec_chan_offs;
	wide_bw_chansw_ie = elems->wide_bw_chansw_ie;
	bwi = elems->bandwidth_indication;
	ext_chansw_ie = elems->ext_chansw_ie;

	if (conn_flags & (IEEE80211_CONN_DISABLE_HT |
			  IEEE80211_CONN_DISABLE_40MHZ)) {
		sec_chan_offs = NULL;
		wide_bw_chansw_ie = NULL;
	}

	if (ext_chansw_ie) {
		new_op_class = ext_chansw_ie->new_operating_class;
		if (!ieee80211_operating_class_to_band(new_op_class, &new_band)) {
			new_op_class = 0;
			sdata_info(sdata, "cannot understand ECSA IE "
					  "operating class, %d, ignoring\n",
				   ext_chansw_ie->new_operating_class);
		} else {
			new_chan_no = ext_chansw_ie->new_ch_num;
			csa_ie->count = ext_chansw_ie->count;
			csa_ie->mode = ext_chansw_ie->mode;
		}
	}

	if (!new_op_class && elems->ch_switch_ie) {
		new_chan_no = elems->ch_switch_ie->new_ch_num;
		csa_ie->count = elems->ch_switch_ie->count;
		csa_ie->mode = elems->ch_switch_ie->mode;
	}

	/* nothing here we understand */
	if (!new_chan_no)
		return 1;

	/* Mesh Channel Switch Parameters Element */
	if (elems->mesh_chansw_params_ie) {
		csa_ie->ttl = elems->mesh_chansw_params_ie->mesh_ttl;
		csa_ie->mode = elems->mesh_chansw_params_ie->mesh_flags;
		csa_ie->pre_value = le16_to_cpu(
				elems->mesh_chansw_params_ie->mesh_pre_value);

		if (elems->mesh_chansw_params_ie->mesh_flags &
				WLAN_EID_CHAN_SWITCH_PARAM_REASON)
			csa_ie->reason_code = le16_to_cpu(
				elems->mesh_chansw_params_ie->mesh_reason);
	}

	new_freq = ieee80211_channel_to_frequency(new_chan_no, new_band);
	new_chan = ieee80211_get_channel(sdata->local->hw.wiphy, new_freq);
	if (!new_chan || new_chan->flags & IEEE80211_CHAN_DISABLED) {
		sdata_info(sdata,
			   "BSS %pM switches to unsupported channel (%d MHz), disconnecting\n",
			   bssid, new_freq);
		return -EINVAL;
	}

	if (sec_chan_offs) {
		secondary_channel_offset = sec_chan_offs->sec_chan_offs;
	} else if (!(conn_flags & IEEE80211_CONN_DISABLE_HT)) {
		/* If the secondary channel offset IE is not present,
		 * we can't know what's the post-CSA offset, so the
		 * best we can do is use 20MHz.
		*/
		secondary_channel_offset = IEEE80211_HT_PARAM_CHA_SEC_NONE;
	}

	switch (secondary_channel_offset) {
	default:
		/* secondary_channel_offset was present but is invalid */
	case IEEE80211_HT_PARAM_CHA_SEC_NONE:
		cfg80211_chandef_create(&csa_ie->chandef, new_chan,
					NL80211_CHAN_HT20);
		break;
	case IEEE80211_HT_PARAM_CHA_SEC_ABOVE:
		cfg80211_chandef_create(&csa_ie->chandef, new_chan,
					NL80211_CHAN_HT40PLUS);
		break;
	case IEEE80211_HT_PARAM_CHA_SEC_BELOW:
		cfg80211_chandef_create(&csa_ie->chandef, new_chan,
					NL80211_CHAN_HT40MINUS);
		break;
	case -1:
		cfg80211_chandef_create(&csa_ie->chandef, new_chan,
					NL80211_CHAN_NO_HT);
		/* keep width for 5/10 MHz channels */
		switch (sdata->vif.bss_conf.chandef.width) {
		case NL80211_CHAN_WIDTH_5:
		case NL80211_CHAN_WIDTH_10:
			csa_ie->chandef.width =
				sdata->vif.bss_conf.chandef.width;
			break;
		default:
			break;
		}
		break;
	}

	if (bwi) {
		/* start with the CSA one */
		new_chandef = csa_ie->chandef;
		/* and update the width accordingly */
		/* FIXME: support 160/320 */
		ieee80211_chandef_eht_oper(&bwi->info, true, true,
					   &new_chandef);
	} else if (new_band == NL80211_BAND_6GHZ) {
		size = sizeof(struct ieee80211_he_operation) +
		       sizeof(struct ieee80211_he_6ghz_oper);
		he_oper = kzalloc(size, GFP_KERNEL);
		if (!he_oper)
			return -ENOMEM;

		size = sizeof(struct ieee80211_eht_operation) +
		       sizeof(struct ieee80211_eht_operation_info);
		eht_oper = kzalloc(size, GFP_KERNEL);
		if (!eht_oper) {
			kfree(he_oper);
			return -ENOMEM;
		}

		if (new_op_class && new_op_class != 135 && new_op_class != 137) {
			/* There is no way to tell the ccfs1 for op_class 135
			 * (80P80 MHz) and 137 (320 MHz).
			 */
			op_class_to_6ghz_he_eht_oper(new_op_class, new_chan,
						     he_oper, eht_oper);
		} else if (wide_bw_chansw_ie) {
			wbcs_ie_to_6ghz_he_eht_oper(wide_bw_chansw_ie,
						    new_chan_no, he_oper,
						    eht_oper);
		}

		new_chandef = csa_ie->chandef;

		/* ignore if parsing fails */
		if (!ieee80211_chandef_he_6ghz_oper(sdata, he_oper, eht_oper,
						    &new_chandef))
			new_chandef.chan = NULL;

		kfree(he_oper);
		kfree(eht_oper);
	} else {
		sband = sdata->local->hw.wiphy->bands[new_band];
		memcpy(&sta_ht_cap, &sband->ht_cap, sizeof(sta_ht_cap));
		ieee80211_apply_htcap_overrides(sdata, &sta_ht_cap);

		if (!sta_ht_cap.ht_supported ||
		    !(sta_ht_cap.cap & IEEE80211_HT_CAP_SUP_WIDTH_20_40))
			goto out;

		ht_oper = kzalloc(sizeof(*ht_oper), GFP_KERNEL);
		if (!ht_oper)
			return -ENOMEM;

		vht_oper = kzalloc(sizeof(*vht_oper), GFP_KERNEL);
		if (!vht_oper) {
			kfree(ht_oper);
			return -ENOMEM;
		}

		if (new_op_class && new_op_class != 130) {
			/* There is no way to tell the ccfs1 for op_class 130
			 * (80P80 MHz).
			 */
			op_class_to_ht_vht_oper(new_op_class, new_chan, ht_oper,
						vht_oper);
		} else if (wide_bw_chansw_ie && new_band == NL80211_BAND_5GHZ &&
			   sband->vht_cap.vht_supported) {
			/* It is assumed that there is no WBCS IE in the beacon
			 * from a 2 GHz BSS during a channel switch.
			 */
			wbcs_ie_to_ht_vht_oper(new_chan, wide_bw_chansw_ie,
					       ht_oper, vht_oper);
		} else {
			kfree(ht_oper);
			kfree(vht_oper);
			goto out;
		}

		new_chandef = csa_ie->chandef;

		ieee80211_chandef_ht_oper(ht_oper, &new_chandef);

		/* ignore if parsing fails */
		if (sband->vht_cap.vht_supported &&
		    !ieee80211_chandef_vht_oper(&sdata->local->hw, vht_cap_info,
						vht_oper, ht_oper, &new_chandef))
			new_chandef.chan = NULL;

		kfree(ht_oper);
		kfree(vht_oper);
	}

	if (new_chandef.chan) {
		if (conn_flags & IEEE80211_CONN_DISABLE_320MHZ &&
		    new_chandef.width == NL80211_CHAN_WIDTH_320)
			ieee80211_chandef_downgrade(&new_chandef);

		if (conn_flags & IEEE80211_CONN_DISABLE_80P80MHZ &&
		    new_chandef.width == NL80211_CHAN_WIDTH_80P80)
			ieee80211_chandef_downgrade(&new_chandef);

		if (conn_flags & IEEE80211_CONN_DISABLE_160MHZ &&
		    new_chandef.width == NL80211_CHAN_WIDTH_160)
			ieee80211_chandef_downgrade(&new_chandef);

		if (!cfg80211_chandef_compatible(&new_chandef,
						 &csa_ie->chandef)) {
			sdata_info(sdata,
				   "BSS %pM: CSA has inconsistent channel data, disconnecting\n",
				   bssid);
			return -EINVAL;
		}

		csa_ie->chandef = new_chandef;
	}

out:
	if (elems->max_channel_switch_time)
		csa_ie->max_switch_time =
			(elems->max_channel_switch_time[0] << 0) |
			(elems->max_channel_switch_time[1] <<  8) |
			(elems->max_channel_switch_time[2] << 16);

	return 0;
}

static void ieee80211_send_refuse_measurement_request(struct ieee80211_sub_if_data *sdata,
					struct ieee80211_msrment_ie *request_ie,
					const u8 *da, const u8 *bssid,
					u8 dialog_token)
{
	struct ieee80211_local *local = sdata->local;
	struct sk_buff *skb;
	struct ieee80211_mgmt *msr_report;

	skb = dev_alloc_skb(sizeof(*msr_report) + local->hw.extra_tx_headroom +
				sizeof(struct ieee80211_msrment_ie));
	if (!skb)
		return;

	skb_reserve(skb, local->hw.extra_tx_headroom);
	msr_report = skb_put_zero(skb, 24);
	memcpy(msr_report->da, da, ETH_ALEN);
	memcpy(msr_report->sa, sdata->vif.addr, ETH_ALEN);
	memcpy(msr_report->bssid, bssid, ETH_ALEN);
	msr_report->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
						IEEE80211_STYPE_ACTION);

	skb_put(skb, 1 + sizeof(msr_report->u.action.u.measurement));
	msr_report->u.action.category = WLAN_CATEGORY_SPECTRUM_MGMT;
	msr_report->u.action.u.measurement.action_code =
				WLAN_ACTION_SPCT_MSR_RPRT;
	msr_report->u.action.u.measurement.dialog_token = dialog_token;

	msr_report->u.action.u.measurement.element_id = WLAN_EID_MEASURE_REPORT;
	msr_report->u.action.u.measurement.length =
			sizeof(struct ieee80211_msrment_ie);

	memset(&msr_report->u.action.u.measurement.msr_elem, 0,
		sizeof(struct ieee80211_msrment_ie));
	msr_report->u.action.u.measurement.msr_elem.token = request_ie->token;
	msr_report->u.action.u.measurement.msr_elem.mode |=
			IEEE80211_SPCT_MSR_RPRT_MODE_REFUSED;
	msr_report->u.action.u.measurement.msr_elem.type = request_ie->type;

	ieee80211_tx_skb(sdata, skb);
}

void ieee80211_process_measurement_req(struct ieee80211_sub_if_data *sdata,
				       struct ieee80211_mgmt *mgmt,
				       size_t len)
{
	/*
	 * Ignoring measurement request is spec violation.
	 * Mandatory measurements must be reported optional
	 * measurements might be refused or reported incapable
	 * For now just refuse
	 * TODO: Answer basic measurement as unmeasured
	 */
	ieee80211_send_refuse_measurement_request(sdata,
			&mgmt->u.action.u.measurement.msr_elem,
			mgmt->sa, mgmt->bssid,
			mgmt->u.action.u.measurement.dialog_token);
}
