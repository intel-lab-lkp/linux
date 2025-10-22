// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Wave6 series multi-standard codec IP - wave6 backend interface
 *
 * Copyright (C) 2025 CHIPS&MEDIA INC
 */

#include <linux/iopoll.h>
#include "wave6-vpu-core.h"
#include "wave6-hw.h"
#include "wave6-regdefine.h"
#include "wave6-trace.h"

void wave6_vpu_writel(struct vpu_core_device *core, u32 addr, u32 data)
{
	wave6_vdi_writel(core->reg_base, addr, data);
	trace_wave6_vpu_writel(core->dev, addr, data);
}

u32 wave6_vpu_readl(struct vpu_core_device *core, u32 addr)
{
	u32 data;

	data = wave6_vdi_readl(core->reg_base, addr);
	trace_wave6_vpu_readl(core->dev, addr, data);

	return data;
}

static void wave6_print_reg_err(struct vpu_core_device *core, u32 fail_reason)
{
	char *caller = __builtin_return_address(0);
	struct device *dev = core->dev;

	switch (fail_reason) {
	case WAVE6_SYSERR_QUEUEING_FAIL:
		dev_dbg(dev, "%s: queueing failure 0x%x\n", caller, fail_reason);
		break;
	case WAVE6_SYSERR_RESULT_NOT_READY:
		dev_err(dev, "%s: result not ready 0x%x\n", caller, fail_reason);
		break;
	case WAVE6_SYSERR_ACCESS_VIOLATION_HW:
		dev_err(dev, "%s: access violation 0x%x\n", caller, fail_reason);
		break;
	case WAVE6_SYSERR_WATCHDOG_TIMEOUT:
		dev_err(dev, "%s: watchdog timeout 0x%x\n", caller, fail_reason);
		break;
	case WAVE6_SYSERR_BUS_ERROR:
		dev_err(dev, "%s: bus error 0x%x\n", caller, fail_reason);
		break;
	case WAVE6_SYSERR_DOUBLE_FAULT:
		dev_err(dev, "%s: double fault 0x%x\n", caller, fail_reason);
		break;
	case WAVE6_SYSERR_VPU_STILL_RUNNING:
		dev_err(dev, "%s: still running 0x%x\n", caller, fail_reason);
		break;
	default:
		dev_err(dev, "%s: failure: 0x%x\n", caller, fail_reason);
		break;
	}
}

static void wave6_dec_set_display_buffer(struct vpu_instance *inst, struct frame_buffer fb)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	int index;

	for (index = 0; index < WAVE6_MAX_FBS; index++) {
		if (!p_dec_info->disp_buf[index].buf_y) {
			p_dec_info->disp_buf[index] = fb;
			p_dec_info->disp_buf[index].index = index;
			break;
		}
	}
}

static struct frame_buffer wave6_dec_get_display_buffer(struct vpu_instance *inst,
							dma_addr_t addr)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	int i;
	struct frame_buffer fb;

	memset(&fb, 0, sizeof(struct frame_buffer));

	for (i = 0; i < WAVE6_MAX_FBS; i++) {
		if (p_dec_info->disp_buf[i].buf_y == addr) {
			fb = p_dec_info->disp_buf[i];
			break;
		}
	}

	return fb;
}

static void wave6_dec_remove_display_buffer(struct vpu_instance *inst,
					    dma_addr_t addr)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	int i;

	for (i = 0; i < WAVE6_MAX_FBS; i++) {
		if (p_dec_info->disp_buf[i].buf_y == addr) {
			memset(&p_dec_info->disp_buf[i], 0, sizeof(struct frame_buffer));
			break;
		}
	}
}

static enum chroma_format_idc get_chroma_format_idc(enum frame_buffer_format fmt)
{
	switch (fmt) {
	case FORMAT_420:
	case FORMAT_420_P10_16BIT_MSB:
	case FORMAT_420_P10_16BIT_LSB:
	case FORMAT_420_P10_32BIT_MSB:
	case FORMAT_420_P10_32BIT_LSB:
		return C_FMT_IDC_YUV420;
	case FORMAT_422:
	case FORMAT_422_P10_16BIT_MSB:
	case FORMAT_422_P10_16BIT_LSB:
	case FORMAT_422_P10_32BIT_MSB:
	case FORMAT_422_P10_32BIT_LSB:
	case FORMAT_YUYV:
	case FORMAT_YVYU:
	case FORMAT_UYVY:
	case FORMAT_VYUY:
	case FORMAT_YUYV_P10_16BIT_MSB:
	case FORMAT_YVYU_P10_16BIT_MSB:
	case FORMAT_UYVY_P10_16BIT_MSB:
	case FORMAT_VYUY_P10_16BIT_MSB:
	case FORMAT_YUYV_P10_16BIT_LSB:
	case FORMAT_YVYU_P10_16BIT_LSB:
	case FORMAT_UYVY_P10_16BIT_LSB:
	case FORMAT_VYUY_P10_16BIT_LSB:
	case FORMAT_YUYV_P10_32BIT_MSB:
	case FORMAT_YVYU_P10_32BIT_MSB:
	case FORMAT_UYVY_P10_32BIT_MSB:
	case FORMAT_VYUY_P10_32BIT_MSB:
	case FORMAT_YUYV_P10_32BIT_LSB:
	case FORMAT_YVYU_P10_32BIT_LSB:
	case FORMAT_UYVY_P10_32BIT_LSB:
	case FORMAT_VYUY_P10_32BIT_LSB:
		return C_FMT_IDC_YUV422;
	case FORMAT_444:
	case FORMAT_444_P10_16BIT_MSB:
	case FORMAT_444_P10_16BIT_LSB:
	case FORMAT_444_P10_32BIT_MSB:
	case FORMAT_444_P10_32BIT_LSB:
		return C_FMT_IDC_YUV444;
	case FORMAT_400:
	case FORMAT_400_P10_16BIT_MSB:
	case FORMAT_400_P10_16BIT_LSB:
	case FORMAT_400_P10_32BIT_MSB:
	case FORMAT_400_P10_32BIT_LSB:
	case FORMAT_YUV444_24BIT:
		return C_FMT_IDC_YUV400;
	case FORMAT_RGB_24BIT_PACKED:
	case FORMAT_YUV444_24BIT_PACKED:
	case FORMAT_RGB_32BIT_PACKED:
	case FORMAT_RGB_P10_32BIT_PACKED:
	case FORMAT_YUV444_32BIT_PACKED:
	case FORMAT_YUV444_P10_32BIT_PACKED:
		return C_FMT_IDC_RGB;
	default:
		return C_FMT_IDC_YUV400;
	}
}

static int wave6_wait_vpu_busy(struct vpu_core_device *core, unsigned int addr)
{
	u32 data;

	lockdep_assert_held(&core->hw_lock);

	return read_poll_timeout(vpu_read_reg, data, !data,
				 W6_VPU_POLL_DELAY_US, W6_VPU_POLL_TIMEOUT,
				 false, core, addr);
}

void wave6_vpu_enable_interrupt(struct vpu_core_device *core)
{
	u32 data;

	lockdep_assert_held(&core->hw_lock);

	data = BIT(W6_INT_BIT_ENC_SET_PARAM);
	data |= BIT(W6_INT_BIT_ENC_PIC);
	data |= BIT(W6_INT_BIT_INIT_SEQ);
	data |= BIT(W6_INT_BIT_DEC_PIC);
	data |= BIT(W6_INT_BIT_BSBUF_ERROR);
	data |= BIT(W6_INT_BIT_REQ_WORK_BUF);
	vpu_write_reg(core, W6_VPU_VINT_ENABLE, data);
}

bool wave6_vpu_is_init(struct vpu_core_device *core)
{
	lockdep_assert_held(&core->hw_lock);

	return vpu_read_reg(core, W6_VPU_VCPU_CUR_PC) != 0;
}

static int32_t wave6_vpu_get_product_id(struct vpu_core_device *core)
{
	u32 product_id = PRODUCT_ID_NONE;
	u32 val;

	lockdep_assert_held(&core->hw_lock);

	val = vpu_read_reg(core, W6_VPU_RET_PRODUCT_CODE);

	switch (val) {
	case WAVE617_CODE:
		product_id = PRODUCT_ID_617; break;
	case WAVE627_CODE:
		product_id = PRODUCT_ID_627; break;
	case WAVE633_CODE:
	case WAVE637_CODE:
	case WAVE663_CODE:
	case WAVE677_CODE:
		product_id = PRODUCT_ID_637; break;
	default:
		dev_err(core->dev, "Invalid product (%x)\n", val);
		break;
	}

	return product_id;
}

static void wave6_send_command(struct vpu_core_device *core, u32 id, u32 std, u32 cmd)
{
	u32 reg_val;

	lockdep_assert_held(&core->hw_lock);

	if (cmd == W6_CMD_CREATE_INSTANCE)
		reg_val = INSTANCE_INFO_CODEC_STD(std);
	else
		reg_val = INSTANCE_INFO_CODEC_STD(std) | INSTANCE_INFO_ID(id);
	vpu_write_reg(core, W6_CMD_INSTANCE_INFO, reg_val);

	vpu_write_reg(core, W6_VPU_BUSY_STATUS, BUSY_STATUS_SET);
	vpu_write_reg(core, W6_COMMAND, cmd);
	vpu_write_reg(core, W6_VPU_HOST_INT_REQ, HOST_INT_REQ_ON);

	trace_wave6_vpu_send_command(core, id, std, cmd);
}

static int wave6_send_query(struct vpu_core_device *core, u32 id, u32 std,
			    enum wave6_query_option query_opt)
{
	int ret;
	u32 reg_val;

	lockdep_assert_held(&core->hw_lock);

	vpu_write_reg(core, W6_QUERY_OPTION, query_opt);
	wave6_send_command(core, id, std, W6_CMD_QUERY);

	ret = wave6_wait_vpu_busy(core, W6_VPU_BUSY_STATUS);
	if (ret) {
		dev_err(core->dev, "query timed out opt=0x%x\n", query_opt);
		return ret;
	}

	if (!vpu_read_reg(core, W6_RET_SUCCESS)) {
		reg_val = vpu_read_reg(core, W6_RET_FAIL_REASON);
		wave6_print_reg_err(core, reg_val);
		return -EIO;
	}

	return 0;
}

int wave6_vpu_get_version(struct vpu_core_device *core)
{
	struct vpu_attr *attr = &core->attr;
	u32 reg_val;
	u8 *str;
	int ret;
	u32 std_def1, conf_feature;

	lockdep_assert_held(&core->hw_lock);

	ret = wave6_send_query(core, 0, 0, W6_QUERY_OPT_GET_VPU_INFO);
	if (ret)
		return ret;

	reg_val = vpu_read_reg(core, W6_RET_PRODUCT_NAME);
	str = (u8 *)&reg_val;
	attr->product_name[0] = str[3];
	attr->product_name[1] = str[2];
	attr->product_name[2] = str[1];
	attr->product_name[3] = str[0];
	attr->product_name[4] = 0;

	attr->product_id = wave6_vpu_get_product_id(core);
	attr->product_code = vpu_read_reg(core, W6_VPU_RET_PRODUCT_CODE);
	attr->product_version = vpu_read_reg(core, W6_RET_PRODUCT_VERSION);
	attr->fw_version = vpu_read_reg(core, W6_RET_FW_API_VERSION);
	attr->fw_revision = vpu_read_reg(core, W6_RET_FW_VERSION);
	attr->hw_version = vpu_read_reg(core, W6_RET_CONF_HW_VERSION);
	std_def1 = vpu_read_reg(core, W6_RET_STD_DEF1);
	conf_feature = vpu_read_reg(core, W6_RET_CONF_FEATURE);

	attr->support_decoders = 0;
	attr->support_encoders = 0;
	attr->support_decoders |= STD_DEF1_HEVC_DEC(std_def1) << W_HEVC_DEC;
	attr->support_hevc10bit_dec = CONF_FEATURE_HEVC10BIT_DEC(conf_feature);
	attr->support_decoders |= STD_DEF1_AVC_DEC(std_def1) << W_AVC_DEC;
	attr->support_avc10bit_dec = CONF_FEATURE_AVC10BIT_DEC(conf_feature);
	attr->support_encoders |= STD_DEF1_HEVC_ENC(std_def1) << W_HEVC_ENC;
	attr->support_hevc10bit_enc = CONF_FEATURE_HEVC10BIT_ENC(conf_feature);
	attr->support_encoders |= STD_DEF1_AVC_ENC(std_def1) << W_AVC_ENC;
	attr->support_avc10bit_enc = CONF_FEATURE_AVC10BIT_ENC(conf_feature);

	return 0;
}

int wave6_vpu_dec_check_open_param(struct vpu_instance *inst,
				   struct dec_open_param *param)
{
	struct vpu_attr *attr = &inst->dev->attr;

	if (!(BIT(inst->std) & attr->support_decoders)) {
		dev_err(inst->dev->dev, "std: %d, support_decoders: 0x%x\n",
			inst->std, attr->support_decoders);
		return -EOPNOTSUPP;
	}

	return 0;
}

int wave6_vpu_build_up_dec_param(struct vpu_instance *inst,
				 struct dec_open_param *param)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	u32 reg_val;
	int ret;

	lockdep_assert_held(&inst->dev->hw_lock);

	p_dec_info->sec_axi.use_dec_ip = true;
	p_dec_info->sec_axi.use_dec_lf_row = true;
	switch (inst->std) {
	case W_HEVC_DEC:
		p_dec_info->seq_change_mask = SEQ_CHANGE_ENABLE_ALL_HEVC;
		break;
	case W_AVC_DEC:
		p_dec_info->seq_change_mask = SEQ_CHANGE_ENABLE_ALL_AVC;
		break;
	default:
		return -EINVAL;
	}

	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_TEMP_BASE, param->inst_buffer.temp_base);
	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_TEMP_SIZE, param->inst_buffer.temp_size);
	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_BS_PARAM, VPU_STREAM_ENDIAN);
	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_ADDR_EXT, param->ext_addr_vcpu);
	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_DISP_MODE, param->disp_mode);

	reg_val = CREATE_INST_CORE_INFO_CQ_DEPTH(COMMAND_QUEUE_DEPTH) |
		  CREATE_INST_CORE_INFO_CORE_IDC(SINGLE_CORE_IDC) |
		  CREATE_INST_CORE_INFO_CORE_NUM(SINGLE_CORE);
	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_CORE_INFO, reg_val);

	reg_val = CREATE_INST_PRIORITY_SECURITY_FLAG(param->is_secure_inst) |
		  CREATE_INST_PRIORITY_VALUE(param->inst_priority);
	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_PRIORITY, reg_val);
	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_TIMEOUT_CYCLE_COUNT,
		      W6_VPU_TIMEOUT_CYCLE_COUNT);

	wave6_send_command(inst->dev, 0, inst->std, W6_CMD_CREATE_INSTANCE);
	ret = wave6_wait_vpu_busy(inst->dev, W6_VPU_BUSY_STATUS);
	if (ret) {
		dev_err(inst->dev->dev, "%s: timeout\n", __func__);
		return ret;
	}

	if (!vpu_read_reg(inst->dev, W6_RET_SUCCESS)) {
		u32 reason_code = vpu_read_reg(inst->dev, W6_RET_FAIL_REASON);

		wave6_print_reg_err(inst->dev, reason_code);
		return -EIO;
	}

	inst->id = vpu_read_reg(inst->dev, W6_RET_INSTANCE_ID);

	return 0;
}

int wave6_vpu_dec_init_seq(struct vpu_instance *inst)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	u32 reg_val;
	int ret;

	lockdep_assert_held(&inst->dev->hw_lock);

	p_dec_info = &inst->codec_info->dec_info;

	vpu_write_reg(inst->dev, W6_CMD_DEC_INIT_SEQ_BS_RD_PTR, p_dec_info->stream_rd_ptr);
	vpu_write_reg(inst->dev, W6_CMD_DEC_INIT_SEQ_BS_WR_PTR, p_dec_info->stream_wr_ptr);

	reg_val = DEC_PIC_BS_OPTION_STREAM_END(p_dec_info->stream_end) |
		  DEC_PIC_BS_OPTION_EXPLICIT_END_ON;
	vpu_write_reg(inst->dev, W6_CMD_DEC_INIT_SEQ_BS_OPTION, reg_val);
	reg_val = INIT_SEQ_OPTION_MODE(W6_INIT_SEQ_OPT_NORMAL);
	vpu_write_reg(inst->dev, W6_CMD_DEC_INIT_SEQ_OPTION, reg_val);

	wave6_send_command(inst->dev, inst->id, inst->std, W6_CMD_INIT_SEQ);
	ret = wave6_wait_vpu_busy(inst->dev, W6_VPU_BUSY_STATUS);
	if (ret) {
		dev_err(inst->dev->dev, "%s: timeout\n", __func__);
		return ret;
	}

	if (!vpu_read_reg(inst->dev, W6_RET_SUCCESS)) {
		u32 reason_code = vpu_read_reg(inst->dev, W6_RET_FAIL_REASON);

		wave6_print_reg_err(inst->dev, reason_code);
		return -EIO;
	}

	return 0;
}

