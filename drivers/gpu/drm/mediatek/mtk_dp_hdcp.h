/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019-2023 MediaTek Inc.
 */

#ifndef _MTK_DP_HDCP_H_
#define _MTK_DP_HDCP_H_

#include <linux/types.h>
#include <drm/display/drm_dp_helper.h>
#include <linux/sched/clock.h>

#define HDCP2_RXSTATUS_SIZE              1
#define HDCP2_RTX_SIZE                   8
#define HDCP2_RRX_SIZE                   8
#define HDCP2_TXCAPS_SIZE                3
#define HDCP2_RXCAPS_SIZE                3
#define HDCP2_M_SIZE                     16
#define HDCP2_KD_SIZE                    32
#define HDCP2_CERTRX_SIZE                522
#define HDCP2_EKPUBKM_SIZE               128
#define HDCP2_EKHKM_SIZE                 16
#define HDCP2_KM_SIZE                    16
#define HDCP2_KH_SIZE                    16
#define HDCP2_RN_SIZE                    8
#define HDCP2_LPRIME_SIZE                32
#define HDCP2_EDKEYKS_SIZE               16
#define HDCP2_KS_SIZE                    16
#define HDCP2_RIV_SIZE                   8
#define HDCP2_RXINFO_SIZE                2
#define HDCP2_SEQ_NUM_V_SIZE             3
#define HDCP2_RECVID_SIZE                5
#define HDCP2_VPRIME_SIZE                16
#define HDCP2_RECV_ID_LIST_SIZE          155
#define HDCP2_SEQ_NUM_M_SIZE             3
#define HDCP2_STREAMID_TYPE_SIZE         2
#define HDCP2_K_SIZE                     2
#define HDCP2_REP_MPRIME_SIZE            32
#define HDCP2_HPRIME_SIZE                32
#define HDCP2_RX_ENCKEY_SIZE             104
#define HDCP2_TX_ENCKEY_SIZE             448
#define HDCP2_LC128_SIZE                 16
#define HDCP2_KPUBRX_MODULUS_SIZE        128
#define HDCP2_KPUBRX_EXPONENT_SIZE       3
#define HDCP2_KPUBRX_RESERVED_SIZE       2
#define HDCP2_MAX_DEVICE_COUNT           31
#define HDCP2_MAX_DEPTH_LEVEL            4
#define HDCP2_KPUB_SIZE                  384

struct hdcp2_info_tx {
	u8 rtx[HDCP2_RTX_SIZE];
	u8 tx_caps[HDCP2_TXCAPS_SIZE];
	u8 ekpub_km[HDCP2_EKPUBKM_SIZE];
	u8 eks[HDCP2_EDKEYKS_SIZE];
	u8 v_prime[HDCP2_VPRIME_SIZE];
	u8 rn[HDCP2_RN_SIZE];
	u8 riv[HDCP2_RIV_SIZE];
	u8 seq_num_m[HDCP2_SEQ_NUM_M_SIZE];
	u8 k[HDCP2_K_SIZE];
	u8 stream_id_type[HDCP2_STREAMID_TYPE_SIZE];
};

struct hdcp2_info_rx {
	u8 cert[HDCP2_CERTRX_SIZE];
	u8 rrx[HDCP2_RRX_SIZE];
	u8 rx_caps[HDCP2_RXCAPS_SIZE];
	u8 rx_info[HDCP2_RXINFO_SIZE];
	u8 ekh_km[HDCP2_EKHKM_SIZE];
	u8 v_prime[HDCP2_VPRIME_SIZE];
	u8 m_prime[HDCP2_REP_MPRIME_SIZE];
	u8 h_prime[HDCP2_HPRIME_SIZE];
	u8 l_prime[HDCP2_LPRIME_SIZE];
	u8 recv_id_list[HDCP2_MAX_DEVICE_COUNT * HDCP2_RECVID_SIZE];
	u8 seq_num_v[HDCP2_SEQ_NUM_V_SIZE];
};

struct hdcp2_handler {
	u8 main_state;
	u8 sub_state;
	u8 down_stream_dev_cnt;
	u8 hdcp_rx_ver;
	bool send_ake_init:1;
	bool get_recv_id_list:1;
	bool stored_km:1;
	bool send_lc_init:1;
	bool send_ack:1;
	bool sink_is_repeater:1;
	bool recv_msg:1;
	bool send_pair:1;
	u32 seq_num_v_cnt;
	u32 retry_cnt;
};

struct hdcp2_pairing_info {
	u8 m[HDCP2_M_SIZE];
	u8 ekh_km[HDCP2_EKHKM_SIZE];
};

struct hdcp1x_info {
	bool enable;
	bool repeater;
	bool r0_read;
	bool ksv_ready;
	bool max_cascade;
	bool max_devs;
	u8 b_status;
	u8 b_ksv[5];
	u8 a_ksv[5];
	u8 v[20];
	u8 b_info[2];
	u8 ksvfifo[5 * 127];
	u8 device_count;
	u8 retry_count;
	int main_states;
	int sub_states;
};

struct hdcp2_info {
	struct hdcp2_info_tx hdcp_tx;
	struct hdcp2_info_rx hdcp_rx;
	struct hdcp2_handler hdcp_handler;
	struct hdcp2_pairing_info stored_pairing_info;
	bool enable;
	bool repeater;
	bool read_certrx;
	bool read_h_prime;
	bool read_pairing;
	bool read_l_prime;
	bool ks_exchange_done;
	bool read_v_prime;
	u8 retry_count;
	u8 device_count;
	u8 stream_id_type;
};

struct mtk_hdcp_info {
	u8 auth_status;
	bool g_init;
	struct dp_tee_private *g_dp_tee_priv;
	struct drm_dp_aux *aux;
	struct regmap *regs;
	struct hdcp1x_info hdcp1x_info;
	struct hdcp2_info hdcp2_info;
};

enum HDCP_RESULT {
	AUTH_ZERO = 0,
	AUTH_PREPARE = 1,
	AUTH_INIT = 2,
	AUTH_PASS = 3,
	AUTH_FAIL = 4,
};

#endif /* _MTK_DP_HDCP_H_ */
