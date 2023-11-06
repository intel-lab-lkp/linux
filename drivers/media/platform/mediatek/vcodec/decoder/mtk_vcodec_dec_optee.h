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

#define MTK_VDEC_OPTEE_MSG_SIZE     128
#define MTK_VDEC_OPTEE_HW_SIZE      (8 * SZ_1K)

/**
 * struct mtk_vdec_optee_shm_memref - share memory reference params
 * @msg_shm:        message shared with TA in TEE.
 * @msg_shm_ca_buf: ca buffer.
 *
 * @msg_shm_size:   share message size.
 * @param_type:     each tee param types.
 * @copy_to_ta:     need to copy data from ca to share memory.
 */
struct mtk_vdec_optee_shm_memref {
	struct tee_shm *msg_shm;
	u8 *msg_shm_ca_buf;

	u32 msg_shm_size;
	u64 param_type;
	bool copy_to_ta;
};

/**
 * struct mtk_vdec_optee_ca_info - ca related param
 * @vdec_session_id:   optee TA session identifier.
 * @hw_id:             hardware index.
 * @vdec_session_func: trusted application function id used specific to the TA.
 * @shm_memref:        share memory reference params.
 */
struct mtk_vdec_optee_ca_info {
	u32 vdec_session_id;
	enum mtk_vdec_hw_id hw_id;
	u32 vdec_session_func;
	struct mtk_vdec_optee_shm_memref shm_memref[MTK_OPTEE_MAX_TEE_PARAMS];
};

/*
 * enum mtk_vdec_optee_data_index - used to indentify each share memory information
 */
enum mtk_vdec_optee_data_index {
	OPTEE_MSG_INDEX = 0,
	OPTEE_DATA_INDEX,
	OPTEE_MAX_INDEX,
};

/**
 * struct mtk_vdec_optee_data_to_shm - shm data used for TA
 * @msg_buf:     msg information to TA.
 * @msg_buf_len: length of msg information.
 */
struct mtk_vdec_optee_data_to_shm {
	void *msg_buf[MTK_OPTEE_MAX_TEE_PARAMS];
	int msg_buf_size[MTK_OPTEE_MAX_TEE_PARAMS];
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

/**
 * mtk_vcodec_dec_optee_set_data - set buffer to share memref.
 * @vcodec_dev: normal world data used to init optee share memory
 * @buf: normal world buffer address
 * @buf_size: buf size
 * @data_index: indentify each share memory informaiton
 */
void mtk_vcodec_dec_optee_set_data(struct mtk_vdec_optee_data_to_shm *data,
				   void *buf, int buf_size,
				   enum mtk_vdec_optee_data_index data_index);

/**
 * mtk_vcodec_dec_optee_invokd_cmd - send share memory data to optee .
 * @optee_private: optee private context
 * @hw_id: hardware index
 * @data: normal world data used to init optee share memory
 */
int mtk_vcodec_dec_optee_invokd_cmd(struct mtk_vdec_optee_private *optee_private,
				    enum mtk_vdec_hw_id hw_id,
				    struct mtk_vdec_optee_data_to_shm *data);

/**
 * mtk_vcodec_dec_get_shm_buffer_va - close the communication channels with TA.
 * @optee_private: optee private context
 * @hw_id:         hardware index
 * @@data_index: indentify each share memory informaiton
 */
void *mtk_vcodec_dec_get_shm_buffer_va(struct mtk_vdec_optee_private *optee_private,
				       enum mtk_vdec_hw_id hw_id,
				       enum mtk_vdec_optee_data_index data_index);

/**
 * mtk_vcodec_dec_get_shm_buffer_size - close the communication channels with TA.
 * @optee_private: optee private context
 * @hw_id:         hardware index
 * @@data_index: indentify each share memory informaiton
 */
int mtk_vcodec_dec_get_shm_buffer_size(struct mtk_vdec_optee_private *optee_private,
				       enum mtk_vdec_hw_id hw_id,
				       enum mtk_vdec_optee_data_index data_index);

#endif /* _MTK_VCODEC_FW_OPTEE_H_ */
