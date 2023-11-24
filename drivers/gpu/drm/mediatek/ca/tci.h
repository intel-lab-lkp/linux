/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019-2023 MediaTek Inc.
 */

#ifndef _TCI_H_
#define _TCI_H_

#define RET_COMPARE_PASS 0
#define RET_COMPARE_FAIL 1
#define RET_NEW_DEVICE 2
#define RET_STORED_DEVICE 3

#define AN_LEN 8
#define AKSV_LEN 5
#define BKSV_LEN 5
#define CERT_LEN 522
#define EKM_LEN 16
#define M_LEN 16
#define ENC_KM_LEN 128
#define RXX_LEN 8
#define CAPS_LEN 3
#define RN_LEN 8
#define RIV_LEN 8

#define TYPE_HDCP_PARAM_AN 10
#define TYPE_HDCP_PARAM_RST_1 11
#define TYPE_HDCP_PARAM_RST_2 12
#define TYPE_HDCP_ENABLE_ENCRYPT 13
#define TYPE_HDCP_DISABLE_ENCRYPT 14

#define TYPE_HDCP13_KEY 20
#define TYPE_HDCP22_KEY 21

#define TCI_LENGTH sizeof(struct tci_t)

struct cryptokeys_t {
	u8 type;
	u32 len;
	u32 key;
};

struct cmd_hdcp_init_for_verion_t {
	u32 version;
	bool need_load_key;
};

struct cmd_hdcp_write_val_t {
	u8 type;
	u8 len;
	u32 val;
};

struct cmd_hdcp_calculate_lm_t {
	u8 bksv[BKSV_LEN];
};

struct cmd_hdcp_get_aksv_t {
	u8 aksv[AKSV_LEN];
};

struct cmd_hdcp_sha1_t {
	u32 message_len;
	u32 message_addr;
};

struct cmd_hdcp_ake_certificate_t {
	u8 certification[CERT_LEN];
	bool stored;
	u8 m[M_LEN];
	u8 ekm[EKM_LEN];
};

struct cmd_hdcp_ake_paring_t {
	u8 ekm[EKM_LEN];
};

struct cmd_hdcp_enc_km_t {
	u8 enc_km[ENC_KM_LEN];
};

struct cmd_hdcp_ake_h_prime_t {
	u8 rtx[RXX_LEN];
	u8 rrx[RXX_LEN];
	u8 rx_caps[CAPS_LEN];
	u8 tx_caps[CAPS_LEN];
	u32 rx_h_len;
	u32 rx_h;
};

struct cmd_hdcp_lc_l_prime_t {
	u8 rn[RN_LEN];
	u32 rx_l_len;
	u32 rx_l;
};

struct cmd_hdcp_ske_eks_t {
	u8 riv[RIV_LEN];
	u32 eks_len;
	u32 eks;
};

struct cmd_hdcp_compare_t {
	u32 rx_val_len;
	u32 rx_val;
	u32 param_len;
	u32 param;
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
