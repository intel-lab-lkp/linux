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

	ret = wave6_vpu_dec_check_open_param(inst, pop);
	if (ret)
		return ret;

	guard(mutex)(&core->hw_lock);

	if (!wave6_vpu_is_init(core))
		return -ENODEV;

	inst->codec_info = kzalloc(sizeof(*inst->codec_info), GFP_KERNEL);
	if (!inst->codec_info)
		return -ENOMEM;

	p_dec_info = &inst->codec_info->dec_info;
	memcpy(&p_dec_info->open_param, pop, sizeof(struct dec_open_param));

	ret = wave6_vpu_build_up_dec_param(inst, pop);
	if (ret)
		kfree(inst->codec_info);

	return ret;
}

int wave6_vpu_dec_close(struct vpu_instance *inst, u32 *fail_res)
{
	int ret;

	if (WARN_ON(!inst->codec_info))
		return -EINVAL;

	guard(mutex)(&inst->dev->hw_lock);

	ret = wave6_vpu_dec_fini_seq(inst, fail_res);
	if (ret) {
		dev_warn(inst->dev->dev, "dec seq end timed out\n");

		if (*fail_res == WAVE6_SYSERR_VPU_STILL_RUNNING)
			return ret;
	}

	dev_dbg(inst->dev->dev, "dec seq end complete\n");

	kfree(inst->codec_info);

	return 0;
}

