/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 MediaTek Inc.
 * Author: Yunfei Dong <yunfei.dong@mediatek.com>
 */

#ifndef _MTK_VCODEC_DEC_OPTEE_H_
#define _MTK_VCODEC_DEC_OPTEE_H_

#include <linux/tee_drv.h>
#include <linux/uuid.h>

#include "mtk_vcodec_dec_drv.h"

/* The TA ID implemented in this TA */
#define MTK_VDEC_OPTEE_TA_LAT_SUBMIT_COMMAND  (0x10)
#define MTK_VDEC_OPTEE_TA_CORE_SUBMIT_COMMAND  (0x20)

#define MTK_OPTEE_MAX_TEE_PARAMS 4

/**
 * struct mtk_vdec_optee_ca_info - ca related param
 * @vdec_session_id:   optee TA session identifier.
 * @hw_id:             hardware index.
 * @vdec_session_func: trusted application function id used specific to the TA.
 */
struct mtk_vdec_optee_ca_info {
	u32 vdec_session_id;
	enum mtk_vdec_hw_id hw_id;
	u32 vdec_session_func;
};

/**
 * struct mtk_vdec_optee_private - optee private data
 * @vcodec_dev:     pointer to the mtk_vcodec_dev of the device
 * @tee_vdec_ctx:   decoder TEE context handler.
 * @lat_ca:         lat hardware information used to communicate with TA.
 * @core_ca:        core hardware information used to communicate with TA.
 *
 * @tee_active_cnt: used to mark whether need to init optee
 * @tee_mutex:      mutex lock used for optee
 */
struct mtk_vdec_optee_private {
	struct mtk_vcodec_dec_dev *vcodec_dev;
	struct tee_context *tee_vdec_ctx;

	struct mtk_vdec_optee_ca_info lat_ca;
	struct mtk_vdec_optee_ca_info core_ca;

	atomic_t tee_active_cnt;
	/* mutext used to lock optee open and release information. */
	struct mutex tee_mutex;
};

/**
 * mtk_vcodec_dec_optee_open - setup the communication channels with TA.
 * @optee_private: optee private context
 */
int mtk_vcodec_dec_optee_open(struct mtk_vdec_optee_private *optee_private);

/**
 * mtk_vcodec_dec_optee_private_init - init optee parameters.
 * @vcodec_dev: pointer to the mtk_vcodec_dev of the device
 */
int mtk_vcodec_dec_optee_private_init(struct mtk_vcodec_dec_dev *vcodec_dev);

/**
 * mtk_vcodec_dec_optee_release - close the communication channels with TA.
 * @optee_private: optee private context
 */
void mtk_vcodec_dec_optee_release(struct mtk_vdec_optee_private *optee_private);

#endif /* _MTK_VCODEC_FW_OPTEE_H_ */
