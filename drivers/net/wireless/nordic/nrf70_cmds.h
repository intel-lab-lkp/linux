/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025 Conclusive Engineering Sp. z o. o.
 */

#ifndef _NRF70_CMDS_H
#define _NRF70_CMDS_H

#include <linux/if_ether.h>
#include <net/cfg80211.h>

enum nrf70_sys_cmds {
	NRF70_CMD_INIT,
	NRF70_CMD_TX,
	NRF70_CMD_IF_TYPE,
	NRF70_CMD_MODE,
	NRF70_CMD_GET_STATS,
	NRF70_CMD_CLEAR_STATS,
	NRF70_CMD_RX,
	NRF70_CMD_PWR,
	NRF70_CMD_DEINIT,
	NRF70_CMD_BTCOEX,
	NRF70_CMD_RF_TEST,
	NRF70_CMD_HE_GI_LTF_CONFIG,
	NRF70_CMD_UMAC_INT_STATS,
	NRF70_CMD_RADIO_TEST_INIT,
	NRF70_CMD_RT_REQ_SET_REG,
	NRF70_CMD_TX_FIX_DATA_RATE,
	NRF70_CMD_CHANNEL,
	NRF70_CMD_RAW_CONFIG_MODE,
	NRF70_CMD_RAW_CONFIG_FILTER,
	NRF70_CMD_RAW_TX_PKT,
	NRF70_CMD_RESET_STATISTICS,
	NRF70_CMD_MAX
};

/* Data commands and events share the same enums. */
enum nrf70_data_cmds {
	NRF70_CMD_MGMT_BUFF_CONFIG,
	NRF70_CMD_TX_BUFF,
	NRF70_CMD_TX_BUFF_DONE,
	NRF70_CMD_RX_BUFF,
	NRF70_CMD_CARRIER_ON,
	NRF70_CMD_CARRIER_OFF,
	NRF70_CMD_PM_MODE,
	NRF70_CMD_PS_GET_FRAMES,
};

enum nrf70_umac_cmds {
	NRF70_UMAC_CMD_TRIGGER_SCAN,
	NRF70_UMAC_CMD_GET_SCAN_RESULTS,
	NRF70_UMAC_CMD_AUTHENTICATE,
	NRF70_UMAC_CMD_ASSOCIATE,
	NRF70_UMAC_CMD_DEAUTHENTICATE,
	NRF70_UMAC_CMD_SET_WIPHY,
	NRF70_UMAC_CMD_NEW_KEY,
	NRF70_UMAC_CMD_DEL_KEY,
	NRF70_UMAC_CMD_SET_KEY,
	NRF70_UMAC_CMD_GET_KEY,
	NRF70_UMAC_CMD_NEW_BEACON,
	NRF70_UMAC_CMD_SET_BEACON,
	NRF70_UMAC_CMD_SET_BSS,
	NRF70_UMAC_CMD_START_AP,
	NRF70_UMAC_CMD_STOP_AP,
	NRF70_UMAC_CMD_NEW_INTERFACE,
	NRF70_UMAC_CMD_SET_INTERFACE,
	NRF70_UMAC_CMD_DEL_INTERFACE,
	NRF70_UMAC_CMD_SET_IFFLAGS,
	NRF70_UMAC_CMD_NEW_STATION,
	NRF70_UMAC_CMD_DEL_STATION,
	NRF70_UMAC_CMD_SET_STATION,
	NRF70_UMAC_CMD_GET_STATION,
	NRF70_UMAC_CMD_START_P2P_DEVICE,
	NRF70_UMAC_CMD_STOP_P2P_DEVICE,
	NRF70_UMAC_CMD_REMAIN_ON_CHANNEL,
	NRF70_UMAC_CMD_CANCEL_REMAIN_ON_CHANNEL,
	NRF70_UMAC_CMD_SET_CHANNEL,
	NRF70_UMAC_CMD_RADAR_DETECT,
	NRF70_UMAC_CMD_REGISTER_FRAME,
	NRF70_UMAC_CMD_FRAME,
	NRF70_UMAC_CMD_JOIN_IBSS,
	NRF70_UMAC_CMD_WIN_STA_CONNECT,
	NRF70_UMAC_CMD_SET_POWER_SAVE,
	NRF70_UMAC_CMD_SET_WOWLAN,
	NRF70_UMAC_CMD_SUSPEND,
	NRF70_UMAC_CMD_RESUME,
	NRF70_UMAC_CMD_SET_QOS_MAP,
	NRF70_UMAC_CMD_GET_CHANNEL,
	NRF70_UMAC_CMD_GET_TX_POWER,
	NRF70_UMAC_CMD_GET_INTERFACE,
	NRF70_UMAC_CMD_GET_WIPHY,
	NRF70_UMAC_CMD_GET_IFHWADDR,
	NRF70_UMAC_CMD_SET_IFHWADDR,
	NRF70_UMAC_CMD_GET_REG,
	NRF70_UMAC_CMD_SET_REG,
	NRF70_UMAC_CMD_REQ_SET_REG,
	NRF70_UMAC_CMD_CONFIG_UAPSD,
	NRF70_UMAC_CMD_CONFIG_TWT,
	NRF70_UMAC_CMD_TEARDOWN_TWT,
	NRF70_UMAC_CMD_ABORT_SCAN,
	NRF70_UMAC_CMD_MCAST_FILTER,
	NRF70_UMAC_CMD_CHANGE_MACADDR,
	NRF70_UMAC_CMD_SET_POWER_SAVE_TIMEOUT,
	NRF70_UMAC_CMD_GET_CONNECTION_INFO,
	NRF70_UMAC_CMD_GET_POWER_SAVE_INFO,
	NRF70_UMAC_CMD_SET_LISTEN_INTERVAL,
	NRF70_UMAC_CMD_CONFIG_EXTENDED_PS,
	NRF70_UMAC_CMD_CONFIG_QUIET_PERIOD,
};