static void wave6_get_dec_seq_result(struct vpu_instance *inst, struct dec_seq_info *info)
{
	u32 reg_val;
	u32 profile, profile_comp;

	lockdep_assert_held(&inst->dev->hw_lock);

	info->rd_ptr = wave6_vpu_dec_get_rd_ptr(inst);

	reg_val = vpu_read_reg(inst->dev, W6_RET_DEC_PIC_SIZE);
	info->pic_width = DEC_PIC_SIZE_WIDTH(reg_val);
	info->pic_height = DEC_PIC_SIZE_HEIGHT(reg_val);

	info->min_frame_buffer_count = vpu_read_reg(inst->dev, W6_RET_DEC_NUM_REQUIRED_FBC_FB);
	info->frame_buf_delay = vpu_read_reg(inst->dev, W6_RET_DEC_NUM_REORDER_DELAY);
	info->req_mv_buffer_count = vpu_read_reg(inst->dev, W6_RET_DEC_NUM_REQUIRED_COL_BUF);

	reg_val = vpu_read_reg(inst->dev, W6_RET_DEC_CROP_TOP_BOTTOM);
	info->pic_crop_rect.top = DEC_CROP_TOP(reg_val);
	info->pic_crop_rect.bottom = info->pic_height - DEC_CROP_BOTTOM(reg_val);

	reg_val = vpu_read_reg(inst->dev, W6_RET_DEC_CROP_LEFT_RIGHT);
	info->pic_crop_rect.left = DEC_CROP_LEFT(reg_val);
	info->pic_crop_rect.right = info->pic_width - DEC_CROP_RIGHT(reg_val);

	info->f_rate_numerator = vpu_read_reg(inst->dev, W6_RET_DEC_FRAME_RATE_NR);
	info->f_rate_denominator = vpu_read_reg(inst->dev, W6_RET_DEC_FRAME_RATE_DR);

	reg_val = vpu_read_reg(inst->dev, W6_RET_DEC_COLOR_SAMPLE);
	info->luma_bit_depth = DEC_COLOR_SAMPLE_L_BIT_DEPTH(reg_val);
	info->chroma_bit_depth = DEC_COLOR_SAMPLE_C_BIT_DEPTH(reg_val);
	info->c_fmt_idc = DEC_COLOR_SAMPLE_C_FMT_IDC(reg_val);
	info->aspect_rate_info = DEC_COLOR_SAMPLE_ASPECT_RATIO_IDC(reg_val);
	info->is_ext_sar = info->aspect_rate_info == H264_VUI_SAR_IDC_EXTENDED;
	if (info->is_ext_sar)
		info->aspect_rate_info = vpu_read_reg(inst->dev, W6_RET_DEC_ASPECT_RATIO);
	info->bitrate = vpu_read_reg(inst->dev, W6_RET_DEC_BIT_RATE);

	reg_val = vpu_read_reg(inst->dev, W6_RET_DEC_SEQ_PARAM);
	info->level = DEC_SEQ_PARAM_LEVEL(reg_val);
	info->tier = DEC_SEQ_PARAM_TIER(reg_val);

	profile = DEC_SEQ_PARAM_PROFILE(reg_val);
	profile_comp = DEC_SEQ_PARAM_PROFILE_COMPATIBILITY(reg_val);
	if (inst->std == W_HEVC_DEC) {
		info->profile = profile;
		if (!info->profile) {
			if ((profile_comp & PROFILE_COMPATIBILITY_MAIN) &&
			    (profile_comp & PROFILE_COMPATIBILITY_MAIN10))
				info->profile = HEVC_PROFILE_MAIN;
			else if (profile_comp & PROFILE_COMPATIBILITY_MAIN10)
				info->profile = HEVC_PROFILE_MAIN10;
			else if (profile_comp & PROFILE_COMPATIBILITY_STILL_PICTURE)
				info->profile = HEVC_PROFILE_STILLPICTURE;
			else
				info->profile = HEVC_PROFILE_MAIN;
		}
	} else if (inst->std == W_AVC_DEC) {
		if (profile == PROFILE_H264_BP)
			info->profile = H264_PROFILE_BP;
		else if (profile == PROFILE_H264_HP)
			info->profile = H264_PROFILE_HP;
		else if (profile == PROFILE_H264_MP)
			info->profile = H264_PROFILE_MP;
		else if (profile == PROFILE_H264_HIGH10)
			info->profile = H264_PROFILE_HIGH10;
		else if (profile == PROFILE_H264_EXTENDED)
			info->profile = H264_PROFILE_EXTENDED;
		else
			info->profile = H264_PROFILE_BP;
	}

	reg_val = vpu_read_reg(inst->dev, W6_RET_DEC_COLOR_CONFIG);
	if (reg_val) {
		info->color.video_signal_type_present = true;
		info->color.color_description_present = DEC_COLOR_CONFIG_COLOR_PRESENT(reg_val);
		info->color.color_primaries = DEC_COLOR_CONFIG_COLOR_PRIMARIES(reg_val);
		info->color.transfer_characteristics = DEC_COLOR_CONFIG_TRANS_CHAR(reg_val);
		info->color.matrix_coefficients = DEC_COLOR_CONFIG_MATRIX_COEFF(reg_val);
		info->color.color_range = DEC_COLOR_CONFIG_COLOR_RANGE(reg_val);
	} else {
		info->color.video_signal_type_present = false;
	}
}

int wave6_vpu_dec_get_seq_info(struct vpu_instance *inst, struct dec_seq_info *info)
{
	int ret;

	lockdep_assert_held(&inst->dev->hw_lock);

	ret = wave6_send_query(inst->dev, inst->id, inst->std, W6_QUERY_OPT_GET_RESULT);
	if (ret)
		return ret;

	if (vpu_read_reg(inst->dev, W6_RET_DEC_DECODING_SUCCESS) != 1) {
		info->err_reason = vpu_read_reg(inst->dev, W6_RET_DEC_ERR_INFO);
		ret = -EIO;
	} else {
		info->warn_info = vpu_read_reg(inst->dev, W6_RET_DEC_WARN_INFO);
	}

	wave6_get_dec_seq_result(inst, info);

	return ret;
}

int wave6_vpu_dec_register_frame_buffer(struct vpu_instance *inst,
					struct frame_buffer *fb_arr,
					enum tiled_map_type map_type, u32 count)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	size_t fbc_remain, mv_remain, fbc_idx = 0, mv_idx = 0;
	size_t i, k, group_num, mv_count;
	dma_addr_t fbc_cr_tbl_addr;
	u32 reg_val;
	int ret;

	lockdep_assert_held(&inst->dev->hw_lock);

	mv_count = p_dec_info->seq_info.req_mv_buffer_count;

	for (i = 0; i < count; i++) {
		if (!p_dec_info->vb_fbc_y_tbl[i].daddr)
			return -EINVAL;
		if (!p_dec_info->vb_fbc_c_tbl[i].daddr)
			return -EINVAL;
	}
	for (i = 0; i < mv_count; i++) {
		if (!p_dec_info->vb_mv[i].daddr)
			return -EINVAL;
	}

	reg_val = SET_FB_PIC_SIZE_WIDTH(p_dec_info->seq_info.pic_width) |
		  SET_FB_PIC_SIZE_HEIGHT(p_dec_info->seq_info.pic_height);
	vpu_write_reg(inst->dev, W6_CMD_SET_FB_PIC_SIZE, reg_val);
	reg_val = SET_FB_PIC_INFO_C_FMT_IDC(p_dec_info->seq_info.c_fmt_idc) |
		  SET_FB_PIC_INFO_L_BIT_DEPTH(p_dec_info->seq_info.luma_bit_depth) |
		  SET_FB_PIC_INFO_C_BIT_DEPTH(p_dec_info->seq_info.chroma_bit_depth);
	vpu_write_reg(inst->dev, W6_CMD_SET_FB_PIC_INFO, reg_val);
	vpu_write_reg(inst->dev, W6_CMD_SET_FB_DEFAULT_CDF, 0);
	vpu_write_reg(inst->dev, W6_CMD_SET_FB_SEGMAP, 0);
	vpu_write_reg(inst->dev, W6_CMD_SET_FB_MV_COL_PRE_ENT, 0);

	fbc_remain = count;
	mv_remain = mv_count;
	group_num = (count > mv_count) ? ((ALIGN(count, 16) / 16) - 1) :
					 ((ALIGN(mv_count, 16) / 16) - 1);
	for (i = 0; i <= group_num; i++) {
		bool first_group = i == 0;
		bool last_group = i == group_num;
		u32 set_fbc_num = (fbc_remain >= 16) ? 16 : fbc_remain;
		u32 set_mv_num = (mv_remain >= 16) ? 16 : mv_remain;
		u32 fbc_start_no = i * 16;
		u32 fbc_end_no = fbc_start_no + set_fbc_num - 1;
		u32 mv_start_no = i * 16;
		u32 mv_end_no = mv_start_no + set_mv_num - 1;

		reg_val = SET_FB_OPTION_ENDIAN(VDI_128BIT_BIG_ENDIAN);
		if (first_group)
			reg_val |= SET_FB_OPTION_START;
		if (last_group)
			reg_val |= SET_FB_OPTION_END;
		vpu_write_reg(inst->dev, W6_CMD_SET_FB_OPTION, reg_val);

		reg_val = SET_FB_NUM_FBC_START_IDX(fbc_start_no) |
			  SET_FB_NUM_FBC_END_IDX(fbc_end_no) |
			  SET_FB_NUM_MV_COL_START_IDX(mv_start_no) |
			  SET_FB_NUM_MV_COL_END_IDX(mv_end_no);
		vpu_write_reg(inst->dev, W6_CMD_SET_FB_NUM, reg_val);

		for (k = 0; k < set_fbc_num; k++) {
			vpu_write_reg(inst->dev, W6_CMD_SET_FB_FBC_Y(k),
				      fb_arr[fbc_idx].buf_y);
			vpu_write_reg(inst->dev, W6_CMD_SET_FB_FBC_C(k),
				      fb_arr[fbc_idx].buf_cb);
			vpu_write_reg(inst->dev, W6_CMD_SET_FB_FBC_CR(k),
				      fb_arr[fbc_idx].buf_cr);
			vpu_write_reg(inst->dev, W6_CMD_SET_FB_FBC_Y_OFFSET(k),
				      p_dec_info->vb_fbc_y_tbl[fbc_idx].daddr);
			vpu_write_reg(inst->dev, W6_CMD_SET_FB_FBC_C_OFFSET(k),
				      p_dec_info->vb_fbc_c_tbl[fbc_idx].daddr);
			fbc_cr_tbl_addr = p_dec_info->vb_fbc_c_tbl[fbc_idx].daddr +
						(p_dec_info->vb_fbc_c_tbl[fbc_idx].size >> 1);
			vpu_write_reg(inst->dev, W6_CMD_SET_FB_FBC_CR_OFFSET(k),
				      fbc_cr_tbl_addr);
			fbc_idx++;
		}
		fbc_remain -= k;

		for (k = 0; k < set_mv_num; k++) {
			vpu_write_reg(inst->dev, W6_CMD_SET_FB_MV_COL(k),
				      p_dec_info->vb_mv[mv_idx].daddr);
			mv_idx++;
		}
		mv_remain -= k;

		wave6_send_command(inst->dev, inst->id, inst->std, W6_CMD_SET_FB);
		ret = wave6_wait_vpu_busy(inst->dev, W6_VPU_BUSY_STATUS);
		if (ret) {
			dev_err(inst->dev->dev, "%s: timeout\n", __func__);
			return ret;
		}
	}

	if (!vpu_read_reg(inst->dev, W6_RET_SUCCESS))
		return -EIO;

	return 0;
}

int wave6_vpu_dec_register_display_buffer(struct vpu_instance *inst, struct frame_buffer fb)
{
	int ret;
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	u32 reg_val;
	u32 c_fmt_idc, out_fmt, out_mode;

	lockdep_assert_held(&inst->dev->hw_lock);

	vpu_write_reg(inst->dev, W6_CMD_DEC_SET_DISP_SCL_PARAM,
		      inst->scaler_info.enable);
	reg_val = SET_DISP_SCL_PIC_SIZE_WIDTH(inst->scaler_info.width) |
		  SET_DISP_SCL_PIC_SIZE_HEIGHT(inst->scaler_info.height);
	vpu_write_reg(inst->dev, W6_CMD_DEC_SET_DISP_SCL_PIC_SIZE, reg_val);
	reg_val = SET_DISP_PIC_SIZE_WIDTH(p_dec_info->seq_info.pic_width) |
		  SET_DISP_PIC_SIZE_HEIGHT(p_dec_info->seq_info.pic_height);
	vpu_write_reg(inst->dev, W6_CMD_DEC_SET_DISP_PIC_SIZE, reg_val);

	c_fmt_idc = get_chroma_format_idc(p_dec_info->wtl_format);
	switch (p_dec_info->wtl_format) {
	case FORMAT_420_P10_16BIT_MSB:
	case FORMAT_422_P10_16BIT_MSB:
	case FORMAT_444_P10_16BIT_MSB:
	case FORMAT_400_P10_16BIT_MSB:
		out_mode = (WTL_RIGHT_JUSTIFIED << 2) | WTL_PIXEL_16BIT;
		break;
	case FORMAT_420_P10_16BIT_LSB:
	case FORMAT_422_P10_16BIT_LSB:
	case FORMAT_444_P10_16BIT_LSB:
	case FORMAT_400_P10_16BIT_LSB:
		out_mode = (WTL_LEFT_JUSTIFIED << 2) | WTL_PIXEL_16BIT;
		break;
	case FORMAT_420_P10_32BIT_MSB:
	case FORMAT_422_P10_32BIT_MSB:
	case FORMAT_444_P10_32BIT_MSB:
	case FORMAT_400_P10_32BIT_MSB:
		out_mode = (WTL_RIGHT_JUSTIFIED << 2) | WTL_PIXEL_32BIT;
		break;
	case FORMAT_420_P10_32BIT_LSB:
	case FORMAT_422_P10_32BIT_LSB:
	case FORMAT_444_P10_32BIT_LSB:
	case FORMAT_400_P10_32BIT_LSB:
		out_mode = (WTL_LEFT_JUSTIFIED << 2) | WTL_PIXEL_32BIT;
		break;
	default:
		out_mode = (WTL_RIGHT_JUSTIFIED << 2) | WTL_PIXEL_8BIT;
		break;
	}
	out_fmt = (inst->nv21 << 1) | inst->cbcr_interleave;

	reg_val = SET_DISP_COMMON_PIC_INFO_BWB_ON |
		  SET_DISP_COMMON_PIC_INFO_C_FMT_IDC(c_fmt_idc) |
		  SET_DISP_COMMON_PIC_INFO_PIXEL_ORDER(PIXEL_ORDER_INCREASING) |
		  SET_DISP_COMMON_PIC_INFO_OUT_MODE(out_mode) |
		  SET_DISP_COMMON_PIC_INFO_OUT_FMT(out_fmt) |
		  SET_DISP_COMMON_PIC_INFO_STRIDE(fb.stride);
	vpu_write_reg(inst->dev, W6_CMD_DEC_SET_DISP_COMMON_PIC_INFO, reg_val);
	reg_val = SET_DISP_OPTION_ENDIAN(VDI_128BIT_BIG_ENDIAN);
	vpu_write_reg(inst->dev, W6_CMD_DEC_SET_DISP_OPTION, reg_val);
	reg_val = SET_DISP_PIC_INFO_L_BIT_DEPTH(fb.luma_bit_depth) |
		  SET_DISP_PIC_INFO_C_BIT_DEPTH(fb.chroma_bit_depth) |
		  SET_DISP_PIC_INFO_C_FMT_IDC(fb.c_fmt_idc);
	vpu_write_reg(inst->dev, W6_CMD_DEC_SET_DISP_PIC_INFO, reg_val);
	vpu_write_reg(inst->dev, W6_CMD_DEC_SET_DISP_Y_BASE, fb.buf_y);
	vpu_write_reg(inst->dev, W6_CMD_DEC_SET_DISP_CB_BASE, fb.buf_cb);
	vpu_write_reg(inst->dev, W6_CMD_DEC_SET_DISP_CR_BASE, fb.buf_cr);

	wave6_send_command(inst->dev, inst->id, inst->std, W6_CMD_DEC_SET_DISP);
	ret = wave6_wait_vpu_busy(inst->dev, W6_VPU_BUSY_STATUS);
	if (ret) {
		dev_err(inst->dev->dev, "%s: timeout\n", __func__);
		return ret;
	}

	if (!vpu_read_reg(inst->dev, W6_RET_SUCCESS))
		return -EIO;

	wave6_dec_set_display_buffer(inst, fb);

	return 0;
}

int wave6_vpu_decode(struct vpu_instance *inst, struct dec_param *option, u32 *fail_res)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	u32 reg_val;
	int ret;

	lockdep_assert_held(&inst->dev->hw_lock);

	vpu_write_reg(inst->dev, W6_CMD_DEC_PIC_BS_RD_PTR, p_dec_info->stream_rd_ptr);
	vpu_write_reg(inst->dev, W6_CMD_DEC_PIC_BS_WR_PTR, p_dec_info->stream_wr_ptr);

	reg_val = DEC_PIC_BS_OPTION_STREAM_END(p_dec_info->stream_end) |
		  DEC_PIC_BS_OPTION_EXPLICIT_END_ON;
	vpu_write_reg(inst->dev, W6_CMD_DEC_PIC_BS_OPTION, reg_val);

	reg_val = DEC_PIC_SEC_AXI_IP_ENABLE(p_dec_info->sec_axi.use_dec_ip) |
		  DEC_PIC_SEC_AXI_LF_ENABLE(p_dec_info->sec_axi.use_dec_lf_row);
	vpu_write_reg(inst->dev, W6_CMD_DEC_PIC_SEC_AXI, reg_val);

	reg_val = DEC_PIC_OPTION_MODE(W6_DEC_PIC_OPT_NORMAL);
	vpu_write_reg(inst->dev, W6_CMD_DEC_PIC_OPTION, reg_val);

	reg_val = DEC_PIC_TEMPORAL_ID_PLUS1_SPATIAL(DECODE_ALL_SPATIAL_LAYERS) |
		  DEC_PIC_TEMPORAL_ID_PLUS1(DECODE_ALL_TEMPORAL_LAYERS);
	vpu_write_reg(inst->dev, W6_CMD_DEC_PIC_TEMPORAL_ID_PLUS1, reg_val);
	vpu_write_reg(inst->dev, W6_CMD_DEC_PIC_SEQ_CHANGE_ENABLE_FLAG,
		      p_dec_info->seq_change_mask);
	reg_val = lower_32_bits(option->timestamp);
	vpu_write_reg(inst->dev, W6_CMD_DEC_PIC_TIMESTAMP_LOW, reg_val);
	reg_val = upper_32_bits(option->timestamp);
	vpu_write_reg(inst->dev, W6_CMD_DEC_PIC_TIMESTAMP_HIGH, reg_val);

	wave6_send_command(inst->dev, inst->id, inst->std, W6_CMD_DEC_PIC);
	ret = wave6_wait_vpu_busy(inst->dev, W6_VPU_BUSY_STATUS);
	if (ret) {
		dev_err(inst->dev->dev, "%s: timeout\n", __func__);
		return ret;
	}

	if (!vpu_read_reg(inst->dev, W6_RET_SUCCESS)) {
		*fail_res = vpu_read_reg(inst->dev, W6_RET_FAIL_REASON);
		wave6_print_reg_err(inst->dev, *fail_res);
		return -EIO;
	}

	return 0;
}

