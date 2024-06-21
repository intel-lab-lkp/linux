// SPDX-License-Identifier: GPL-2.0-only
/*
 * NXP Wireless LAN device driver: 802.11h
 *
 * Copyright 2011-2024 NXP
 */

#include "main.h"
#include "cmdevt.h"
#include "fw.h"
#include "cfg80211.h"

void nxpwifi_init_11h_params(struct nxpwifi_private *priv)
{
	priv->state_11h.is_11h_enabled = true;
	priv->state_11h.is_11h_active = false;
}

inline int nxpwifi_is_11h_active(struct nxpwifi_private *priv)
{
	return priv->state_11h.is_11h_active;
}

/* This function appends 11h info to a buffer while joining an
 * infrastructure BSS
 */
static void
nxpwifi_11h_process_infra_join(struct nxpwifi_private *priv, u8 **buffer,
			       struct nxpwifi_bssdescriptor *bss_desc)
{
	struct nxpwifi_ie_types_header *ie_header;
	struct nxpwifi_ie_types_pwr_capability *cap;
	struct nxpwifi_ie_types_local_pwr_constraint *constraint;
	struct ieee80211_supported_band *sband;
	u8 radio_type;
	int i;

	if (!buffer || !(*buffer))
		return;

	radio_type = nxpwifi_band_to_radio_type((u8)bss_desc->bss_band);
	sband = priv->wdev.wiphy->bands[radio_type];

	cap = (struct nxpwifi_ie_types_pwr_capability *)*buffer;
	cap->header.type = cpu_to_le16(WLAN_EID_PWR_CAPABILITY);
	cap->header.len = cpu_to_le16(2);
	cap->min_pwr = 0;
	cap->max_pwr = 0;
	*buffer += sizeof(*cap);

	constraint = (struct nxpwifi_ie_types_local_pwr_constraint *)*buffer;
	constraint->header.type = cpu_to_le16(WLAN_EID_PWR_CONSTRAINT);
	constraint->header.len = cpu_to_le16(2);
	constraint->chan = bss_desc->channel;
	constraint->constraint = bss_desc->local_constraint;
	*buffer += sizeof(*constraint);

	ie_header = (struct nxpwifi_ie_types_header *)*buffer;
	ie_header->type = cpu_to_le16(TLV_TYPE_PASSTHROUGH);
	ie_header->len  = cpu_to_le16(2 * sband->n_channels + 2);
	*buffer += sizeof(*ie_header);
	*(*buffer)++ = WLAN_EID_SUPPORTED_CHANNELS;
	*(*buffer)++ = 2 * sband->n_channels;
	for (i = 0; i < sband->n_channels; i++) {
		u32 center_freq;

		center_freq = sband->channels[i].center_freq;
		*(*buffer)++ = ieee80211_frequency_to_channel(center_freq);
		*(*buffer)++ = 1; /* one channel in the subband */
	}
}

/* Enable or disable the 11h extensions in the firmware */
int nxpwifi_11h_activate(struct nxpwifi_private *priv, bool flag)
{
	u32 enable = flag;

	/* enable master mode radar detection on AP interface */
	if ((GET_BSS_ROLE(priv) == NXPWIFI_BSS_ROLE_UAP) && enable)
		enable |= NXPWIFI_MASTER_RADAR_DET_MASK;

	return nxpwifi_send_cmd(priv, HOST_CMD_802_11_SNMP_MIB,
				HOST_ACT_GEN_SET, DOT11H_I, &enable, true);
}

/* This functions processes TLV buffer for a pending BSS Join command.
 *
 * Activate 11h functionality in the firmware if the spectrum management
 * capability bit is found in the network we are joining. Also, necessary
 * TLVs are set based on requested network's 11h capability.
 */
void nxpwifi_11h_process_join(struct nxpwifi_private *priv, u8 **buffer,
			      struct nxpwifi_bssdescriptor *bss_desc)
{
	if (bss_desc->sensed_11h) {
		/* Activate 11h functions in firmware, turns on capability
		 * bit
		 */
		nxpwifi_11h_activate(priv, true);
		priv->state_11h.is_11h_active = true;
		bss_desc->cap_info_bitmap |= WLAN_CAPABILITY_SPECTRUM_MGMT;
		nxpwifi_11h_process_infra_join(priv, buffer, bss_desc);
	} else {
		/* Deactivate 11h functions in the firmware */
		nxpwifi_11h_activate(priv, false);
		priv->state_11h.is_11h_active = false;
		bss_desc->cap_info_bitmap &= ~WLAN_CAPABILITY_SPECTRUM_MGMT;
	}
}