enum nrf70_sys_events {
	NRF70_EVENT_PWR_DATA,
	NRF70_EVENT_INIT_DONE,
	NRF70_EVENT_STATS,
	NRF70_EVENT_DEINIT_DONE,
	NRF70_EVENT_RF_TEST,
	NRF70_EVENT_COEX_CONFIG,
	NRF70_EVENT_INT_UMAC_STATS,
	NRF70_EVENT_RADIOCMD_STATUS,
	NRF70_EVENT_CHANNEL_SET_DONE,
	NRF70_EVENT_MODE_SET_DONE,
	NRF70_EVENT_FILTER_SET_DONE,
	NRF70_EVENT_RAW_TX_DONE,
};

enum nrf70_umac_events {
	NRF70_UMAC_EVENT_UNSPECIFIED = 256,
	NRF70_UMAC_EVENT_TRIGGER_SCAN_START,
	NRF70_UMAC_EVENT_SCAN_ABORTED,
	NRF70_UMAC_EVENT_SCAN_DONE,
	NRF70_UMAC_EVENT_SCAN_RESULT,
	NRF70_UMAC_EVENT_AUTHENTICATE,
	NRF70_UMAC_EVENT_ASSOCIATE,
	NRF70_UMAC_EVENT_CONNECT,
	NRF70_UMAC_EVENT_DEAUTHENTICATE,
	NRF70_UMAC_EVENT_DISASSOCIATE,
	NRF70_UMAC_EVENT_NEW_STATION,
	NRF70_UMAC_EVENT_DEL_STATION,
	NRF70_UMAC_EVENT_GET_STATION,
	NRF70_UMAC_EVENT_REMAIN_ON_CHANNEL,
	NRF70_UMAC_EVENT_CANCEL_REMAIN_ON_CHANNEL,
	NRF70_UMAC_EVENT_DISCONNECT,
	NRF70_UMAC_EVENT_FRAME,
	NRF70_UMAC_EVENT_COOKIE_RESP,
	NRF70_UMAC_EVENT_FRAME_TX_STATUS,
	NRF70_UMAC_EVENT_IFFLAGS_STATUS,
	NRF70_UMAC_EVENT_GET_TX_POWER,
	NRF70_UMAC_EVENT_GET_CHANNEL,
	NRF70_UMAC_EVENT_SET_INTERFACE,
	NRF70_UMAC_EVENT_UNPROT_DEAUTHENTICATE,
	NRF70_UMAC_EVENT_UNPROT_DISASSOCIATE,
	NRF70_UMAC_EVENT_NEW_INTERFACE,
	NRF70_UMAC_EVENT_NEW_WIPHY,
	NRF70_UMAC_EVENT_GET_IFHWADDR,
	NRF70_UMAC_EVENT_GET_REG,
	NRF70_UMAC_EVENT_SET_REG,
	NRF70_UMAC_EVENT_REQ_SET_REG,
	NRF70_UMAC_EVENT_GET_KEY,
	NRF70_UMAC_EVENT_BEACON_HINT,
	NRF70_UMAC_EVENT_REG_CHANGE,
	NRF70_UMAC_EVENT_WIPHY_REG_CHANGE,
	NRF70_UMAC_EVENT_SCAN_DISPLAY_RESULT,
	NRF70_UMAC_EVENT_CMD_STATUS,
	NRF70_UMAC_EVENT_BSS_INFO,
	NRF70_UMAC_EVENT_CONFIG_TWT,
	NRF70_UMAC_EVENT_TEARDOWN_TWT,
	NRF70_UMAC_EVENT_TWT_SLEEP,
	NRF70_UMAC_EVENT_COALESCING,
	NRF70_UMAC_EVENT_MCAST_FILTER,
	NRF70_UMAC_EVENT_GET_CONNECTION_INFO,
	NRF70_UMAC_EVENT_GET_POWER_SAVE_INFO
};

#define	NRF70_MSG_SYSTEM		0
#define	NRF70_MSG_DATA			2
#define	NRF70_MSG_UMAC			3

struct __packed nrf70_msg {
	u32 len;
	u32 resubmit;
	u32 type;
	u8 data[];
};

struct __packed nrf70_header {
	u32 id;
	u32 len;
};

#define	NRF70_UMAC_ID_WDEV		BIT(0)
struct __packed nrf70_umac_header {
	u32 portid;	/* unused */
	u32 seq;	/* used only for EVENT_SCAN_DISPLAY_RESULT */
	u32 id;
	s32 ret_val;	/* unused */
	struct __packed {
		u32 valid_fields;
		s32 iface_id;	/* unused */
		s32 wiphy_id;	/* unused */
		u64 wdev_id;
	} idx;
};

