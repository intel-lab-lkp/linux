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

static void mtk_vcodec_dec_optee_deinit_memref(struct mtk_vdec_optee_ca_info *ca_info,
					       enum mtk_vdec_optee_data_index data_index)
{
	tee_shm_free(ca_info->shm_memref[data_index].msg_shm);
}

static int mtk_vcodec_dec_optee_init_memref(struct tee_context *tee_vdec_ctx,
					    struct mtk_vdec_optee_ca_info *ca_info,
					    enum mtk_vdec_optee_data_index data_index)
{
	struct mtk_vdec_optee_shm_memref *shm_memref;
	int alloc_size = 0, err = 0;
	u64 shm_param_type = 0;
	bool copy_buffer;

	switch (data_index) {
	case OPTEE_MSG_INDEX:
		shm_param_type = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
		alloc_size = MTK_VDEC_OPTEE_MSG_SIZE;
		copy_buffer = true;
		break;
	case OPTEE_DATA_INDEX:
		shm_param_type = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
		alloc_size = MTK_VDEC_OPTEE_HW_SIZE;
		copy_buffer = false;
		break;
	default:
		pr_err(MTK_DBG_VCODEC_STR "tee invalid data_index: %d.\n", data_index);
		return -EINVAL;
	}

	shm_memref = &ca_info->shm_memref[data_index];

	/* Allocate dynamic shared memory with decoder TA */
	shm_memref->msg_shm_size = alloc_size;
	shm_memref->param_type = shm_param_type;
	shm_memref->copy_to_ta = copy_buffer;
	shm_memref->msg_shm = tee_shm_alloc_kernel_buf(tee_vdec_ctx, shm_memref->msg_shm_size);
	if (IS_ERR(shm_memref->msg_shm)) {
		pr_err(MTK_DBG_VCODEC_STR "tee alloc buf fail: data_index:%d.\n", data_index);
		return -ENOMEM;
	}

	shm_memref->msg_shm_ca_buf = tee_shm_get_va(shm_memref->msg_shm, 0);
	if (IS_ERR(shm_memref->msg_shm_ca_buf)) {
		pr_err(MTK_DBG_VCODEC_STR "tee get shm va fail: data_index:%d.\n", data_index);
		err = PTR_ERR(shm_memref->msg_shm_ca_buf);
		goto err_get_msg_va;
	}

	return err;
err_get_msg_va:
	tee_shm_free(shm_memref->msg_shm);
	return err;
}

static int mtk_vcodec_dec_optee_init_hw_info(struct mtk_vdec_optee_private *optee_private,
					     enum mtk_vdec_hw_id hardware_index)
{
	struct device *dev = &optee_private->vcodec_dev->plat_dev->dev;
	struct tee_ioctl_open_session_arg session_arg;
	struct mtk_vdec_optee_ca_info *ca_info;
	int err, i, j, session_func;

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

	/* Allocate dynamic shared memory with decoder TA */
	for (i = 0; i < OPTEE_MAX_INDEX; i++) {
		err = mtk_vcodec_dec_optee_init_memref(optee_private->tee_vdec_ctx, ca_info, i);
		if (err) {
			dev_err(dev, MTK_DBG_VCODEC_STR "init vdec memref failed: %d.\n", i);
			goto err_init_memref;
		}
	}

	return err;
err_init_memref:
	if (i != 0) {
		for (j = 0; j < i; j++)
			mtk_vcodec_dec_optee_deinit_memref(ca_info, j);
	}

	tee_client_close_session(optee_private->tee_vdec_ctx, ca_info->vdec_session_id);

	return err;
}

static void mtk_vcodec_dec_optee_deinit_hw_info(struct mtk_vdec_optee_private *optee_private,
						enum mtk_vdec_hw_id hw_id)
{
	struct mtk_vdec_optee_ca_info *ca_info;
	int i;

	if (hw_id == MTK_VDEC_LAT0)
		ca_info = &optee_private->lat_ca;
	else
		ca_info = &optee_private->core_ca;

