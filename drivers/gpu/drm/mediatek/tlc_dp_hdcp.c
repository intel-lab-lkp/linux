// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019-2024 MediaTek Inc.
 */

#include <linux/types.h>
#include <linux/string.h>
#include <linux/slab.h>
#include "tlc_dp_hdcp.h"

#define DEFAULT_WRITE_VAL_LEN 1
#define DEFAULT_WRITE_VAL 0

/*
 * TA_FTPM_UUID: 99975014-3c7c-54ea-8487-a80d215ea92c
 *
 * Randomly generated, and must correspond to the GUID on the TA side.
 * Defined here in the reference implementation:
 * https://github.com/microsoft/ms-tpm-20-ref/blob/master/Samples/ARM32-FirmwareTPM/optee_ta/fTPM/include/fTPM.h#L42
 */
static const uuid_t dp_ta_uuid =
	UUID_INIT(0x99975014, 0x3c7c, 0x54ea,
		  0x84, 0x87, 0xa8, 0x0d, 0x21, 0x5e, 0xa9, 0x2c);

/**
 * dp_tee_op_send() - send dp commands through the TEE shared memory.
 * @len:	the number of bytes to send.
 *
 * Return:
 *	In case of success, returns 0.
 *	On failure, -errno
 */
static int dp_tee_op_send(struct dp_tee_private *dp_tee_priv, size_t len, u32 cmd_id)
{
	int rc;
	u8 *temp_buf;
	struct tee_ioctl_invoke_arg transceive_args;
	struct tee_param command_params[4];
	struct tee_shm *shm = dp_tee_priv->shm;

	if (len > MAX_COMMAND_SIZE) {
		TLCERR("%s: len=%zd exceeds MAX_COMMAND_SIZE supported by dp TA\n", __func__, len);
		return -EIO;
	}

	memset(&transceive_args, 0, sizeof(transceive_args));
	memset(command_params, 0, sizeof(command_params));
	dp_tee_priv->resp_len = 0;

	/* Invoke FTPM_OPTEE_TA_SUBMIT_COMMAND function of dp TA */
	transceive_args = (struct tee_ioctl_invoke_arg) {
		.func = cmd_id,
		.session = dp_tee_priv->session,
		.num_params = 4,
	};

	/* Fill FTPM_OPTEE_TA_SUBMIT_COMMAND parameters */
	command_params[0] = (struct tee_param) {
		.attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT,
		.u.memref = {
			.shm = shm,
			.size = len,
			.shm_offs = 0,
		},
	};

	command_params[1] = (struct tee_param) {
		.attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT,
		.u.memref = {
			.shm = shm,
			.size = MAX_RESPONSE_SIZE,
			.shm_offs = MAX_COMMAND_SIZE,
		},
	};

	rc = tee_client_invoke_func(dp_tee_priv->ctx, &transceive_args, command_params);
	if (rc < 0 || transceive_args.ret != 0) {
		TLCERR("%s: invoke error: 0x%x\n", __func__, transceive_args.ret);
		return (rc < 0) ? rc : transceive_args.ret;
	}

	temp_buf = tee_shm_get_va(shm, command_params[1].u.memref.shm_offs);
	if (IS_ERR(temp_buf)) {
		TLCERR("%s: tee_shm_get_va failed for receive\n", __func__);
		return PTR_ERR(temp_buf);
	}

	/* Sanity checks look good, cache the response */
	memcpy(dp_tee_priv->resp_buf, temp_buf, MAX_RESPONSE_SIZE / 2);
	dp_tee_priv->resp_len = MAX_RESPONSE_SIZE / 2;

	return 0;
}

/*
 * Check whether this driver supports the dp TA in the TEE instance
 * represented by the params (ver/data) to this function.
 */
static int dp_tee_match(struct tee_ioctl_version_data *ver, const void *data)
{
	/*
	 * Currently this driver only support GP Complaint OPTEE based dp TA
	 */
	if (ver->impl_id == TEE_IMPL_ID_OPTEE && ver->gen_caps & TEE_GEN_CAP_GP)
		return 1;
	else
		return 0;
}