#define NRF70_PHY_BLOB_SZ	113
struct __packed nrf70_rf_params {
	u8 reserved[6];
	u8 xo_freq_offset;
	struct __packed {
		s8 lb_chan;
		s8 hb_low_chan;
		s8 hb_mid_chan;
		s8 hb_high_chan;
	} pd_adjust; /* unused */
	struct __packed {
		s8 lb_chan;
		s8 hb_low_chan;
		s8 hb_mid_chan;
		s8 hb_high_chan;
	} tx_pwr_syst_offset_dbm;
	struct __packed {
		s8 dsss;
		s8 lb_mcs7;
		s8 lb_mcs0;
		s8 hb_low_chan_mcs7;
		s8 hb_mid_chan_mcs7;
		s8 hb_high_chan_mcs7;
		s8 hb_low_chan_mcs0;
		s8 hb_mid_chan_mcs0;
		s8 hb_high_chan_mcs0;
	} max_tx_pwr_ceil;
	struct __packed {
		s8 lb_chan;
		s8 hb_low_chan;
		s8 hb_mid_chan;
		s8 hb_high_chan;
	} rx_gain_offset;
	struct __packed {
		s8 max_chip_temp_C;
		s8 min_chip_temp_C;
		s8 lb_hi_temp_max_pwr_backoff;
		s8 lb_low_temp_max_pwr_backoff;
		s8 hb_hi_temp_max_pwr_backoff;
		s8 hb_low_temp_max_pwr_backoff;
		s8 lb_vbat_lt_vlow;
		s8 hb_vbat_lt_vlow;
		s8 lb_vbat_lt_low;
		s8 hb_vbat_lt_low;
		s8 reserved[4];
	} temp_volt_backoffs;
	struct __packed {
		u8 blob[NRF70_PHY_BLOB_SZ]; /* Undocumented params. */
		u8 band_2g_lower_edge_backoff_dsss;
		u8 band_2g_lower_edge_backoff_ht;
		u8 band_2g_lower_edge_backoff_he;
		u8 band_2g_upper_edge_backoff_dsss;
		u8 band_2g_upper_edge_backoff_ht;
		u8 band_2g_upper_edge_backoff_he;
		u8 band_unii_1_lower_edge_backoff_ht;
		u8 band_unii_1_lower_edge_backoff_he;
		u8 band_unii_1_upper_edge_backoff_ht;
		u8 band_unii_1_upper_edge_backoff_he;
		u8 band_unii_2a_lower_edge_backoff_ht;
		u8 band_unii_2a_lower_edge_backoff_he;
		u8 band_unii_2a_upper_edge_backoff_ht;
		u8 band_unii_2a_upper_edge_backoff_he;
		u8 band_unii_2c_lower_edge_backoff_ht;
		u8 band_unii_2c_lower_edge_backoff_he;
		u8 band_unii_2c_upper_edge_backoff_ht;
		u8 band_unii_2c_upper_edge_backoff_he;
		u8 band_unii_3_lower_edge_backoff_ht;
		u8 band_unii_3_lower_edge_backoff_he;
		u8 band_unii_3_upper_edge_backoff_ht;
		u8 band_unii_3_upper_edge_backoff_he;
		u8 band_unii_4_lower_edge_backoff_ht;
		u8 band_unii_4_lower_edge_backoff_he;
		u8 band_unii_4_upper_edge_backoff_ht;
		u8 band_unii_4_upper_edge_backoff_he;
		u8 ant_gain_2g;
		u8 ant_gain_5g_band1;
		u8 ant_gain_5g_band2;
		u8 ant_gain_5g_band3;
		u8 pcb_loss_2g;
		u8 pcb_loss_5g_band1;
		u8 pcb_loss_5g_band2;
		u8 pcb_loss_5g_band3;
		u8 zero[11]; /* Undocumented, set to zero. */
	} phy_params;
};

/* System commands. */
#define	NRF70_RF_PARAMS_SZ		200
#define	NRF70_RX_QUEUE_CNT		3
#define	NRF70_COUNTRY_CODE_LEN		2
#define	NRF70_OP_BAND_ALL		0
#define	NRF70_OP_BAND_2G		1
struct __packed nrf70_cmd_sys_init {
	struct nrf70_header header;
	u32 dev_id;
	struct __packed {
		u32 sleep_enable;
		u32 hw_bringup_time;
		u32 sw_bringup_time;
		u32 bcn_time_out;
		u32 calib_sleep_clk;
		u32 phy_calib;
		u8 hwaddr[ETH_ALEN];
		struct nrf70_rf_params rf_params;
		u8 rf_params_valid;
	} sys_param;
	struct __packed {
		u16 size;
		u16 count;
	} rx_buf_pools[NRF70_RX_QUEUE_CNT];
	struct __packed {
		u8 rate_protection_type;
		u8 aggregation;
		u8 wmm;
		u8 max_tx_agg_sessions;
		u8 max_rx_agg_sessions;
		u8 max_tx_aggregation;
		u8 reorder_buf_size;
		u32 max_rxampdu_size;
	} data_config_params;
	struct __packed {
		u32 temp_based_calib_en;
		u32 temp_calib_bitmap;
		u32 vbat_calibp_bitmap;
		u32 temp_vbat_mon_period;
		s32 vth_very_low;
		s32 vth_low;
		s32 vth_hi;
		s32 temp_threshold;
		s32 vbat_threshold;
	} vbat_config;
	u8 tcp_ip_checksum_offload;
	u8 country_code[NRF70_COUNTRY_CODE_LEN];
	u32 op_band;
	u8 mgmt_buff_offload;
	u32 feature_flags;
	u32 disable_beamforming;
	u32 discon_timeout;
	u8 ps_data_retrieval_mech;
};

#define	NRF70_OP_MODE_STA		BIT(0)
#define	NRF70_OP_MODE_MONITOR		BIT(1)
#define	NRF70_OP_MODE_AP		BIT(4)
struct __packed nrf70_cmd_raw_config_mode {
	struct nrf70_header header;
	u8 if_idx;
	u8 mode;
};

struct __packed nrf70_event_raw_config_mode {
	struct nrf70_header header;
	u8 if_idx;
	u8 mode;
	s32 status;
};

struct __packed nrf70_cmd_set_channel {
	struct nrf70_header header;
	u8 if_idx;
	struct __packed {
		u32 primary_num;
		u8 bandwidth;
		s32 sec_20_offset;
		s32 sec_40_offset;
	} chan;
};

struct __packed nrf70_event_set_channel {
	struct nrf70_header header;
	u8 if_idx;
	u32 chan;
	s32 status;
};

#define	NRF70_OP_MODE_PRODUCTION	0
struct __packed nrf70_cmd_get_stats {
	struct nrf70_header header;
	u32 stats_type;
	u32 op_mode;
};

