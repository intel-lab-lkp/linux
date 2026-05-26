// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (C) 2025 Amlogic, Inc. All rights reserved
 */
#include <linux/tee_drv.h>
#include <linux/firmware.h>

#include "aml_vdec_tee_fw.h"
#include "aml_vdec_drv.h"

#define VIDEO_DEC_H264_MULTI	15

#define CORE_VDEC_LEGENCY	0

#define FIRMWARE_PATH		"video_ucode.bin"
#define ONCE_SENT_SIZE		(1024 * 128)
#define UCODE_HEADER_SIZE	(1024 * 32)

#define TEEC_SUCCESS		0x0
#define TEEC_ERROR_BUSY	0xffff000d
#define FIRMWARE_CMD_PROCESS	0

#define TEE_SMC_FUNCID_LOAD_VIDEO_FW	15
#define TEE_SMC_LOAD_VIDEO_FW \
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_32, \
			ARM_SMCCC_OWNER_TRUSTED_OS, TEE_SMC_FUNCID_LOAD_VIDEO_FW)

#define PTA_LOAD_FW UUID_INIT(0x526fc4fc, 0x7ee6, 0x4a12, \
			0x96, 0xe3, 0x83, 0xda, 0x95, 0x65, 0xbc, 0xe8)

#define TEE_PARAM_NUM	4

static struct aml_tee_fw firmware[] = {
	[CODEC_TYPE_H264] = {
		.fw_format = VIDEO_DEC_H264_MULTI,
		.core = CORE_VDEC_LEGENCY,
		.is_swap = 1,
	},
};

static int optee_ctx_match(struct tee_ioctl_version_data *ver, const void *data)
{
	return (ver->impl_id == TEE_IMPL_ID_OPTEE);
}

static void prepare_tee_grgs(size_t firmware_size, struct tee_param *param0,
			     struct tee_param *param1)
{
	memset(param0, 0, TEE_PARAM_NUM * sizeof(*param0));
	memset(param1, 0, TEE_PARAM_NUM * sizeof(*param1));