int wave6_vpu_dec_get_result(struct vpu_instance *inst, struct dec_output_info *result)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	u32 reg_val, i;
	int decoded_idx = -1, disp_idx = -1;
	int ret;

	lockdep_assert_held(&inst->dev->hw_lock);

	ret = wave6_send_query(inst->dev, inst->id, inst->std, W6_QUERY_OPT_GET_RESULT);
	if (ret)
		return ret;

	result->decoding_success = vpu_read_reg(inst->dev, W6_RET_DEC_DECODING_SUCCESS);
	if (!result->decoding_success)
		result->error_reason = vpu_read_reg(inst->dev, W6_RET_DEC_ERR_INFO);
	else
		result->warn_info = vpu_read_reg(inst->dev, W6_RET_DEC_WARN_INFO);

	reg_val = vpu_read_reg(inst->dev, W6_RET_DEC_PIC_TYPE);
	result->ctu_size = DEC_PIC_TYPE_CTU_SIZE(reg_val);
	result->nal_type = DEC_PIC_TYPE_NAL_UNIT_TYPE(reg_val);

	if (reg_val & DEC_PIC_TYPE_B)
		result->pic_type = PIC_TYPE_B;
	else if (reg_val & DEC_PIC_TYPE_P)
		result->pic_type = PIC_TYPE_P;
	else if (reg_val & DEC_PIC_TYPE_I)
		result->pic_type = PIC_TYPE_I;
	else
		result->pic_type = PIC_TYPE_MAX;
	if (inst->std == W_HEVC_DEC) {
		if (result->pic_type == PIC_TYPE_I &&
		    (result->nal_type == H265_NAL_UNIT_TYPE_IDR_W_RADL ||
		     result->nal_type == H265_NAL_UNIT_TYPE_IDR_N_LP))
			result->pic_type = PIC_TYPE_IDR;
	} else if (inst->std == W_AVC_DEC) {
		if (result->pic_type == PIC_TYPE_I &&
		    result->nal_type == H264_NAL_UNIT_TYPE_IDR_PICTURE)
			result->pic_type = PIC_TYPE_IDR;
	}

	reg_val = vpu_read_reg(inst->dev, W6_RET_DEC_DECODED_FLAG);
	if (reg_val) {
		struct frame_buffer fb;
		dma_addr_t addr = vpu_read_reg(inst->dev, W6_RET_DEC_DECODED_ADDR);

		fb = wave6_dec_get_display_buffer(inst, addr);
		result->frame_decoded_addr = addr;
		result->frame_decoded = true;
		decoded_idx = fb.index;
	}

	reg_val = vpu_read_reg(inst->dev, W6_RET_DEC_DISPLAY_FLAG);
	if (reg_val) {
		struct frame_buffer fb;
		dma_addr_t addr = vpu_read_reg(inst->dev, W6_RET_DEC_DISPLAY_ADDR);

		fb = wave6_dec_get_display_buffer(inst, addr);
		result->frame_display_addr = addr;
		result->frame_display = true;
		disp_idx = fb.index;
	}

	reg_val = vpu_read_reg(inst->dev, W6_RET_DEC_DISP_IDC);
	for (i = 0; i < WAVE6_MAX_FBS; i++) {
		if (reg_val & (1 << i)) {
			dma_addr_t addr;

			addr = vpu_read_reg(inst->dev, W6_RET_DEC_DISP_LINEAR_ADDR(i));

			result->disp_frame_addr[result->disp_frame_num] = addr;
			result->disp_frame_num++;
		}
	}

	reg_val = vpu_read_reg(inst->dev, W6_RET_DEC_RELEASE_IDC);
	for (i = 0; i < WAVE6_MAX_FBS; i++) {
		if (reg_val & (1 << i)) {
			dma_addr_t addr;

			addr = vpu_read_reg(inst->dev, W6_RET_DEC_DISP_LINEAR_ADDR(i));

			wave6_dec_remove_display_buffer(inst, addr);
			result->release_disp_frame_addr[result->release_disp_frame_num] = addr;
			result->release_disp_frame_num++;
		}
	}

	if (inst->std == W_HEVC_DEC) {
		result->decoded_poc = -1;
		result->display_poc = -1;
		if (decoded_idx >= 0)
			result->decoded_poc = vpu_read_reg(inst->dev, W6_RET_DEC_PIC_POC);
	} else if (inst->std == W_AVC_DEC) {
		result->decoded_poc = -1;
		result->display_poc = -1;
		if (decoded_idx >= 0)
			result->decoded_poc = vpu_read_reg(inst->dev, W6_RET_DEC_PIC_POC);
	}

	reg_val = vpu_read_reg(inst->dev, W6_RET_DEC_PIC_SIZE);
	result->dec_pic_width = DEC_PIC_SIZE_WIDTH(reg_val);
	result->dec_pic_height = DEC_PIC_SIZE_HEIGHT(reg_val);

	result->rd_ptr = wave6_vpu_dec_get_rd_ptr(inst);
	result->wr_ptr = p_dec_info->stream_wr_ptr;
	result->byte_pos_frame_start = vpu_read_reg(inst->dev, W6_RET_DEC_AU_START_POS);
	result->byte_pos_frame_end = vpu_read_reg(inst->dev, W6_RET_DEC_AU_END_POS);
	result->stream_end = vpu_read_reg(inst->dev, W6_RET_DEC_STREAM_END);
	result->notification_flags = vpu_read_reg(inst->dev, W6_RET_DEC_NOTIFICATION);
	result->last_frame_in_au = vpu_read_reg(inst->dev, W6_RET_DEC_LAST_FRAME_FLAG);

	reg_val = vpu_read_reg(inst->dev, W6_RET_DEC_TIMESTAMP_LOW);
	result->timestamp = vpu_read_reg(inst->dev, W6_RET_DEC_TIMESTAMP_HIGH);
	result->timestamp = (result->timestamp << 32) | reg_val;

	result->cycle.host_cmd_s = vpu_read_reg(inst->dev, W6_RET_CQ_IN_TICK);
	result->cycle.host_cmd_e = vpu_read_reg(inst->dev, W6_RET_RQ_OUT_TICK);
	result->cycle.proc_s = vpu_read_reg(inst->dev, W6_RET_HW_RUN_TICK);
	result->cycle.proc_e = vpu_read_reg(inst->dev, W6_RET_HW_DONE_TICK);
	result->cycle.vpu_s = vpu_read_reg(inst->dev, W6_RET_FW_RUN_TICK);
	result->cycle.vpu_e = vpu_read_reg(inst->dev, W6_RET_FW_DONE_TICK);
	result->cycle.frame_cycle = (result->cycle.vpu_e - result->cycle.host_cmd_s) *
				    CYCLE_PER_TICK_VALUE;
	result->cycle.proc_cycle = (result->cycle.proc_e - result->cycle.proc_s) *
				   CYCLE_PER_TICK_VALUE;
	result->cycle.vpu_cycle = ((result->cycle.vpu_e - result->cycle.vpu_s) -
				   (result->cycle.proc_e - result->cycle.proc_s)) *
				  CYCLE_PER_TICK_VALUE;

	if (decoded_idx >= 0 && decoded_idx < WAVE6_MAX_FBS)
		p_dec_info->dec_out_info[decoded_idx].decoded_poc = result->decoded_poc;

	if (disp_idx >= 0 && disp_idx < WAVE6_MAX_FBS) {
		result->display_poc = p_dec_info->dec_out_info[disp_idx].decoded_poc;
		result->disp_pic_width = p_dec_info->dec_out_info[disp_idx].dec_pic_width;
		result->disp_pic_height = p_dec_info->dec_out_info[disp_idx].dec_pic_height;
	}

	result->sequence_no = p_dec_info->seq_info.sequence_no;
	if (decoded_idx >= 0 && decoded_idx < WAVE6_MAX_FBS)
		p_dec_info->dec_out_info[decoded_idx] = *result;

	if (result->notification_flags & DEC_NOTI_FLAG_SEQ_CHANGE) {
		wave6_get_dec_seq_result(inst, &p_dec_info->seq_info);
		p_dec_info->seq_info.sequence_no++;
	}

	return 0;
}

int wave6_vpu_dec_fini_seq(struct vpu_instance *inst, u32 *fail_res)
{
	int ret;

	lockdep_assert_held(&inst->dev->hw_lock);

	wave6_send_command(inst->dev, inst->id, inst->std, W6_CMD_DESTROY_INSTANCE);
	ret = wave6_wait_vpu_busy(inst->dev, W6_VPU_BUSY_STATUS);
	if (ret)
		return -ETIMEDOUT;

	if (!vpu_read_reg(inst->dev, W6_RET_SUCCESS)) {
		*fail_res = vpu_read_reg(inst->dev, W6_RET_FAIL_REASON);
		wave6_print_reg_err(inst->dev, *fail_res);
		return -EIO;
	}

	return 0;
}

dma_addr_t wave6_vpu_dec_get_rd_ptr(struct vpu_instance *inst)
{
	lockdep_assert_held(&inst->dev->hw_lock);

	return vpu_read_reg(inst->dev, W6_RET_DEC_BS_RD_PTR);
}

int wave6_vpu_dec_flush(struct vpu_instance *inst)
{
	int ret, index;
	u32 unused_idc;
	u32 used_idc;
	u32 using_idc;

	lockdep_assert_held(&inst->dev->hw_lock);

	wave6_send_command(inst->dev, inst->id, inst->std, W6_CMD_FLUSH_INSTANCE);
	ret = wave6_wait_vpu_busy(inst->dev, W6_VPU_BUSY_STATUS);
	if (ret)
		return -ETIMEDOUT;

	if (!vpu_read_reg(inst->dev, W6_RET_SUCCESS)) {
		u32 reg_val;

		reg_val = vpu_read_reg(inst->dev, W6_RET_FAIL_REASON);
		wave6_print_reg_err(inst->dev, reg_val);
		return -EIO;
	}

	ret = wave6_send_query(inst->dev, inst->id, inst->std, W6_QUERY_OPT_GET_FLUSH_CMD_INFO);
	if (ret)
		return ret;

	unused_idc = vpu_read_reg(inst->dev, W6_RET_DEC_FLUSH_CMD_BUF_STATE_UNUSED_IDC);
	if (unused_idc)
		dev_dbg(inst->dev->dev, "%s: unused_idc %d\n", __func__, unused_idc);

	used_idc = vpu_read_reg(inst->dev, W6_RET_DEC_FLUSH_CMD_BUF_STATE_USED_IDC);
	if (used_idc)
		dev_dbg(inst->dev->dev, "%s: used_idc %d\n", __func__, used_idc);

	using_idc = vpu_read_reg(inst->dev, W6_RET_DEC_FLUSH_CMD_BUF_STATE_USING_IDC);
	if (using_idc)
		dev_err(inst->dev->dev, "%s: using_idc %d\n", __func__, using_idc);

	for (index = 0; index < WAVE6_MAX_FBS; index++) {
		dma_addr_t addr;

		addr = vpu_read_reg(inst->dev, W6_RET_DEC_FLUSH_CMD_DISP_ADDR(index));

		if ((unused_idc >> index) & 0x1)
			wave6_dec_remove_display_buffer(inst, addr);
		if ((used_idc >> index) & 0x1)
			wave6_dec_remove_display_buffer(inst, addr);
	}

	return 0;
}

struct enc_cmd_set_param_reg {
	u32 enable;
	u32 src_size;
	u32 custom_map_endian;
	u32 sps;
	u32 pps;
	u32 gop;
	u32 intra;
	u32 conf_win0;
	u32 conf_win1;
	u32 rdo;
	u32 slice;
	u32 intra_refresh;
	u32 intra_qp;
	u32 rc_frame_rate;
	u32 rc_target_rate;
	u32 rc;
	u32 hvs;
	u32 rc_max_bitrate;
	u32 rc_vbv_buffer_size;
	u32 inter_qp;
	u32 rot_param;
	u32 num_units_in_tick;
	u32 time_scale;
	u32 num_ticks_poc_diff_one;
	u32 max_intra_pic_bit;
	u32 max_inter_pic_bit;
	u32 bg;
	u32 non_vcl_param;
	u32 vui_rbsp_addr;
	u32 hrd_rbsp_addr;
	u32 qround_offset;
	u32 quant1;
	u32 quant2;
	u32 custom_gop;
	u32 custom_gop_pic[MAX_CUSTOM_GOP_NUM];
	u32 tile_param;
	u32 custom_lambda[MAX_CUSTOM_LAMBDA_NUM];
	u32 temp_layer_qp[MAX_NUM_CHANGEABLE_TEMP_LAYER];
	u32 scaler_size;
	u32 scaler;
	u32 color;
	u32 sar;
	u32 sar_extended;
};

struct enc_cmd_change_param_reg {
	u32 enable;
	u32 rc_target_rate;
};

int wave6_vpu_build_up_enc_param(struct device *dev, struct vpu_instance *inst,
				 struct enc_open_param *param)
{
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	u32 reg_val;
	int ret;

	lockdep_assert_held(&inst->dev->hw_lock);

	p_enc_info->stride = 0;
	p_enc_info->seq_info_obtained = false;
	p_enc_info->sec_axi.use_enc_rdo = true;
	p_enc_info->sec_axi.use_enc_lf = true;
	p_enc_info->mir_dir = param->mir_dir;
	p_enc_info->rot_angle = param->rot_angle;

	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_TEMP_BASE, param->inst_buffer.temp_base);
	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_TEMP_SIZE, param->inst_buffer.temp_size);
	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_AR_TABLE_BASE, param->inst_buffer.ar_base);
	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_BS_PARAM, VPU_STREAM_ENDIAN);
	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_SRC_OPT, 0);
	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_ADDR_EXT, param->ext_addr_vcpu);

	reg_val = CREATE_INST_CORE_INFO_CQ_DEPTH(COMMAND_QUEUE_DEPTH) |
		  CREATE_INST_CORE_INFO_CORE_IDC(SINGLE_CORE_IDC) |
		  CREATE_INST_CORE_INFO_CORE_NUM(SINGLE_CORE);
	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_CORE_INFO, reg_val);

	reg_val = CREATE_INST_PRIORITY_SECURITY_FLAG(param->is_secure_inst) |
		  CREATE_INST_PRIORITY_VALUE(param->inst_priority);
	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_PRIORITY, reg_val);
	vpu_write_reg(inst->dev, W6_CMD_CREATE_INST_TIMEOUT_CYCLE_COUNT,
		      W6_VPU_TIMEOUT_CYCLE_COUNT);

	wave6_send_command(inst->dev, 0, inst->std, W6_CMD_CREATE_INSTANCE);
	ret = wave6_wait_vpu_busy(inst->dev, W6_VPU_BUSY_STATUS);
	if (ret) {
		dev_err(inst->dev->dev, "%s: timeout\n", __func__);
		return ret;
	}

	if (!vpu_read_reg(inst->dev, W6_RET_SUCCESS)) {
		u32 reason_code = vpu_read_reg(inst->dev, W6_RET_FAIL_REASON);

		wave6_print_reg_err(inst->dev, reason_code);
		return -EIO;
	}

	inst->id = vpu_read_reg(inst->dev, W6_RET_INSTANCE_ID);

	return 0;
}

static int wave6_set_enc_crop_info(u32 codec, struct enc_codec_param *param, int rot_mode,
				   int width, int height)
{
	int aligned_width = (codec == W_HEVC_ENC) ? ALIGN(width, 32) : ALIGN(width, 16);
	int aligned_height = (codec == W_HEVC_ENC) ? ALIGN(height, 32) : ALIGN(height, 16);
	int pad_right, pad_bot;
	int crop_right, crop_left, crop_top, crop_bot;
	int prp_mode = rot_mode >> 1;

	if (codec == W_HEVC_ENC &&
	    (!rot_mode || prp_mode == 14))
		return 0;

	pad_right = aligned_width - width;
	pad_bot = aligned_height - height;

	if (param->conf_win.right > 0)
		crop_right = param->conf_win.right + pad_right;
	else
		crop_right = pad_right;

	if (param->conf_win.bottom > 0)
		crop_bot = param->conf_win.bottom + pad_bot;
	else
		crop_bot = pad_bot;

	crop_top = param->conf_win.top;
	crop_left = param->conf_win.left;

	param->conf_win.top = crop_top;
	param->conf_win.left = crop_left;
	param->conf_win.bottom = crop_bot;
	param->conf_win.right = crop_right;

	if (prp_mode == 1 || prp_mode == 15) {
		param->conf_win.top = crop_right;
		param->conf_win.left = crop_top;
		param->conf_win.bottom = crop_left;
		param->conf_win.right = crop_bot;
	} else if (prp_mode == 2 || prp_mode == 12) {
		param->conf_win.top = crop_bot;
		param->conf_win.left = crop_right;
		param->conf_win.bottom = crop_top;
		param->conf_win.right = crop_left;
	} else if (prp_mode == 3 || prp_mode == 13) {
		param->conf_win.top = crop_left;
		param->conf_win.left = crop_bot;
		param->conf_win.bottom = crop_right;
		param->conf_win.right = crop_top;
	} else if (prp_mode == 4 || prp_mode == 10) {
		param->conf_win.top = crop_bot;
		param->conf_win.bottom = crop_top;
	} else if (prp_mode == 8 || prp_mode == 6) {
		param->conf_win.left = crop_right;
		param->conf_win.right = crop_left;
	} else if (prp_mode == 5 || prp_mode == 11) {
		param->conf_win.top = crop_left;
		param->conf_win.left = crop_top;
		param->conf_win.bottom = crop_right;
		param->conf_win.right = crop_bot;
	} else if (prp_mode == 7 || prp_mode == 9) {
		param->conf_win.top = crop_right;
		param->conf_win.left = crop_bot;
		param->conf_win.bottom = crop_left;
		param->conf_win.right = crop_top;
	}

	return 0;
}

static void wave6_update_enc_info(struct enc_info *p_enc_info)
{
	struct enc_open_param op = p_enc_info->open_param;

	p_enc_info->width = op.pic_width;
	p_enc_info->height = op.pic_height;
	p_enc_info->c_fmt_idc = get_chroma_format_idc(op.output_format);
}

static void wave6_gen_set_param_reg_common(struct enc_info *p_enc_info, enum codec_std std,
					   struct enc_cmd_set_param_reg *reg)
{
	struct enc_open_param *p_open_param = &p_enc_info->open_param;
	struct enc_codec_param *p_param = &p_open_param->codec_param;
	unsigned int i;
	u32 rot_mir_mode = 0;