/* Data commands/events. */
#define	NRF70_BUF_TIMESTAMP_SZ		6
struct __packed nrf70_cmd_rx_buf {
	struct nrf70_header header;
	s16 rx_pkt_type;
	u8 rate_flags;
	u8 rate;
	u8 wdev_id;
	u8 rx_pkt_cnt;
	u8 reserved;
	u8 mac_header_len;
	u16 frequency;
	s16 signal;
	struct __packed {
		u16 desc_id;
		u16 pkt_len;
		u8 pkt_type;
		u8 timestamp_rec[NRF70_BUF_TIMESTAMP_SZ];
		u8 timestamp_ack[NRF70_BUF_TIMESTAMP_SZ];
	} buf_info[];
};

#define	NRF70_SAP_PM_CLIENT_ACTIVE	0
#define	NRF70_SAP_PM_CLIENT_PS_MODE	1
struct __packed nrf70_cmd_sap_pm {
	struct nrf70_header header;
	u32 wdev_id;
	u8 state;
	u8 hwaddr[ETH_ALEN];
};

struct __packed nrf70_buf_info {
	u16 pkt_len;
	u32 ddr_ptr;
};

#define	NRF70_TX_QOS_MASK		0xffff
#define	NRF70_TX_FLAG_CSUM_AVAIL	BIT(30)
#define	NRF70_TX_FLAG_TWT_EMERG		BIT(31)
struct __packed nrf70_cmd_tx_buf {
	struct nrf70_header header;
	u8 wdev_id;
	u8 tx_desc_num;
	struct __packed {
		s32 umac_fill_flags;
		u16 fc;
		u8 dst[ETH_ALEN];
		u8 src[ETH_ALEN];
		u16 etype;
		u32 tx_flags;
		u8 more_data;
		u8 eosp;
	} mac_hdr_info;
	u32 pending_buf_size;
	u8 num_tx_pkts;
	struct nrf70_buf_info buf_info[];
};

struct __packed nrf70_event_tx_buff_done {
	struct nrf70_header header;
	u8 tx_desc_num;
	u8 num_tx_status_code;
	u8 timestamp_sent[NRF70_BUF_TIMESTAMP_SZ];
	u8 timestamp_rec[NRF70_BUF_TIMESTAMP_SZ];
	u8 tx_status_code[];
};

struct __packed nrf70_event_carrier_state {
	struct nrf70_header header;
	u32 wdev_id;
};

/* UMAC commands/events. */
#define NRF70_SSID_SZ			32
struct __packed nrf70_ssid {
	u8 len;
	u8 ssid[NRF70_SSID_SZ];
};

#define NRF70_IE_SZ			400
struct __packed nrf70_ie {
	u16 len;
	s8 ie[NRF70_IE_SZ];
};

#define	NRF70_FRAME_SZ			400
struct __packed nrf70_frame {
	s32 len;
	s8 data[NRF70_FRAME_SZ];
};

#define	NRF70_SCAN_REASON_DISPLAY	0
#define	NRF70_SCAN_REASON_CONNECT	1
#define	NRF70_SCAN_BAND_ANY		0
#define	NRF70_SCAN_BAND_2GHZ		BIT(0)
#define	NRF70_SCAN_BAND_5GHZ		BIT(1)
struct __packed nrf70_cmd_scan {
	struct nrf70_umac_header header;
	struct __packed {
		s32 reason;
		u16 passive_scan;
		u8 num_scan_ssids;
		struct nrf70_ssid scan_ssids[2];
		u8 no_cck;
		u8 bands;
		struct nrf70_ie ie;
		u8 hwaddr[ETH_ALEN];
		u16 dwell_time_active;
		u16 dwell_time_passive;
		u16 num_scan_channels;
		u8 skip_local_admin_macs;
		u32 center_freq[];
	} scan_info;
};

struct __packed nrf70_event_cmd_status {
	struct nrf70_umac_header header;
	u32 cmd_id;
	s32 status;
};

struct __packed nrf70_cmd_change_hwaddr {
	struct nrf70_umac_header header;
	u8 hwaddr[ETH_ALEN];
};

struct __packed nrf70_cmd_get_scan_results {
	struct nrf70_umac_header header;
	s32 reason;
};

struct __packed nrf70_cmd_chg_vif_state {
	struct nrf70_umac_header header;
	struct __packed {
		u32 state;
		s8 if_idx;
	} info;
};

#define	NRF70_CHG_VIF_IFTYPE			BIT(0)
#define	NRF70_CHG_VIF_USE_4ADDR			BIT(1)
struct __packed nrf70_cmd_chg_vif_attr {
	struct nrf70_umac_header header;
	u32 valid_fields;
	struct __packed {
		s32 iftype;
		s32 use_4addr;
	} info;
};

#define	NRF70_ADD_VIF_USE_4ADDR			BIT(0)
#define	NRF70_ADD_VIF_HWADDR			BIT(1)
#define	NRF70_ADD_VIF_IFTYPE			BIT(2)
#define	NRF70_ADD_VIF_IFNAME			BIT(3)
struct __packed nrf70_cmd_add_vif {
	struct nrf70_umac_header header;
	u32 valid_fields;
	struct __packed {
		s32 iftype;
		s32 use_4addr;
		u32 mon_flags;
		u8 hwaddr[ETH_ALEN];
		s8 ifacename[IFNAMSIZ];
	} info;
};

#define	NRF70_FRAME_MATCH_MAX_LEN	8
struct __packed nrf70_cmd_mgmt_frame_reg {
	struct nrf70_umac_header header;
	struct __packed {
		u16 type;
		u32 match_len;
		u8 match[NRF70_FRAME_MATCH_MAX_LEN];
	} info;
};