int tee_add_device(struct mtk_hdcp_info *hdcp_info, u32 version)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = NULL;
	struct tee_ioctl_open_session_arg sess_arg;
	struct tci_t *tci;

	if (hdcp_info->g_init)
		tee_remove_device(hdcp_info);

	dp_tee_priv = kzalloc(sizeof(*dp_tee_priv), GFP_KERNEL);
	if (!dp_tee_priv) {
		kfree(dp_tee_priv);
		TLCERR("%s: tee_alloc_memory failed\n", __func__);
		return -ENOMEM;
	}
	hdcp_info->g_dp_tee_priv = dp_tee_priv;

	/* Open context with TEE driver */
	dp_tee_priv->ctx = tee_client_open_context(NULL, dp_tee_match, NULL, NULL);
	if (IS_ERR(dp_tee_priv->ctx)) {
		if (PTR_ERR(dp_tee_priv->ctx) == -ENOENT) {
			kfree(dp_tee_priv);
			return -EPROBE_DEFER;
		}
		kfree(dp_tee_priv);
		TLCERR("%s: tee_client_open_context failed\n", __func__);
		return PTR_ERR(dp_tee_priv->ctx);
	}

	/* Open a session with dp TA */
	memset(&sess_arg, 0, sizeof(sess_arg));
	export_uuid(sess_arg.uuid, &dp_ta_uuid);
	sess_arg.clnt_login = TEE_IOCTL_LOGIN_PUBLIC;
	sess_arg.num_params = 0;

	rc = tee_client_open_session(dp_tee_priv->ctx, &sess_arg, NULL);
	if (rc < 0 || sess_arg.ret != 0) {
		kfree(dp_tee_priv);
		TLCERR("tee_client_open_session failed, err=%x\n", sess_arg.ret);
		rc = -EINVAL;
		goto out_tee_session;
	}
	dp_tee_priv->session = sess_arg.session;

	/* Allocate dynamic shared memory with dp TA */
	dp_tee_priv->shm = tee_shm_alloc_kernel_buf(dp_tee_priv->ctx, MAX_COMMAND_SIZE
	 + MAX_RESPONSE_SIZE);
	if (IS_ERR(dp_tee_priv->shm)) {
		kfree(dp_tee_priv);
		TLCERR("%s: tee_shm_alloc_kernel_buf failed\n", __func__);
		rc = -ENOMEM;
		goto out_shm_alloc;
	}
	TLCINFO("Register 8k share memory successfully, (%p)",
		dp_tee_priv->shm->kaddr);

	/* Copy parameter for add new device */
	tci = (struct tci_t *)dp_tee_priv->shm->kaddr;
	memset(tci, 0, TCI_LENGTH);
	tci->command_id = CMD_DEVICE_ADDED;
	tci->cmd_body.cmd_hdcp_init_for_verion.version = version;
	tci->cmd_body.cmd_hdcp_init_for_verion.need_load_key = true;

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_DEVICE_ADDED);
	if (rc != 0) {
		TLCERR("tee_op_send failed, error=%x\n", rc);
		tee_remove_device(hdcp_info);
		return rc;
	}

	hdcp_info->g_init = true;

	return rc;

out_shm_alloc:
	tee_client_close_session(dp_tee_priv->ctx, dp_tee_priv->session);
out_tee_session:
	tee_client_close_context(dp_tee_priv->ctx);

	return rc;
}

void tee_remove_device(struct mtk_hdcp_info *hdcp_info)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;

	if (!hdcp_info->g_init)
		return;

	hdcp_info->g_init = false;
	memset(tci, 0, TCI_LENGTH);
	tci->command_id = CMD_DEVICE_REMOVE;
	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_DEVICE_REMOVE);
	if (rc != 0)
		TLCERR("tee_op_send failed, error=%x\n", rc);

	/* Free the shared memory pool */
	tee_shm_free(dp_tee_priv->shm);

	/* Close the existing session with fTPM TA */
	tee_client_close_session(dp_tee_priv->ctx, dp_tee_priv->session);

	/* Close the context with TEE driver */
	tee_client_close_context(dp_tee_priv->ctx);

	/* Free the memory */
	kfree(dp_tee_priv);
}

int tee_clear_paring(struct mtk_hdcp_info *hdcp_info)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;

	/* Copy parameters */
	memset(tci, 0, TCI_LENGTH);
	tci->command_id = CMD_DEVICE_CLEAN;
	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_DEVICE_CLEAN);
	if (rc != 0) {
		TLCERR("tee_op_send failed, error=%x\n", rc);
		return rc;
	}

	return rc;
}

