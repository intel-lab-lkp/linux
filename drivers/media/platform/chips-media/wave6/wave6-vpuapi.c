// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Wave6 series multi-standard codec IP - wave6 helper interface
 *
 * Copyright (C) 2025 CHIPS&MEDIA INC
 */

#include <linux/bug.h>
#include "wave6-vpuapi.h"
#include "wave6-regdefine.h"
#include "wave6-hw.h"
#include "wave6-vpu-dbg.h"
#include "wave6-trace.h"

int wave6_vpu_dec_open(struct vpu_instance *inst, struct dec_open_param *pop)
{
	struct dec_info *p_dec_info;
	int ret;
	struct vpu_core_device *core = inst->dev;

	guard(mutex)(&core->hw_lock);

	if (!wave6_vpu_is_init(core))
		return -ENODEV;

	inst->codec_info = kzalloc_obj(*inst->codec_info);
	if (!inst->codec_info)
		return -ENOMEM;

	p_dec_info = &inst->codec_info->dec_info;
	memcpy(&p_dec_info->open_param, pop, sizeof(struct dec_open_param));

	ret = wave6_vpu_build_up_dec_param(inst, pop);
	if (ret) {
		kfree(inst->codec_info);
		inst->codec_info = NULL;
	}

	return ret;
}

int wave6_vpu_dec_close(struct vpu_instance *inst, u32 *fail_res)
{
	int ret;

	if (WARN_ON(!inst->codec_info))
		return -EINVAL;

	guard(mutex)(&inst->dev->hw_lock);

	ret = wave6_vpu_dec_fini_seq(inst, fail_res);
	if (ret)
		dev_warn(inst->dev->dev, "dec seq end timed out\n");

	kfree(inst->codec_info);
	inst->codec_info = NULL;

	return ret;
}

int wave6_vpu_dec_complete_seq_init(struct vpu_instance *inst,
				    struct dec_seq_info *info)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	int ret;

	guard(mutex)(&inst->dev->hw_lock);

	ret = wave6_vpu_dec_get_seq_info(inst, info);
	if (!ret)
		p_dec_info->seq_info_obtained = true;

	info->rd_ptr = wave6_vpu_dec_get_rd_ptr(inst);
	info->wr_ptr = p_dec_info->stream_wr_ptr;

	p_dec_info->seq_info = *info;

	return ret;
}

int wave6_vpu_dec_get_aux_buffer_size(struct vpu_instance *inst,
				      struct aux_buffer_size_info info, u32 *size)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	int width = info.width;
	int height = info.height;
	int buf_size, twice;

	if (info.type == AUX_BUF_FBC_Y_TBL) {
		switch (inst->std) {
		case W_HEVC_DEC:
		case W_AVC_DEC:
			buf_size = WAVE6_FBC_LUMA_TABLE_SIZE(width, height);
			break;
		default:
			return -EINVAL;
		}
		buf_size = ALIGN(buf_size, 16);
	} else if (info.type == AUX_BUF_FBC_C_TBL) {
		if (p_dec_info->seq_info.c_fmt_idc == C_FMT_IDC_YUV422)
			twice = 2;
		else if (p_dec_info->seq_info.c_fmt_idc == C_FMT_IDC_YUV444)
			twice = 4;
		else
			twice = 1;

		switch (inst->std) {
		case W_HEVC_DEC:
		case W_AVC_DEC:
			buf_size = WAVE6_FBC_CHROMA_TABLE_SIZE(width, height);
			break;
		default:
			return -EINVAL;
		}
		buf_size = buf_size * twice;
		buf_size = ALIGN(buf_size, 16);
	} else if (info.type == AUX_BUF_MV_COL) {
		switch (inst->std) {
		case W_HEVC_DEC:
			buf_size = WAVE6_DEC_HEVC_MVCOL_BUF_SIZE(width, height);
			break;
		case W_AVC_DEC:
			buf_size = WAVE6_DEC_AVC_MVCOL_BUF_SIZE(width, height);
			break;
		default:
			return -EINVAL;
		}
		buf_size = ALIGN(buf_size, 16);
	} else {
		return -EINVAL;
	}

	*size = buf_size;

	return 0;
}

void wave6_vpu_dec_get_bitstream_buffer(struct vpu_instance *inst,
					dma_addr_t *p_rd_ptr,
					dma_addr_t *p_wr_ptr)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	dma_addr_t rd_ptr;
	dma_addr_t wr_ptr;

	guard(mutex)(&inst->dev->hw_lock);

	rd_ptr = wave6_vpu_dec_get_rd_ptr(inst);
	wr_ptr = p_dec_info->stream_wr_ptr;

	if (p_rd_ptr)
		*p_rd_ptr = rd_ptr;
	if (p_wr_ptr)
		*p_wr_ptr = wr_ptr;
}

int wave6_vpu_dec_update_bitstream_buffer(struct vpu_instance *inst, int size)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	dma_addr_t wr_ptr;
	dma_addr_t rd_ptr;

	wr_ptr = p_dec_info->stream_wr_ptr;
	rd_ptr = p_dec_info->stream_rd_ptr;

	if (size > 0) {
		if (wr_ptr < rd_ptr && rd_ptr <= wr_ptr + size)
			return -EINVAL;

		wr_ptr += size;

		p_dec_info->stream_wr_ptr = wr_ptr;
		p_dec_info->stream_rd_ptr = rd_ptr;
	}

	p_dec_info->stream_end = size == 0;

	return 0;
}