#define	NRF70_KEY_INFO_KEY_SZ			256
#define	NRF70_KEY_INFO_SEQ_SZ			256
#define	NRF70_KEY_INFO_FLAG_DEFAULT		BIT(0)
#define	NRF70_KEY_INFO_FLAG_DEFAULT_MGMT	BIT(2)
#define	NRF70_KEY_INFO_FLAG_DEFAULT_UNICAST	BIT(3)
#define	NRF70_KEY_INFO_FLAG_DEFAULT_MULTICAST	BIT(4)
#define	NRF70_KEY_INFO_KEY			BIT(0)
#define	NRF70_KEY_INFO_KEY_TYPE			BIT(1)
#define	NRF70_KEY_INFO_KEY_IDX			BIT(2)
#define	NRF70_KEY_INFO_SEQ			BIT(3)
#define	NRF70_KEY_INFO_CIPHER_SUITE		BIT(4)
#define	NRF70_KEY_INFO_KEY_INFO			BIT(5)
struct __packed nrf70_key_info {
	u32 valid_fields;
	u32 cipher_suite;
	u16 wifi_flags;
	struct __packed {
		s32 type;
		u32 len;
		u8 data[NRF70_KEY_INFO_KEY_SZ];
	} key;
	struct __packed {
		s32 len;
		u8 data[NRF70_KEY_INFO_SEQ_SZ];
	} seq;
	u8 key_idx;
};

#define	NRF70_KEY_HWADDR		BIT(0)
struct __packed nrf70_cmd_key {
	struct nrf70_umac_header header;
	u32 valid_fields;
	struct nrf70_key_info info;
	u8 hwaddr[ETH_ALEN];
};

struct __packed nrf70_cmd_set_key {
	struct nrf70_umac_header header;
	struct nrf70_key_info info;
};

#define	NRF70_AUTHTYPE_OPEN_SYSTEM	0
#define	NRF70_AUTHTYPE_SHARED_KEY	1
#define	NRF70_AUTHTYPE_FT		2
#define	NRF70_AUTHTYPE_NETWORK_EAP	3
#define	NRF70_AUTHTYPE_SAE		4
#define	NRF70_AUTHTYPE_AUTOMATIC	7

#define	NRF70_AUTH_INFO_SAE_SZ		256
#define	NRF70_AUTH_KEY_INFO		BIT(0)
#define	NRF70_AUTH_BSSID		BIT(1)
#define	NRF70_AUTH_FREQ			BIT(2)
#define	NRF70_AUTH_SSID			BIT(3)
#define	NRF70_AUTH_IE			BIT(4)
#define	NRF70_AUTH_SAE			BIT(5)
struct __packed nrf70_cmd_auth {
	struct nrf70_umac_header header;
	u32 valid_fields;
	struct __packed {
		u32 frequency;
		u16 wifi_flags;
		s32 auth_type;
		struct nrf70_key_info key_info;
		struct nrf70_ssid ssid;
		struct nrf70_ie ie;
		struct __packed {
			s32 len;
			u8 data[NRF70_AUTH_INFO_SAE_SZ];
		} sae;
		u8 bssid[ETH_ALEN];
		s32 scan_width;
		s32 signal;
		s32 from_beacon;
		struct nrf70_ie bss_ie;
		u16 capability;
		u16 beacon_interval;
		u64 tsf;
	} info;
};

#define	NRF70_CONNECT_HWADDR			BIT(0)
#define	NRF70_CONNECT_FREQ			BIT(2)
#define	NRF70_CONNECT_SSID			BIT(5)
#define	NRF70_CONNECT_WPA_IE			BIT(6)
#define	NRF70_CONNECT_WPA_VERSIONS		BIT(7)
#define	NRF70_CONNECT_CIPHER_PAIRWISE		BIT(8)
#define	NRF70_CONNECT_CIPHER_GROUP		BIT(9)
#define	NRF70_CONNECT_AKM_SUITES		BIT(10)
#define	NRF70_CONNECT_MFP			BIT(11)
#define	NRF70_CONNECT_CONTROL_PORT_ETHER_TYPE	BIT(12)
#define	NRF70_CONNECT_CONTROL_PORT_NO_ENCRYPT	BIT(13)
#define	NRF70_CONNECT_FLAGS_USE_RRM		BIT(14)
#define	NRF70_HT_VHT_CAP_MAX_SZ			256
struct __packed nrf70_connect_info {
	u32 valid_fields;
	u32 frequency;
	u32 freq_hint;
	u32 wpa_versions;
	s32 num_cipher_suites_pairwise;
	u32 cipher_suites_pairwise[7];
	u32 cipher_suite_group;
	u32 num_akm_suites;
	u32 akm_suites[2];
	s32 use_mfp;
	u32 wifi_flags;
	u16 bg_scan_period;
	u8 hwaddr[ETH_ALEN];
	u8 hwaddr_hint[ETH_ALEN];
	struct nrf70_ssid ssid;
	struct nrf70_ie wpa_ie;
	struct __packed {
		u32 valid_fields;
		u16 flags;
		u8 ht_capa[NRF70_HT_VHT_CAP_MAX_SZ];
		u8 ht_capa_mask[NRF70_HT_VHT_CAP_MAX_SZ];
		u8 vht_capa[NRF70_HT_VHT_CAP_MAX_SZ];
		u8 vht_capa_mask[NRF70_HT_VHT_CAP_MAX_SZ];
	} ht_vht_cap;
	u16 control_port_ethertype;
	u8 control_port_no_encrypt;
	s8 control_port;
	u8 prev_bssid[ETH_ALEN];
	u16 maxidle_insec;
};

struct __packed nrf70_cmd_assoc {
	struct nrf70_umac_header header;
	u32 valid_fields;
	struct nrf70_connect_info info;
	u8 hwaddr[ETH_ALEN];
};

#define	NRF70_DISCONN_FLAGS_LOCAL_STATE_CHANGE	BIT(0)
#define	NRF70_DISCONN_HWADDR			BIT(0)
struct __packed nrf70_cmd_disconn {
	struct nrf70_umac_header header;
	u32 valid_fields;
	struct __packed {
		u16 flags;
		u16 reason;
		u8 hwaddr[ETH_ALEN];
	} info;
};

