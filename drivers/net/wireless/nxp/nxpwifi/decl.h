/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * NXP Wireless LAN device driver: generic data structures and APIs
 *
 * Copyright 2011-2024 NXP
 */

#ifndef _NXPWIFI_DECL_H_
#define _NXPWIFI_DECL_H_

#undef pr_fmt
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/wait.h>
#include <linux/timer.h>
#include <linux/ieee80211.h>
#include <uapi/linux/if_arp.h>
#include <net/cfg80211.h>

#define NXPWIFI_BSS_COEX_COUNT	     2
#define NXPWIFI_MAX_BSS_NUM         (2)

#define NXPWIFI_MAX_CSA_COUNTERS     5

#define NXPWIFI_DMA_ALIGN_SZ	    64
#define NXPWIFI_RX_HEADROOM	    64
#define MAX_TXPD_SZ		    32
#define INTF_HDR_ALIGN		     4
/* special FW 4 address management header */
#define NXPWIFI_MIN_DATA_HEADER_LEN (NXPWIFI_DMA_ALIGN_SZ + INTF_HDR_ALIGN + \
				     MAX_TXPD_SZ)

#define NXPWIFI_MGMT_FRAME_HEADER_SIZE	8	/* sizeof(pkt_type)
						 *   + sizeof(tx_control)
						 */

#define FRMCTL_LEN                2
#define DURATION_LEN              2
#define SEQCTL_LEN                2
#define NXPWIFI_MGMT_HEADER_LEN   (FRMCTL_LEN + FRMCTL_LEN + ETH_ALEN + \
				   ETH_ALEN + ETH_ALEN + SEQCTL_LEN + ETH_ALEN)

#define AUTH_ALG_LEN              2
#define AUTH_TRANSACTION_LEN      2
#define AUTH_STATUS_LEN           2
#define NXPWIFI_AUTH_BODY_LEN     (AUTH_ALG_LEN + AUTH_TRANSACTION_LEN + \
				   AUTH_STATUS_LEN)

#define HOST_MLME_AUTH_PENDING    BIT(0)
#define HOST_MLME_AUTH_DONE       BIT(1)

#define HOST_MLME_MGMT_MASK       (BIT(IEEE80211_STYPE_AUTH >> 4) | \
				   BIT(IEEE80211_STYPE_DEAUTH >> 4) | \
				   BIT(IEEE80211_STYPE_DISASSOC >> 4))

#define AUTH_TX_DEFAULT_WAIT_TIME 2400

#define WLAN_AUTH_NONE            0xFFFF

#define NXPWIFI_MAX_TX_BASTREAM_SUPPORTED	2
#define NXPWIFI_MAX_RX_BASTREAM_SUPPORTED	16

#define NXPWIFI_STA_AMPDU_DEF_TXWINSIZE        64
#define NXPWIFI_STA_AMPDU_DEF_RXWINSIZE        64
#define NXPWIFI_STA_COEX_AMPDU_DEF_RXWINSIZE   16

#define NXPWIFI_UAP_AMPDU_DEF_TXWINSIZE        32

#define NXPWIFI_UAP_COEX_AMPDU_DEF_RXWINSIZE   16

#define NXPWIFI_UAP_AMPDU_DEF_RXWINSIZE        16
#define NXPWIFI_11AC_STA_AMPDU_DEF_TXWINSIZE   64
#define NXPWIFI_11AC_STA_AMPDU_DEF_RXWINSIZE   64
#define NXPWIFI_11AC_UAP_AMPDU_DEF_TXWINSIZE   64
#define NXPWIFI_11AC_UAP_AMPDU_DEF_RXWINSIZE   64

#define NXPWIFI_DEFAULT_BLOCK_ACK_TIMEOUT  0xffff

#define NXPWIFI_RATE_BITMAP_MCS0   32

#define NXPWIFI_RX_DATA_BUF_SIZE     (4 * 1024)
#define NXPWIFI_RX_CMD_BUF_SIZE	     (2 * 1024)