	switch (p_enc_info->rot_angle) {
	case ROT_0:
		rot_mir_mode |= 0x0; break;
	case ROT_90:
		rot_mir_mode |= 0x3; break;
	case ROT_180:
		rot_mir_mode |= 0x5; break;
	case ROT_270:
		rot_mir_mode |= 0x7; break;
	}

	switch (p_enc_info->mir_dir) {
	case MIR_NONE:
		rot_mir_mode |= 0x0; break;
	case MIR_VER:
		rot_mir_mode |= 0x9; break;
	case MIR_HOR:
		rot_mir_mode |= 0x11; break;
	case MIR_HOR_VER:
		rot_mir_mode |= 0x19; break;
	}

	wave6_set_enc_crop_info(std, p_param, rot_mir_mode, p_enc_info->width, p_enc_info->height);

	reg->custom_map_endian = VPU_USER_DATA_ENDIAN;
	reg->rot_param = rot_mir_mode;
	reg->src_size = SET_PARAM_SRC_SIZE_HEIGHT(p_enc_info->height) |
			SET_PARAM_SRC_SIZE_WIDTH(p_enc_info->width);
	reg->conf_win0 = SET_PARAM_CONF_WIN0_BOTTOM(p_param->conf_win.bottom) |
			 SET_PARAM_CONF_WIN0_TOP(p_param->conf_win.top);
	reg->conf_win1 = SET_PARAM_CONF_WIN1_RIGHT(p_param->conf_win.right) |
			 SET_PARAM_CONF_WIN1_LEFT(p_param->conf_win.left);
	reg->gop = SET_PARAM_GOP_TEMP_LAYER_CNT(p_param->temp_layer_cnt) |
		   SET_PARAM_GOP_TEMP_LAYER3_QP_ENABLE(p_param->temp_layer[3].change_qp) |
		   SET_PARAM_GOP_TEMP_LAYER2_QP_ENABLE(p_param->temp_layer[2].change_qp) |
		   SET_PARAM_GOP_TEMP_LAYER1_QP_ENABLE(p_param->temp_layer[1].change_qp) |
		   SET_PARAM_GOP_TEMP_LAYER0_QP_ENABLE(p_param->temp_layer[0].change_qp) |
		   SET_PARAM_GOP_PRESET_IDX(p_param->gop_preset_idx);
	for (i = 0; i < MAX_NUM_CHANGEABLE_TEMP_LAYER; i++) {
		reg->temp_layer_qp[i] = SET_PARAM_TEMP_LAYER_QP_B(p_param->temp_layer[i].qp_b) |
					SET_PARAM_TEMP_LAYER_QP_P(p_param->temp_layer[i].qp_p) |
					SET_PARAM_TEMP_LAYER_QP_I(p_param->temp_layer[i].qp_i);
	}
	reg->intra_refresh = SET_PARAM_INTRA_REFRESH_ARGUMENT(p_param->intra_refresh_arg) |
			     SET_PARAM_INTRA_REFRESH_MODE(p_param->intra_refresh_mode);
	reg->intra_qp = SET_PARAM_INTRA_QP_MAX(p_param->max_qp_i) |
			SET_PARAM_INTRA_QP_MIN(p_param->min_qp_i);
	reg->inter_qp = SET_PARAM_INTER_QP_MAX_B(p_param->max_qp_b) |
			SET_PARAM_INTER_QP_MIN_B(p_param->min_qp_b) |
			SET_PARAM_INTER_QP_MAX_P(p_param->max_qp_p) |
			SET_PARAM_INTER_QP_MIN_P(p_param->min_qp_p);
	reg->rc_frame_rate = p_param->frame_rate;
	reg->rc_target_rate = p_param->bitrate;
	reg->rc_max_bitrate = p_param->max_bitrate;
	reg->rc_vbv_buffer_size = p_param->cpb_size;
	reg->rc = SET_PARAM_RC_UPDATE_SPEED(p_param->rc_update_speed) |
		  SET_PARAM_RC_INITIAL_LEVEL(p_param->rc_initial_level) |
		  SET_PARAM_RC_INITIAL_QP(p_param->rc_initial_qp) |
		  SET_PARAM_RC_MODE(p_param->rc_mode) |
		  SET_PARAM_RC_PIC_LEVEL_MAX_DELTA_QP(p_param->pic_rc_max_dqp) |
		  SET_PARAM_RC_VBV_OVERFLOW_DROP_FRAME(p_param->en_skip_frame) |
		  SET_PARAM_RC_CU_LEVEL_ENABLE(p_param->en_cu_level_rate_control) |
		  SET_PARAM_RC_ENABLE(p_param->en_rate_control);
	reg->hvs = SET_PARAM_HVS_MAX_DELTA_QP(p_param->max_delta_qp) |
		   SET_PARAM_HVS_QP_SCALE(p_param->hvs_qp_scale_div2);
	reg->num_units_in_tick = p_param->num_units_in_tick;
	reg->time_scale = p_param->time_scale;
	reg->num_ticks_poc_diff_one = p_param->num_ticks_poc_diff_one;
	reg->max_intra_pic_bit = p_param->max_intra_pic_bit;
	reg->max_inter_pic_bit = p_param->max_inter_pic_bit;
	reg->bg = SET_PARAM_BG_DELTA_QP(p_param->bg_delta_qp) |
		  SET_PARAM_BG_THRESHOLD_MEAN_DIFF(p_param->bg_th_mean_diff) |
		  SET_PARAM_BG_THRESHOLD_MAX_DIFF(p_param->bg_th_diff) |
		  SET_PARAM_BG_ENABLE(p_param->en_bg_detect);
	reg->qround_offset = SET_PARAM_QROUND_OFFSET_INTER(p_param->qround_inter) |
			     SET_PARAM_QROUND_OFFSET_INTRA(p_param->qround_intra);
	reg->custom_gop = p_param->gop_param.size;
	for (i = 0; i < p_param->gop_param.size; i++) {
		struct custom_gop_pic_param pic = p_param->gop_param.pic[i];

		reg->custom_gop_pic[i] = SET_PARAM_CUSTOM_GOP_PIC_TEMP_ID(pic.temporal_id) |
					 SET_PARAM_CUSTOM_GOP_PIC_REF1_POC(pic.ref_poc_l1) |
					 SET_PARAM_CUSTOM_GOP_PIC_REF0_POC(pic.ref_poc_l0) |
					 SET_PARAM_CUSTOM_GOP_PIC_MULTI_REF_P(pic.multi_ref_p) |
					 SET_PARAM_CUSTOM_GOP_PIC_QP(pic.pic_qp) |
					 SET_PARAM_CUSTOM_GOP_PIC_POC_OFFSET(pic.poc_offset) |
					 SET_PARAM_CUSTOM_GOP_PIC_TYPE(pic.pic_type);
	}
	for (i = 0; i < MAX_CUSTOM_LAMBDA_NUM; i++) {
		reg->custom_lambda[i] = SET_PARAM_CUSTOM_LAMBDA_SSD(p_param->lambda_ssd[i]) |
					SET_PARAM_CUSTOM_LAMBDA_SAD(p_param->lambda_sad[i]);
	}
	reg->scaler_size = SET_PARAM_SCALER_SIZE_HEIGHT(p_enc_info->height) |
			   SET_PARAM_SCALER_SIZE_WIDTH(p_enc_info->width);
	reg->scaler = SET_PARAM_SCALER_COEF_MODE(p_enc_info->scaler.coef_mode) |
		      SET_PARAM_SCALER_ENABLE(p_enc_info->scaler.enable);
	reg->color = SET_PARAM_COLOR_RANGE(p_param->color.color_range) |
		     SET_PARAM_COLOR_MATRIX_COEFF(p_param->color.matrix_coefficients) |
		     SET_PARAM_COLOR_TRANS_CHAR(p_param->color.transfer_characteristics) |
		     SET_PARAM_COLOR_PRIMARIES(p_param->color.color_primaries) |
		     SET_PARAM_COLOR_DESCRIPTION_PRESENT_ON;
	reg->sar = SET_PARAM_SAR_ASPECT_RATIO_IDC(p_param->sar.idc) |
		   SET_PARAM_SAR_ASPECT_RATIO_ENABLE(p_param->sar.enable);
	reg->sar_extended = SET_PARAM_SAR_EXTENDED_HEIGHT(p_param->sar.height) |
			    SET_PARAM_SAR_EXTENDED_WIDTH(p_param->sar.width);
}

static void wave6_gen_set_param_reg_hevc(struct enc_info *p_enc_info,
					 struct enc_cmd_set_param_reg *reg)
{
	struct enc_open_param *p_open_param = &p_enc_info->open_param;
	struct enc_codec_param *p_param = &p_open_param->codec_param;

	reg->sps = SET_PARAM_SPS_DEFAULT_SCALING_LIST(p_param->en_scaling_list) |
		   SET_PARAM_SPS_STILL_PICTURE(p_param->en_still_picture) |
		   SET_PARAM_SPS_AUTO_LEVEL_ADJUSTING_ON |
		   SET_PARAM_SPS_STRONG_INTRA_SMOOTHING(p_param->en_intra_smooth) |
		   SET_PARAM_SPS_INTRA_TRANSFORM_SKIP_ON |
		   SET_PARAM_SPS_SAMPLE_ADAPTIVE_OFFSET(p_param->en_sao) |
		   SET_PARAM_SPS_TEMPORAL_MVP(p_param->en_temporal_mvp) |
		   SET_PARAM_SPS_LONGTERM_REFERENCE(p_param->en_longterm) |
		   SET_PARAM_SPS_C_FMT_IDC(p_enc_info->c_fmt_idc) |
		   SET_PARAM_SPS_BIT_DEPTH(p_param->internal_bit_depth) |
		   SET_PARAM_SPS_TIER(p_param->tier) |
		   SET_PARAM_SPS_LEVEL(p_param->level) |
		   SET_PARAM_SPS_PROFILE(p_param->profile);
	reg->pps = SET_PARAM_PPS_CR_QP_OFFSET(p_param->cr_qp_offset) |
		   SET_PARAM_PPS_CB_QP_OFFSET(p_param->cb_qp_offset) |
		   SET_PARAM_PPS_TC_OFFSET_DIV2(p_param->tc_offset_div2) |
		   SET_PARAM_PPS_BETA_OFFSET_DIV2(p_param->beta_offset_div2) |
		   SET_PARAM_PPS_DEBLOCKING_FILTER(!p_param->en_dbk) |
		   SET_PARAM_PPS_LF_SLICE_BOUNDARY(p_param->en_lf_slice_boundary) |
		   SET_PARAM_PPS_CONST_INTRA_PREDICTION(p_param->en_const_intra_pred);
	reg->intra = SET_PARAM_INTRA_PERIOD(p_param->intra_period) |
		     SET_PARAM_INTRA_HEADER_MODE(p_param->forced_idr_header) |
		     SET_PARAM_INTRA_QP(p_param->qp) |
		     SET_PARAM_INTRA_REFRESH_TYPE(p_param->decoding_refresh_type);
	reg->rdo = SET_PARAM_RDO_CUSTOM_LAMBDA(p_param->en_custom_lambda) |
		   SET_PARAM_RDO_ME_SEARCH_CENTER_ON |
		   SET_PARAM_RDO_QROUND_OFFSET(p_param->en_qround_offset) |
		   SET_PARAM_RDO_ADAPTIVE_ROUND_ON |
		   SET_PARAM_RDO_HVS_QP(p_param->en_hvs_qp);
	reg->slice = SET_PARAM_SLICE_ARGUMENT(p_param->slice_arg) |
		     SET_PARAM_SLICE_MODE(p_param->slice_mode);
	reg->quant2 = SET_PARAM_QUANT2_LAMBDA_DQP_INTER(p_param->lambda_dqp_inter) |
		      SET_PARAM_QUANT2_LAMBDA_DQP_INTRA(p_param->lambda_dqp_intra);
	reg->non_vcl_param = SET_PARAM_NON_VCL_PARAM_ENCODE_VUI;
}

static void wave6_gen_set_param_reg_avc(struct enc_info *p_enc_info,
					struct enc_cmd_set_param_reg *reg)
{
	struct enc_open_param *p_open_param = &p_enc_info->open_param;
	struct enc_codec_param *p_param = &p_open_param->codec_param;

	reg->sps = SET_PARAM_SPS_DEFAULT_SCALING_LIST(p_param->en_scaling_list) |
		   SET_PARAM_SPS_AUTO_LEVEL_ADJUSTING_ON |
		   SET_PARAM_SPS_LONGTERM_REFERENCE(p_param->en_longterm) |
		   SET_PARAM_SPS_C_FMT_IDC(p_enc_info->c_fmt_idc) |
		   SET_PARAM_SPS_BIT_DEPTH(p_param->internal_bit_depth) |
		   SET_PARAM_SPS_LEVEL(p_param->level) |
		   SET_PARAM_SPS_PROFILE(p_param->profile);
	reg->pps = SET_PARAM_PPS_ENTROPY_CODING_MODE(p_param->en_cabac) |
		   SET_PARAM_PPS_TRANSFORM8X8(p_param->en_transform8x8) |
		   SET_PARAM_PPS_CR_QP_OFFSET(p_param->cr_qp_offset) |
		   SET_PARAM_PPS_CB_QP_OFFSET(p_param->cb_qp_offset) |
		   SET_PARAM_PPS_TC_OFFSET_DIV2(p_param->tc_offset_div2) |
		   SET_PARAM_PPS_BETA_OFFSET_DIV2(p_param->beta_offset_div2) |
		   SET_PARAM_PPS_DEBLOCKING_FILTER(!p_param->en_dbk) |
		   SET_PARAM_PPS_LF_SLICE_BOUNDARY(p_param->en_lf_slice_boundary) |
		   SET_PARAM_PPS_CONST_INTRA_PREDICTION(p_param->en_const_intra_pred);
	reg->intra = SET_PARAM_INTRA_HEADER_MODE_AVC(p_param->forced_idr_header) |
		     SET_PARAM_INTRA_IDR_PERIOD_AVC(p_param->idr_period) |
		     SET_PARAM_INTRA_PERIOD_AVC(p_param->intra_period) |
		     SET_PARAM_INTRA_QP_AVC(p_param->qp);
	reg->rdo = SET_PARAM_RDO_CUSTOM_LAMBDA(p_param->en_custom_lambda) |
		   SET_PARAM_RDO_QROUND_OFFSET(p_param->en_qround_offset) |
		   SET_PARAM_RDO_ADAPTIVE_ROUND_ON |
		   SET_PARAM_RDO_HVS_QP(p_param->en_hvs_qp);
	reg->slice = SET_PARAM_SLICE_ARGUMENT(p_param->slice_arg) |
		     SET_PARAM_SLICE_MODE(p_param->slice_mode);
	reg->quant2 = SET_PARAM_QUANT2_LAMBDA_DQP_INTER(p_param->lambda_dqp_inter) |
		      SET_PARAM_QUANT2_LAMBDA_DQP_INTRA(p_param->lambda_dqp_intra);
	reg->non_vcl_param = SET_PARAM_NON_VCL_PARAM_ENCODE_VUI;
}

static void wave6_gen_change_param_reg_common(struct vpu_instance *inst,
					      struct enc_info *p_enc_info,
					      struct enc_cmd_change_param_reg *reg)
{
	if (p_enc_info->open_param.codec_param.bitrate != inst->enc_ctrls.bitrate) {
		reg->enable |= SET_PARAM_ENABLE_RC_TARGET_RATE;
		reg->rc_target_rate = inst->enc_ctrls.bitrate;
		p_enc_info->open_param.codec_param.bitrate = inst->enc_ctrls.bitrate;
	}
}

int wave6_vpu_enc_init_seq(struct vpu_instance *inst)
{
	struct enc_cmd_set_param_reg reg;
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	u32 i;
	int ret;

	lockdep_assert_held(&inst->dev->hw_lock);

	memset(&reg, 0, sizeof(struct enc_cmd_set_param_reg));

	wave6_update_enc_info(p_enc_info);

	wave6_gen_set_param_reg_common(p_enc_info, inst->std, &reg);
	if (inst->std == W_HEVC_ENC)
		wave6_gen_set_param_reg_hevc(p_enc_info, &reg);
	else if (inst->std == W_AVC_ENC)
		wave6_gen_set_param_reg_avc(p_enc_info, &reg);

	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_OPTION, W6_SET_PARAM_OPT_COMMON);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_ENABLE, reg.enable);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_SRC_SIZE, reg.src_size);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_CUSTOM_MAP_ENDIAN, reg.custom_map_endian);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_SPS, reg.sps);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_PPS, reg.pps);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_GOP, reg.gop);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_INTRA, reg.intra);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_CONF_WIN0, reg.conf_win0);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_CONF_WIN1, reg.conf_win1);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_RDO, reg.rdo);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_SLICE, reg.slice);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_INTRA_REFRESH, reg.intra_refresh);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_INTRA_QP, reg.intra_qp);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_RC_FRAME_RATE, reg.rc_frame_rate);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_RC_TARGET_RATE, reg.rc_target_rate);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_RC, reg.rc);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_HVS, reg.hvs);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_RC_MAX_BITRATE, reg.rc_max_bitrate);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_RC_VBV_BUFFER_SIZE, reg.rc_vbv_buffer_size);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_INTER_QP, reg.inter_qp);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_ROT_PARAM, reg.rot_param);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_NUM_UNITS_IN_TICK, reg.num_units_in_tick);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_TIME_SCALE, reg.time_scale);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_NUM_TICKS_POC_DIFF_ONE,
		      reg.num_ticks_poc_diff_one);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_MAX_INTRA_PIC_BIT, reg.max_intra_pic_bit);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_MAX_INTER_PIC_BIT, reg.max_inter_pic_bit);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_BG, reg.bg);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_NON_VCL_PARAM, reg.non_vcl_param);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_VUI_RBSP_ADDR, reg.vui_rbsp_addr);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_HRD_RBSP_ADDR, reg.hrd_rbsp_addr);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_QROUND_OFFSET, reg.qround_offset);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_QUANT1, reg.quant1);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_QUANT2, reg.quant2);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_CUSTOM_GOP, reg.custom_gop);
	for (i = 0; i < MAX_CUSTOM_GOP_NUM; i++)
		vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_CUSTOM_GOP_PIC(i),
			      reg.custom_gop_pic[i]);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_TILE_PARAM, reg.tile_param);
	for (i = 0; i < MAX_CUSTOM_LAMBDA_NUM; i++)
		vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_CUSTOM_LAMBDA(i),
			      reg.custom_lambda[i]);
	for (i = 0; i < MAX_NUM_CHANGEABLE_TEMP_LAYER; i++)
		vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_TEMP_LAYER_QP(i),
			      reg.temp_layer_qp[i]);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_SCALER_SIZE, reg.scaler_size);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_SCALER, reg.scaler);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_COLOR, reg.color);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_SAR, reg.sar);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_SAR_EXTENDED, reg.sar_extended);

	wave6_send_command(inst->dev, inst->id, inst->std, W6_CMD_ENC_SET_PARAM);
	ret = wave6_wait_vpu_busy(inst->dev, W6_VPU_BUSY_STATUS);
	if (ret) {
		dev_err(inst->dev->dev, "%s: timeout\n", __func__);
		return ret;
	}

	if (!vpu_read_reg(inst->dev, W6_RET_SUCCESS)) {
		u32 reason_code = vpu_read_reg(inst->dev, W6_RET_FAIL_REASON);

		wave6_print_reg_err(inst->dev, reason_code);
		return -EIO;
	}

	return 0;
}