/* This is DFS CAC work queue function.
 * This delayed work emits CAC finished event for cfg80211 if
 * CAC was started earlier.
 */
void nxpwifi_dfs_cac_work_queue(struct work_struct *work)
{
	struct cfg80211_chan_def chandef;
	struct delayed_work *delayed_work = to_delayed_work(work);
	struct nxpwifi_private *priv =
			container_of(delayed_work, struct nxpwifi_private,
				     dfs_cac_work);

	chandef = priv->dfs_chandef;
	if (priv->wdev.cac_started) {
		nxpwifi_dbg(priv->adapter, MSG,
			    "CAC timer finished; No radar detected\n");
		cfg80211_cac_event(priv->netdev, &chandef,
				   NL80211_RADAR_CAC_FINISHED,
				   GFP_KERNEL);
	}
}

static u8 nxpwifi_get_channel_2_offset(int chan)
{
	u8 chan2_offset = SEC_CHAN_NONE;

	switch (chan) {
	case 36:
	case 44:
	case 52:
	case 60:
	case 100:
	case 108:
	case 116:
	case 124:
	case 132:
	case 140:
	case 149:
	case 157:
	case 165:
	case 173:
		chan2_offset = SEC_CHAN_ABOVE;
		break;
	case 40:
	case 48:
	case 56:
	case 64:
	case 104:
	case 112:
	case 120:
	case 128:
	case 136:
	case 144:
	case 153:
	case 161:
	case 169:
	case 177:
		chan2_offset = SEC_CHAN_BELOW;
		break;
	}

	return chan2_offset;
}

static void nxpwifi_convert_chan_to_band_cfg(u8 *band_cfg,
					     struct cfg80211_chan_def *chan_def)
{
	u8 chan_band, chan_width, chan2_offset;

	switch (chan_def->chan->band) {
	case NL80211_BAND_2GHZ:
		chan_band = BAND_2GHZ;
		break;
	case NL80211_BAND_5GHZ:
		chan_band = BAND_5GHZ;
		break;
	default:
		break;
	}

	switch (chan_def->width) {
	case NL80211_CHAN_WIDTH_20_NOHT:
	case NL80211_CHAN_WIDTH_20:
		chan_width = CHAN_BW_20MHZ;
		break;
	case NL80211_CHAN_WIDTH_40:
		chan_width = CHAN_BW_40MHZ;
		if (chan_def->center_freq1 > chan_def->chan->center_freq)
			chan2_offset = SEC_CHAN_ABOVE;
		else
			chan2_offset = SEC_CHAN_BELOW;
		break;
	case NL80211_CHAN_WIDTH_80:
		chan2_offset =
			nxpwifi_get_channel_2_offset(chan_def->chan->hw_value);
		chan_width = CHAN_BW_80MHZ;
		break;
	case NL80211_CHAN_WIDTH_80P80:
	case NL80211_CHAN_WIDTH_160:
	default:
		break;
	}

	*band_cfg = ((chan2_offset << BAND_CFG_CHAN2_SHIFT_BIT) &
		     BAND_CFG_CHAN2_OFFSET_MASK) |
		    ((chan_width << BAND_CFG_CHAN_WIDTH_SHIFT_BIT) &
		     BAND_CFG_CHAN_WIDTH_MASK) |
		    ((chan_band << BAND_CFG_CHAN_BAND_SHIFT_BIT) &
		     BAND_CFG_CHAN_BAND_MASK);
}

/* This function prepares channel report request command to FW for
 * starting radar detection.
 */