void wave6_vpu_dec_set_rd_ptr(struct vpu_instance *inst, dma_addr_t addr,
			      bool update_wr_ptr)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;

	p_dec_info->stream_rd_ptr = addr;
	if (update_wr_ptr)
		p_dec_info->stream_wr_ptr = addr;
}

void wave6_vpu_dec_reset_frame_buffer_info(struct vpu_instance *inst)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	int i;

	for (i = 0; i < WAVE6_MAX_FBS; i++) {
		wave6_vdi_free_dma(&inst->frame_vbuf[i]);
		memset(&inst->frame_buf[i], 0, sizeof(struct frame_buffer));
		memset(&p_dec_info->disp_buf[i], 0, sizeof(struct frame_buffer));

		wave6_vdi_free_dma(&inst->aux_vbuf[AUX_BUF_MV_COL][i]);
		wave6_vdi_free_dma(&inst->aux_vbuf[AUX_BUF_FBC_Y_TBL][i]);
		wave6_vdi_free_dma(&inst->aux_vbuf[AUX_BUF_FBC_C_TBL][i]);
	}
}

int wave6_vpu_enc_open(struct vpu_instance *inst, struct enc_open_param *pop)
{
	struct enc_info *p_enc_info;
	int ret;
	struct vpu_core_device *core = inst->dev;

	guard(mutex)(&core->hw_lock);

	if (!wave6_vpu_is_init(core))
		return -ENODEV;

	inst->codec_info = kzalloc_obj(*inst->codec_info);
	if (!inst->codec_info)
		return -ENOMEM;

	p_enc_info = &inst->codec_info->enc_info;
	p_enc_info->open_param = *pop;

	ret = wave6_vpu_build_up_enc_param(core->dev, inst, pop);
	if (ret) {
		kfree(inst->codec_info);
		inst->codec_info = NULL;
	}

	return ret;
}

int wave6_vpu_enc_close(struct vpu_instance *inst, u32 *fail_res)
{
	int ret;

	if (WARN_ON(!inst->codec_info))
		return -EINVAL;

	guard(mutex)(&inst->dev->hw_lock);

	ret = wave6_vpu_enc_fini_seq(inst, fail_res);
	if (ret)
		dev_warn(inst->dev->dev, "enc seq end timed out\n");

	kfree(inst->codec_info);
	inst->codec_info = NULL;

	return ret;
}

int wave6_vpu_enc_get_aux_buffer_size(struct vpu_instance *inst,
				      struct aux_buffer_size_info info, u32 *size)
{
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	int width = info.width;
	int height = info.height;
	int buf_size, twice;

	if (info.type == AUX_BUF_FBC_Y_TBL) {
		switch (inst->std) {
		case W_HEVC_ENC:
		case W_AVC_ENC:
			buf_size = WAVE6_FBC_LUMA_TABLE_SIZE(width, height);
			break;
		default:
			return -EINVAL;
		}
	} else if (info.type == AUX_BUF_FBC_C_TBL) {
		if (p_enc_info->c_fmt_idc == C_FMT_IDC_YUV422)
			twice = 2;
		else if (p_enc_info->c_fmt_idc == C_FMT_IDC_YUV444)
			twice = 4;
		else
			twice = 1;

		switch (inst->std) {
		case W_HEVC_ENC:
		case W_AVC_ENC:
			buf_size = WAVE6_FBC_CHROMA_TABLE_SIZE(width, height);
			break;
		default:
			return -EINVAL;
		}
		buf_size = buf_size * twice;
	} else if (info.type == AUX_BUF_MV_COL) {
		switch (inst->std) {
		case W_HEVC_ENC:
			buf_size = WAVE6_ENC_HEVC_MVCOL_BUF_SIZE(width, height);
			break;
		case W_AVC_ENC:
			buf_size = WAVE6_ENC_AVC_MVCOL_BUF_SIZE(width, height);
			break;
		default:
			return -EINVAL;
		}
	} else if (info.type == AUX_BUF_SUB_SAMPLE) {
		switch (inst->std) {
		case W_HEVC_ENC:
		case W_AVC_ENC:
			buf_size = WAVE6_ENC_SUBSAMPLED_SIZE(width, height);
			break;
		default:
			return -EINVAL;
		}
	} else {
		return -EINVAL;
	}

	*size = buf_size;

	return 0;
}

int wave6_vpu_enc_complete_seq_init(struct vpu_instance *inst,
				    struct enc_seq_info *info)
{
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	int ret;

	guard(mutex)(&inst->dev->hw_lock);

	ret = wave6_vpu_enc_get_seq_info(inst, info);
	if (ret) {
		p_enc_info->seq_info_obtained = false;
		return ret;
	}

	if (!p_enc_info->seq_info_obtained) {
		p_enc_info->seq_info_obtained = true;
		p_enc_info->seq_info = *info;
	}

	return 0;
}