int wave6_vpu_enc_get_seq_info(struct vpu_instance *inst, struct enc_seq_info *info)
{
	int ret;

	lockdep_assert_held(&inst->dev->hw_lock);

	ret = wave6_send_query(inst->dev, inst->id, inst->std, W6_QUERY_OPT_GET_RESULT);
	if (ret)
		return ret;

	if (vpu_read_reg(inst->dev, W6_RET_ENC_ENCODING_SUCCESS) != 1) {
		info->err_reason = vpu_read_reg(inst->dev, W6_RET_ENC_ERR_INFO);
		ret = -EIO;
	} else {
		info->warn_info = vpu_read_reg(inst->dev, W6_RET_ENC_WARN_INFO);
	}

	info->min_frame_buffer_count = vpu_read_reg(inst->dev, W6_RET_ENC_NUM_REQUIRED_FBC_FB);
	info->min_src_frame_count = vpu_read_reg(inst->dev, W6_RET_ENC_MIN_SRC_BUF_NUM);
	info->max_latency_pictures = vpu_read_reg(inst->dev, W6_RET_ENC_PIC_MAX_LATENCY_PICTURES);
	info->req_mv_buffer_count = vpu_read_reg(inst->dev, W6_RET_ENC_NUM_REQUIRED_COL_BUF);

	return ret;
}

int wave6_vpu_enc_change_seq(struct vpu_instance *inst, bool *changed)
{
	struct enc_cmd_change_param_reg reg;
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	int ret;

	lockdep_assert_held(&inst->dev->hw_lock);

	memset(&reg, 0, sizeof(struct enc_cmd_change_param_reg));

	wave6_gen_change_param_reg_common(inst, p_enc_info, &reg);

	if (!reg.enable)
		return 0;

	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_OPTION, W6_SET_PARAM_OPT_CHANGE_PARAM);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_ENABLE, reg.enable);
	vpu_write_reg(inst->dev, W6_CMD_ENC_SET_PARAM_RC_TARGET_RATE, reg.rc_target_rate);

	wave6_send_command(inst->dev, inst->id, inst->std, W6_CMD_ENC_SET_PARAM);
	ret = wave6_wait_vpu_busy(inst->dev, W6_VPU_BUSY_STATUS);
	if (ret) {
		dev_warn(inst->dev->dev, "enc set param timed out\n");
		return ret;
	}

	if (!vpu_read_reg(inst->dev, W6_RET_SUCCESS)) {
		u32 reason_code = vpu_read_reg(inst->dev, W6_RET_FAIL_REASON);

		wave6_print_reg_err(inst->dev, reason_code);
		return -EIO;
	}

	*changed = true;

	return 0;
}

struct enc_cmd_set_fb_reg {
	u32 option;
	u32 pic_info;
	u32 pic_size;
	u32 num_fb;
	u32 fbc_stride;
	u32 fbc_y[WAVE6_MAX_FBS];
	u32 fbc_c[WAVE6_MAX_FBS];
	u32 fbc_cr[WAVE6_MAX_FBS];
	u32 fbc_y_offset[WAVE6_MAX_FBS];
	u32 fbc_c_offset[WAVE6_MAX_FBS];
	u32 fbc_cr_offset[WAVE6_MAX_FBS];
	u32 mv_col[WAVE6_MAX_FBS];
	u32 sub_sampled[WAVE6_MAX_FBS];
	u32 default_cdf;
};

static void wave6_gen_set_fb_reg(struct enc_info *p_enc_info, enum codec_std std,
				 struct frame_buffer *fb_arr, struct enc_cmd_set_fb_reg *reg)
{
	u32 mv_count = p_enc_info->seq_info.req_mv_buffer_count;
	u32 buf_width, buf_height;
	u32 stride_l, stride_c, i;

	if (std == W_AVC_ENC) {
		buf_width = ALIGN(p_enc_info->width, 16);
		buf_height = ALIGN(p_enc_info->height, 16);
		if (p_enc_info->rot_angle == ROT_90 || p_enc_info->rot_angle == ROT_270) {
			buf_width = ALIGN(p_enc_info->height, 16);
			buf_height = ALIGN(p_enc_info->width, 16);
		}
	} else {
		buf_width = ALIGN(p_enc_info->width, 8);
		buf_height = ALIGN(p_enc_info->height, 8);
		if ((p_enc_info->rot_angle || p_enc_info->mir_dir) &&
		    !(p_enc_info->rot_angle == ROT_180 && p_enc_info->mir_dir == MIR_HOR_VER)) {
			buf_width = ALIGN(p_enc_info->width, 32);
			buf_height = ALIGN(p_enc_info->height, 32);
		}
		if (p_enc_info->rot_angle == ROT_90 || p_enc_info->rot_angle == ROT_270) {
			buf_width = ALIGN(p_enc_info->height, 32);
			buf_height = ALIGN(p_enc_info->width, 32);
		}
	}

	if ((p_enc_info->rot_angle || p_enc_info->mir_dir) &&
	    !(p_enc_info->rot_angle == ROT_180 && p_enc_info->mir_dir == MIR_HOR_VER)) {
		stride_l = ALIGN((buf_width + 63), 64);
		stride_c = ALIGN((buf_width + 31), 32) / 2;
	} else {
		stride_l = ALIGN((p_enc_info->width) + 63, 64);
		stride_c = ALIGN((p_enc_info->width) + 31, 32) / 2;
	}

	reg->option = SET_FB_OPTION_END | SET_FB_OPTION_START;
	reg->pic_info = SET_FB_PIC_INFO_STRIDE(p_enc_info->stride);
	reg->pic_size = SET_FB_PIC_SIZE_WIDTH(buf_width) |
			SET_FB_PIC_SIZE_HEIGHT(buf_height);
	reg->num_fb = SET_FB_NUM_FBC_END_IDX(p_enc_info->num_frame_buffers - 1) |
		      SET_FB_NUM_MV_COL_END_IDX(mv_count - 1);
	reg->fbc_stride = SET_FB_FBC_STRIDE_L(stride_l) |
			  SET_FB_FBC_STRIDE_C(stride_c);
	reg->default_cdf = 0;

	for (i = 0; i < p_enc_info->num_frame_buffers; i++) {
		reg->fbc_y[i] = fb_arr[i].buf_y;
		reg->fbc_c[i] = fb_arr[i].buf_cb;
		reg->fbc_cr[i] = fb_arr[i].buf_cr;
		reg->fbc_y_offset[i] = p_enc_info->vb_fbc_y_tbl[i].daddr;
		reg->fbc_c_offset[i] = p_enc_info->vb_fbc_c_tbl[i].daddr;
		reg->fbc_cr_offset[i] = p_enc_info->vb_fbc_c_tbl[i].daddr +
						(p_enc_info->vb_fbc_c_tbl[i].size >> 1);
		reg->sub_sampled[i] = p_enc_info->vb_sub_sam_buf[i].daddr;
	}
	for (i = 0; i < mv_count; i++)
		reg->mv_col[i] = p_enc_info->vb_mv[i].daddr;
}

int wave6_vpu_enc_register_frame_buffer(struct vpu_instance *inst, struct frame_buffer *fb_arr)
{
	struct enc_cmd_set_fb_reg *reg;
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	u32 mv_count = p_enc_info->seq_info.req_mv_buffer_count;
	int ret;
	u32 idx;

	lockdep_assert_held(&inst->dev->hw_lock);

	for (idx = 0; idx < p_enc_info->num_frame_buffers; idx++) {
		if (!p_enc_info->vb_fbc_y_tbl[idx].daddr)
			return -EINVAL;
		if (!p_enc_info->vb_fbc_c_tbl[idx].daddr)
			return -EINVAL;
		if (!p_enc_info->vb_sub_sam_buf[idx].daddr)
			return -EINVAL;
	}
	for (idx = 0; idx < mv_count; idx++) {
		if (!p_enc_info->vb_mv[idx].daddr)
			return -EINVAL;
	}

	reg = kzalloc(sizeof(*reg), GFP_KERNEL);
	if (!reg)
		return -ENOMEM;

	wave6_gen_set_fb_reg(p_enc_info, inst->std, fb_arr, reg);

	vpu_write_reg(inst->dev, W6_CMD_SET_FB_OPTION, reg->option);
	vpu_write_reg(inst->dev, W6_CMD_SET_FB_PIC_INFO, reg->pic_info);
	vpu_write_reg(inst->dev, W6_CMD_SET_FB_PIC_SIZE, reg->pic_size);
	vpu_write_reg(inst->dev, W6_CMD_SET_FB_NUM, reg->num_fb);
	vpu_write_reg(inst->dev, W6_CMD_SET_FB_FBC_STRIDE, reg->fbc_stride);
	vpu_write_reg(inst->dev, W6_CMD_SET_FB_DEFAULT_CDF, reg->default_cdf);
	for (idx = 0; idx < p_enc_info->num_frame_buffers; idx++) {
		vpu_write_reg(inst->dev, W6_CMD_SET_FB_FBC_Y(idx), reg->fbc_y[idx]);
		vpu_write_reg(inst->dev, W6_CMD_SET_FB_FBC_C(idx), reg->fbc_c[idx]);
		vpu_write_reg(inst->dev, W6_CMD_SET_FB_FBC_CR(idx), reg->fbc_cr[idx]);
		vpu_write_reg(inst->dev, W6_CMD_SET_FB_FBC_Y_OFFSET(idx),
			      reg->fbc_y_offset[idx]);
		vpu_write_reg(inst->dev, W6_CMD_SET_FB_FBC_C_OFFSET(idx),
			      reg->fbc_c_offset[idx]);
		vpu_write_reg(inst->dev, W6_CMD_SET_FB_FBC_CR_OFFSET(idx),
			      reg->fbc_cr_offset[idx]);
		vpu_write_reg(inst->dev, W6_CMD_SET_FB_SUB_SAMPLED(idx),
			      reg->sub_sampled[idx]);
	}
	for (idx = 0; idx < mv_count; idx++)
		vpu_write_reg(inst->dev, W6_CMD_SET_FB_MV_COL(idx), reg->mv_col[idx]);

	wave6_send_command(inst->dev, inst->id, inst->std, W6_CMD_SET_FB);
	ret = wave6_wait_vpu_busy(inst->dev, W6_VPU_BUSY_STATUS);
	if (ret) {
		dev_err(inst->dev->dev, "%s: timeout\n", __func__);
		kfree(reg);
		return ret;
	}

	if (!vpu_read_reg(inst->dev, W6_RET_SUCCESS)) {
		kfree(reg);
		return -EIO;
	}

	kfree(reg);
	return 0;
}

struct enc_cmd_enc_pic_reg {
	u32 bs_start;
	u32 bs_size;
	u32 bs_option;
	u32 sec_axi;
	u32 report;
	u32 mv_histo0;
	u32 mv_histo1;
	u32 custom_map_param;
	u32 custom_map_addr;
	u32 src_pic_idx;
	u32 src_addr_y;
	u32 src_addr_u;
	u32 src_addr_v;
	u32 src_stride;
	u32 src_fmt;
	u32 src_axi_sel;
	u32 code_option;
	u32 param;
	u32 longterm_pic;
	u32 prefix_sei_nal_addr;
	u32 prefix_sei_info;
	u32 suffix_sei_nal_addr;
	u32 suffix_sei_info;
	u32 timestamp_low;
	u32 timestamp_high;
	u32 csc_coeff[MAX_CSC_COEFF_NUM];
};

static bool is_format_conv(enum frame_buffer_format in_fmt,
			   enum frame_buffer_format out_fmt)
{
	if (in_fmt == FORMAT_420 ||
	    in_fmt == FORMAT_420_P10_16BIT_MSB ||
	    in_fmt == FORMAT_420_P10_16BIT_LSB ||
	    in_fmt == FORMAT_420_P10_32BIT_MSB ||
	    in_fmt == FORMAT_420_P10_32BIT_LSB) {
		if (out_fmt != FORMAT_420 &&
		    out_fmt != FORMAT_420_P10_16BIT_MSB &&
		    out_fmt != FORMAT_420_P10_16BIT_LSB &&
		    out_fmt != FORMAT_420_P10_32BIT_MSB &&
		    out_fmt != FORMAT_420_P10_32BIT_LSB)
			return true;
	} else if (in_fmt == FORMAT_422 ||
		   in_fmt == FORMAT_422_P10_16BIT_MSB ||
		   in_fmt == FORMAT_422_P10_16BIT_LSB ||
		   in_fmt == FORMAT_422_P10_32BIT_MSB ||
		   in_fmt == FORMAT_422_P10_32BIT_LSB) {
		if (out_fmt != FORMAT_422 &&
		    out_fmt != FORMAT_422_P10_16BIT_MSB &&
		    out_fmt != FORMAT_422_P10_16BIT_LSB &&
		    out_fmt != FORMAT_422_P10_32BIT_MSB &&
		    out_fmt != FORMAT_422_P10_32BIT_LSB)
			return true;
	} else if (in_fmt == FORMAT_444 ||
		   in_fmt == FORMAT_444_P10_16BIT_MSB ||
		   in_fmt == FORMAT_444_P10_16BIT_LSB ||
		   in_fmt == FORMAT_444_P10_32BIT_MSB ||
		   in_fmt == FORMAT_444_P10_32BIT_LSB) {
		if (out_fmt != FORMAT_444 &&
		    out_fmt != FORMAT_444_P10_16BIT_MSB &&
		    out_fmt != FORMAT_444_P10_16BIT_LSB &&
		    out_fmt != FORMAT_444_P10_32BIT_MSB &&
		    out_fmt != FORMAT_444_P10_32BIT_LSB)
			return true;
	}

	return false;
}

static void wave6_gen_enc_pic_reg(struct enc_info *p_enc_info, bool cbcr_interleave, bool nv21,
				  struct enc_param *opt, struct enc_cmd_enc_pic_reg *reg)
{
	struct enc_open_param open = p_enc_info->open_param;
	struct enc_codec_param param = open.codec_param;
	bool is_lsb = false;
	bool is_10bit = false;
	bool is_3p4b = false;
	bool is_cr_first = nv21;
	u32 stride_c;
	u32 c_fmt_idc;
	bool is_ayuv = false;
	bool is_csc_format = false;
	bool is_24bit = false;
	bool is_packed = false;
	bool is_packed_uv_first = false;
	bool format_conv = is_format_conv(open.src_format, open.output_format);

	c_fmt_idc = get_chroma_format_idc(open.src_format);

	switch (open.src_format) {
	case FORMAT_420:
	case FORMAT_420_P10_16BIT_MSB:
	case FORMAT_420_P10_16BIT_LSB:
		stride_c = (cbcr_interleave) ? opt->source_frame->stride :
					       (opt->source_frame->stride / 2);
		break;
	case FORMAT_420_P10_32BIT_MSB:
	case FORMAT_420_P10_32BIT_LSB:
		stride_c = (cbcr_interleave) ? opt->source_frame->stride :
					       ALIGN((opt->source_frame->stride / 2), 16);
		break;
	case FORMAT_422:
	case FORMAT_422_P10_16BIT_MSB:
	case FORMAT_422_P10_16BIT_LSB:
		stride_c = (cbcr_interleave) ? opt->source_frame->stride :
					       (opt->source_frame->stride / 2);
		stride_c = (format_conv) ? (stride_c * 2) : stride_c;
		break;
	case FORMAT_422_P10_32BIT_MSB:
	case FORMAT_422_P10_32BIT_LSB:
		stride_c = (cbcr_interleave) ? opt->source_frame->stride :
					       ALIGN((opt->source_frame->stride / 2), 16);
		stride_c = (format_conv) ? (stride_c * 2) : stride_c;
		break;
	case FORMAT_444:
	case FORMAT_444_P10_16BIT_MSB:
	case FORMAT_444_P10_16BIT_LSB:
		stride_c = (cbcr_interleave) ? (opt->source_frame->stride * 2) :
					       opt->source_frame->stride;
		stride_c = (format_conv) ? (stride_c * 2) : stride_c;
		break;
	case FORMAT_444_P10_32BIT_MSB:
	case FORMAT_444_P10_32BIT_LSB:
		stride_c = (cbcr_interleave) ? ALIGN((opt->source_frame->stride * 2), 16) :
					       opt->source_frame->stride;
		stride_c = (format_conv) ? (stride_c * 2) : stride_c;
		break;
	case FORMAT_YUV444_24BIT:
		stride_c = ALIGN((opt->source_frame->stride * 2), 16);
		break;
	default:
		stride_c = 0;
		break;
	}