int nxpwifi_cmd_issue_chan_report_request(struct nxpwifi_private *priv,
					  struct host_cmd_ds_command *cmd,
					  void *data_buf)
{
	struct host_cmd_ds_chan_rpt_req *cr_req = &cmd->params.chan_rpt_req;
	struct nxpwifi_radar_params *radar_params = (void *)data_buf;
	u16 size;

	cmd->command = cpu_to_le16(HOST_CMD_CHAN_REPORT_REQUEST);
	size = S_DS_GEN;

	cr_req->chan_desc.start_freq = cpu_to_le16(NXPWIFI_A_BAND_START_FREQ);
	nxpwifi_convert_chan_to_band_cfg(&cr_req->chan_desc.band_cfg,
					 radar_params->chandef);
	cr_req->chan_desc.chan_num = radar_params->chandef->chan->hw_value;
	cr_req->msec_dwell_time = cpu_to_le32(radar_params->cac_time_ms);
	size += sizeof(*cr_req);

	if (radar_params->cac_time_ms) {
		struct nxpwifi_ie_types_chan_rpt_data *rpt;

		rpt = (struct nxpwifi_ie_types_chan_rpt_data *)((u8 *)cmd + size);
		rpt->header.type = cpu_to_le16(TLV_TYPE_CHANRPT_11H_BASIC);
		rpt->header.len = cpu_to_le16(sizeof(u8));
		rpt->meas_rpt_map = 1 << MEAS_RPT_MAP_RADAR_SHIFT_BIT;
		size += sizeof(*rpt);

		nxpwifi_dbg(priv->adapter, MSG,
			    "11h: issuing DFS Radar check for channel=%d\n",
			    radar_params->chandef->chan->hw_value);
	} else {
		nxpwifi_dbg(priv->adapter, MSG, "cancelling CAC\n");
	}

	cmd->size = cpu_to_le16(size);

	return 0;
}

int nxpwifi_stop_radar_detection(struct nxpwifi_private *priv,
				 struct cfg80211_chan_def *chandef)
{
	struct nxpwifi_radar_params radar_params;

	memset(&radar_params, 0, sizeof(struct nxpwifi_radar_params));
	radar_params.chandef = chandef;
	radar_params.cac_time_ms = 0;

	return nxpwifi_send_cmd(priv, HOST_CMD_CHAN_REPORT_REQUEST,
				HOST_ACT_GEN_SET, 0, &radar_params, true);
}

/* This function is to abort ongoing CAC upon stopping AP operations
 * or during unload.
 */
void nxpwifi_abort_cac(struct nxpwifi_private *priv)
{
	if (priv->wdev.cac_started) {
		if (nxpwifi_stop_radar_detection(priv, &priv->dfs_chandef))
			nxpwifi_dbg(priv->adapter, ERROR,
				    "failed to stop CAC in FW\n");
		nxpwifi_dbg(priv->adapter, MSG,
			    "Aborting delayed work for CAC.\n");
		cancel_delayed_work_sync(&priv->dfs_cac_work);
		cfg80211_cac_event(priv->netdev, &priv->dfs_chandef,
				   NL80211_RADAR_CAC_ABORTED, GFP_KERNEL);
	}
}

/* This function handles channel report event from FW during CAC period.
 * If radar is detected during CAC, driver indicates the same to cfg80211
 * and also cancels ongoing delayed work.
 */
int nxpwifi_11h_handle_chanrpt_ready(struct nxpwifi_private *priv,
				     struct sk_buff *skb)
{
	struct host_cmd_ds_chan_rpt_event *rpt_event;
	struct nxpwifi_ie_types_chan_rpt_data *rpt;
	u16 event_len, tlv_len;

	rpt_event = (void *)(skb->data + sizeof(u32));
	event_len = skb->len - (sizeof(struct host_cmd_ds_chan_rpt_event) +
				sizeof(u32));

	if (le32_to_cpu(rpt_event->result) != HOST_RESULT_OK) {
		nxpwifi_dbg(priv->adapter, ERROR,
			    "Error in channel report event\n");
		return -1;
	}

	while (event_len >= sizeof(struct nxpwifi_ie_types_header)) {
		rpt = (void *)&rpt_event->tlvbuf;
		tlv_len = le16_to_cpu(rpt->header.len);

		switch (le16_to_cpu(rpt->header.type)) {
		case TLV_TYPE_CHANRPT_11H_BASIC:
			if (rpt->meas_rpt_map & MEAS_RPT_MAP_RADAR_MASK) {
				nxpwifi_dbg(priv->adapter, MSG,
					    "RADAR Detected on channel %d!\n",
					    priv->dfs_chandef.chan->hw_value);
				cancel_delayed_work_sync(&priv->dfs_cac_work);
				cfg80211_cac_event(priv->netdev,
						   &priv->dfs_chandef,
						   NL80211_RADAR_CAC_ABORTED,
						   GFP_KERNEL);
				cfg80211_radar_event(priv->adapter->wiphy,
						     &priv->dfs_chandef,
						     GFP_KERNEL);
			}
			break;
		default:
			break;
		}

		event_len -= (tlv_len + sizeof(rpt->header));
	}