#define MAX_BEACON_PERIOD                  (4000)
#define MIN_BEACON_PERIOD                  (50)
#define MAX_DTIM_PERIOD                    (100)
#define MIN_DTIM_PERIOD                    (1)

#define NXPWIFI_RTS_MIN_VALUE              (0)
#define NXPWIFI_RTS_MAX_VALUE              (2347)
#define NXPWIFI_FRAG_MIN_VALUE             (256)
#define NXPWIFI_FRAG_MAX_VALUE             (2346)
#define NXPWIFI_WMM_VERSION                0x01
#define NXPWIFI_WMM_SUBTYPE                0x01

#define NXPWIFI_RETRY_LIMIT                14
#define NXPWIFI_SDIO_BLOCK_SIZE            256

#define NXPWIFI_BUF_FLAG_REQUEUED_PKT      BIT(0)
#define NXPWIFI_BUF_FLAG_BRIDGED_PKT	   BIT(1)
#define NXPWIFI_BUF_FLAG_EAPOL_TX_STATUS   BIT(3)
#define NXPWIFI_BUF_FLAG_ACTION_TX_STATUS  BIT(4)
#define NXPWIFI_BUF_FLAG_AGGR_PKT          BIT(5)

#define NXPWIFI_BRIDGED_PKTS_THR_HIGH      1024
#define NXPWIFI_BRIDGED_PKTS_THR_LOW        128

/* 54M rates, index from 0 to 11 */
#define NXPWIFI_RATE_INDEX_MCS0 12
/* 12-27=MCS0-15(BW20) */
#define NXPWIFI_BW20_MCS_NUM 15

/* Rate index for OFDM 0 */
#define NXPWIFI_RATE_INDEX_OFDM0   4

#define NXPWIFI_MAX_STA_NUM		3
#define NXPWIFI_MAX_UAP_NUM		3

#define NXPWIFI_A_BAND_START_FREQ	5000

/* SDIO Aggr data packet special info */
#define SDIO_MAX_AGGR_BUF_SIZE		(256 * 255)
#define BLOCK_NUMBER_OFFSET		15
#define SDIO_HEADER_OFFSET		28

#define NXPWIFI_SIZE_4K 0x4000

enum nxpwifi_bss_type {
	NXPWIFI_BSS_TYPE_STA = 0,
	NXPWIFI_BSS_TYPE_UAP = 1,
	NXPWIFI_BSS_TYPE_ANY = 0xff,
};

enum nxpwifi_bss_role {
	NXPWIFI_BSS_ROLE_STA = 0,
	NXPWIFI_BSS_ROLE_UAP = 1,
	NXPWIFI_BSS_ROLE_ANY = 0xff,
};

#define BSS_ROLE_BIT_MASK    BIT(0)

#define GET_BSS_ROLE(priv)   ((priv)->bss_role & BSS_ROLE_BIT_MASK)

enum nxpwifi_data_frame_type {
	NXPWIFI_DATA_FRAME_TYPE_ETH_II = 0,
	NXPWIFI_DATA_FRAME_TYPE_802_11,
};

struct nxpwifi_fw_image {
	u8 *helper_buf;
	u32 helper_len;
	u8 *fw_buf;
	u32 fw_len;
};

struct nxpwifi_802_11_ssid {
	u32 ssid_len;
	u8 ssid[IEEE80211_MAX_SSID_LEN];
};

struct nxpwifi_wait_queue {
	wait_queue_head_t wait;
	int status;
};

struct nxpwifi_rxinfo {
	struct sk_buff *parent;
	u8 bss_num;
	u8 bss_type;
	u8 use_count;
	u8 buf_type;
};

struct nxpwifi_txinfo {
	u8 flags;
	u8 bss_num;
	u8 bss_type;
	u8 aggr_num;
	u32 pkt_len;
	u8 ack_frame_id;
	u64 cookie;
};