	switch (open.src_format) {
	case FORMAT_420:
	case FORMAT_422:
	case FORMAT_444:
	case FORMAT_400:
	case FORMAT_YUYV:
	case FORMAT_YVYU:
	case FORMAT_UYVY:
	case FORMAT_VYUY:
		is_lsb = false;
		is_3p4b = false;
		break;
	case FORMAT_420_P10_16BIT_MSB:
	case FORMAT_422_P10_16BIT_MSB:
	case FORMAT_444_P10_16BIT_MSB:
	case FORMAT_400_P10_16BIT_MSB:
	case FORMAT_YUYV_P10_16BIT_MSB:
	case FORMAT_YVYU_P10_16BIT_MSB:
	case FORMAT_UYVY_P10_16BIT_MSB:
	case FORMAT_VYUY_P10_16BIT_MSB:
		is_lsb = false;
		is_10bit = true;
		is_3p4b = false;
		break;
	case FORMAT_420_P10_16BIT_LSB:
	case FORMAT_422_P10_16BIT_LSB:
	case FORMAT_444_P10_16BIT_LSB:
	case FORMAT_400_P10_16BIT_LSB:
	case FORMAT_YUYV_P10_16BIT_LSB:
	case FORMAT_YVYU_P10_16BIT_LSB:
	case FORMAT_UYVY_P10_16BIT_LSB:
	case FORMAT_VYUY_P10_16BIT_LSB:
		is_lsb = true;
		is_10bit = true;
		is_3p4b = false;
		break;
	case FORMAT_420_P10_32BIT_MSB:
	case FORMAT_422_P10_32BIT_MSB:
	case FORMAT_444_P10_32BIT_MSB:
	case FORMAT_400_P10_32BIT_MSB:
	case FORMAT_YUYV_P10_32BIT_MSB:
	case FORMAT_YVYU_P10_32BIT_MSB:
	case FORMAT_UYVY_P10_32BIT_MSB:
	case FORMAT_VYUY_P10_32BIT_MSB:
		is_lsb = false;
		is_10bit = true;
		is_3p4b = true;
		break;
	case FORMAT_420_P10_32BIT_LSB:
	case FORMAT_422_P10_32BIT_LSB:
	case FORMAT_444_P10_32BIT_LSB:
	case FORMAT_400_P10_32BIT_LSB:
	case FORMAT_YUYV_P10_32BIT_LSB:
	case FORMAT_YVYU_P10_32BIT_LSB:
	case FORMAT_UYVY_P10_32BIT_LSB:
	case FORMAT_VYUY_P10_32BIT_LSB:
		is_lsb = true;
		is_10bit = true;
		is_3p4b = true;
		break;
	case FORMAT_RGB_32BIT_PACKED:
		is_ayuv = false;
		is_csc_format = true;
		break;
	case FORMAT_RGB_P10_32BIT_PACKED:
		is_ayuv = false;
		is_csc_format = true;
		is_10bit = true;
		break;
	case FORMAT_YUV444_32BIT_PACKED:
		is_ayuv = true;
		is_csc_format = true;
		break;
	case FORMAT_YUV444_P10_32BIT_PACKED:
		is_ayuv = true;
		is_csc_format = true;
		is_10bit = true;
		break;
	case FORMAT_RGB_24BIT_PACKED:
		is_ayuv = false;
		is_csc_format = true;
		is_24bit = true;
		break;
	case FORMAT_YUV444_24BIT_PACKED:
		is_ayuv = true;
		is_csc_format = true;
		is_24bit = true;
		break;
	case FORMAT_YUV444_24BIT:
		is_ayuv = true;
		break;
	default:
		break;
	}

	switch (open.src_format) {
	case FORMAT_YUYV:
	case FORMAT_YUYV_P10_16BIT_MSB:
	case FORMAT_YUYV_P10_16BIT_LSB:
	case FORMAT_YUYV_P10_32BIT_MSB:
	case FORMAT_YUYV_P10_32BIT_LSB:
		is_packed = true;
		break;
	case FORMAT_YVYU:
	case FORMAT_YVYU_P10_16BIT_MSB:
	case FORMAT_YVYU_P10_16BIT_LSB:
	case FORMAT_YVYU_P10_32BIT_MSB:
	case FORMAT_YVYU_P10_32BIT_LSB:
		is_packed = true;
		is_cr_first = true;
		break;
	case FORMAT_UYVY:
	case FORMAT_UYVY_P10_16BIT_MSB:
	case FORMAT_UYVY_P10_16BIT_LSB:
	case FORMAT_UYVY_P10_32BIT_MSB:
	case FORMAT_UYVY_P10_32BIT_LSB:
		is_packed = true;
		is_packed_uv_first = true;
		break;
	case FORMAT_VYUY:
	case FORMAT_VYUY_P10_16BIT_MSB:
	case FORMAT_VYUY_P10_16BIT_LSB:
	case FORMAT_VYUY_P10_32BIT_MSB:
	case FORMAT_VYUY_P10_32BIT_LSB:
		is_packed = true;
		is_packed_uv_first = true;
		is_cr_first = true;
		break;
	default:
		break;
	}

	reg->src_fmt = ENC_PIC_SRC_FMT_C_FMT_IDC(c_fmt_idc) |
		       ENC_PIC_SRC_FMT_CSC_24BIT(is_24bit) |
		       ENC_PIC_SRC_FMT_CSC_AYUV(is_ayuv) |
		       ENC_PIC_SRC_FMT_CSC_ENABLE(is_csc_format) |
		       ENC_PIC_SRC_FMT_CSC_FMT_ORDER(opt->csc.fmt_order) |
		       ENC_PIC_SRC_FMT_ENDIAN(open.source_endian) |
		       ENC_PIC_SRC_FMT_10BIT_ORDER(is_lsb) |
		       ENC_PIC_SRC_FMT_10BIT_3PIXEL_4BYTE(is_3p4b) |
		       ENC_PIC_SRC_FMT_10BIT_ENABLE(is_10bit) |
		       ENC_PIC_SRC_FMT_YUV422_PACKED_ORDER(is_packed_uv_first) |
		       ENC_PIC_SRC_FMT_CBCR_ORDER(is_cr_first) |
		       ENC_PIC_SRC_FMT_2PLANE(cbcr_interleave) |
		       ENC_PIC_SRC_FMT_YUV422_PACKED(is_packed);
	reg->bs_start = opt->pic_stream_buffer_addr;
	reg->bs_size = opt->pic_stream_buffer_size;
	reg->sec_axi = ENC_PIC_SEC_AXI_RDO_ENABLE(p_enc_info->sec_axi.use_enc_rdo) |
		       ENC_PIC_SEC_AXI_LF_ENABLE(p_enc_info->sec_axi.use_enc_lf);
	reg->report = ENC_PIC_REPORT_MV_HISTO_ENABLE(param.en_report_mv_histo);
	reg->mv_histo0 = ENC_PIC_MV_HISTO0_THRESHOLD0(param.mv_histo_th0) |
			 ENC_PIC_MV_HISTO0_THRESHOLD1(param.mv_histo_th1);
	reg->mv_histo1 = ENC_PIC_MV_HISTO1_THRESHOLD2(param.mv_histo_th2) |
			 ENC_PIC_MV_HISTO1_THRESHOLD3(param.mv_histo_th3);
	reg->src_pic_idx = (opt->src_end) ? ENC_PIC_SRC_PIC_IDX_END : opt->src_idx;
	reg->src_addr_y = opt->source_frame->buf_y;
	reg->src_addr_u = opt->source_frame->buf_cb;
	reg->src_addr_v = opt->source_frame->buf_cr;
	reg->src_stride = ENC_PIC_SRC_STRIDE_L(opt->source_frame->stride) |
			  ENC_PIC_SRC_STRIDE_C(stride_c);
	reg->src_axi_sel = ENC_PIC_SRC_AXI_SEL_PRP_PORT;
	reg->code_option = ENC_PIC_CODE_OPTION_ENCODE_VCL |
			   ENC_PIC_CODE_OPTION_ENCODE_HEADER;
	reg->param = ENC_PIC_PARAM_INTRA_4X4(param.intra_4x4) |
		     ENC_PIC_PARAM_FORCE_PIC_TYPE(opt->force_pic_type) |
		     ENC_PIC_PARAM_FORCE_PIC_TYPE_ENABLE(opt->force_pic) |
		     ENC_PIC_PARAM_FORCE_PIC_QP_B(opt->force_pic_qp_b) |
		     ENC_PIC_PARAM_FORCE_PIC_QP_P(opt->force_pic_qp_p) |
		     ENC_PIC_PARAM_FORCE_PIC_QP_I(opt->force_pic_qp_i) |
		     ENC_PIC_PARAM_FORCE_PIC_QP_ENABLE(opt->force_pic_qp) |
		     ENC_PIC_PARAM_PIC_SKIP_FLAG(opt->skip_picture);
	reg->timestamp_low = lower_32_bits(opt->timestamp);
	reg->timestamp_high = upper_32_bits(opt->timestamp);
	reg->csc_coeff[0] = ENC_PIC_CSC_COEFF0_RY(opt->csc.coef_ry) |
			    ENC_PIC_CSC_COEFF0_GY(opt->csc.coef_gy) |
			    ENC_PIC_CSC_COEFF0_BY(opt->csc.coef_by);
	reg->csc_coeff[1] = ENC_PIC_CSC_COEFF1_RCB(opt->csc.coef_rcb) |
			    ENC_PIC_CSC_COEFF1_GCB(opt->csc.coef_gcb) |
			    ENC_PIC_CSC_COEFF1_BCB(opt->csc.coef_bcb);
	reg->csc_coeff[2] = ENC_PIC_CSC_COEFF2_RCR(opt->csc.coef_rcr) |
			    ENC_PIC_CSC_COEFF2_GCR(opt->csc.coef_gcr) |
			    ENC_PIC_CSC_COEFF2_BCR(opt->csc.coef_bcr);
	reg->csc_coeff[3] = ENC_PIC_CSC_COEFF3_OFFSET_Y(opt->csc.offset_y) |
			    ENC_PIC_CSC_COEFF3_OFFSET_CB(opt->csc.offset_cb) |
			    ENC_PIC_CSC_COEFF3_OFFSET_CR(opt->csc.offset_cr);
}

int wave6_vpu_encode(struct vpu_instance *inst, struct enc_param *option, u32 *fail_res)
{
	struct enc_cmd_enc_pic_reg reg;
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	int ret;

	lockdep_assert_held(&inst->dev->hw_lock);

	memset(&reg, 0, sizeof(struct enc_cmd_enc_pic_reg));

	wave6_gen_enc_pic_reg(p_enc_info, inst->cbcr_interleave,
			      inst->nv21, option, &reg);

	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_BS_START, reg.bs_start);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_BS_SIZE, reg.bs_size);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_BS_OPTION, reg.bs_option);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_SEC_AXI, reg.sec_axi);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_REPORT, reg.report);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_MV_HISTO0, reg.mv_histo0);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_MV_HISTO1, reg.mv_histo1);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_CUSTOM_MAP_PARAM, reg.custom_map_param);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_CUSTOM_MAP_ADDR, reg.custom_map_addr);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_SRC_PIC_IDX, reg.src_pic_idx);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_SRC_ADDR_Y, reg.src_addr_y);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_SRC_ADDR_U, reg.src_addr_u);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_SRC_ADDR_V, reg.src_addr_v);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_SRC_STRIDE, reg.src_stride);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_SRC_FMT, reg.src_fmt);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_SRC_AXI_SEL, reg.src_axi_sel);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_CODE_OPTION, reg.code_option);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_PARAM, reg.param);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_LONGTERM_PIC, reg.longterm_pic);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_PREFIX_SEI_NAL_ADDR, reg.prefix_sei_nal_addr);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_PREFIX_SEI_INFO, reg.prefix_sei_info);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_SUFFIX_SEI_NAL_ADDR, reg.suffix_sei_nal_addr);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_SUFFIX_SEI_INFO, reg.suffix_sei_info);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_TIMESTAMP_LOW, reg.timestamp_low);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_TIMESTAMP_HIGH, reg.timestamp_high);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_CSC_COEFF0, reg.csc_coeff[0]);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_CSC_COEFF1, reg.csc_coeff[1]);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_CSC_COEFF2, reg.csc_coeff[2]);
	vpu_write_reg(inst->dev, W6_CMD_ENC_PIC_CSC_COEFF3, reg.csc_coeff[3]);

	wave6_send_command(inst->dev, inst->id, inst->std, W6_CMD_ENC_PIC);
	ret = wave6_wait_vpu_busy(inst->dev, W6_VPU_BUSY_STATUS);
	if (ret) {
		dev_err(inst->dev->dev, "%s: timeout\n", __func__);
		return -ETIMEDOUT;
	}

	if (!vpu_read_reg(inst->dev, W6_RET_SUCCESS)) {
		*fail_res = vpu_read_reg(inst->dev, W6_RET_FAIL_REASON);
		wave6_print_reg_err(inst->dev, *fail_res);
		return -EIO;
	}

	return 0;
}

int wave6_vpu_enc_get_result(struct vpu_instance *inst, struct enc_output_info *result)
{
	u32 reg_val;
	int ret;

	lockdep_assert_held(&inst->dev->hw_lock);

	ret = wave6_send_query(inst->dev, inst->id, inst->std, W6_QUERY_OPT_GET_RESULT);
	if (ret)
		return ret;

	result->encoding_success = vpu_read_reg(inst->dev, W6_RET_ENC_ENCODING_SUCCESS);
	if (!result->encoding_success)
		result->error_reason = vpu_read_reg(inst->dev, W6_RET_ENC_ERR_INFO);
	else
		result->warn_info = vpu_read_reg(inst->dev, W6_RET_DEC_WARN_INFO);

	result->enc_pic_cnt = vpu_read_reg(inst->dev, W6_RET_ENC_PIC_NUM);
	result->pic_type = vpu_read_reg(inst->dev, W6_RET_ENC_PIC_TYPE);
	result->enc_vcl_nut = vpu_read_reg(inst->dev, W6_RET_ENC_VCL_NUT);
	result->non_ref_pic = vpu_read_reg(inst->dev, W6_RET_ENC_PIC_NON_REF_PIC_FLAG);
	result->num_of_slices = vpu_read_reg(inst->dev, W6_RET_ENC_PIC_SLICE_NUM);
	result->pic_skipped = vpu_read_reg(inst->dev, W6_RET_ENC_PIC_SKIP);
	result->num_of_intra = vpu_read_reg(inst->dev, W6_RET_ENC_PIC_NUM_INTRA);
	result->num_of_merge = vpu_read_reg(inst->dev, W6_RET_ENC_PIC_NUM_MERGE);
	result->num_of_skip_block = vpu_read_reg(inst->dev, W6_RET_ENC_PIC_NUM_SKIP);
	result->avg_ctu_qp = vpu_read_reg(inst->dev, W6_RET_ENC_PIC_AVG_CTU_QP);
	result->enc_pic_byte = vpu_read_reg(inst->dev, W6_RET_ENC_PIC_BYTE);
	result->enc_gop_pic_idx = vpu_read_reg(inst->dev, W6_RET_ENC_GOP_PIC_IDX);
	result->enc_pic_poc = vpu_read_reg(inst->dev, W6_RET_ENC_PIC_POC);
	result->enc_src_idx = vpu_read_reg(inst->dev, W6_RET_ENC_USED_SRC_IDX);
	result->wr_ptr = vpu_read_reg(inst->dev, W6_RET_ENC_WR_PTR);
	result->rd_ptr = vpu_read_reg(inst->dev, W6_RET_ENC_RD_PTR);
	result->bitstream_buffer = vpu_read_reg(inst->dev, W6_RET_ENC_RD_PTR);
	result->pic_distortion_low = vpu_read_reg(inst->dev, W6_RET_ENC_PIC_DIST_LOW);
	result->pic_distortion_high = vpu_read_reg(inst->dev, W6_RET_ENC_PIC_DIST_HIGH);
	result->mv_histo.cnt0 = vpu_read_reg(inst->dev, W6_RET_ENC_HISTO_CNT0);
	result->mv_histo.cnt1 = vpu_read_reg(inst->dev, W6_RET_ENC_HISTO_CNT1);
	result->mv_histo.cnt2 = vpu_read_reg(inst->dev, W6_RET_ENC_HISTO_CNT2);
	result->mv_histo.cnt3 = vpu_read_reg(inst->dev, W6_RET_ENC_HISTO_CNT3);
	result->mv_histo.cnt4 = vpu_read_reg(inst->dev, W6_RET_ENC_HISTO_CNT4);
	result->fme_sum.lower_x0 = vpu_read_reg(inst->dev, W6_RET_ENC_SUM_ME0_X_DIR_LOWER);
	result->fme_sum.higher_x0 = vpu_read_reg(inst->dev, W6_RET_ENC_SUM_ME0_X_DIR_HIGHER);
	result->fme_sum.lower_y0 = vpu_read_reg(inst->dev, W6_RET_ENC_SUM_ME0_Y_DIR_LOWER);
	result->fme_sum.higher_y0 = vpu_read_reg(inst->dev, W6_RET_ENC_SUM_ME0_Y_DIR_HIGHER);
	result->fme_sum.lower_x1 = vpu_read_reg(inst->dev, W6_RET_ENC_SUM_ME1_X_DIR_LOWER);
	result->fme_sum.higher_x1 = vpu_read_reg(inst->dev, W6_RET_ENC_SUM_ME1_X_DIR_HIGHER);
	result->fme_sum.lower_y1 = vpu_read_reg(inst->dev, W6_RET_ENC_SUM_ME1_Y_DIR_LOWER);
	result->fme_sum.higher_y1 = vpu_read_reg(inst->dev, W6_RET_ENC_SUM_ME1_Y_DIR_HIGHER);
	result->src_y_addr = vpu_read_reg(inst->dev, W6_RET_ENC_SRC_Y_ADDR);
	result->custom_map_addr = vpu_read_reg(inst->dev, W6_RET_ENC_CUSTOM_MAP_ADDR);
	result->prefix_sei_nal_addr = vpu_read_reg(inst->dev, W6_RET_ENC_PREFIX_SEI_NAL_ADDR);
	result->suffix_sei_nal_addr = vpu_read_reg(inst->dev, W6_RET_ENC_SUFFIX_SEI_NAL_ADDR);

	result->recon_frame_index = vpu_read_reg(inst->dev, W6_RET_ENC_PIC_IDX);
	if (result->recon_frame_index == RECON_IDX_FLAG_HEADER_ONLY)
		result->bitstream_size = result->enc_pic_byte;
	else if (result->recon_frame_index < 0)
		result->bitstream_size = 0;
	else
		result->bitstream_size = result->enc_pic_byte;

	reg_val = vpu_read_reg(inst->dev, W6_RET_ENC_TIMESTAMP_LOW);
	result->timestamp = vpu_read_reg(inst->dev, W6_RET_ENC_TIMESTAMP_HIGH);
	result->timestamp = (result->timestamp << 32) | reg_val;

	result->cycle.host_cmd_s = vpu_read_reg(inst->dev, W6_RET_CQ_IN_TICK);
	result->cycle.host_cmd_e = vpu_read_reg(inst->dev, W6_RET_RQ_OUT_TICK);
	result->cycle.proc_s = vpu_read_reg(inst->dev, W6_RET_HW_RUN_TICK);
	result->cycle.proc_e = vpu_read_reg(inst->dev, W6_RET_HW_DONE_TICK);
	result->cycle.vpu_s = vpu_read_reg(inst->dev, W6_RET_FW_RUN_TICK);
	result->cycle.vpu_e = vpu_read_reg(inst->dev, W6_RET_FW_DONE_TICK);
	result->cycle.frame_cycle = (result->cycle.vpu_e - result->cycle.host_cmd_s) *
				    CYCLE_PER_TICK_VALUE;
	result->cycle.proc_cycle = (result->cycle.proc_e - result->cycle.proc_s) *
				   CYCLE_PER_TICK_VALUE;
	result->cycle.vpu_cycle = ((result->cycle.vpu_e - result->cycle.vpu_s) -
				   (result->cycle.proc_e - result->cycle.proc_s)) *
				  CYCLE_PER_TICK_VALUE;

	return 0;
}