#define	NRF70_FREQ_PARAMS_FREQ		BIT(0)
#define	NRF70_FREQ_PARAMS_CHAN_WIDTH	BIT(1)
#define	NRF70_FREQ_PARAMS_CENTER_FREQ1	BIT(2)
#define	NRF70_FREQ_PARAMS_CENTER_FREQ2	BIT(3)
#define	NRF70_FREQ_PARAMS_CHAN_TYPE	BIT(4)
#define	NRF70_CHAN_NO_HT	0
#define	NRF70_CHAN_HT20		1
#define	NRF70_CHAN_HT40MINUS	2
#define	NRF70_CHAN_HT40PLUS	3
struct __packed nrf70_freq_params {
	u32 valid_fields;
	s32 frequency;
	s32 channel_width;
	s32 center_freq1;
	s32 center_freq2;
	s32 channel_type;
};

#define	NRF70_MGMT_TX_FREQ			BIT(0)
#define	NRF70_MGMT_TX_DURATION			BIT(1)
#define	NRF70_MGMT_TX_SET_FRAME_FREQ		BIT(2)
#define	NRF70_MGMT_TX_FLAGS_OFFCHAN_TX		BIT(0)
#define	NRF70_MGMT_TX_FLAGS_NO_CCK_RATE		BIT(1)
#define	NRF70_MGMT_TX_FLAGS_NO_ACK		BIT(2)
#define	NRF70_MGMT_TX_FREQ_MASK			GENMASK(4, 0)
struct __packed nrf70_cmd_mgmt_tx {
	struct nrf70_umac_header header;
	u32 valid_fields;
	struct __packed {
		u32 wifi_flags;
		u32 frequency;
		u32 dur;
		struct nrf70_frame frame;
		struct nrf70_freq_params freq_params;
		u64 cookie;
	} info;
};

struct __packed nrf70_sta_flag_update {
	u32 mask;
	u32 set;
};

#define	NRF70_CHG_STA_SUPP_RATES		BIT(0)
#define	NRF70_CHG_STA_AID			BIT(1)
#define	NRF70_CHG_STA_STA_CAPAB			BIT(3)
#define	NRF70_CHG_STA_EXT_CAPAB			BIT(4)
#define	NRF70_CHG_STA_HT_CAP			BIT(6)
#define	NRF70_CHG_STA_VHT_CAP			BIT(7)
#define	NRF70_CHG_STA_OPMODE_NOTIF		BIT(9)
#define	NRF70_CHG_STA_SUP_CHANS			BIT(10)
#define	NRF70_CHG_STA_OPER_CLASSES		BIT(11)
#define	NRF70_CHG_STA_FLAGS2			BIT(12)
#define	NRF70_CHG_STA_WME_UAPSD_QUEUES		BIT(13)
#define	NRF70_CHG_STA_WME_MAX_SP		BIT(14)
#define	NRF70_CHG_STA_LISTEN_INTERVAL		BIT(15)
struct __packed nrf70_cmd_chg_sta {
	struct nrf70_umac_header header;
	u32 valid_fields;
	s32 listen_interval;
	u32 sta_vlan;
	u16 aid;
	u16 peer_aid;
	u16 sta_capability;
	u16 spare;
	struct __packed {
		u32 valid_fields;
		s32 band;
		s32 num_rates;
		u8 rates[60];
	} supp_rates;
	u32 ext_cap_len;
	u8 ext_cap[32];
	u32 sup_chans_len;
	u8 sup_chans[64];
	u32 sup_oper_classes_len;
	u8 sup_oper_classes[64];
	struct nrf70_sta_flag_update sta_flags2;
	u8 ht_cap[NRF70_HT_VHT_CAP_MAX_SZ];
	u8 vht_cap[NRF70_HT_VHT_CAP_MAX_SZ];
	u8 hwaddr[ETH_ALEN];
	u8 opmode_notif;
	u8 wme_uapsd_queues;
	u8 wme_max_sp;
};

#define	NRF70_DEL_STA_HWADDR			BIT(0)
#define	NRF70_DEL_STA_MGMT_SUBTYPE		BIT(1)
#define	NRF70_DEL_STA_REASON			BIT(2)
struct __packed nrf70_cmd_del_sta {
	struct nrf70_umac_header header;
	u32 valid_fields;
	u8 hwaddr[ETH_ALEN];
	u8 mgmt_subtype;
	u16 reason;
};

struct __packed nrf70_wiphy_info {
	u32 rts_threshold;
	u32 frag_threshold;
	u32 antenna_tx;
	u32 antenna_rx;
	struct nrf70_freq_params freq_params;
	struct __packed {
		u16 toxp;
		u16 cwmin;
		u16 cwmax;
		u8 aifs;
		u8 ac;
	} txq_params;
	struct __packed {
		u32 valid_fields;
		s32 type;
		s32 power_level;
	} tx_power_settings;
	u8 retry_short;
	u8 retry_long;
	u8 coverage_class;
	s8 wiphy_name[32];
};

#define	NRF70_SET_WIPHY_FREQ			BIT(0)
#define	NRF70_SET_WIPHY_RTS_THRESHOLD		BIT(2)
#define	NRF70_SET_WIPHY_FRAG_THRESHOLD		BIT(3)
#define	NRF70_SET_WIPHY_RETRY_SHORT		BIT(7)
#define	NRF70_SET_WIPHY_RETRY_LONG		BIT(8)
#define	NRF70_SET_WIPHY_COVERAGE_CLASS		BIT(9)
struct __packed nrf70_cmd_set_wiphy {
	struct nrf70_umac_header header;
	u32 valid_fields;
	struct nrf70_wiphy_info info;
};

