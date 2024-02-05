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

#define TLCINFO(string, args...) pr_info("[TLC_HDCP]info: "string, ##args)
#define TLCERR(string, args...) pr_info("[TLC_HDCP]line:%d,err:%s:"string,\
	__LINE__, __func__, ##args)

#define DPTXHDCPFUNC(fmt, arg...)		\
	pr_info("[DPTXHDCP][%s line:%d]"pr_fmt(fmt), __func__, __LINE__, ##arg)
#define DPTXHDCPMSG(fmt, arg...)                                  \
		pr_info("[DPTXHDCP]"pr_fmt(fmt), ##arg)
#define DPTXHDCPERR(fmt, arg...)                                  \
		pr_err("[DPTXHDCP]"pr_fmt(fmt), ##arg)

#define RET_SUCCESS 0

/**
 * hdcp version definitions
 */
#define HDCP_NONE                0x0 // No HDCP supported, no secure data path
#define HDCP_V1                  0x1 // HDCP version 1.0
#define HDCP_V2                  0x2 // HDCP version 2.0 Type 1
#define HDCP_V2_1                0x3 // HDCP version 2.1 Type 1
#define HDCP_V2_2                0x4 // HDCP version 2.2 Type 1
#define HDCP_V2_3                0x5 // HDCP version 2.3 Type 1

/* Local display only(content required version use only) */
#define HDCP_LOCAL_DISPLAY_ONLY  0xf
#define HDCP_NO_DIGITAL_OUTPUT   0xff // No digital output
#define HDCP_DEFAULT             HDCP_NO_DIGITAL_OUTPUT // Default value

#define HDCP_VERSION_1X 1
#define HDCP_VERSION_2X 2

/* max. buffer size supported by dp  */
#define MAX_COMMAND_SIZE       4096
#define MAX_RESPONSE_SIZE      4096

#define HDCP1X_REP_MAXDEVS 128
#define HDCP1X_V_LEN 20
#define HDCP1X_B_INFO_LEN 2

#define HDCP2_K_LEN 2
#define HDCP2_STREAMID_TYPE_LEN 2

enum HDCP_RESULT {
	AUTH_ZERO = 0,
	AUTH_PREPARE = 1,
	AUTH_INIT = 2,
	AUTH_PASS = 3,
	AUTH_FAIL = 4,
};

struct dp_header {
	__be16 tag;
	__be32 length;
	union {
		__be32 ordinal;
		__be32 return_code;
	};
} __packed;

/**
 * struct dp_tee_private - fTPM's private data
 * @session:  dp TA session identifier.
 * @resp_len: cached response buffer length.
 * @resp_buf: cached response buffer.
 * @ctx:      TEE context handler.
 * @shm:      Memory pool shared with fTPM TA in TEE.
 */
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
	struct hdcp2_rep_send_receiverid_list receiverid_list;
	struct hdcp2_ake_send_pairing_info pairing_info;
	struct hdcp2_rep_stream_ready stream_ready;
	struct hdcp2_ake_send_hprime send_hprime;
	struct hdcp2_lc_send_lprime send_lprime;
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

struct hdcp1x_info {
	bool enable;
	bool repeater;
	bool r0_read;
	bool ksv_ready;
	bool max_cascade;
	bool max_devs;
	u8 b_status;
	u8 b_ksv[DRM_HDCP_KSV_LEN];
	u8 a_ksv[DRM_HDCP_KSV_LEN];
	u8 v[HDCP1X_V_LEN];
	u8 b_info[HDCP1X_B_INFO_LEN];
	u8 ksvfifo[DRM_HDCP_KSV_LEN * (HDCP1X_REP_MAXDEVS - 1)];
	u8 device_count;
	u8 retry_count;
	int main_states;
	int sub_states;
};

struct hdcp2_info {
	struct hdcp2_info_tx hdcp_tx;
	struct hdcp2_info_rx hdcp_rx;
	struct hdcp2_ake_stored_km ake_stored_km;
	struct hdcp2_handler hdcp_handler;
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
	u32 hdcp_content_type;
	u32 content_protection;
	struct dp_tee_private *g_dp_tee_priv;
	struct hdcp1x_info hdcp1x_info;
	struct hdcp2_info hdcp2_info;
};

#ifdef __cplusplus
extern "C"
{
#endif

/*
 *Description:
 *  A device connect and do some initializations.
 *
 *Input:
 *  version: HDCP version
 *
 *Returns:
 *  TEEC_SUCCESS success*
 */
int tee_add_device(struct mtk_hdcp_info *hdcp_info, u32 version);

/*
 *Description:
 *  Device disconnect.
 *
 *Returns:
 *  N/A
 */
void tee_remove_device(struct mtk_hdcp_info *hdcp_info);

/*
 *Description:
 *  Clearing paring info.
 *
 *Returns:
 *  TEEC_SUCCESS success*
 */
int tee_clear_paring(struct mtk_hdcp_info *hdcp_info);

/*
 *Description:
 *  Calculate Km base on Bksv and write it to HW.
 *
 *Input:
 *  bksv[5] input
 *
 *Returns:
 *  TEEC_SUCCESS success
 */
int tee_calculate_lm(struct mtk_hdcp_info *hdcp_info, u8 *bksv);

/*
 *Description:
 *  Get Aksv from TEE.
 *
 *Output:
 *  aksv[5]
 *
 *Returns:
 *  TEEC_SUCCESS success
 */
int tee_get_aksv(struct mtk_hdcp_info *hdcp_info, u8 *aksv);

/*
 *Description:
 *  Get r0 from HW and compare to rx_r0.
 *
 *Parameters:
 *  r0[len] input
 *
 *Returns:
 *  TEEC_SUCCESS success
 */
int tee_compare_r0(struct mtk_hdcp_info *hdcp_info, u8 *r0, u32 len);

/*
 *Description:
 *  Compute and compare v value.
 *
 *Input:
 *  crypto_param[param_len] params used to calculate
 *  rx_v[20] v value from rx
 *
 *Returns:
 *  RET_COMPARE_PASS verify pass
 */
int tee_hdcp1x_compute_compare_v(struct mtk_hdcp_info *hdcp_info,
				 u8 *crypto_param, u32 param_len, u8 *rx_v);

/*
 *Description:
 *  Write An to HW.
 *
 *Input:
 *  an_code[8]
 *
 *Returns:
 *  TEEC_SUCCESS success
 */
int tee_hdcp1x_set_tx_an(struct mtk_hdcp_info *hdcp_info, u8 *an_code);

/*
 *Description:
 *  Write RST to HW.
 *
 *Returns:
 *  TEEC_SUCCESS success
 */
int tee_hdcp1x_soft_rst(struct mtk_hdcp_info *hdcp_info);
int tee_hdcp2_soft_rst(struct mtk_hdcp_info *hdcp_info);

/*
 *Description:
 *  Set enable or disable to HW.
 *
 *Returns:
 *  TEEC_SUCCESS success
 */
int tee_hdcp_enable_encrypt(struct mtk_hdcp_info *hdcp_info, bool enable, u8 version);

/*
 *Description:
 *  AKE cetificate verify.
 *
 *Input:
 *  certificate[522]: cert use to calculate
 *output:
 *  stored: whether be stored before
 *  out_m[16]
 *  out_ekm[16]
 *
 *Returns:
 *  TEEC_SUCCESS success*
 */
int tee_ake_certificate(struct mtk_hdcp_info *hdcp_info,
			u8 *certificate, bool *stored, u8 *out_m, u8 *out_ekm);

/*
 *Description:
 *  Encrypt km.
 *
 *Output:
 *  ekm[128]: encrypted km
 *
 *Returns:
 *  TEEC_SUCCESS success*
 */
int tee_enc_rsaes_oaep(struct mtk_hdcp_info *hdcp_info, u8 *ekm);

/*
 *Description:
 *  Calculate h prime and compare to rx_h
 *
 *Input:
 *  rtx[8]
 *  rrx[8]
 *  rx_caps[3]
 *  tx_caps[3]
 *  rx_h[rx_h_len]
 *
 *Returns:
 *  RET_COMPARE_PASS: compare pass
 */
int tee_ake_h_prime(struct mtk_hdcp_info *hdcp_info,
		    u8 *rtx, u8 *rrx, u8 *rx_caps, u8 *tx_caps, u8 *rx_h, u32 rx_h_len);

/*
 *Description:
 *  Store paring info.
 *
 *Input:
 *  rx_ekm[16]
 *
 *Returns:
 *  TEEC_SUCCESS success*
 */
int tee_ake_paring(struct mtk_hdcp_info *hdcp_info, u8 *rx_ekm);

/*
 *Description:
 *  Calculate l prime and compare.
 *
 *Input:
 *  rn[8]
 *  rx_l[len]
 *
 *Returns:
 *  RET_COMPARE_PASS compare pass
 */
int tee_lc_l_prime(struct mtk_hdcp_info *hdcp_info, u8 *rn, u8 *rx_l, u32 len);

/*
 *Description:
 *  Encrypt ks
 *  Write contentkey and riv to hw
 *
 *Input:
 *  riv[8]
 *Output:
 *  eks[16]
 *
 *Returns:
 *  TEEC_SUCCESS success*
 */
int tee_ske_enc_ks(struct mtk_hdcp_info *hdcp_info, u8 *riv, u8 *eks);

/*
 *Description:
 *  Calculate and compare v prime for repeater.
 *
 *Input:
 *  crypto_param[param_len] params used to calculate
 *  rx_v[16] v value from rx
 *Output:
 *  tx_v[16]
 *
 *Returns:
 *  TEEC_SUCCESS success*
 */
int tee_hdcp2_compute_compare_v(struct mtk_hdcp_info *hdcp_info,
				u8 *crypto_param, u32 param_len, u8 *rx_v, u8 *tx_v);

/*
 *Description:
 *  Calculate and compare m prime for repeater.
 *
 *Input:
 *  crypto_param[param_len] params used to calculate
 *  rx_m[32] m value from rx
 *
 *Returns:
 *  RET_COMPARE_PASS verify pass
 */
int tee_hdcp2_compute_compare_m(struct mtk_hdcp_info *hdcp_info,
				u8 *crypto_param, u32 param_len, u8 *rx_m);

#ifdef __cplusplus
}
#endif

#endif /* _TLC_DP_HDCP_H_ */