int wave6_vpu_enc_fini_seq(struct vpu_instance *inst, u32 *fail_res)
{
	int ret;

	lockdep_assert_held(&inst->dev->hw_lock);

	wave6_send_command(inst->dev, inst->id, inst->std, W6_CMD_DESTROY_INSTANCE);
	ret = wave6_wait_vpu_busy(inst->dev, W6_VPU_BUSY_STATUS);
	if (ret)
		return -ETIMEDOUT;

	if (!vpu_read_reg(inst->dev, W6_RET_SUCCESS)) {
		*fail_res = vpu_read_reg(inst->dev, W6_RET_FAIL_REASON);
		wave6_print_reg_err(inst->dev, *fail_res);
		return -EIO;
	}

	return 0;
}

static int wave6_vpu_enc_check_gop_param(struct vpu_instance *inst, struct enc_codec_param *p_param)
{
	struct device *dev = inst->dev->dev;
	int i;
	bool low_delay = true;

	if (p_param->gop_preset_idx == PRESET_IDX_CUSTOM_GOP) {
		if (p_param->gop_param.size > 1) {
			int min_val = p_param->gop_param.pic[0].poc_offset;

			for (i = 1; i < p_param->gop_param.size; i++) {
				if (min_val > p_param->gop_param.pic[i].poc_offset) {
					low_delay = false;
					break;
				}
				min_val = p_param->gop_param.pic[i].poc_offset;
			}
		}
	} else if (p_param->gop_preset_idx == PRESET_IDX_ALL_I ||
		   p_param->gop_preset_idx == PRESET_IDX_IPP ||
		   p_param->gop_preset_idx == PRESET_IDX_IBBB ||
		   p_param->gop_preset_idx == PRESET_IDX_IPPPP ||
		   p_param->gop_preset_idx == PRESET_IDX_IBBBB ||
		   p_param->gop_preset_idx == PRESET_IDX_IPP_SINGLE) {
	}

	if (p_param->gop_preset_idx >= PRESET_IDX_MAX) {
		dev_err(dev, "gop_preset_idx: %d\n", p_param->gop_preset_idx);
		return -EINVAL;
	}

	if (p_param->gop_preset_idx == PRESET_IDX_CUSTOM_GOP) {
		if (p_param->gop_param.size < 1 ||
		    p_param->gop_param.size > MAX_CUSTOM_GOP_NUM) {
			dev_err(dev, "gop size: %d\n", p_param->gop_param.size);
			return -EINVAL;
		}
		for (i = 0; i < p_param->gop_param.size; i++) {
			struct custom_gop_pic_param pic = p_param->gop_param.pic[i];

			if (pic.pic_type != PIC_TYPE_I &&
			    pic.pic_type != PIC_TYPE_P &&
			    pic.pic_type != PIC_TYPE_B) {
				dev_err(dev, "pic[%d].pic_type: %d\n", i, pic.pic_type);
				return -EINVAL;
			}
			if (pic.poc_offset < 1 ||
			    pic.poc_offset > p_param->gop_param.size) {
				dev_err(dev, "pic[%d].poc_offset: %d\n", i, pic.poc_offset);
				return -EINVAL;
			}
			if (pic.temporal_id < 0 || pic.temporal_id > 3) {
				dev_err(dev, "pic[%d].temporal_id: %d\n", i, pic.temporal_id);
				return -EINVAL;
			}
		}
		if (inst->std == W_AVC_ENC && !low_delay) {
			for (i = 0; i < p_param->gop_param.size; i++) {
				if (p_param->gop_param.pic[i].temporal_id > 0) {
					dev_err(dev, "std: %d, pic[%d].temporal_id: %d\n",
						inst->std, i,
						p_param->gop_param.pic[i].temporal_id);
					return -EINVAL;
				}
			}
		}
	}

	if (inst->std == W_HEVC_ENC) {
		if (p_param->decoding_refresh_type > DEC_REFRESH_TYPE_IDR) {
			dev_err(dev, "decoding_refresh_type: %d\n", p_param->decoding_refresh_type);
			return -EINVAL;
		}
	} else {
		if (p_param->decoding_refresh_type != DEC_REFRESH_TYPE_NON_IRAP) {
			dev_err(dev, "decoding_refresh_type: %d\n", p_param->decoding_refresh_type);
			return -EINVAL;
		}
	}

	return 0;
}

static int wave6_vpu_enc_check_tile_slice_param(struct vpu_instance *inst,
						int width, int height,
						struct enc_codec_param *p_param)
{
	struct device *dev = inst->dev->dev;

	if (p_param->slice_mode > 2) {
		dev_err(dev, "slice_mode: %d\n", p_param->slice_mode);
		return -EINVAL;
	}
	if (p_param->slice_mode == 1) {
		unsigned int ctu_size = (inst->std == W_AVC_ENC) ? 16 : 64;
		unsigned int mb_num = ((width + ctu_size - 1) / ctu_size) *
				      ((height + ctu_size - 1) / ctu_size);

		if (p_param->slice_arg < 1 || p_param->slice_arg > 0x3FFFF) {
			dev_err(dev, "slice_arg: %d\n", p_param->slice_arg);
			return -EINVAL;
		}
		if (p_param->slice_arg > mb_num) {
			dev_info(dev, "slice_arg: %d, mb_num: %d\n",
				 p_param->slice_arg, mb_num);
			p_param->slice_arg = mb_num;
		}
		if (inst->std == W_AVC_ENC && p_param->slice_arg < 4) {
			dev_info(dev, "std: %d, slice_arg: %d\n",
				 inst->std, p_param->slice_arg);
			p_param->slice_arg = 4;
		}
	}

	return 0;
}

static int wave6_vpu_enc_check_rc_param(struct vpu_instance *inst, struct enc_codec_param *p_param)
{
	struct device *dev = inst->dev->dev;

	if (p_param->frame_rate < 1 || p_param->frame_rate > 960) {
		dev_err(dev, "frame_rate: %d\n", p_param->frame_rate);
		return -EINVAL;
	}
	if (p_param->bitrate > 1500000000) {
		dev_err(dev, "bitrate: %d\n", p_param->bitrate);
		return -EINVAL;
	}
	if (p_param->qp > 51) {
		dev_err(dev, "qp: %d\n", p_param->qp);
		return -EINVAL;
	}
	if (p_param->min_qp_i > 51 || p_param->min_qp_p > 51 || p_param->min_qp_b > 51) {
		dev_err(dev, "min_qp_i: %d, min_qp_p: %d, min_qp_b: %d\n",
			p_param->min_qp_i, p_param->min_qp_p, p_param->min_qp_b);
		return -EINVAL;
	}
	if (p_param->max_qp_i > 51 || p_param->max_qp_p > 51 || p_param->max_qp_b > 51) {
		dev_err(dev, "max_qp_i: %d, max_qp_p: %d, max_qp_b: %d\n",
			p_param->max_qp_i, p_param->max_qp_p, p_param->max_qp_b);
		return -EINVAL;
	}
	if (p_param->min_qp_i > p_param->max_qp_i) {
		dev_err(dev, "min_qp_i: %d, max_qp_i: %d\n", p_param->min_qp_i, p_param->max_qp_i);
		return -EINVAL;
	}
	if (p_param->min_qp_p > p_param->max_qp_p) {
		dev_err(dev, "min_qp_p: %d, max_qp_p: %d\n", p_param->min_qp_p, p_param->max_qp_p);
		return -EINVAL;
	}
	if (p_param->min_qp_b > p_param->max_qp_b) {
		dev_err(dev, "min_qp_b: %d, max_qp_b: %d\n", p_param->min_qp_b, p_param->max_qp_b);
		return -EINVAL;
	}
	if (p_param->rc_initial_qp < -1 || p_param->rc_initial_qp > 51) {
		dev_err(dev, "rc_initial_qp: %d\n", p_param->rc_initial_qp);
		return -EINVAL;
	}
	if (p_param->en_rate_control != 1 && p_param->en_rate_control != 0) {
		dev_err(dev, "en_rate_control: %d\n", p_param->en_rate_control);
		return -EINVAL;
	}
	if (p_param->rc_mode > 1) {
		dev_err(dev, "rc_mode: %d\n", p_param->rc_mode);
		return -EINVAL;
	}
	if (p_param->en_rate_control) {
		if (p_param->bitrate <= p_param->frame_rate) {
			dev_err(dev, "bitrate: %d, frame_rate: %d\n",
				p_param->bitrate, p_param->frame_rate);
			return -EINVAL;
		}
		if (p_param->rc_initial_qp != -1) {
			if (p_param->rc_initial_qp < p_param->min_qp_i) {
				dev_err(dev, "rc_initial_qp: %d, min_qp_i: %d\n",
					p_param->rc_initial_qp, p_param->min_qp_i);
				return -EINVAL;
			}
			if (p_param->rc_initial_qp > p_param->max_qp_i) {
				dev_err(dev, "rc_initial_qp: %d, max_qp_i: %d\n",
					p_param->rc_initial_qp, p_param->max_qp_i);
				return -EINVAL;
			}
		}
	} else {
		if (p_param->qp < p_param->min_qp_i) {
			dev_err(dev, "qp: %d, min_qp_i: %d\n", p_param->qp, p_param->min_qp_i);
			return -EINVAL;
		}
		if (p_param->qp < p_param->min_qp_p) {
			dev_err(dev, "qp: %d, min_qp_p: %d\n", p_param->qp, p_param->min_qp_p);
			return -EINVAL;
		}
		if (p_param->qp < p_param->min_qp_b) {
			dev_err(dev, "qp: %d, min_qp_b: %d\n", p_param->qp, p_param->min_qp_b);
			return -EINVAL;
		}
		if (p_param->qp > p_param->max_qp_i) {
			dev_err(dev, "qp: %d, max_qp_i: %d\n", p_param->qp, p_param->max_qp_i);
			return -EINVAL;
		}
		if (p_param->qp > p_param->max_qp_p) {
			dev_err(dev, "qp: %d, max_qp_p: %d\n", p_param->qp, p_param->max_qp_p);
			return -EINVAL;
		}
		if (p_param->qp > p_param->max_qp_b) {
			dev_err(dev, "qp: %d, max_qp_b: %d\n", p_param->qp, p_param->max_qp_b);
			return -EINVAL;
		}
	}

	return 0;
}

static int wave6_vpu_enc_check_intra_param(struct vpu_instance *inst,
					   int width, int height,
					   struct enc_codec_param *p_param)
{
	struct device *dev = inst->dev->dev;
	unsigned int ctu_size = (inst->std == W_AVC_ENC) ? 16 : 64;
	unsigned int num_ctu_col = (width + ctu_size - 1) / ctu_size;
	unsigned int num_ctu_row = (height + ctu_size - 1) / ctu_size;

	if (p_param->intra_refresh_mode > INTRA_REFRESH_COLUMN) {
		dev_err(dev, "intra_refresh_mode: %d\n", p_param->intra_refresh_mode);
		return -EINVAL;
	}
	if (p_param->intra_refresh_mode != INTRA_REFRESH_NONE) {
		if (p_param->intra_refresh_arg < 1 || p_param->intra_refresh_arg > 511) {
			dev_err(dev, "intra_refresh_arg: %d\n", p_param->intra_refresh_arg);
			return -EINVAL;
		}
	}
	if (p_param->intra_refresh_mode == INTRA_REFRESH_ROW &&
	    p_param->intra_refresh_arg > num_ctu_row) {
		dev_err(dev, "intra_refresh_mode: %d, intra_refresh_arg: %d\n",
			p_param->intra_refresh_mode, p_param->intra_refresh_arg);
		return -EINVAL;
	}
	if (p_param->intra_refresh_mode == INTRA_REFRESH_COLUMN &&
	    p_param->intra_refresh_arg > num_ctu_col) {
		dev_err(dev, "intra_refresh_mode: %d, intra_refresh_arg: %d\n",
			p_param->intra_refresh_mode, p_param->intra_refresh_arg);
		return -EINVAL;
	}

	return 0;
}

static int wave6_vpu_enc_check_custom_param(struct vpu_instance *inst,
					    struct enc_codec_param *p_param)
{
	struct device *dev = inst->dev->dev;
	int i;

	if (p_param->en_custom_lambda != 1 && p_param->en_custom_lambda != 0) {
		dev_err(dev, "en_custom_lambda: %d\n", p_param->en_custom_lambda);
		return -EINVAL;
	}
	for (i = 0; i < MAX_CUSTOM_LAMBDA_NUM; i++) {
		if (p_param->lambda_ssd[i] > 16383) {
			dev_err(dev, "lambda_ssd[%d]: %d\n", i, p_param->lambda_ssd[i]);
			return -EINVAL;
		}
		if (p_param->lambda_sad[i] > 127) {
			dev_err(dev, "lambda_sad[%d]: %d\n", i, p_param->lambda_sad[i]);
			return -EINVAL;
		}
	}

	return 0;
}

static int wave6_vpu_enc_check_conf_win_size_param(struct vpu_instance *inst,
						   int width, int height,
						   struct vpu_rect conf_win)
{
	struct device *dev = inst->dev->dev;

	if (conf_win.left % 2 || conf_win.top % 2 || conf_win.right % 2 || conf_win.bottom % 2) {
		dev_err(dev, "conf_win left: %d, top: %d, right: %d, bottom: %d\n",
			conf_win.left, conf_win.top, conf_win.right, conf_win.bottom);
		return -EINVAL;
	}
	if (conf_win.left > 8192 || conf_win.top > 8192 ||
	    conf_win.right > 8192 || conf_win.bottom > 8192) {
		dev_err(dev, "conf_win left: %d, top: %d, right: %d, bottom: %d\n",
			conf_win.left, conf_win.top, conf_win.right, conf_win.bottom);
		return -EINVAL;
	}
	if ((conf_win.right + conf_win.left) > width) {
		dev_err(dev, "conf_win.left: %d, conf_win.right: %d, width: %d\n",
			conf_win.left, conf_win.right, width);
		return -EINVAL;
	}
	if ((conf_win.bottom + conf_win.top) > height) {
		dev_err(dev, "conf_win.top: %d, conf_win.bottom: %d, height: %d\n",
			conf_win.top, conf_win.bottom, height);
		return -EINVAL;
	}

	return 0;
}

static int wave6_vpu_enc_check_temporal_layer_param(struct vpu_instance *inst,
						    struct enc_codec_param *p_param)
{
	struct device *dev = inst->dev->dev;
	int i;

	if (p_param->temp_layer_cnt < 1 || p_param->temp_layer_cnt > 4) {
		dev_err(dev, "temp_layer_cnt: %d\n", p_param->temp_layer_cnt);
		return -EINVAL;
	}
	if (p_param->temp_layer_cnt > 1) {
		if (p_param->gop_preset_idx == PRESET_IDX_CUSTOM_GOP ||
		    p_param->gop_preset_idx == PRESET_IDX_ALL_I) {
			dev_err(dev, "temp_layer_cnt: %d, gop_preset_idx: %d\n",
				p_param->temp_layer_cnt, p_param->gop_preset_idx);
			return -EINVAL;
		}
	}
	for (i = 0; i < MAX_NUM_CHANGEABLE_TEMP_LAYER; i++) {
		if (p_param->temp_layer[i].change_qp != 1 &&
		    p_param->temp_layer[i].change_qp != 0) {
			dev_err(dev, "temp_layer[%d].change_qp: %d\n",
				i, p_param->temp_layer[i].change_qp);
			return -EINVAL;
		}
		if (p_param->temp_layer[i].qp_b > 51) {
			dev_err(dev, "temp_layer[%d].qp_b: %d\n", i, p_param->temp_layer[i].qp_b);
			return -EINVAL;
		}
		if (p_param->temp_layer[i].qp_p > 51) {
			dev_err(dev, "temp_layer[%d].qp_p: %d\n", i, p_param->temp_layer[i].qp_p);
			return -EINVAL;
		}
		if (p_param->temp_layer[i].qp_i > 51) {
			dev_err(dev, "temp_layer[%d].qp_i: %d\n", i, p_param->temp_layer[i].qp_i);
			return -EINVAL;
		}
	}

	return 0;
}