int tee_hdcp1x_set_tx_an(struct mtk_hdcp_info *hdcp_info, u8 *an_code)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;

	/* Copy parameters */
	tci->command_id = CMD_WRITE_VAL;
	tci->cmd_body.cmd_hdcp_write_val.len = DRM_HDCP_AN_LEN;
	tci->cmd_body.cmd_hdcp_write_val.type = TYPE_HDCP_PARAM_AN;
	memcpy(tci->cmd_body.cmd_hdcp_write_val.val, an_code, DRM_HDCP_AN_LEN);

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_WRITE_VAL);
	if (rc != 0) {
		TLCERR("tee_op_send failed, error=%x\n", rc);
		return rc;
	}

	return rc;
}

int tee_hdcp_enable_encrypt(struct mtk_hdcp_info *hdcp_info, bool enable, u8 version)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;

	/* Copy parameters */
	tci->command_id = CMD_ENABLE_ENCRYPT;
	if (enable)
		tci->cmd_body.cmd_hdcp_write_val.type = TYPE_HDCP_ENABLE_ENCRYPT;
	else
		tci->cmd_body.cmd_hdcp_write_val.type = TYPE_HDCP_DISABLE_ENCRYPT;

	/* Set HDCP version supportted by device */
	tci->cmd_body.cmd_hdcp_write_val.len = 1;
	tci->cmd_body.cmd_hdcp_write_val.val[0] = version;

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_ENABLE_ENCRYPT);
	if (rc != 0) {
		TLCERR("tee_op_send failed, error=%x\n", rc);
		return rc;
	}

	return rc;
}

int tee_hdcp1x_soft_rst(struct mtk_hdcp_info *hdcp_info)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;

	/* Copy parameters */
	tci->command_id = CMD_WRITE_VAL;
	tci->cmd_body.cmd_hdcp_write_val.type = TYPE_HDCP_PARAM_RST_1;
	/* No need input. Set default value 0 for check */
	tci->cmd_body.cmd_hdcp_write_val.len = DEFAULT_WRITE_VAL_LEN;
	memset(tci->cmd_body.cmd_hdcp_write_val.val, DEFAULT_WRITE_VAL, DEFAULT_WRITE_VAL_LEN);

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_WRITE_VAL);
	if (rc != 0) {
		TLCERR("tee_op_send failed, error=%x\n", rc);
		return rc;
	}

	return rc;
}

int tee_hdcp2_soft_rst(struct mtk_hdcp_info *hdcp_info)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;

	tci->command_id = CMD_WRITE_VAL;
	tci->cmd_body.cmd_hdcp_write_val.type = TYPE_HDCP_PARAM_RST_2;
	/* No need input. Set default value 0 for check */
	tci->cmd_body.cmd_hdcp_write_val.len =
		DEFAULT_WRITE_VAL_LEN;
	memset(tci->cmd_body.cmd_hdcp_write_val.val, DEFAULT_WRITE_VAL, DEFAULT_WRITE_VAL_LEN);

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_WRITE_VAL);
	if (rc != 0) {
		TLCERR("tee_op_send failed, error=%x\n", rc);
		return rc;
	}

	return rc;
}

/** V1.X **/
int tee_get_aksv(struct mtk_hdcp_info *hdcp_info, u8 *aksv)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;

	/* Copy parameters */
	tci->command_id = CMD_GET_AKSV;

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_GET_AKSV);
	if (rc != 0) {
		TLCERR("tee_op_send failed, error=%x\n", rc);
		return rc;
	}

	tci = (struct tci_t *)dp_tee_priv->resp_buf;
	memcpy(aksv, tci->cmd_body.cmd_hdcp_get_aksv.aksv, DRM_HDCP_KSV_LEN);

	return rc;
}

int tee_calculate_lm(struct mtk_hdcp_info *hdcp_info, u8 *bksv)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;

	/* Copy parameters */
	tci->command_id = CMD_CALCULATE_LM;
	memcpy(tci->cmd_body.cmd_hdcp_calculate_lm.bksv, bksv, DRM_HDCP_KSV_LEN);

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_CALCULATE_LM);
	if (rc != 0) {
		TLCERR("tee_op_send failed, error=%x\n", rc);
		return rc;
	}

	return rc;
}