#define	NRF70_BEACON_DATA_MAX_HEAD_LEN		256
#define	NRF70_BEACON_DATA_MAX_TAIL_LEN		512
#define	NRF70_BEACON_DATA_MAX_PROBE_RESP_LEN	400
struct __packed nrf70_beacon_data {
	u32 head_len;
	u32 tail_len;
	u32 probe_resp_len;
	u8 head[NRF70_BEACON_DATA_MAX_HEAD_LEN];
	u8 tail[NRF70_BEACON_DATA_MAX_TAIL_LEN];
	u8 probe_resp[NRF70_BEACON_DATA_MAX_PROBE_RESP_LEN];
};

#define	NRF70_SET_BSS_CTS			BIT(0)
#define	NRF70_SET_BSS_PREAMBLE			BIT(1)
#define	NRF70_SET_BSS_SLOT			BIT(2)
#define	NRF70_SET_BSS_HT_OPMODE			BIT(3)
#define	NRF70_SET_BSS_AP_ISOLATE		BIT(4)
#define	NRF70_SET_BSS_P2P_CTWINDOW		BIT(5)
#define	NRF70_SET_BSS_P2P_OPPPS			BIT(6)
#define	NRF70_BSS_INFO_MAX_BASIC_RATES		32
struct __packed nrf70_cmd_set_bss {
	struct nrf70_umac_header header;
	u32 valid_fields;
	struct __packed {
		u32 p2p_go_ctwindow;
		u32 p2p_opp_ps;
		u32 num_basic_rates;
		u16 ht_opmode;
		u8 cts;
		u8 preamble;
		u8 slot;
		u8 ap_isolate;
		u8 basic_rates[NRF70_BSS_INFO_MAX_BASIC_RATES];
	} info;
};

#define	NRF70_START_AP_BEACON_INTERVAL		BIT(0)
#define	NRF70_START_AP_AUTH_TYPE		BIT(1)
#define	NRF70_START_AP_VERSIONS			BIT(2)
#define	NRF70_START_AP_CIPHER_SUITE_GROUP	BIT(3)
#define	NRF70_START_AP_INACTIVITY_TIMEOUT	BIT(4)
#define	NRF70_START_AP_FREQ_PARAMS		BIT(5)
#define	NRF70_START_AP_FLAG_PRIVACY		BIT(0)
#define	NRF70_START_AP_FLAG_NO_ENCRYPT		BIT(1)
#define	NRF70_START_AP_FLAG_P2P_CTWINDOW	BIT(6)
#define	NRF70_START_AP_FLAG_P2P_OPPPS		BIT(7)
struct __packed nrf70_cmd_start_ap {
	struct nrf70_umac_header header;
	u32 valid_fields;
	struct __packed {
		u16 beacon_interval;
		u8 dtim_period;
		s32 hidden_ssid;
		s32 auth_type;
		s32 smps_mode;
		u32 flags;
		struct nrf70_beacon_data beacon_data;
		struct nrf70_ssid ssid;
		struct nrf70_connect_info connect_info;
		struct nrf70_freq_params freq_params;
		u16 inactivity_timeout;
		u8 p2p_go_ctwindow;
		u8 p2p_opp_ps;
	} info;
};

struct __packed nrf70_cmd_set_beacon {
	struct nrf70_umac_header header;
	struct nrf70_beacon_data beacon_data;
};

struct __packed nrf70_cmd_get_sta {
	struct nrf70_umac_header header;
	u8 hwaddr[ETH_ALEN];
};

struct __packed nrf70_cmd_set_qos_map {
	struct nrf70_umac_header header;
	struct __packed {
		u32 len;
		u8 data[256];
	} map_info;
};

struct __packed nrf70_display_results {
	struct nrf70_ssid ssid;
	u8 hwaddr[ETH_ALEN];
	s32 band;
	u32 chan;
	u8 protocol_flags;
	s32 security_type;
	u16 beacon_interval;
	u16 capability;
	struct __packed {
		u32 type;
		union __packed {
			u32 mbm_signal;
			u8 unspec_signal;
		};
	} signal;
	u8 reserved[4];
};

#define	NRF70_DISP_SCAN_RES_SZ			8
struct __packed nrf70_event_scan_display_results {
	struct nrf70_umac_header header;
	u8 bss_count;
	struct nrf70_display_results results[NRF70_DISP_SCAN_RES_SZ];
};

struct __packed nrf70_event_scan_done {
	struct nrf70_umac_header header;
	u32 status;
	u32 scan_type;
};

struct __packed nrf70_event_get_reg {
	struct nrf70_umac_header header;
	u8 alpha2[NRF70_COUNTRY_CODE_LEN];
	u32 num_chans;
	struct __packed {
		u32 center_freq;
		u32 max_power;
		u8 supported;
		u8 passive_channel;
		u8 dfs;
	} chan_info[];
};

#define	NRF70_EVENT_MLME_TIMED_OUT		BIT(0)
#define	NRF70_EVENT_MLME_ACK			BIT(1)
struct __packed nrf70_event_mlme {
	struct nrf70_umac_header header;
	u32 valid_fields;
	u32 frequency;
	u32 rx_signal_dbm;
	u32 wifi_flags;
	u64 cookie;
	struct nrf70_frame frame;
	u8 bssid[ETH_ALEN];
	u8 wme_uapsd_queues;
	u32 req_ie_len;
	u8 req_ie[];
};

struct __packed nrf70_event_iface_update {
	struct nrf70_umac_header header;
	s32 status;
};

struct __packed nrf70_event_cookie_resp {
	struct nrf70_umac_header header;
	u32 valid_fields;
	u64 host_cookie;
	u64 cookie;
	u8 hwaddr[ETH_ALEN];
};

#define	NRF70_RATE_INFO_BITRATE			BIT(0)
#define	NRF70_RATE_INFO_BITRATE_COMPAT		BIT(1)
#define	NRF70_RATE_INFO_MCS			BIT(2)
#define	NRF70_RATE_INFO_VHT_MCS			BIT(3)
#define	NRF70_RATE_INFO_VHT_NSS			BIT(4)