int wave6_vpu_enc_check_open_param(struct vpu_instance *inst, struct enc_open_param *pop)
{
	struct device *dev = inst->dev->dev;
	struct vpu_attr *attr = &inst->dev->attr;
	struct enc_codec_param *p_param = &pop->codec_param;

	if (!(BIT(inst->std) & attr->support_encoders)) {
		dev_err(dev, "std: %d, support_encoders: 0x%x\n",
			inst->std, attr->support_encoders);
		return -EOPNOTSUPP;
	}
	if (pop->pic_width % W6_ENC_PIC_SIZE_STEP || pop->pic_height % W6_ENC_PIC_SIZE_STEP) {
		dev_err(dev, "pic_width: %d | pic_height: %d\n", pop->pic_width, pop->pic_height);
		return -EINVAL;
	}
	if (pop->pic_width < W6_MIN_ENC_PIC_WIDTH || pop->pic_width > W6_MAX_ENC_PIC_WIDTH) {
		dev_err(dev, "pic_width: %d\n", pop->pic_width);
		return -EINVAL;
	}
	if (pop->pic_height < W6_MIN_ENC_PIC_HEIGHT || pop->pic_height > W6_MAX_ENC_PIC_HEIGHT) {
		dev_err(dev, "pic_height: %d\n", pop->pic_height);
		return -EINVAL;
	}
	if (pop->src_format == FORMAT_RGB_32BIT_PACKED ||
	    pop->src_format == FORMAT_YUV444_32BIT_PACKED ||
	    pop->src_format == FORMAT_RGB_P10_32BIT_PACKED ||
	    pop->src_format == FORMAT_YUV444_P10_32BIT_PACKED ||
	    pop->src_format == FORMAT_RGB_24BIT_PACKED ||
	    pop->src_format == FORMAT_YUV444_24BIT_PACKED) {
		if (!inst->cbcr_interleave) {
			dev_err(dev, "src_format: %d, cbcr_interleave: %d\n",
				pop->src_format, inst->cbcr_interleave);
			return -EINVAL;
		}
		if (inst->nv21) {
			dev_err(dev, "src_format: %d, nv21: %d\n",
				pop->src_format, inst->nv21);
			return -EINVAL;
		}
		if (pop->mir_dir || pop->rot_angle) {
			dev_warn(dev, "rot/mir is not supported (fmt %d)\n",
				 pop->src_format);
			pop->mir_dir = MIR_NONE;
			pop->rot_angle = ROT_0;
		}
	}
	if (pop->src_format == FORMAT_YUV444_24BIT) {
		if (!inst->cbcr_interleave) {
			dev_err(dev, "src_format: %d, cbcr_interleave: %d\n",
				pop->src_format, inst->cbcr_interleave);
			return -EINVAL;
		}
		if (pop->mir_dir || pop->rot_angle) {
			dev_warn(dev, "rot/mir is not supported (fmt %d)\n",
				 pop->src_format);
			pop->mir_dir = MIR_NONE;
			pop->rot_angle = ROT_0;
		}
	}

	if (wave6_vpu_enc_check_gop_param(inst, p_param)) {
		dev_err(dev, "failed wave6_vpu_enc_check_gop_param()\n");
		return -EINVAL;
	}
	if (wave6_vpu_enc_check_tile_slice_param(inst, pop->pic_width, pop->pic_height, p_param)) {
		dev_err(dev, "failed wave6_vpu_enc_check_tile_slice_param()\n");
		return -EINVAL;
	}
	if (wave6_vpu_enc_check_rc_param(inst, p_param)) {
		dev_err(dev, "failed wave6_vpu_enc_check_rc_param()\n");
		return -EINVAL;
	}
	if (wave6_vpu_enc_check_intra_param(inst, pop->pic_width, pop->pic_height, p_param)) {
		dev_err(dev, "failed wave6_vpu_enc_check_intra_param()\n");
		return -EINVAL;
	}
	if (wave6_vpu_enc_check_custom_param(inst, p_param)) {
		dev_err(dev, "failed wave6_vpu_enc_check_custom_param()\n");
		return -EINVAL;
	}
	if (wave6_vpu_enc_check_conf_win_size_param(inst, pop->pic_width, pop->pic_height,
						    p_param->conf_win)) {
		dev_err(dev, "failed wave6_vpu_enc_check_conf_win_size_param()\n");
		return -EINVAL;
	}
	if (wave6_vpu_enc_check_temporal_layer_param(inst, p_param)) {
		dev_err(dev, "failed wave6_vpu_enc_check_temporal_layer_param()\n");
		return -EINVAL;
	}

	if (p_param->internal_bit_depth != 8 && p_param->internal_bit_depth != 10) {
		dev_err(dev, "internal_bit_depth: %d\n", p_param->internal_bit_depth);
		return -EINVAL;
	}
	if (p_param->intra_period > 2047) {
		dev_err(dev, "intra_period: %d\n", p_param->intra_period);
		return -EINVAL;
	}
	if (p_param->intra_period == 1 && p_param->gop_preset_idx == PRESET_IDX_ALL_I) {
		dev_err(dev, "intra_period: %d, gop_preset_idx: %d\n",
			p_param->intra_period, p_param->gop_preset_idx);
		return -EINVAL;
	}
	if (p_param->en_longterm != 1 && p_param->en_longterm != 0) {
		dev_err(dev, "en_longterm: %d\n", p_param->en_longterm);
		return -EINVAL;
	}
	if (p_param->cpb_size < 10 || p_param->cpb_size > 100000) {
		dev_err(dev, "cpb_size: %d\n", p_param->cpb_size);
		return -EINVAL;
	}
	if (p_param->en_cu_level_rate_control != 1 && p_param->en_cu_level_rate_control != 0) {
		dev_err(dev, "en_cu_level_rate_control: %d\n", p_param->en_cu_level_rate_control);
		return -EINVAL;
	}
	if (p_param->en_skip_frame != 1 && p_param->en_skip_frame != 0) {
		dev_err(dev, "en_skip_frame: %d\n", p_param->en_skip_frame);
		return -EINVAL;
	}
	if (p_param->en_hvs_qp != 1 && p_param->en_hvs_qp != 0) {
		dev_err(dev, "en_hvs_qp: %d\n", p_param->en_hvs_qp);
		return -EINVAL;
	}
	if (p_param->en_hvs_qp) {
		if (p_param->hvs_qp_scale_div2 < 1 || p_param->hvs_qp_scale_div2 > 4) {
			dev_err(dev, "hvs_qp_scale_div2: %d\n", p_param->hvs_qp_scale_div2);
			return -EINVAL;
		}
	}
	if (p_param->max_delta_qp > 12) {
		dev_err(dev, "max_delta_qp: %d\n", p_param->max_delta_qp);
		return -EINVAL;
	}
	if (p_param->rc_update_speed > 255) {
		dev_err(dev, "rc_update_speed: %d\n", p_param->rc_update_speed);
		return -EINVAL;
	}
	if (p_param->max_bitrate > 1500000000) {
		dev_err(dev, "max_bitrate: %d\n", p_param->max_bitrate);
		return -EINVAL;
	}
	if (p_param->rc_initial_level > 15) {
		dev_err(dev, "rc_initial_level: %d\n", p_param->rc_initial_level);
		return -EINVAL;
	}
	if (p_param->pic_rc_max_dqp > 51) {
		dev_err(dev, "pic_rc_max_dqp: %d\n", p_param->pic_rc_max_dqp);
		return -EINVAL;
	}
	if (p_param->en_bg_detect != 1 && p_param->en_bg_detect != 0) {
		dev_err(dev, "en_bg_detect: %d\n", p_param->en_bg_detect);
		return -EINVAL;
	}
	if (p_param->bg_th_diff > 255) {
		dev_err(dev, "bg_th_diff: %d\n", p_param->bg_th_diff);
		return -EINVAL;
	}
	if (p_param->bg_th_mean_diff > 255) {
		dev_err(dev, "bg_th_mean_diff: %d\n", p_param->bg_th_mean_diff);
		return -EINVAL;
	}
	if (p_param->bg_delta_qp < -16 || p_param->bg_delta_qp > 15) {
		dev_err(dev, "bg_delta_qp: %d\n", p_param->bg_delta_qp);
		return -EINVAL;
	}
	if (p_param->en_dbk != 1 && p_param->en_dbk != 0) {
		dev_err(dev, "en_dbk: %d\n", p_param->en_dbk);
		return -EINVAL;
	}
	if (p_param->en_scaling_list != 1 && p_param->en_scaling_list != 0) {
		dev_err(dev, "en_scaling_list: %d\n", p_param->en_scaling_list);
		return -EINVAL;
	}
	if (p_param->qround_intra > 255) {
		dev_err(dev, "qround_intra: %d\n", p_param->qround_intra);
		return -EINVAL;
	}
	if (p_param->qround_inter > 255) {
		dev_err(dev, "qround_inter: %d\n", p_param->qround_inter);
		return -EINVAL;
	}
	if (p_param->lambda_dqp_intra < -32 || p_param->lambda_dqp_intra > 31) {
		dev_err(dev, "lambda_dqp_intra: %d\n", p_param->lambda_dqp_intra);
		return -EINVAL;
	}
	if (p_param->lambda_dqp_inter < -32 || p_param->lambda_dqp_inter > 31) {
		dev_err(dev, "lambda_dqp_inter: %d\n", p_param->lambda_dqp_inter);
		return -EINVAL;
	}
	if (p_param->en_qround_offset != 1 && p_param->en_qround_offset != 0) {
		dev_err(dev, "en_qround_offset: %d\n", p_param->en_qround_offset);
		return -EINVAL;
	}
	if (p_param->forced_idr_header > 2) {
		dev_err(dev, "forced_idr_header: %d\n", p_param->forced_idr_header);
		return -EINVAL;
	}
	if (p_param->num_units_in_tick > INT_MAX) {
		dev_err(dev, "num_units_in_tick: %d\n", p_param->num_units_in_tick);
		return -EINVAL;
	}
	if (p_param->time_scale > INT_MAX) {
		dev_err(dev, "time_scale: %d\n", p_param->time_scale);
		return -EINVAL;
	}
	if (p_param->max_intra_pic_bit > 1500000000) {
		dev_err(dev, "max_intra_pic_bit: %d\n", p_param->max_intra_pic_bit);
		return -EINVAL;
	}
	if (p_param->max_inter_pic_bit > 1500000000) {
		dev_err(dev, "max_inter_pic_bit: %d\n", p_param->max_inter_pic_bit);
		return -EINVAL;
	}

	if (p_param->color.color_range > 1) {
		dev_err(dev, "color_range: %d\n", p_param->color.color_range);
		return -EINVAL;
	}
	if (p_param->color.matrix_coefficients > 255) {
		dev_err(dev, "matrix_coefficients: %d\n", p_param->color.matrix_coefficients);
		return -EINVAL;
	}
	if (p_param->color.transfer_characteristics > 255) {
		dev_err(dev, "transfer_characteristics: %d\n",
			p_param->color.transfer_characteristics);
		return -EINVAL;
	}
	if (p_param->color.color_primaries > 255) {
		dev_err(dev, "color_primaries: %d\n", p_param->color.color_primaries);
		return -EINVAL;
	}
	if (inst->std == W_HEVC_ENC) {
		if (p_param->internal_bit_depth == 10 && !attr->support_hevc10bit_enc) {
			dev_err(dev, "internal_bit_depth: %d, support_hevc10bit_enc: %d\n",
				p_param->internal_bit_depth, attr->support_hevc10bit_enc);
			return -EOPNOTSUPP;
		}
		if (p_param->idr_period != 0) {
			dev_err(dev, "idr_period: %d\n", p_param->idr_period);
			return -EINVAL;
		}
		if (p_param->en_intra_smooth != 1 &&
		    p_param->en_intra_smooth != 0) {
			dev_err(dev, "en_intra_smooth: %d\n", p_param->en_intra_smooth);
			return -EINVAL;
		}
		if (p_param->en_const_intra_pred != 1 &&
		    p_param->en_const_intra_pred != 0) {
			dev_err(dev, "en_const_intra_pred: %d\n",
				p_param->en_const_intra_pred);
			return -EINVAL;
		}
		if (p_param->en_temporal_mvp != 1 && p_param->en_temporal_mvp != 0) {
			dev_err(dev, "en_temporal_mvp: %d\n", p_param->en_temporal_mvp);
			return -EINVAL;
		}
		if (p_param->en_cabac != 0) {
			dev_err(dev, "en_cabac: %d\n", p_param->en_cabac);
			return -EINVAL;
		}
		if (p_param->en_transform8x8 != 0) {
			dev_err(dev, "en_transform8x8: %d\n", p_param->en_transform8x8);
			return -EINVAL;
		}
		if (p_param->en_lf_slice_boundary != 1 &&
		    p_param->en_lf_slice_boundary != 0) {
			dev_err(dev, "en_lf_slice_boundary: %d\n",
				p_param->en_lf_slice_boundary);
			return -EINVAL;
		}
		if (p_param->beta_offset_div2 < -6 || p_param->beta_offset_div2 > 6) {
			dev_err(dev, "beta_offset_div2: %d\n", p_param->beta_offset_div2);
			return -EINVAL;
		}
		if (p_param->tc_offset_div2 < -6 || p_param->tc_offset_div2 > 6) {
			dev_err(dev, "tc_offset_div2: %d\n", p_param->tc_offset_div2);
			return -EINVAL;
		}
		if (p_param->en_sao != 1 && p_param->en_sao != 0) {
			dev_err(dev, "en_sao: %d\n", p_param->en_sao);
			return -EINVAL;
		}
		if (p_param->cb_qp_offset < -12 || p_param->cb_qp_offset > 12) {
			dev_err(dev, "cb_qp_offset: %d\n", p_param->cb_qp_offset);
			return -EINVAL;
		}
		if (p_param->cr_qp_offset < -12 || p_param->cr_qp_offset > 12) {
			dev_err(dev, "cr_qp_offset: %d\n", p_param->cr_qp_offset);
			return -EINVAL;
		}
		if (p_param->en_still_picture != 1 && p_param->en_still_picture != 0) {
			dev_err(dev, "en_still_picture: %d\n", p_param->en_still_picture);
			return -EINVAL;
		}
		if (p_param->tier > 1) {
			dev_err(dev, "tier: %d\n", p_param->tier);
			return -EINVAL;
		}
		if (p_param->profile > HEVC_PROFILE_STILLPICTURE) {
			dev_err(dev, "profile: %d\n", p_param->profile);
			return -EINVAL;
		}
		if (p_param->internal_bit_depth == 10 && p_param->profile == HEVC_PROFILE_MAIN) {
			dev_err(dev, "internal_bit_depth: %d, profile: %d\n",
				p_param->internal_bit_depth, p_param->profile);
			return -EINVAL;
		}
		if (p_param->num_ticks_poc_diff_one < 1 ||
		    p_param->num_ticks_poc_diff_one > 65535) {
			dev_err(dev, "num_ticks_poc_diff_one: %d\n",
				p_param->num_ticks_poc_diff_one);
			return -EINVAL;
		}
		if (p_param->intra_4x4 > 3 || p_param->intra_4x4 == 1) {
			dev_err(dev, "intra_4x4: %d\n", p_param->intra_4x4);
			return -EINVAL;
		}
	} else if (inst->std == W_AVC_ENC) {
		if (p_param->internal_bit_depth == 10 && !attr->support_avc10bit_enc) {
			dev_err(dev, "internal_bit_depth: %d, support_avc10bit_enc: %d\n",
				p_param->internal_bit_depth, attr->support_avc10bit_enc);
			return -EOPNOTSUPP;
		}
		if (p_param->idr_period > 2047) {
			dev_err(dev, "idr_period: %d\n", p_param->idr_period);
			return -EINVAL;
		}
		if (p_param->idr_period == 1 && p_param->gop_preset_idx == PRESET_IDX_ALL_I) {
			dev_err(dev, "idr_period: %d, gop_preset_idx: %d\n",
				p_param->idr_period, p_param->gop_preset_idx);
			return -EINVAL;
		}
		if (p_param->en_intra_smooth != 0) {
			dev_err(dev, "en_intra_smooth: %d\n", p_param->en_intra_smooth);
			return -EINVAL;
		}
		if (p_param->en_const_intra_pred != 1 &&
		    p_param->en_const_intra_pred != 0) {
			dev_err(dev, "en_const_intra_pred: %d\n",
				p_param->en_const_intra_pred);
			return -EINVAL;
		}
		if (p_param->en_temporal_mvp != 0) {
			dev_err(dev, "en_temporal_mvp: %d\n", p_param->en_temporal_mvp);
			return -EINVAL;
		}
		if (p_param->en_cabac != 1 && p_param->en_cabac != 0) {
			dev_err(dev, "en_cabac: %d\n", p_param->en_cabac);
			return -EINVAL;
		}
		if (p_param->en_transform8x8 != 1 && p_param->en_transform8x8 != 0) {
			dev_err(dev, "en_transform8x8: %d\n", p_param->en_transform8x8);
			return -EINVAL;
		}
		if (p_param->en_lf_slice_boundary != 1 &&
		    p_param->en_lf_slice_boundary != 0) {
			dev_err(dev, "en_lf_slice_boundary: %d\n",
				p_param->en_lf_slice_boundary);
			return -EINVAL;
		}
		if (p_param->beta_offset_div2 < -6 || p_param->beta_offset_div2 > 6) {
			dev_err(dev, "beta_offset_div2: %d\n", p_param->beta_offset_div2);
			return -EINVAL;
		}
		if (p_param->tc_offset_div2 < -6 || p_param->tc_offset_div2 > 6) {
			dev_err(dev, "tc_offset_div2: %d\n", p_param->tc_offset_div2);
			return -EINVAL;
		}
		if (p_param->en_sao != 0) {
			dev_err(dev, "en_sao: %d\n", p_param->en_sao);
			return -EINVAL;
		}
		if (p_param->cb_qp_offset < -12 || p_param->cb_qp_offset > 12) {
			dev_err(dev, "cb_qp_offset: %d\n", p_param->cb_qp_offset);
			return -EINVAL;
		}
		if (p_param->cr_qp_offset < -12 || p_param->cr_qp_offset > 12) {
			dev_err(dev, "cr_qp_offset: %d\n", p_param->cr_qp_offset);
			return -EINVAL;
		}
		if (p_param->en_still_picture != 0) {
			dev_err(dev, "en_still_picture: %d\n", p_param->en_still_picture);
			return -EINVAL;
		}
		if (p_param->tier != 0) {
			dev_err(dev, "tier: %d\n", p_param->tier);
			return -EINVAL;
		}
		if (p_param->profile > H264_PROFILE_HIGH10) {
			dev_err(dev, "profile: %d\n", p_param->profile);
			return -EINVAL;
		}
		if (p_param->profile) {
			if (p_param->internal_bit_depth == 10 &&
			    p_param->profile != H264_PROFILE_HIGH10) {
				dev_err(dev, "internal_bit_depth: %d, profile: %d\n",
					p_param->internal_bit_depth, p_param->profile);
				return -EINVAL;
			}
		}
		if (p_param->num_ticks_poc_diff_one != 0) {
			dev_err(dev, "num_ticks_poc_diff_one: %d\n",
				p_param->num_ticks_poc_diff_one);
			return -EINVAL;
		}
		if (p_param->intra_4x4 != 0) {
			dev_err(dev, "intra_4x4: %d\n", p_param->intra_4x4);
			return -EINVAL;
		}
	}

	return 0;
}
