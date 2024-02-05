/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019-2024 MediaTek Inc.
 */

#ifndef _TCI_H_
#define _TCI_H_

#include <drm/display/drm_hdcp.h>

#define CMD_DEVICE_ADDED        1
#define CMD_DEVICE_REMOVE       2
#define CMD_WRITE_VAL           3
#define CMD_DEVICE_CLEAN        4
#define CMD_ENABLE_ENCRYPT      5

/* V1.3 */
#define CMD_CALCULATE_LM        11
#define CMD_COMPARE_R0          12
#define CMD_COMPARE_V1          13
#define CMD_GET_AKSV            14

/* V2.2 */
#define CMD_AKE_CERTIFICATE     20
#define CMD_ENC_KM              21
#define CMD_AKE_H_PRIME         22
#define CMD_AKE_PARING          23
#define CMD_LC_L_PRIME          24
#define CMD_COMPARE_L           25
#define CMD_SKE_CAL_EKS         26

#define CMD_COMPARE_V2          27
#define CMD_COMPARE_M           28

/* Need remove in furture */
#define CMD_LOAD_KEY            50

#define RET_COMPARE_PASS 0
#define RET_COMPARE_FAIL 1
#define RET_NEW_DEVICE 2
#define RET_STORED_DEVICE 3

#define TYPE_HDCP_PARAM_AN 10
#define TYPE_HDCP_PARAM_RST_1 11
#define TYPE_HDCP_PARAM_RST_2 12
#define TYPE_HDCP_ENABLE_ENCRYPT 13
#define TYPE_HDCP_DISABLE_ENCRYPT 14

#define TYPE_HDCP13_KEY 20
#define TYPE_HDCP22_KEY 21

// reserved:2
#define HDCP2_CERTRX_LEN (HDCP_2_2_RECEIVER_ID_LEN + HDCP_2_2_K_PUB_RX_LEN + \
	2 + HDCP_2_2_DCP_LLC_SIG_LEN)
// version:1
#define HDCP_2_2_TXCAPS_LEN (HDCP_2_2_TXCAP_MASK_LEN + 1)
#define PARAM_LEN 1024

#define TCI_LENGTH sizeof(struct tci_t)

struct cmd_hdcp_init_for_verion_t {
	u32 version;
	bool need_load_key;
};

struct cmd_hdcp_write_val_t {
	u8 type;
	u8 len;
	u8 val[DRM_HDCP_AN_LEN];
};

struct cmd_hdcp_calculate_lm_t {
	u8 bksv[DRM_HDCP_KSV_LEN];
};

struct cmd_hdcp_get_aksv_t {
	u8 aksv[DRM_HDCP_KSV_LEN];
};

struct cmd_hdcp_ake_certificate_t {
	u8 certification[HDCP2_CERTRX_LEN];
	bool stored;
	u8 m[HDCP_2_2_E_KH_KM_M_LEN - HDCP_2_2_E_KH_KM_LEN];
	u8 ekm[HDCP_2_2_E_KH_KM_LEN];
};

struct cmd_hdcp_ake_paring_t {
	u8 ekm[HDCP_2_2_E_KH_KM_LEN];
};

struct cmd_hdcp_enc_km_t {
	u8 enc_km[HDCP_2_2_E_KPUB_KM_LEN];
};

struct cmd_hdcp_ake_h_prime_t {
	u8 rtx[HDCP_2_2_RTX_LEN];
	u8 rrx[HDCP_2_2_RRX_LEN];
	u8 rx_caps[HDCP_2_2_RXCAPS_LEN];
	u8 tx_caps[HDCP_2_2_TXCAPS_LEN];
	u32 rx_h_len;
	u8 rx_h[HDCP_2_2_H_PRIME_LEN];
};

struct cmd_hdcp_lc_l_prime_t {
	u8 rn[HDCP_2_2_RN_LEN];
	u32 rx_l_len;
	u8 rx_l[HDCP_2_2_L_PRIME_LEN];
};

struct cmd_hdcp_ske_eks_t {
	u8 riv[HDCP_2_2_RIV_LEN];
	u32 eks_len;
	u32 eks;
};

struct cmd_hdcp_compare_t {
	u32 rx_val_len;
	u8 rx_val[HDCP_2_2_MPRIME_LEN];
	u32 param_len;
	u8 param[PARAM_LEN];
	u32 out_len;
	u32 out;
};

union tci_cmd_body_t {
	/* Init with special HDCP version */
	struct cmd_hdcp_init_for_verion_t cmd_hdcp_init_for_verion;
	/* Write uint32 data to hw */
	struct cmd_hdcp_write_val_t cmd_hdcp_write_val;
	/* Get aksv */
	struct cmd_hdcp_get_aksv_t cmd_hdcp_get_aksv;
	/* Calculate r0 */
	struct cmd_hdcp_calculate_lm_t cmd_hdcp_calculate_lm;
	/* Generate signature for certificate */
	struct cmd_hdcp_ake_certificate_t cmd_hdcp_ake_certificate;
	/* To store ekm */
	struct cmd_hdcp_ake_paring_t cmd_hdcp_ake_paring;
	/* Encrypt km for V2.2 */
	struct cmd_hdcp_enc_km_t cmd_hdcp_enc_km;
	/* Compute H prime */
	struct cmd_hdcp_ake_h_prime_t cmd_hdcp_ake_h_prime;
	/* Compute L prime */
	struct cmd_hdcp_lc_l_prime_t cmd_hdcp_lc_l_prime;
	/* Compute eks */
	struct cmd_hdcp_ske_eks_t cmd_hdcp_ske_eks;
	/* Compare */
	struct cmd_hdcp_compare_t cmd_hdcp_compare;
} __packed;

struct tci_t {
	u32 command_id;
	u32 return_code;
	union tci_cmd_body_t cmd_body;
};

#endif /* _TCI_H_ */