#define	NRF70_RATE_INFO_0_MHZ_WIDTH		BIT(0)
#define	NRF70_RATE_INFO_5_MHZ_WIDTH		BIT(1)
#define	NRF70_RATE_INFO_10_MHZ_WIDTH		BIT(2)
#define	NRF70_RATE_INFO_40_MHZ_WIDTH		BIT(3)
#define	NRF70_RATE_INFO_80_MHZ_WIDTH		BIT(4)
#define	NRF70_RATE_INFO_160_MHZ_WIDTH		BIT(5)
#define	NRF70_RATE_INFO_SHORT_GI		BIT(6)
#define	NRF70_RATE_INFO_80P80_MHZ_WIDTH		BIT(7)
struct __packed nrf70_rate_info {
	u32 valid_fields;
	u32 bitrate;
	u16 bitrate_compat;
	u8 mcs;
	u8 vht_mcs;
	u8 vht_nss;
	u32 flags;
};

#define	NRF70_STA_INFO_CONNECTED_TIME		BIT(0)
#define	NRF70_STA_INFO_INACTIVE_TIME		BIT(1)
#define	NRF70_STA_INFO_RX_BYTES			BIT(2)
#define	NRF70_STA_INFO_TX_BYTES			BIT(3)
#define	NRF70_STA_INFO_CHAIN_SIGNAL		BIT(4)
#define	NRF70_STA_INFO_CHAIN_SIGNAL_AVG		BIT(5)
#define	NRF70_STA_INFO_TX_BITRATE		BIT(6)
#define	NRF70_STA_INFO_RX_BITRATE		BIT(7)
#define	NRF70_STA_INFO_STA_FLAGS		BIT(8)
#define	NRF70_STA_INFO_LLID			BIT(9)
#define	NRF70_STA_INFO_PLID			BIT(10)
#define	NRF70_STA_INFO_PLINK_STATE		BIT(11)
#define	NRF70_STA_INFO_SIGNAL			BIT(12)
#define	NRF70_STA_INFO_SIGNAL_AVG		BIT(13)
#define	NRF70_STA_INFO_RX_PACKETS		BIT(14)
#define	NRF70_STA_INFO_TX_PACKETS		BIT(15)
#define	NRF70_STA_INFO_TX_RETRIES		BIT(16)
#define	NRF70_STA_INFO_TX_FAILED		BIT(17)
#define	NRF70_STA_INFO_EXPECTED_THROUGHPUT	BIT(18)
#define	NRF70_STA_INFO_BEACON_LOSS_COUNT	BIT(19)
#define	NRF70_STA_INFO_LOCAL_PM			BIT(20)
#define	NRF70_STA_INFO_PEER_PM			BIT(21)
#define	NRF70_STA_INFO_NONPEER_PM		BIT(22)
#define	NRF70_STA_INFO_T_OFFSET			BIT(23)
#define	NRF70_STA_INFO_RX_DROPPED_MISC		BIT(24)
#define	NRF70_STA_INFO_RX_BEACON		BIT(25)
#define	NRF70_STA_INFO_RX_BEACON_SIGNAL_AVG	BIT(26)
#define	NRF70_STA_INFO_BSS_PARAMS		BIT(27)
struct __packed nrf70_event_new_station {
	struct nrf70_umac_header header;
	u32 valid_fields;
	u8 wme;
	u8 sta_legacy;
	u8 hwaddr[ETH_ALEN];
	u32 generation;
	struct __packed {
		u32 valid_fields;
		u32 connected_time;
		u32 inactive_time;
		u32 rx_bytes;
		u32 tx_bytes;
		struct __packed {
			u32 signal_mask;
			u8 signal[IEEE80211_MAX_CHAINS];
			u32 signal_avg_mask;
			u8 signal_avg[IEEE80211_MAX_CHAINS];
		} chain;
		struct nrf70_rate_info tx_bitrate;
		struct nrf70_rate_info rx_bitrate;
		u16 llid; /* unused */
		u16 plid; /* unused */
		u8 plink_state; /* unused */
		s32 signal;
		s32 signal_avg;
		u32 rx_packets;
		struct __packed {
			u32 packets;
			u32 retries;
			u32 failed;
		} tx;
		u32 expected_throughput;
		u32 beacon_loss_count;
		u32 local_pm; /* unused */
		u32 peer_pm; /* unused */
		u32 nonpeer_pm; /* unused */
		struct nrf70_sta_flag_update sta_flags;
		u64 t_offset;
		u64 rx_dropped_misc;
		u64 rx_beacon;
		s64 rx_beacon_signal_avg;
		struct __packed {
			u8 flags;
			u8 dtim_period;
			u16 beacon_interval;
		} bss_param;
	} sta_info;
	struct nrf70_ie assoc_req_ies;
};

struct __packed nrf70_event_get_chan {
	struct nrf70_umac_header header;
	struct __packed {
		s32 band;
		u32 center_freq;
		u32 flags;
		s32 max_antenna_gain;
		s32 max_power;
		s32 max_reg_power;
		u32 orig_flags;
		s32 orig_mag;
		s32 orig_mpwr;
		u16 hw_value;
		s8 beacon_found;
	} chan;
	s32 width;
	u32 center_freq1;
	u32 center_freq2;
};

#define	NRF70_SET_REG_ALPHA2		BIT(0)
#define	NRF70_SET_REG_USER_REG_FORCE	BIT(2)
struct __packed nrf70_cmd_set_reg {
	struct nrf70_umac_header header;
	u32 valid_fields;
	u32 user_reg_hint_type;
	u8 alpha2[NRF70_COUNTRY_CODE_LEN];
};

struct __packed nrf70_event_reg_change {
	struct nrf70_umac_header header;
	u16 flags;
	s32 intr;
	s8 reg_type;
	u8 alpha2[NRF70_COUNTRY_CODE_LEN];
};

#endif /* _NRF70_CMDS_H */