enum nxpwifi_wmm_ac_e {
	WMM_AC_BK,
	WMM_AC_BE,
	WMM_AC_VI,
	WMM_AC_VO
} __packed;

struct ieee_types_wmm_ac_parameters {
	u8 aci_aifsn_bitmap;
	u8 ecw_bitmap;
	__le16 tx_op_limit;
} __packed;

struct nxpwifi_types_wmm_info {
	u8 oui[4];
	u8 subtype;
	u8 version;
	u8 qos_info;
	u8 reserved;
	struct ieee_types_wmm_ac_parameters ac_params[IEEE80211_NUM_ACS];
} __packed;

struct nxpwifi_arp_eth_header {
	struct arphdr hdr;
	u8 ar_sha[ETH_ALEN];
	u8 ar_sip[4];
	u8 ar_tha[ETH_ALEN];
	u8 ar_tip[4];
} __packed;

struct nxpwifi_chan_stats {
	u8 chan_num;
	u8 bandcfg;
	u8 flags;
	s8 noise;
	u16 total_bss;
	u16 cca_scan_dur;
	u16 cca_busy_dur;
} __packed;

#define NXPWIFI_HIST_MAX_SAMPLES	1048576
#define NXPWIFI_MAX_RX_RATES		     44
#define NXPWIFI_MAX_AC_RX_RATES		     74
#define NXPWIFI_MAX_SNR			    256
#define NXPWIFI_MAX_NOISE_FLR		    256
#define NXPWIFI_MAX_SIG_STRENGTH	    256

struct nxpwifi_histogram_data {
	atomic_t rx_rate[NXPWIFI_MAX_AC_RX_RATES];
	atomic_t snr[NXPWIFI_MAX_SNR];
	atomic_t noise_flr[NXPWIFI_MAX_NOISE_FLR];
	atomic_t sig_str[NXPWIFI_MAX_SIG_STRENGTH];
	atomic_t num_samples;
};

struct nxpwifi_iface_comb {
	u8 sta_intf;
	u8 uap_intf;
};

struct nxpwifi_radar_params {
	struct cfg80211_chan_def *chandef;
	u32 cac_time_ms;
} __packed;

struct nxpwifi_11h_intf_state {
	bool is_11h_enabled;
	bool is_11h_active;
} __packed;

#define NXPWIFI_FW_DUMP_IDX		0xff
#define NXPWIFI_FW_DUMP_MAX_MEMSIZE     0x160000
#define NXPWIFI_DRV_INFO_IDX		20
#define FW_DUMP_MAX_NAME_LEN		8
#define FW_DUMP_HOST_READY      0xEE
#define FW_DUMP_DONE			0xFF
#define FW_DUMP_READ_DONE		0xFE

struct memory_type_mapping {
	u8 mem_name[FW_DUMP_MAX_NAME_LEN];
	u8 *mem_ptr;
	u32 mem_size;
	u8 done_flag;
};

enum rdwr_status {
	RDWR_STATUS_SUCCESS = 0,
	RDWR_STATUS_FAILURE = 1,
	RDWR_STATUS_DONE = 2
};

enum nxpwifi_chan_band {
	BAND_2GHZ = 0,
	BAND_5GHZ,
	BAND_6GHZ,
	BAND_4GHZ,
};

enum nxpwifi_chan_width {
	CHAN_BW_20MHZ = 0,
	CHAN_BW_10MHZ,
	CHAN_BW_40MHZ,
	CHAN_BW_80MHZ,
	CHAN_BW_8080MHZ,
	CHAN_BW_160MHZ,
	CHAN_BW_5MHZ,
};

enum nxpwifi_chan_offset {
	SEC_CHAN_NONE = 0,
	SEC_CHAN_ABOVE = 1,
	SEC_CHAN_5MHZ = 2,
	SEC_CHAN_BELOW = 3
};

#endif /* !_NXPWIFI_DECL_H_ */