	return 0;
}

/* Handler for radar detected event from FW.*/
int nxpwifi_11h_handle_radar_detected(struct nxpwifi_private *priv,
				      struct sk_buff *skb)
{
	struct nxpwifi_radar_det_event *rdr_event;

	rdr_event = (void *)(skb->data + sizeof(u32));

	nxpwifi_dbg(priv->adapter, MSG,
		    "radar detected; indicating kernel\n");
	if (priv->wdev.cac_started) {
		if (nxpwifi_stop_radar_detection(priv, &priv->dfs_chandef))
			nxpwifi_dbg(priv->adapter, ERROR,
				    "Failed to stop CAC in FW\n");
		cancel_delayed_work_sync(&priv->dfs_cac_work);
		cfg80211_cac_event(priv->netdev, &priv->dfs_chandef,
				   NL80211_RADAR_CAC_ABORTED, GFP_KERNEL);
	}
	cfg80211_radar_event(priv->adapter->wiphy, &priv->dfs_chandef,
			     GFP_KERNEL);
	nxpwifi_dbg(priv->adapter, MSG, "regdomain: %d\n",
		    rdr_event->reg_domain);
	nxpwifi_dbg(priv->adapter, MSG, "radar detection type: %d\n",
		    rdr_event->det_type);

	return 0;
}

/* This is work queue function for channel switch handling.
 * This function takes care of updating new channel definitin to
 * bss config structure, restart AP and indicate channel switch success
 * to cfg80211.
 */
void nxpwifi_dfs_chan_sw_work_queue(struct work_struct *work)
{
	struct nxpwifi_uap_bss_param *bss_cfg;
	struct delayed_work *delayed_work = to_delayed_work(work);
	struct nxpwifi_private *priv = container_of(delayed_work,
						    struct nxpwifi_private,
						    dfs_chan_sw_work);
	struct nxpwifi_adapter *adapter = priv->adapter;

	if (nxpwifi_del_mgmt_ies(priv))
		nxpwifi_dbg(priv->adapter, ERROR,
			    "Failed to delete mgmt IEs!\n");

	bss_cfg = &priv->bss_cfg;
	if (!bss_cfg->beacon_period) {
		nxpwifi_dbg(adapter, ERROR,
			    "channel switch: AP already stopped\n");
		return;
	}

	if (nxpwifi_send_cmd(priv, HOST_CMD_UAP_BSS_STOP,
			     HOST_ACT_GEN_SET, 0, NULL, true)) {
		nxpwifi_dbg(adapter, ERROR,
			    "channel switch: Failed to stop the BSS\n");
		return;
	}

	if (nxpwifi_cfg80211_change_beacon_data(adapter->wiphy, priv->netdev,
						&priv->beacon_after)) {
		nxpwifi_dbg(adapter, ERROR,
			    "channel switch: Failed to set beacon\n");
		return;
	}

	nxpwifi_uap_set_channel(priv, bss_cfg, priv->dfs_chandef);

	if (nxpwifi_config_start_uap(priv, bss_cfg)) {
		nxpwifi_dbg(adapter, ERROR,
			    "Failed to start AP after channel switch\n");
		return;
	}

	nxpwifi_dbg(adapter, MSG,
		    "indicating channel switch completion to kernel\n");
	wiphy_lock(priv->wdev.wiphy);
	cfg80211_ch_switch_notify(priv->netdev, &priv->dfs_chandef, 0);
	wiphy_unlock(priv->wdev.wiphy);

	if (priv->uap_stop_tx) {
		if (!netif_carrier_ok(priv->netdev))
			netif_carrier_on(priv->netdev);
		nxpwifi_wake_up_net_dev_queue(priv->netdev, adapter);
		priv->uap_stop_tx = false;
	}
}