int tee_compare_r0(struct mtk_hdcp_info *hdcp_info, u8 *r0, u32 len)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;

	/* Copy parameters */
	tci->command_id = CMD_COMPARE_R0;
	tci->cmd_body.cmd_hdcp_compare.rx_val_len = len;
	memcpy(tci->cmd_body.cmd_hdcp_compare.rx_val, r0, len);

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_COMPARE_R0);
	if (rc != 0) {
		TLCERR("tee_op_send failed, error=%x\n", rc);
		return rc;
	}

	return rc;
}

int tee_hdcp1x_compute_compare_v(struct mtk_hdcp_info *hdcp_info,
				 u8 *crypto_param, u32 param_len, u8 *rx_v)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;

	/* Copy parameters */
	tci->command_id = CMD_COMPARE_V1;
	tci->cmd_body.cmd_hdcp_compare.rx_val_len = 20;
	tci->cmd_body.cmd_hdcp_compare.param_len = param_len;
	memcpy(tci->cmd_body.cmd_hdcp_compare.rx_val, rx_v, 20);
	memcpy(tci->cmd_body.cmd_hdcp_compare.param, crypto_param, param_len);

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_COMPARE_V1);
	if (rc != 0) {
		TLCERR("tee_op_send failed, error=%x\n", rc);
		return rc;
	}

	return rc;
}

/** V2.X **/
int tee_ake_certificate(struct mtk_hdcp_info *hdcp_info,
			u8 *certificate, bool *stored, u8 *out_m, u8 *out_ekm)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;

	/* Copy parameters */
	tci->command_id = CMD_AKE_CERTIFICATE;
	memcpy(tci->cmd_body.cmd_hdcp_ake_certificate.certification,
	       certificate, HDCP2_CERTRX_LEN);

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_AKE_CERTIFICATE);
	if (rc != 0) {
		TLCERR("tee_op_send failed, error=%x\n", rc);
		return rc;
	}

	TLCINFO("verify signature: result %d", rc);
	tci = (struct tci_t *)dp_tee_priv->resp_buf;
	*stored = tci->cmd_body.cmd_hdcp_ake_certificate.stored;
	memcpy(out_m, tci->cmd_body.cmd_hdcp_ake_certificate.m,
	       HDCP_2_2_E_KH_KM_M_LEN - HDCP_2_2_E_KH_KM_LEN);
	memcpy(out_ekm, tci->cmd_body.cmd_hdcp_ake_certificate.ekm, HDCP_2_2_E_KH_KM_LEN);

	return rc;
}

int tee_enc_rsaes_oaep(struct mtk_hdcp_info *hdcp_info, u8 *ekm)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;

	/* Copy parameters */
	tci->command_id = CMD_ENC_KM;

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_ENC_KM);
	if (rc != 0) {
		TLCERR("tee_op_send failed, error=%x\n", rc);
		return rc;
	}

	tci = (struct tci_t *)dp_tee_priv->resp_buf;
	memcpy(ekm, tci->cmd_body.cmd_hdcp_enc_km.enc_km, HDCP_2_2_E_KPUB_KM_LEN);

	return rc;
}

int tee_ake_h_prime(struct mtk_hdcp_info *hdcp_info,
		    u8 *rtx, u8 *rrx, u8 *rx_caps, u8 *tx_caps, u8 *rx_h, u32 rx_h_len)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;

	/* Copy parameters */
	tci->command_id = CMD_AKE_H_PRIME;
	tci->cmd_body.cmd_hdcp_ake_h_prime.rx_h_len = rx_h_len;

	memcpy(tci->cmd_body.cmd_hdcp_ake_h_prime.rtx, rtx, HDCP_2_2_RTX_LEN);
	memcpy(tci->cmd_body.cmd_hdcp_ake_h_prime.rrx, rrx, HDCP_2_2_RRX_LEN);
	memcpy(tci->cmd_body.cmd_hdcp_ake_h_prime.rx_caps, rx_caps, HDCP_2_2_RXCAPS_LEN);
	memcpy(tci->cmd_body.cmd_hdcp_ake_h_prime.tx_caps, tx_caps, HDCP_2_2_TXCAPS_LEN);
	memcpy(tci->cmd_body.cmd_hdcp_ake_h_prime.rx_h, rx_h, rx_h_len);

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_AKE_H_PRIME);
	if (rc != 0) {
		TLCERR("tee_op_send failed, error=%x\n", rc);
		return rc;
	}

	tci = (struct tci_t *)dp_tee_priv->resp_buf;
	return tci->return_code;
}

