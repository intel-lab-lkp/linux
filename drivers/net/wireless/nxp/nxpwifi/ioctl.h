/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * NXP Wireless LAN device driver: ioctl data structures & APIs
 *
 * Copyright 2011-2024 NXP
 */

#ifndef _NXPWIFI_IOCTL_H_
#define _NXPWIFI_IOCTL_H_

#include <net/lib80211.h>

enum {
	NXPWIFI_SCAN_TYPE_UNCHANGED = 0,
	NXPWIFI_SCAN_TYPE_ACTIVE,
	NXPWIFI_SCAN_TYPE_PASSIVE
};

#define NXPWIFI_PROMISC_MODE            1
#define NXPWIFI_MULTICAST_MODE		2
#define	NXPWIFI_ALL_MULTI_MODE		4
#define NXPWIFI_MAX_MULTICAST_LIST_SIZE	32

struct nxpwifi_multicast_list {
	u32 mode;
	u32 num_multicast_addr;
	u8 mac_list[NXPWIFI_MAX_MULTICAST_LIST_SIZE][ETH_ALEN];
};

struct nxpwifi_chan_freq {
	u32 channel;
	u32 freq;
};

struct nxpwifi_ssid_bssid {
	struct cfg80211_ssid ssid;
	u8 bssid[ETH_ALEN];
};

enum {
	BAND_B = 1,
	BAND_G = 2,
	BAND_A = 4,
	BAND_GN = 8,
	BAND_AN = 16,
	BAND_AAC = 32,
};

#define NXPWIFI_WPA_PASSHPHRASE_LEN 64
struct wpa_param {
	u8 pairwise_cipher_wpa;
	u8 pairwise_cipher_wpa2;
	u8 group_cipher;
	u32 length;
	u8 passphrase[NXPWIFI_WPA_PASSHPHRASE_LEN];
};

struct wep_key {
	u8 key_index;
	u8 is_default;
	u16 length;
	u8 key[WLAN_KEY_LEN_WEP104];
};

#define KEY_MGMT_ON_HOST        0x03
#define NXPWIFI_AUTH_MODE_AUTO  0xFF
#define BAND_CONFIG_BG          0x00
#define BAND_CONFIG_A           0x01
#define NXPWIFI_SEC_CHAN_BELOW	0x30
#define NXPWIFI_SEC_CHAN_ABOVE	0x10
#define NXPWIFI_SUPPORTED_RATES                 14
#define NXPWIFI_SUPPORTED_RATES_EXT             32
#define NXPWIFI_PRIO_BK				2
#define NXPWIFI_PRIO_VI				5
#define NXPWIFI_SUPPORTED_CHANNELS		2
#define NXPWIFI_OPERATING_CLASSES		16

struct nxpwifi_uap_bss_param {
	u8 mac_addr[ETH_ALEN];
	u8 channel;
	u8 band_cfg;
	u16 rts_threshold;
	u16 frag_threshold;
	u8 retry_limit;
	struct nxpwifi_802_11_ssid ssid;
	u8 bcast_ssid_ctl;
	u8 radio_ctl;
	u8 dtim_period;
	u16 beacon_period;
	u16 auth_mode;
	u16 protocol;
	u16 key_mgmt;
	u16 key_mgmt_operation;
	struct wpa_param wpa_cfg;
	struct wep_key wep_cfg[NUM_WEP_KEYS];
	struct ieee80211_ht_cap ht_cap;
	struct ieee80211_vht_cap vht_cap;
	u8 rates[NXPWIFI_SUPPORTED_RATES];
	u32 sta_ao_timer;
	u32 ps_sta_ao_timer;
	u8 qos_info;
	u8 power_constraint;
	struct nxpwifi_types_wmm_info wmm_info;
};

struct nxpwifi_ds_get_stats {
	u32 mcast_tx_frame;
	u32 failed;
	u32 retry;
	u32 multi_retry;
	u32 frame_dup;
	u32 rts_success;
	u32 rts_failure;
	u32 ack_failure;
	u32 rx_frag;
	u32 mcast_rx_frame;
	u32 fcs_error;
	u32 tx_frame;
	u32 wep_icv_error[4];
	u32 bcn_rcv_cnt;
	u32 bcn_miss_cnt;
};

#define NXPWIFI_MAX_VER_STR_LEN    128

struct nxpwifi_ver_ext {
	u32 version_str_sel;
	char version_str[NXPWIFI_MAX_VER_STR_LEN];
};

struct nxpwifi_bss_info {
	u32 bss_mode;
	struct cfg80211_ssid ssid;
	u32 bss_chan;
	u8 country_code[3];
	u32 media_connected;
	u32 max_power_level;
	u32 min_power_level;
	signed int bcn_nf_last;
	u32 wep_status;
	u32 is_hs_configured;
	u32 is_deep_sleep;
	u8 bssid[ETH_ALEN];
};

struct nxpwifi_sta_info {
	u8 peer_mac[ETH_ALEN];
	struct station_parameters *params;
};