	for (i = 0; i < OPTEE_MAX_INDEX; i++)
		mtk_vcodec_dec_optee_deinit_memref(ca_info, i);

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

static int mtk_vcodec_dec_optee_fill_shm(struct tee_param *command_params,
					 struct mtk_vdec_optee_shm_memref *shm_memref,
					 struct mtk_vdec_optee_data_to_shm *data,
					 int index, struct device *dev)
{
	if (!data->msg_buf_size[index] || !data->msg_buf[index]) {
		pr_err(MTK_DBG_VCODEC_STR "tee invalid buf param: %d.\n", index);
		return -EINVAL;
	}

	*command_params = (struct tee_param) {
		.attr = shm_memref->param_type,
		.u.memref = {
			.shm = shm_memref->msg_shm,
			.size = data->msg_buf_size[index],
			.shm_offs = 0,
		},
	};

	if (!shm_memref->copy_to_ta) {
		dev_dbg(dev, MTK_DBG_VCODEC_STR "share memref data: 0x%x param_type:%llu.\n",
			*((unsigned int *)shm_memref->msg_shm_ca_buf), shm_memref->param_type);
		return 0;
	}

	memset(shm_memref->msg_shm_ca_buf, 0, shm_memref->msg_shm_size);
	memcpy(shm_memref->msg_shm_ca_buf, data->msg_buf[index], data->msg_buf_size[index]);

	dev_dbg(dev, MTK_DBG_VCODEC_STR "share memref data => msg id:0x%x 0x%x param_type:%llu.\n",
		*((unsigned int *)data->msg_buf[index]),
		*((unsigned int *)shm_memref->msg_shm_ca_buf),
		shm_memref->param_type);

	return 0;
}

void mtk_vcodec_dec_optee_set_data(struct mtk_vdec_optee_data_to_shm *data,
				   void *buf, int buf_size,
				   enum mtk_vdec_optee_data_index index)
{
	data->msg_buf[index] = buf;
	data->msg_buf_size[index] = buf_size;
}
EXPORT_SYMBOL_GPL(mtk_vcodec_dec_optee_set_data);

int mtk_vcodec_dec_optee_invokd_cmd(struct mtk_vdec_optee_private *optee_private,
				    enum mtk_vdec_hw_id hw_id,
				    struct mtk_vdec_optee_data_to_shm *data)
{
	struct device *dev = &optee_private->vcodec_dev->plat_dev->dev;
	struct tee_ioctl_invoke_arg trans_args;
	struct tee_param command_params[MTK_OPTEE_MAX_TEE_PARAMS];
	struct mtk_vdec_optee_ca_info *ca_info;
	struct mtk_vdec_optee_shm_memref *shm_memref;
	int ret, index;

	if (hw_id == MTK_VDEC_LAT0)
		ca_info = &optee_private->lat_ca;
	else
		ca_info = &optee_private->core_ca;

	memset(&trans_args, 0, sizeof(trans_args));
	memset(command_params, 0, sizeof(command_params));

	trans_args = (struct tee_ioctl_invoke_arg) {
		.func = ca_info->vdec_session_func,
		.session = ca_info->vdec_session_id,
		.num_params = MTK_OPTEE_MAX_TEE_PARAMS,
	};

	/* Fill msg command parameters */
	for (index = 0; index < MTK_OPTEE_MAX_TEE_PARAMS; index++) {
		shm_memref = &ca_info->shm_memref[index];

		if (shm_memref->param_type == TEE_IOCTL_PARAM_ATTR_TYPE_NONE ||
		    data->msg_buf_size[index] == 0)
			continue;

		dev_dbg(dev, MTK_DBG_VCODEC_STR "tee share memory data size: %d -> %d.\n",
			data->msg_buf_size[index], shm_memref->msg_shm_size);

		if (data->msg_buf_size[index] > shm_memref->msg_shm_size) {
			dev_err(dev, MTK_DBG_VCODEC_STR "tee buf size big than shm (%d -> %d).\n",
				data->msg_buf_size[index], shm_memref->msg_shm_size);
			return -EINVAL;
		}

		ret = mtk_vcodec_dec_optee_fill_shm(&command_params[index], shm_memref,
						    data, index, dev);
		if (ret)
			return ret;
	}

	ret = tee_client_invoke_func(optee_private->tee_vdec_ctx, &trans_args, command_params);
	if (ret < 0 || trans_args.ret != 0) {
		dev_err(dev, MTK_DBG_VCODEC_STR "tee submit command fail: 0x%x 0x%x.\n",
			trans_args.ret, ret);
		return (ret < 0) ? ret : trans_args.ret;
	}

	/* clear all attrs, set all command param to unused */
	for (index = 0; index < MTK_OPTEE_MAX_TEE_PARAMS; index++) {
		data->msg_buf[index] = NULL;
		data->msg_buf_size[index] = 0;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_vcodec_dec_optee_invokd_cmd);

void *mtk_vcodec_dec_get_shm_buffer_va(struct mtk_vdec_optee_private *optee_private,
				       enum mtk_vdec_hw_id hw_id,
				       enum mtk_vdec_optee_data_index data_index)
{
	struct mtk_vdec_optee_ca_info *ca_info;

	if (hw_id == MTK_VDEC_LAT0)
		ca_info = &optee_private->lat_ca;
	else
		ca_info = &optee_private->core_ca;

	return ca_info->shm_memref[data_index].msg_shm_ca_buf;
}
EXPORT_SYMBOL_GPL(mtk_vcodec_dec_get_shm_buffer_va);

int mtk_vcodec_dec_get_shm_buffer_size(struct mtk_vdec_optee_private *optee_private,
				       enum mtk_vdec_hw_id hw_id,
				       enum mtk_vdec_optee_data_index data_index)
{
	struct mtk_vdec_optee_ca_info *ca_info;

	if (hw_id == MTK_VDEC_LAT0)
		ca_info = &optee_private->lat_ca;
	else
		ca_info = &optee_private->core_ca;

	return ca_info->shm_memref[data_index].msg_shm_size;
}
EXPORT_SYMBOL_GPL(mtk_vcodec_dec_get_shm_buffer_size);
