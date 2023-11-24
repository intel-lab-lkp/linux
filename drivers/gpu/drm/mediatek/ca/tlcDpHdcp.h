/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019-2023 MediaTek Inc.
 */

#ifndef _TLCDPHDCP_H_
#define _TLCDPHDCP_H_

#include <linux/printk.h>
#include <linux/tee_drv.h>
#include <linux/uuid.h>
#include "tci.h"
#include "tlDPHdcpCMD.h"
#include "mtk_dp_hdcp.h"

#define TLCINFO(string, args...) pr_info("[TLC_HDCP]info: "string, ##args)
#define TLCERR(string, args...) pr_info("[TLC_HDCP]line:%d,err:%s:"string,\
	__LINE__, __func__, ##args)

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

#endif /* _TLCDPHDCP_H_ */