#define MAX_NUM_TID     8

#define MAX_RX_WINSIZE  64

struct nxpwifi_ds_rx_reorder_tbl {
	u16 tid;
	u8 ta[ETH_ALEN];
	u32 start_win;
	u32 win_size;
	u32 buffer[MAX_RX_WINSIZE];
};

struct nxpwifi_ds_tx_ba_stream_tbl {
	u16 tid;
	u8 ra[ETH_ALEN];
	u8 amsdu;
};

#define DBG_CMD_NUM    5
#define NXPWIFI_DBG_SDIO_MP_NUM    10

struct nxpwifi_debug_info {
	unsigned int debug_mask;
	u32 int_counter;
	u32 packets_out[MAX_NUM_TID];
	u32 tx_buf_size;
	u32 curr_tx_buf_size;
	u32 tx_tbl_num;
	struct nxpwifi_ds_tx_ba_stream_tbl
		tx_tbl[NXPWIFI_MAX_TX_BASTREAM_SUPPORTED];
	u32 rx_tbl_num;
	struct nxpwifi_ds_rx_reorder_tbl rx_tbl
		[NXPWIFI_MAX_RX_BASTREAM_SUPPORTED];
	u16 ps_mode;
	u32 ps_state;
	u8 is_deep_sleep;
	u8 pm_wakeup_card_req;
	u32 pm_wakeup_fw_try;
	u8 is_hs_configured;
	u8 hs_activated;
	u32 num_cmd_host_to_card_failure;
	u32 num_cmd_sleep_cfm_host_to_card_failure;
	u32 num_tx_host_to_card_failure;
	u32 num_event_deauth;
	u32 num_event_disassoc;
	u32 num_event_link_lost;
	u32 num_cmd_deauth;
	u32 num_cmd_assoc_success;
	u32 num_cmd_assoc_failure;
	u32 num_tx_timeout;
	u8 is_cmd_timedout;
	u16 timeout_cmd_id;
	u16 timeout_cmd_act;
	u16 last_cmd_id[DBG_CMD_NUM];
	u16 last_cmd_act[DBG_CMD_NUM];
	u16 last_cmd_index;
	u16 last_cmd_resp_id[DBG_CMD_NUM];
	u16 last_cmd_resp_index;
	u16 last_event[DBG_CMD_NUM];
	u16 last_event_index;
	u8 data_sent;
	u8 cmd_sent;
	u8 cmd_resp_received;
	u8 event_received;
	u32 last_mp_wr_bitmap[NXPWIFI_DBG_SDIO_MP_NUM];
	u32 last_mp_wr_ports[NXPWIFI_DBG_SDIO_MP_NUM];
	u32 last_mp_wr_len[NXPWIFI_DBG_SDIO_MP_NUM];
	u32 last_mp_curr_wr_port[NXPWIFI_DBG_SDIO_MP_NUM];
	u8 last_sdio_mp_index;
};

#define NXPWIFI_KEY_INDEX_UNICAST	0x40000000
#define PN_LEN				16

struct nxpwifi_ds_encrypt_key {
	u32 key_disable;
	u32 key_index;
	u32 key_len;
	u8 key_material[WLAN_MAX_KEY_LEN];
	u8 mac_addr[ETH_ALEN];
	u8 pn[PN_LEN];		/* packet number */
	u8 pn_len;
	u8 is_igtk_key;
	u8 is_current_wep_key;
	u8 is_rx_seq_valid;
	u8 is_igtk_def_key;
};

struct nxpwifi_power_cfg {
	u32 is_power_auto;
	u32 is_power_fixed;
	u32 power_level;
};

struct nxpwifi_ds_hs_cfg {
	u32 is_invoke_hostcmd;
	/*  Bit0: non-unicast data
	 *  Bit1: unicast data
	 *  Bit2: mac events
	 *  Bit3: magic packet
	 */
	u32 conditions;
	u32 gpio;
	u32 gap;
};

struct nxpwifi_ds_wakeup_reason {
	u16  hs_wakeup_reason;
};

#define DEEP_SLEEP_ON  1
#define DEEP_SLEEP_OFF 0
#define DEEP_SLEEP_IDLE_TIME	100
#define PS_MODE_AUTO		1

struct nxpwifi_ds_auto_ds {
	u16 auto_ds;
	u16 idle_time;
};

struct nxpwifi_ds_pm_cfg {
	union {
		u32 ps_mode;
		struct nxpwifi_ds_hs_cfg hs_cfg;
		struct nxpwifi_ds_auto_ds auto_deep_sleep;
		u32 sleep_period;
	} param;
};

struct nxpwifi_11ac_vht_cfg {
	u8 band_config;
	u8 misc_config;
	u32 cap_info;
	u32 mcs_tx_set;
	u32 mcs_rx_set;
};

struct nxpwifi_ds_11n_tx_cfg {
	u16 tx_htcap;
	u16 tx_htinfo;
	u16 misc_config; /* Needed for 802.11AC cards only */
};