	param0[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param0[0].u.value.a = firmware_size;

	param0[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;

	param0[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;
	param0[3].attr = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;

	param1[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	param1[0].u.memref.size = firmware_size;

	param1[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;
	param1[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;
	param1[3].attr = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;
}

static int tee_pta_invoke_cmd(struct aml_vdec_hw *hw, struct tee_context *ctx,
			      uuid_t uuid, u32 cmd, void *firmware_data,
			      struct tee_param *param_init,
			      struct tee_param *param_invoke)
{
	int ret = 0;
	struct tee_ioctl_open_session_arg sess_arg = { 0 };
	struct tee_ioctl_invoke_arg inv_arg = { 0 };
	u32 sent_size = 0;
	u32 fw_size = 0;
	struct tee_shm *shm = NULL;
	void *shm_va = NULL;

	fw_size = param_invoke[0].u.memref.size;

	shm = tee_shm_alloc_kernel_buf(ctx, ONCE_SENT_SIZE);
	if (IS_ERR(shm)) {
		dev_info(hw->dev, "Failed to allocate shared memory size %d\n",
			 ONCE_SENT_SIZE);
		ret = PTR_ERR(shm);
		goto out;
	}

	shm_va = tee_shm_get_va(shm, 0);
	if (IS_ERR(shm_va)) {
		dev_info(hw->dev, "Failed to get VA for shared memory\n");
		ret = PTR_ERR(shm_va);
		goto free_shm;
	}

	/* Open session */
	memcpy(sess_arg.uuid, uuid.b, TEE_IOCTL_UUID_LEN);
	sess_arg.clnt_login = TEE_IOCTL_LOGIN_PUBLIC;
	sess_arg.num_params = TEE_PARAM_NUM;
	ret = tee_client_open_session(ctx, &sess_arg, param_init);
	if (ret < 0 || sess_arg.ret != TEEC_SUCCESS) {
		dev_info(hw->dev,
			 "%s open session failed, cmd = %u, ret = %d, res = 0x%x, origin = 0x%x\n",
			 __func__, cmd, ret, sess_arg.ret, sess_arg.ret_origin);
		ret = sess_arg.ret;
		goto free_shm;
	}

	inv_arg.func = cmd;
	inv_arg.session = sess_arg.session;
	inv_arg.num_params = TEE_PARAM_NUM;

	while (sent_size < fw_size) {
		memset(shm_va, 0, ONCE_SENT_SIZE);
		if (fw_size - sent_size > ONCE_SENT_SIZE) {
			memcpy(shm_va, (firmware_data + sent_size),
			       ONCE_SENT_SIZE);
			param_invoke[0].u.memref.size = ONCE_SENT_SIZE;
		} else {
			memcpy(shm_va, (firmware_data + sent_size),
			       fw_size - sent_size);
			param_invoke[0].u.memref.size = (fw_size - sent_size);
		}
		param_invoke[0].u.memref.shm = shm;
		ret = tee_client_invoke_func(ctx, &inv_arg, param_invoke);
		if (ret < 0 || (inv_arg.ret != TEEC_SUCCESS && inv_arg.ret != TEEC_ERROR_BUSY)) {
			dev_info(hw->dev,
				 "%s invoke func failed, cmd = %u, ret= %d, res = 0x%x, origin = 0x%x\n",
				 __func__, cmd, ret, inv_arg.ret,
				 inv_arg.ret_origin);
			ret = inv_arg.ret;
			goto close_session;
		}
		sent_size += param_invoke[0].u.memref.size;
	}
close_session:
	tee_client_close_session(ctx, sess_arg.session);
free_shm:
	tee_shm_free(shm);
out:
	return ret;
}

int load_firmware(struct aml_vdec_hw *hw, u32 type)
{
	int ret = -1;
	struct aml_tee_fw *video_fw;

	if (type >= CODEC_TYPE_FRAME) {
		dev_info(hw->dev, "codec type %d invalid\n", type);
		return ret;
	}
	video_fw = &firmware[type];

	meson_sm_call(hw->sec_fw, SM_LOAD_VIDEO_FW, &ret,
		      video_fw->fw_format, video_fw->core,
		      video_fw->is_swap, 0, 0);
	if (ret < 0)
		dev_err(hw->dev, "loading fw type %d core %d, ret %x\n",
			video_fw->fw_format, video_fw->core, ret);

	return ret;
}

static int get_firmware(const char *path, void **data, size_t *size)
{
	const struct firmware *fw = NULL;
	int ret;
	void *buf;

	ret = request_firmware(&fw, FIRMWARE_PATH, NULL);
	if (ret)
		return ret;

	/* get rid of the first 32K bytes plaintext */
	buf = kzalloc((fw->size - UCODE_HEADER_SIZE), GFP_KERNEL);
	if (!buf) {
		release_firmware(fw);
		return -ENOMEM;
	}

	memcpy(buf, fw->data + UCODE_HEADER_SIZE, fw->size - UCODE_HEADER_SIZE);
	release_firmware(fw);

	*data = buf;
	*size = fw->size - UCODE_HEADER_SIZE;

	return 0;
}

static int pass_firmware_to_tee(struct aml_vdec_hw *hw)
{
	int ret;
	struct tee_context *ctx = NULL;
	uuid_t uuid = PTA_LOAD_FW;
	struct tee_param param_init[TEE_PARAM_NUM];
	struct tee_param param_invoke[TEE_PARAM_NUM];
	void *firmware_data;
	size_t firmware_size;

	ret = get_firmware(FIRMWARE_PATH, &firmware_data, &firmware_size);
	if (ret) {
		dev_info(hw->dev, "Failed get firmware %s from FS\n",
			 FIRMWARE_PATH);
		return ret;
	}

	ctx = tee_client_open_context(NULL, optee_ctx_match, NULL, NULL);
	if (IS_ERR(ctx)) {
		dev_info(hw->dev, "Failed to open TEE context\n");
		ret = PTR_ERR(ctx);
		goto free_firmware;
	}

	prepare_tee_grgs(firmware_size, param_init, param_invoke);

	ret = tee_pta_invoke_cmd(hw, ctx, uuid, FIRMWARE_CMD_PROCESS,
				 firmware_data, param_init, param_invoke);
	if (ret)
		dev_info(hw->dev, "TEE firmware processing failed, ret = %d\n",
			 ret);

	tee_client_close_context(ctx);
free_firmware:
	kfree(firmware_data);
	return ret;
}

int aml_tee_fw_preload(struct aml_vdec_hw *hw)
{
	int ret;

	ret = pass_firmware_to_tee(hw);
	if (ret)
		dev_err(hw->dev, "Failed to preload firmware via TEE\n");

	return ret;
}