int wave6_vpu_dec_issue_seq_init(struct vpu_instance *inst)
{
	guard(mutex)(&inst->dev->hw_lock);

	return wave6_vpu_dec_init_seq(inst);
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
				      struct dec_aux_buffer_size_info info,
				      uint32_t *size)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	int width = info.width;
	int height = info.height;
	int buf_size, twice;

	if (info.type == AUX_BUF_FBC_Y_TBL) {
		switch (inst->std) {
		case W_HEVC_DEC:
			buf_size = WAVE6_FBC_LUMA_TABLE_SIZE(width, height);
			break;
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
			buf_size = WAVE6_FBC_CHROMA_TABLE_SIZE(width, height);
			break;
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

int wave6_vpu_dec_register_aux_buffer(struct vpu_instance *inst,
				      struct aux_buffer_info info)
{
	struct dec_info *p_dec_info;
	struct aux_buffer *aux_bufs = info.buf_array;
	struct dec_aux_buffer_size_info size_info;
	unsigned int expected_size;
	unsigned int i;
	int ret;

	p_dec_info = &inst->codec_info->dec_info;

	size_info.width = p_dec_info->seq_info.pic_width;
	size_info.height = p_dec_info->seq_info.pic_height;
	size_info.type = info.type;

	ret = wave6_vpu_dec_get_aux_buffer_size(inst, size_info, &expected_size);
	if (ret)
		return ret;

	switch (info.type) {
	case AUX_BUF_FBC_Y_TBL:
		for (i = 0; i < info.num; i++) {
			if (expected_size > aux_bufs[i].size)
				return -EINVAL;

			p_dec_info->vb_fbc_y_tbl[aux_bufs[i].index].daddr = aux_bufs[i].addr;
			p_dec_info->vb_fbc_y_tbl[aux_bufs[i].index].size = aux_bufs[i].size;
		}
		break;
	case AUX_BUF_FBC_C_TBL:
		for (i = 0; i < info.num; i++) {
			if (expected_size > aux_bufs[i].size)
				return -EINVAL;

			p_dec_info->vb_fbc_c_tbl[aux_bufs[i].index].daddr = aux_bufs[i].addr;
			p_dec_info->vb_fbc_c_tbl[aux_bufs[i].index].size = aux_bufs[i].size;
		}
		break;
	case AUX_BUF_MV_COL:
		for (i = 0; i < info.num; i++) {
			if (expected_size > aux_bufs[i].size)
				return -EINVAL;

			p_dec_info->vb_mv[aux_bufs[i].index].daddr = aux_bufs[i].addr;
			p_dec_info->vb_mv[aux_bufs[i].index].size = aux_bufs[i].size;
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int wave6_vpu_dec_register_frame_buffer_ex(struct vpu_instance *inst,
					   int num_of_dec_fbs, int stride,
					   int height, int map_type)
{
	struct dec_info *p_dec_info;
	struct frame_buffer *fb;

	if (num_of_dec_fbs > WAVE6_MAX_FBS)
		return -EINVAL;

	p_dec_info = &inst->codec_info->dec_info;
	p_dec_info->stride = stride;

	if (!p_dec_info->seq_info_obtained)
		return -EINVAL;

	if (stride < p_dec_info->seq_info.pic_width || (stride % 8) ||
	    height < p_dec_info->seq_info.pic_height)
		return -EINVAL;

	guard(mutex)(&inst->dev->hw_lock);

	fb = inst->frame_buf;
	return wave6_vpu_dec_register_frame_buffer(inst, &fb[0],
						   COMPRESSED_FRAME_MAP,
						   num_of_dec_fbs);
}

int wave6_vpu_dec_register_display_buffer_ex(struct vpu_instance *inst, struct frame_buffer fb)
{
	struct dec_info *p_dec_info;

	p_dec_info = &inst->codec_info->dec_info;
	if (!p_dec_info->seq_info_obtained)
		return -EINVAL;

	guard(mutex)(&inst->dev->hw_lock);

	return wave6_vpu_dec_register_display_buffer(inst, fb);
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

int wave6_vpu_dec_start_one_frame(struct vpu_instance *inst, struct dec_param *param, u32 *res_fail)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;

	if (!p_dec_info->stride)
		return -EINVAL;

	guard(mutex)(&inst->dev->hw_lock);

	return wave6_vpu_decode(inst, param, res_fail);
}

void wave6_vpu_dec_set_rd_ptr(struct vpu_instance *inst, dma_addr_t addr,
			      bool update_wr_ptr)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;

	p_dec_info->stream_rd_ptr = addr;
	if (update_wr_ptr)
		p_dec_info->stream_wr_ptr = addr;
}

int wave6_vpu_dec_get_output_info(struct vpu_instance *inst, struct dec_output_info *info)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	int ret;

	if (WARN_ON(!info))
		return -EINVAL;

	guard(mutex)(&inst->dev->hw_lock);

	ret = wave6_vpu_dec_get_result(inst, info);
	if (ret) {
		info->rd_ptr = p_dec_info->stream_rd_ptr;
		info->wr_ptr = p_dec_info->stream_wr_ptr;
	}

	return ret;
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
		memset(&p_dec_info->vb_mv[i], 0, sizeof(struct vpu_buf));

		wave6_vdi_free_dma(&inst->aux_vbuf[AUX_BUF_FBC_Y_TBL][i]);
		memset(&p_dec_info->vb_fbc_y_tbl[i], 0, sizeof(struct vpu_buf));

		wave6_vdi_free_dma(&inst->aux_vbuf[AUX_BUF_FBC_C_TBL][i]);
		memset(&p_dec_info->vb_fbc_c_tbl[i], 0, sizeof(struct vpu_buf));
	}
}

int wave6_vpu_dec_flush_instance(struct vpu_instance *inst)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;

	if (!p_dec_info->seq_info_obtained)
		return -EINVAL;

	guard(mutex)(&inst->dev->hw_lock);

	return wave6_vpu_dec_flush(inst);
}

int wave6_vpu_enc_open(struct vpu_instance *inst, struct enc_open_param *pop)
{
	struct enc_info *p_enc_info;
	int ret;
	struct vpu_core_device *core = inst->dev;

	ret = wave6_vpu_enc_check_open_param(inst, pop);
	if (ret)
		return ret;

	guard(mutex)(&core->hw_lock);

	if (!wave6_vpu_is_init(core))
		return -ENODEV;

	inst->codec_info = kzalloc(sizeof(*inst->codec_info), GFP_KERNEL);
	if (!inst->codec_info)
		return -ENOMEM;

	p_enc_info = &inst->codec_info->enc_info;
	p_enc_info->open_param = *pop;

	ret = wave6_vpu_build_up_enc_param(core->dev, inst, pop);
	if (ret)
		kfree(inst->codec_info);

	return ret;
}

int wave6_vpu_enc_close(struct vpu_instance *inst, u32 *fail_res)
{
	int ret;

	if (WARN_ON(!inst->codec_info))
		return -EINVAL;

	guard(mutex)(&inst->dev->hw_lock);

	ret = wave6_vpu_enc_fini_seq(inst, fail_res);
	if (ret) {
		dev_warn(inst->dev->dev, "enc seq end timed out\n");

		if (*fail_res == WAVE6_SYSERR_VPU_STILL_RUNNING)
			return ret;
	}

	dev_dbg(inst->dev->dev, "enc seq end timed out\n");
	kfree(inst->codec_info);

	return 0;
}

int wave6_vpu_enc_get_aux_buffer_size(struct vpu_instance *inst,
				      struct enc_aux_buffer_size_info info,
				      uint32_t *size)
{
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	int width, height, buf_size, twice;

	if (inst->std == W_AVC_ENC) {
		width = ALIGN(info.width, 16);
		height = ALIGN(info.height, 16);
		if (p_enc_info->rot_angle == ROT_90 || p_enc_info->rot_angle == ROT_270) {
			width = ALIGN(info.height, 16);
			height = ALIGN(info.width, 16);
		}
	} else {
		width = ALIGN(info.width, 8);
		height = ALIGN(info.height, 8);
		if ((p_enc_info->rot_angle || p_enc_info->mir_dir) &&
		    !(p_enc_info->rot_angle == ROT_180 && p_enc_info->mir_dir == MIR_HOR_VER)) {
			width = ALIGN(info.width, 32);
			height = ALIGN(info.height, 32);
		}
		if (p_enc_info->rot_angle == ROT_90 || p_enc_info->rot_angle == ROT_270) {
			width = ALIGN(info.height, 32);
			height = ALIGN(info.width, 32);
		}
	}

	if (info.type == AUX_BUF_FBC_Y_TBL) {
		switch (inst->std) {
		case W_HEVC_ENC:
			buf_size = WAVE6_FBC_LUMA_TABLE_SIZE(width, height);
			break;
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
			buf_size = WAVE6_FBC_CHROMA_TABLE_SIZE(width, height);
			break;
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

int wave6_vpu_enc_register_aux_buffer(struct vpu_instance *inst,
				      struct aux_buffer_info info)
{
	struct enc_info *p_enc_info;
	struct aux_buffer *aux_bufs = info.buf_array;
	struct enc_aux_buffer_size_info size_info;
	unsigned int expected_size;
	unsigned int i;
	int ret;

	p_enc_info = &inst->codec_info->enc_info;

	size_info.width = p_enc_info->width;
	size_info.height = p_enc_info->height;
	size_info.type = info.type;

	ret = wave6_vpu_enc_get_aux_buffer_size(inst, size_info, &expected_size);
	if (ret)
		return ret;

	switch (info.type) {
	case AUX_BUF_FBC_Y_TBL:
		for (i = 0; i < info.num; i++) {
			if (expected_size > aux_bufs[i].size)
				return -EINVAL;

			p_enc_info->vb_fbc_y_tbl[aux_bufs[i].index].daddr = aux_bufs[i].addr;
			p_enc_info->vb_fbc_y_tbl[aux_bufs[i].index].size = aux_bufs[i].size;
		}
		break;
	case AUX_BUF_FBC_C_TBL:
		for (i = 0; i < info.num; i++) {
			if (expected_size > aux_bufs[i].size)
				return -EINVAL;

			p_enc_info->vb_fbc_c_tbl[aux_bufs[i].index].daddr = aux_bufs[i].addr;
			p_enc_info->vb_fbc_c_tbl[aux_bufs[i].index].size = aux_bufs[i].size;
		}
		break;
	case AUX_BUF_MV_COL:
		for (i = 0; i < info.num; i++) {
			if (expected_size > aux_bufs[i].size)
				return -EINVAL;

			p_enc_info->vb_mv[aux_bufs[i].index].daddr = aux_bufs[i].addr;
			p_enc_info->vb_mv[aux_bufs[i].index].size = aux_bufs[i].size;
		}
		break;
	case AUX_BUF_SUB_SAMPLE:
		for (i = 0; i < info.num; i++) {
			if (expected_size > aux_bufs[i].size)
				return -EINVAL;

			p_enc_info->vb_sub_sam_buf[aux_bufs[i].index].daddr = aux_bufs[i].addr;
			p_enc_info->vb_sub_sam_buf[aux_bufs[i].index].size = aux_bufs[i].size;
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int wave6_vpu_enc_register_frame_buffer_ex(struct vpu_instance *inst, int num, unsigned int stride,
					   int height, enum tiled_map_type map_type)
{
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;

	if (p_enc_info->stride)
		return -EINVAL;

	if (!p_enc_info->seq_info_obtained)
		return -EINVAL;

	if (num < p_enc_info->seq_info.min_frame_buffer_count)
		return -EINVAL;

	if (!stride || stride % 8)
		return -EINVAL;

	if (height < 0)
		return -EINVAL;

	guard(mutex)(&inst->dev->hw_lock);

	p_enc_info->num_frame_buffers = num;
	p_enc_info->stride = stride;

	return wave6_vpu_enc_register_frame_buffer(inst, &inst->frame_buf[0]);
}

static int wave6_check_enc_param(struct vpu_instance *inst, struct enc_param *param)
{
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	bool is_rgb_format = false;

	if (!param->skip_picture && !param->source_frame)
		return -EINVAL;

	if (!p_enc_info->open_param.codec_param.bitrate && inst->std == W_HEVC_ENC) {
		if (param->force_pic_qp) {
			if (param->force_pic_qp_i < 0 || param->force_pic_qp_i > 63)
				return -EINVAL;

			if (param->force_pic_qp_p < 0 || param->force_pic_qp_p > 63)
				return -EINVAL;

			if (param->force_pic_qp_b < 0 || param->force_pic_qp_b > 63)
				return -EINVAL;
		}
		if ((param->pic_stream_buffer_addr % 16 || !param->pic_stream_buffer_size))
			return -EINVAL;
	}

	if ((param->pic_stream_buffer_addr % 8 || !param->pic_stream_buffer_size))
		return -EINVAL;

	if (p_enc_info->open_param.src_format == FORMAT_RGB_32BIT_PACKED ||
	    p_enc_info->open_param.src_format == FORMAT_RGB_P10_32BIT_PACKED ||
	    p_enc_info->open_param.src_format == FORMAT_RGB_24BIT_PACKED)
		is_rgb_format = true;

	if (is_rgb_format) {
		if (param->csc.coef_ry < -512 || param->csc.coef_ry > 511)
			return -EINVAL;
		if (param->csc.coef_gy < -512 || param->csc.coef_gy > 511)
			return -EINVAL;
		if (param->csc.coef_by < -512 || param->csc.coef_by > 511)
			return -EINVAL;
		if (param->csc.coef_rcb < -512 || param->csc.coef_rcb > 511)
			return -EINVAL;
		if (param->csc.coef_gcb < -512 || param->csc.coef_gcb > 511)
			return -EINVAL;
		if (param->csc.coef_bcb < -512 || param->csc.coef_bcb > 511)
			return -EINVAL;
		if (param->csc.coef_rcr < -512 || param->csc.coef_rcr > 511)
			return -EINVAL;
		if (param->csc.coef_gcr < -512 || param->csc.coef_gcr > 511)
			return -EINVAL;
		if (param->csc.coef_bcr < -512 || param->csc.coef_bcr > 511)
			return -EINVAL;
		if (param->csc.offset_y > 1023)
			return -EINVAL;
		if (param->csc.offset_cb > 1023)
			return -EINVAL;
		if (param->csc.offset_cr > 1023)
			return -EINVAL;
	}

	return 0;
}

int wave6_vpu_enc_start_one_frame(struct vpu_instance *inst, struct enc_param *param, u32 *fail_res)
{
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	int ret;

	*fail_res = 0;

	if (!p_enc_info->stride)
		return -EINVAL;

	ret = wave6_check_enc_param(inst, param);
	if (ret)
		return ret;

	guard(mutex)(&inst->dev->hw_lock);

	return wave6_vpu_encode(inst, param, fail_res);
}

int wave6_vpu_enc_get_output_info(struct vpu_instance *inst, struct enc_output_info *info)
{
	if (WARN_ON(!info))
		return -EINVAL;

	guard(mutex)(&inst->dev->hw_lock);

	return wave6_vpu_enc_get_result(inst, info);
}

int wave6_vpu_enc_issue_seq_init(struct vpu_instance *inst)
{
	guard(mutex)(&inst->dev->hw_lock);

	return wave6_vpu_enc_init_seq(inst);
}

int wave6_vpu_enc_issue_seq_change(struct vpu_instance *inst, bool *changed)
{
	guard(mutex)(&inst->dev->hw_lock);

	return wave6_vpu_enc_change_seq(inst, changed);
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
