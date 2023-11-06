// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 * Author: Yunfei Dong <yunfei.dong@mediatek.com>
 */

#include "mtk_vcodec_dec_drv.h"
#include "mtk_vcodec_dec_optee.h"

/*
 * Randomly generated, and must correspond to the GUID on the TA side.
 */
static const uuid_t mtk_vdec_lat_uuid =
	UUID_INIT(0xBC50D971, 0xD4C9, 0x42C4,
		  0x82, 0xCB, 0x34, 0x3F, 0xB7, 0xF3, 0x78, 0x90);

static const uuid_t mtk_vdec_core_uuid =
	UUID_INIT(0xBC50D971, 0xD4C9, 0x42C4,
		  0x82, 0xCB, 0x34, 0x3F, 0xB7, 0xF3, 0x78, 0x91);

/*
 * Check whether this driver supports decoder TA in the TEE instance,
 * represented by the params (ver/data) of this function.
 */
static int mtk_vcodec_dec_optee_match(struct tee_ioctl_version_data *ver_data, const void *not_used)
{
	if (ver_data->impl_id == TEE_IMPL_ID_OPTEE)
		return 1;
	else
		return 0;
}

int mtk_vcodec_dec_optee_private_init(struct mtk_vcodec_dec_dev *vcodec_dev)
{
	vcodec_dev->optee_private = devm_kzalloc(&vcodec_dev->plat_dev->dev,
						 sizeof(*vcodec_dev->optee_private),
						 GFP_KERNEL);
	if (!vcodec_dev->optee_private)
		return -ENOMEM;

	vcodec_dev->optee_private->vcodec_dev = vcodec_dev;

	atomic_set(&vcodec_dev->optee_private->tee_active_cnt, 0);
	mutex_init(&vcodec_dev->optee_private->tee_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_vcodec_dec_optee_private_init);

static int mtk_vcodec_dec_optee_init_hw_info(struct mtk_vdec_optee_private *optee_private,
					     enum mtk_vdec_hw_id hardware_index)
{
	struct device *dev = &optee_private->vcodec_dev->plat_dev->dev;
	struct tee_ioctl_open_session_arg session_arg;
	struct mtk_vdec_optee_ca_info *ca_info;
	int err = 0, session_func;

	/* Open lat and core session with vdec TA. */
	switch (hardware_index) {
	case MTK_VDEC_LAT0:
		export_uuid(session_arg.uuid, &mtk_vdec_lat_uuid);
		session_func = MTK_VDEC_OPTEE_TA_LAT_SUBMIT_COMMAND;
		ca_info = &optee_private->lat_ca;
		break;
	case MTK_VDEC_CORE:
		export_uuid(session_arg.uuid, &mtk_vdec_core_uuid);
		session_func = MTK_VDEC_OPTEE_TA_CORE_SUBMIT_COMMAND;
		ca_info = &optee_private->core_ca;
		break;
	default:
		return -EINVAL;
	}

	session_arg.clnt_login = TEE_IOCTL_LOGIN_PUBLIC;
	session_arg.num_params = 0;

	err = tee_client_open_session(optee_private->tee_vdec_ctx, &session_arg, NULL);
	if (err < 0 || session_arg.ret != 0) {
		dev_err(dev, MTK_DBG_VCODEC_STR "open vdec tee session fail hw_id:%d err:%x.\n",
			hardware_index, session_arg.ret);
		return -EINVAL;
	}
	ca_info->vdec_session_id = session_arg.session;
	ca_info->hw_id = hardware_index;
	ca_info->vdec_session_func = session_func;

	dev_dbg(dev, MTK_DBG_VCODEC_STR "open vdec tee session hw_id:%d session_id=%x.\n",
		hardware_index, ca_info->vdec_session_id);

	return err;
}

static void mtk_vcodec_dec_optee_deinit_hw_info(struct mtk_vdec_optee_private *optee_private,
						enum mtk_vdec_hw_id hw_id)
{
	struct mtk_vdec_optee_ca_info *ca_info;

	if (hw_id == MTK_VDEC_LAT0)
		ca_info = &optee_private->lat_ca;
	else
		ca_info = &optee_private->core_ca;

	tee_client_close_session(optee_private->tee_vdec_ctx, ca_info->vdec_session_id);
}

int mtk_vcodec_dec_optee_open(struct mtk_vdec_optee_private *optee_private)
{
	struct device *dev = &optee_private->vcodec_dev->plat_dev->dev;
	int ret;

	mutex_lock(&optee_private->tee_mutex);
	if (atomic_inc_return(&optee_private->tee_active_cnt) > 1) {
		mutex_unlock(&optee_private->tee_mutex);
		dev_dbg(dev, MTK_DBG_VCODEC_STR "already init vdec optee private data!\n");
		return 0;
	}

	/* Open context with TEE driver */
	optee_private->tee_vdec_ctx = tee_client_open_context(NULL, mtk_vcodec_dec_optee_match,
							      NULL, NULL);
	if (IS_ERR(optee_private->tee_vdec_ctx)) {
		dev_err(dev, MTK_DBG_VCODEC_STR "optee vdec tee context failed.\n");
		ret = PTR_ERR(optee_private->tee_vdec_ctx);
		goto err_ctx_open;
	}

	ret = mtk_vcodec_dec_optee_init_hw_info(optee_private, MTK_VDEC_LAT0);
	if (ret < 0)
		goto err_lat_init;

	if (IS_VDEC_LAT_ARCH(optee_private->vcodec_dev->vdec_pdata->hw_arch)) {
		ret = mtk_vcodec_dec_optee_init_hw_info(optee_private, MTK_VDEC_CORE);
		if (ret < 0)
			goto err_core_init;
	}

	mutex_unlock(&optee_private->tee_mutex);
	return 0;
err_core_init:
	mtk_vcodec_dec_optee_deinit_hw_info(optee_private, MTK_VDEC_LAT0);
err_lat_init:
	tee_client_close_context(optee_private->tee_vdec_ctx);
err_ctx_open:

	mutex_unlock(&optee_private->tee_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_vcodec_dec_optee_open);

void mtk_vcodec_dec_optee_release(struct mtk_vdec_optee_private *optee_private)
{
	mutex_lock(&optee_private->tee_mutex);
	if (!atomic_dec_and_test(&optee_private->tee_active_cnt)) {
		mutex_unlock(&optee_private->tee_mutex);
		return;
	}

	mtk_vcodec_dec_optee_deinit_hw_info(optee_private, MTK_VDEC_LAT0);
	if (IS_VDEC_LAT_ARCH(optee_private->vcodec_dev->vdec_pdata->hw_arch))
		mtk_vcodec_dec_optee_deinit_hw_info(optee_private, MTK_VDEC_CORE);

	tee_client_close_context(optee_private->tee_vdec_ctx);
	mutex_unlock(&optee_private->tee_mutex);
}
EXPORT_SYMBOL_GPL(mtk_vcodec_dec_optee_release);