struct nxpwifi_ds_11n_amsdu_aggr_ctrl {
	u16 enable;
	u16 curr_buf_size;
};

struct nxpwifi_ds_ant_cfg {
	u32 tx_ant;
	u32 rx_ant;
};

#define NXPWIFI_NUM_OF_CMD_BUFFER	50
#define NXPWIFI_SIZE_OF_CMD_BUFFER	2048

enum {
	NXPWIFI_IE_TYPE_GEN_IE = 0,
	NXPWIFI_IE_TYPE_ARP_FILTER,
};

enum {
	NXPWIFI_REG_MAC = 1,
	NXPWIFI_REG_BBP,
	NXPWIFI_REG_RF,
	NXPWIFI_REG_PMIC,
	NXPWIFI_REG_CAU,
};

struct nxpwifi_ds_reg_rw {
	u32 type;
	u32 offset;
	u32 value;
};

#define MAX_EEPROM_DATA 256

struct nxpwifi_ds_read_eeprom {
	u16 offset;
	u16 byte_count;
	u8 value[MAX_EEPROM_DATA];
};

struct nxpwifi_ds_mem_rw {
	u32 addr;
	u32 value;
};

#define IEEE_MAX_IE_SIZE		256

#define NXPWIFI_IE_HDR_SIZE	(sizeof(struct nxpwifi_ie) - IEEE_MAX_IE_SIZE)

struct nxpwifi_ds_misc_gen_ie {
	u32 type;
	u32 len;
	u8 ie_data[IEEE_MAX_IE_SIZE];
};

struct nxpwifi_ds_misc_cmd {
	u32 len;
	u8 cmd[NXPWIFI_SIZE_OF_CMD_BUFFER];
};

#define BITMASK_BCN_RSSI_LOW	BIT(0)
#define BITMASK_BCN_RSSI_HIGH	BIT(4)

enum subsc_evt_rssi_state {
	EVENT_HANDLED,
	RSSI_LOW_RECVD,
	RSSI_HIGH_RECVD
};

struct subsc_evt_cfg {
	u8 abs_value;
	u8 evt_freq;
};

struct nxpwifi_ds_misc_subsc_evt {
	u16 action;
	u16 events;
	struct subsc_evt_cfg bcn_l_rssi_cfg;
	struct subsc_evt_cfg bcn_h_rssi_cfg;
};

#define NXPWIFI_MEF_MAX_BYTESEQ		6	/* non-adjustable */
#define NXPWIFI_MEF_MAX_FILTERS		10

struct nxpwifi_mef_filter {
	u16 repeat;
	u16 offset;
	s8 byte_seq[NXPWIFI_MEF_MAX_BYTESEQ + 1];
	u8 filt_type;
	u8 filt_action;
};

struct nxpwifi_mef_entry {
	u8 mode;
	u8 action;
	struct nxpwifi_mef_filter filter[NXPWIFI_MEF_MAX_FILTERS];
};

struct nxpwifi_ds_mef_cfg {
	u32 criteria;
	u16 num_entries;
	struct nxpwifi_mef_entry *mef_entry;
};

#define NXPWIFI_MAX_VSIE_LEN       (256)
#define NXPWIFI_MAX_VSIE_NUM       (8)
#define NXPWIFI_VSIE_MASK_CLEAR    0x00
#define NXPWIFI_VSIE_MASK_SCAN     0x01
#define NXPWIFI_VSIE_MASK_ASSOC    0x02
#define NXPWIFI_VSIE_MASK_BGSCAN   0x08

enum {
	NXPWIFI_FUNC_INIT = 1,
	NXPWIFI_FUNC_SHUTDOWN,
};

enum COALESCE_OPERATION {
	RECV_FILTER_MATCH_TYPE_EQ = 0x80,
	RECV_FILTER_MATCH_TYPE_NE,
};

enum COALESCE_PACKET_TYPE {
	PACKET_TYPE_UNICAST = 1,
	PACKET_TYPE_MULTICAST = 2,
	PACKET_TYPE_BROADCAST = 3
};

#define NXPWIFI_COALESCE_MAX_RULES	8
#define NXPWIFI_COALESCE_MAX_BYTESEQ	4	/* non-adjustable */
#define NXPWIFI_COALESCE_MAX_FILTERS	4
#define NXPWIFI_MAX_COALESCING_DELAY	100     /* in msecs */

struct filt_field_param {
	u8 operation;
	u8 operand_len;
	u16 offset;
	u8 operand_byte_stream[NXPWIFI_COALESCE_MAX_BYTESEQ];
};

struct nxpwifi_coalesce_rule {
	u16 max_coalescing_delay;
	u8 num_of_fields;
	u8 pkt_type;
	struct filt_field_param params[NXPWIFI_COALESCE_MAX_FILTERS];
};

struct nxpwifi_ds_coalesce_cfg {
	u16 num_of_rules;
	struct nxpwifi_coalesce_rule rule[NXPWIFI_COALESCE_MAX_RULES];
};

#endif /* !_NXPWIFI_IOCTL_H_ */