int tee_ake_paring(struct mtk_hdcp_info *hdcp_info, u8 *rx_ekm)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;

	/* Copy parameters */
	tci->command_id = CMD_AKE_PARING;
	memcpy(tci->cmd_body.cmd_hdcp_ake_paring.ekm, rx_ekm, HDCP_2_2_E_KH_KM_LEN);

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_AKE_PARING);
	if (rc != 0)
		TLCERR("tee_op_send failed, error=%x\n", rc);

	return rc;
}

int tee_lc_l_prime(struct mtk_hdcp_info *hdcp_info, u8 *rn, u8 *rx_l, u32 len)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;

	/* Copy parameters */
	tci->command_id = CMD_LC_L_PRIME;
	memcpy(tci->cmd_body.cmd_hdcp_lc_l_prime.rn, rn, HDCP_2_2_RN_LEN);
	tci->cmd_body.cmd_hdcp_lc_l_prime.rx_l_len = len;
	memcpy(tci->cmd_body.cmd_hdcp_lc_l_prime.rx_l, rx_l, len);

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_LC_L_PRIME);
	if (rc != 0) {
		TLCERR("tee_op_send failed, error=%x\n", rc);
		return rc;
	}

	tci = (struct tci_t *)dp_tee_priv->resp_buf;
	return tci->return_code;
}

int tee_ske_enc_ks(struct mtk_hdcp_info *hdcp_info, u8 *riv, u8 *eks)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;
	u8 *share_buffer = NULL;

	/* Copy parameters */
	tci->command_id = CMD_SKE_CAL_EKS;
	memcpy(tci->cmd_body.cmd_hdcp_ske_eks.riv, riv, HDCP_2_2_RIV_LEN);

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH + 16, CMD_SKE_CAL_EKS);
	if (rc != 0)
		TLCERR("tee_op_send failed, error=%x\n", rc);

	share_buffer = (u8 *)dp_tee_priv->resp_buf;
	memcpy(eks, share_buffer + TCI_LENGTH, 16);

	return rc;
}

int tee_hdcp2_compute_compare_v(struct mtk_hdcp_info *hdcp_info,
				u8 *crypto_param, u32 param_len, u8 *rx_v, u8 *tx_v)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;
	u8 *share_buffer = NULL;

	/* Copy parameters */
	tci->command_id = CMD_COMPARE_V2;
	tci->cmd_body.cmd_hdcp_compare.rx_val_len = 16;
	tci->cmd_body.cmd_hdcp_compare.param_len = param_len;
	memcpy(tci->cmd_body.cmd_hdcp_compare.rx_val, rx_v, 16);
	memcpy(tci->cmd_body.cmd_hdcp_compare.param, crypto_param, param_len);

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_COMPARE_V2);
	if (rc != 0) {
		TLCERR("tee_op_send failed, error=%x\n", rc);
		return rc;
	}

	share_buffer = (u8 *)dp_tee_priv->resp_buf;
	memcpy(tx_v, share_buffer + TCI_LENGTH, 16);

	return rc;
}

int tee_hdcp2_compute_compare_m(struct mtk_hdcp_info *hdcp_info,
				u8 *crypto_param, u32 param_len, u8 *rx_m)
{
	int rc;
	struct dp_tee_private *dp_tee_priv = hdcp_info->g_dp_tee_priv;
	struct tci_t *tci = (struct tci_t *)dp_tee_priv->shm->kaddr;

	/* Copy parameters */
	tci->command_id = CMD_COMPARE_M;
	tci->cmd_body.cmd_hdcp_compare.rx_val_len = HDCP_2_2_MPRIME_LEN;
	tci->cmd_body.cmd_hdcp_compare.param_len = param_len;
	memcpy(tci->cmd_body.cmd_hdcp_compare.rx_val, rx_m, HDCP_2_2_MPRIME_LEN);
	memcpy(tci->cmd_body.cmd_hdcp_compare.param, crypto_param, param_len);

	rc = dp_tee_op_send(dp_tee_priv, TCI_LENGTH, CMD_COMPARE_M);
	if (rc != 0)
		TLCERR("tee_op_send failed, error=%x\n", rc);

	return rc;
}
