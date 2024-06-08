/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019-2024 MediaTek Inc.
 */

#ifndef _TLC_DP_HDCP_H_
#define _TLC_DP_HDCP_H_

#include <linux/printk.h>
#include <linux/tee_drv.h>
#include <linux/uuid.h>
#include <linux/types.h>
#include <linux/sched/clock.h>
#include <drm/display/drm_dp_helper.h>
#include "tci.h"

/* HDCP version definitions */
#define HDCP_NONE                0x0 /* No HDCP supported, no secure data path */
#define HDCP_V1                  0x1 /* HDCP version 1.0 */
#define HDCP_V2                  0x2 /* HDCP version 2.0 Type 1 */
#define HDCP_V2_3                0x5 /* HDCP version 2.3 Type 1 */

/* Max buffer size supported by dp */
#define MAX_COMMAND_SIZE       4096
#define MAX_RESPONSE_SIZE      MAX_COMMAND_SIZE

#define HDCP1X_REP_MAXDEVS 128
#define HDCP1X_V_LEN 20
#define HDCP1X_B_INFO_LEN 2

#define HDCP2_K_LEN 2
#define HDCP2_STREAMID_TYPE_LEN 2

enum hdcp_result {
	AUTH_ZERO = 0,
	AUTH_INIT = 2,
	AUTH_PASS = 3,
	AUTH_FAIL = 4,
};

struct dp_tee_private {
	u32 session;
	size_t resp_len;
	u8 resp_buf[MAX_RESPONSE_SIZE];
	struct tee_context *ctx;
	struct tee_shm *shm;
};

struct hdcp2_info_tx {
	struct hdcp2_ake_init ake_init;
	struct hdcp2_ake_no_stored_km no_stored_km;
	struct hdcp2_ske_send_eks send_eks;
	struct hdcp2_lc_init lc_init;
	struct hdcp2_rep_stream_manage stream_manage;
	struct hdcp2_rep_send_ack send_ack;
	struct hdcp2_tx_caps tx_caps;
	u8 k[HDCP2_K_LEN];
	u8 stream_id_type[HDCP2_STREAMID_TYPE_LEN];
};

struct hdcp2_info_rx {
	struct hdcp2_cert_rx cert_rx;
	struct hdcp2_ake_send_cert send_cert;
	struct hdcp2_rep_send_receiverid_list recvid_list;
	struct hdcp2_ake_send_pairing_info pairing_info;
	struct hdcp2_rep_stream_ready stream_ready;
	struct hdcp2_ake_send_hprime send_hprime;
	struct hdcp2_lc_send_lprime send_lprime;
};

struct hdcp1x_info {
	bool capable;
	bool repeater;
	u8 device_count;
	u8 v[HDCP1X_V_LEN];
	u8 b_ksv[DRM_HDCP_KSV_LEN];
	u8 a_ksv[DRM_HDCP_KSV_LEN];
	u8 b_info[HDCP1X_B_INFO_LEN];
	u8 ksvfifo[DRM_HDCP_KSV_LEN * (HDCP1X_REP_MAXDEVS - 1)];
};

struct hdcp2_info {
	bool capable;
	bool repeater;
	bool stored_km;
	u8 device_count;
	u8 stream_id_type;
	u32 seq_num_v_cnt;
	atomic_t cp_irq;
	int cp_irq_cached;
	wait_queue_head_t cp_irq_queue;
	struct hdcp2_info_tx hdcp_tx;
	struct hdcp2_info_rx hdcp_rx;
	struct hdcp2_ake_stored_km ake_stored_km;
};

struct mtk_hdcp_info {
	bool g_init;
	u8 auth_status;
	u8 auth_version;
	u32 hdcp_content_type;
	u32 content_protection;
	struct hdcp2_info hdcp2_info;
	struct hdcp1x_info hdcp1x_info;
	struct dp_tee_private *g_dp_tee_priv;
};

int tee_add_device(struct mtk_hdcp_info *hdcp_info, u32 version);
void tee_remove_device(struct mtk_hdcp_info *hdcp_info);
int tee_clear_paring(struct mtk_hdcp_info *hdcp_info);
int tee_calculate_lm(struct mtk_hdcp_info *hdcp_info, u8 *bksv);
int tee_get_aksv(struct mtk_hdcp_info *hdcp_info, u8 *aksv);
int tee_compare_r0(struct mtk_hdcp_info *hdcp_info, u8 *r0, u32 len);
int tee_hdcp1x_compute_compare_v(struct mtk_hdcp_info *hdcp_info,
				 u8 *crypto_param, u32 param_len, u8 *rx_v);
int tee_hdcp1x_set_tx_an(struct mtk_hdcp_info *hdcp_info, u8 *an_code);
int tee_hdcp1x_soft_rst(struct mtk_hdcp_info *hdcp_info);
int tee_hdcp2_soft_rst(struct mtk_hdcp_info *hdcp_info);
int tee_hdcp_enable_encrypt(struct mtk_hdcp_info *hdcp_info, bool enable, u8 version);
int tee_ake_certificate(struct mtk_hdcp_info *hdcp_info,
			u8 *certificate, bool *stored, u8 *out_m, u8 *out_ekm);
int tee_enc_rsaes_oaep(struct mtk_hdcp_info *hdcp_info, u8 *ekm);
int tee_ake_h_prime(struct mtk_hdcp_info *hdcp_info,
		    u8 *rtx, u8 *rrx, u8 *rx_caps, u8 *tx_caps, u8 *rx_h, u32 rx_h_len);
int tee_ake_paring(struct mtk_hdcp_info *hdcp_info, u8 *rx_ekm);
int tee_lc_l_prime(struct mtk_hdcp_info *hdcp_info, u8 *rn, u8 *rx_l, u32 len);

int tee_ske_enc_ks(struct mtk_hdcp_info *hdcp_info, u8 *riv, u8 *eks);
int tee_hdcp2_compute_compare_v(struct mtk_hdcp_info *hdcp_info,
				u8 *crypto_param, u32 param_len, u8 *rx_v, u8 *tx_v);
int tee_hdcp2_compute_compare_m(struct mtk_hdcp_info *hdcp_info,
				u8 *crypto_param, u32 param_len, u8 *rx_m);
#endif /* _TLC_DP_HDCP_H_ */
