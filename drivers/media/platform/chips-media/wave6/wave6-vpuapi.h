/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * Wave6 series multi-standard codec IP - wave6 helper interface
 *
 * Copyright (C) 2025 CHIPS&MEDIA INC
 */

#ifndef __WAVE6_VPUAPI_H__
#define __WAVE6_VPUAPI_H__

#include <linux/kfifo.h>
#include <linux/idr.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-ctrls.h>
#include "wave6-vpuerror.h"
#include "wave6-vpuconfig.h"
#include "wave6-vdi.h"
#include "wave6-vpu.h"

struct vpu_attr;

enum vpu_instance_type {
	VPU_INST_TYPE_DEC,
	VPU_INST_TYPE_ENC
};

/**
 * enum vpu_instance_state - VPU instance states
 * @VPU_INST_STATE_NONE:	Instance is not created or has been destroyed
 * @VPU_INST_STATE_OPEN:	Instance is created
 * @VPU_INST_STATE_INIT_SEQ:	For decoder, sequence header is parsed
 *				For encoder, parameter configuration is complete
 * @VPU_INST_STATE_PIC_RUN:	Instance is decoding or encoding
 * @VPU_INST_STATE_SEEK:	Ready to seek a decoding resume point
 *				Valid only for decoder
 * @VPU_INST_STATE_STOP:	Decoding or encoding process is stopped
 */
enum vpu_instance_state {
	VPU_INST_STATE_NONE,
	VPU_INST_STATE_OPEN,
	VPU_INST_STATE_INIT_SEQ,
	VPU_INST_STATE_PIC_RUN,
	VPU_INST_STATE_SEEK,
	VPU_INST_STATE_STOP
};

#define WAVE6_MAX_FBS 31

#define WAVE6_DEC_HEVC_MVCOL_BUF_SIZE(_w, _h) \
	((ALIGN((_w), 256) / 16) * (ALIGN((_h), 64) / 16) * 1 * 16)
#define WAVE6_DEC_AVC_MVCOL_BUF_SIZE(_w, _h) \
	((ALIGN((_w), 64) / 16) * (ALIGN((_h), 16) / 16) * 5 * 16)
#define WAVE6_FBC_LUMA_TABLE_SIZE(_w, _h) \
	(ALIGN((_w), 256) * ALIGN((_h), 64) / 32)
#define WAVE6_FBC_CHROMA_TABLE_SIZE(_w, _h) \
	(ALIGN(((_w) / 2), 256) * ALIGN((_h), 64) / 32)
#define WAVE6_ENC_AVC_MVCOL_BUF_SIZE(_w, _h) \
	((ALIGN((_w), 512) / 512) * (ALIGN((_h), 16) / 16) * 16)
#define WAVE6_ENC_HEVC_MVCOL_BUF_SIZE(_w, _h) \
	((ALIGN((_w), 64) / 64) * (ALIGN((_h), 64) / 64) * 128)
#define WAVE6_ENC_SUBSAMPLED_SIZE(_w, _h) \
	(ALIGN(((_w) / 4), 16) * ALIGN(((_h) / 4), 32))

enum codec_std {
	W_HEVC_DEC	= 0x00,
	W_HEVC_ENC	= 0x01,
	W_AVC_DEC	= 0x02,
	W_AVC_ENC	= 0x03,
	STD_UNKNOWN	= 0xFF
};

#define HEVC_PROFILE_MAIN 1
#define HEVC_PROFILE_MAIN10 2
#define HEVC_PROFILE_STILLPICTURE 3
#define HEVC_PROFILE_MAIN10_STILLPICTURE 2

#define H264_PROFILE_BP 1
#define H264_PROFILE_MP 2
#define H264_PROFILE_EXTENDED 3
#define H264_PROFILE_HP 4
#define H264_PROFILE_HIGH10 5

#define H264_VUI_SAR_IDC_EXTENDED 255

#define DEC_REFRESH_TYPE_NON_IRAP 0
#define DEC_REFRESH_TYPE_IDR 2

#define DEFAULT_TEMP_LAYER_CNT 1
#define DEFAULT_RC_INITIAL_LEVEL 8
#define DEFAULT_RC_INITIAL_QP -1
#define DEFAULT_PIC_RC_MAX_DQP 3
#define DEFAULT_QROUND_INTER 85
#define DEFAULT_QROUND_INTRA 171
#define DEFAULT_INTRA_4X4 3
#define DEFAULT_NUM_TICKS_POC_DIFF 100
#define DEFAULT_RC_UPDATE_SPEED_CBR 64
#define DEFAULT_RC_UPDATE_SPEED_VBR 16

#define SEQ_CHANGE_ENABLE_PROFILE BIT(5)
#define SEQ_CHANGE_ENABLE_SIZE BIT(16)
#define SEQ_CHANGE_ENABLE_BIT_DEPTH BIT(18)
#define SEQ_CHANGE_ENABLE_DPB_COUNT BIT(19)

#define SEQ_CHANGE_ENABLE_ALL_HEVC (SEQ_CHANGE_ENABLE_PROFILE | \
		SEQ_CHANGE_ENABLE_SIZE | \
		SEQ_CHANGE_ENABLE_BIT_DEPTH | \
		SEQ_CHANGE_ENABLE_DPB_COUNT)

#define SEQ_CHANGE_ENABLE_ALL_AVC (SEQ_CHANGE_ENABLE_SIZE | \
		SEQ_CHANGE_ENABLE_BIT_DEPTH | \
		SEQ_CHANGE_ENABLE_DPB_COUNT)

#define DEC_NOTI_FLAG_NO_FB		BIT(1)
#define DEC_NOTI_FLAG_SEQ_CHANGE	BIT(0)

#define RECON_IDX_FLAG_ENC_END -1
#define RECON_IDX_FLAG_ENC_DELAY -2
#define RECON_IDX_FLAG_HEADER_ONLY -3
#define RECON_IDX_FLAG_CHANGE_PARAM -4

#define FW_VERSION_MAJOR_MASK	0xFF000000
#define FW_VERSION_MINOR_MASK	0x00FF0000
#define FW_VERSION_REL_MASK	0x0000FFFF
#define FW_VERSION_MAJOR(x)	FIELD_GET(FW_VERSION_MAJOR_MASK, (x))
#define FW_VERSION_MINOR(x)	FIELD_GET(FW_VERSION_MINOR_MASK, (x))
#define FW_VERSION_REL(x)	FIELD_GET(FW_VERSION_REL_MASK, (x))

enum mirror_direction {
	MIR_NONE,
	MIR_VER,
	MIR_HOR,
	MIR_HOR_VER
};

enum rotation_angle {
	ROT_0 = 0,
	ROT_90 = 90,
	ROT_180 = 180,
	ROT_270 = 270
};

enum chroma_format_idc {
	C_FMT_IDC_YUV400,
	C_FMT_IDC_YUV420,
	C_FMT_IDC_YUV422,
	C_FMT_IDC_YUV444,
	C_FMT_IDC_RGB
};

enum csc_format_order {
	CSC_FMT_ORDER_RGB	= 0,
	CSC_FMT_ORDER_RBG	= 1,
	CSC_FMT_ORDER_GRB	= 2,
	CSC_FMT_ORDER_GBR	= 3,
	CSC_FMT_ORDER_BGR	= 4,
	CSC_FMT_ORDER_BRG	= 5,

	CSC_FMT_ORDER_ARGB	= 0,
	CSC_FMT_ORDER_ARBG	= 1,
	CSC_FMT_ORDER_AGRB	= 2,
	CSC_FMT_ORDER_AGBR	= 3,
	CSC_FMT_ORDER_ABGR	= 4,
	CSC_FMT_ORDER_ABRG	= 5,
	CSC_FMT_ORDER_RGBA	= 8,
	CSC_FMT_ORDER_RBGA	= 9,
	CSC_FMT_ORDER_GRBA	= 10,
	CSC_FMT_ORDER_GBRA	= 11,
	CSC_FMT_ORDER_BGRA	= 12,
	CSC_FMT_ORDER_BRGA	= 13,
};

enum frame_buffer_format {
	FORMAT_ERR = -1,

	FORMAT_420 = 0,
	FORMAT_422,
	FORMAT_224,
	FORMAT_444,
	FORMAT_400,

	FORMAT_420_P10_16BIT_MSB = 5,
	FORMAT_420_P10_16BIT_LSB,
	FORMAT_420_P10_32BIT_MSB,
	FORMAT_420_P10_32BIT_LSB,

	FORMAT_422_P10_16BIT_MSB,
	FORMAT_422_P10_16BIT_LSB,
	FORMAT_422_P10_32BIT_MSB,
	FORMAT_422_P10_32BIT_LSB,

	FORMAT_444_P10_16BIT_MSB,
	FORMAT_444_P10_16BIT_LSB,
	FORMAT_444_P10_32BIT_MSB,
	FORMAT_444_P10_32BIT_LSB,

	FORMAT_400_P10_16BIT_MSB,
	FORMAT_400_P10_16BIT_LSB,
	FORMAT_400_P10_32BIT_MSB,
	FORMAT_400_P10_32BIT_LSB,

	FORMAT_YUYV,
	FORMAT_YUYV_P10_16BIT_MSB,
	FORMAT_YUYV_P10_16BIT_LSB,
	FORMAT_YUYV_P10_32BIT_MSB,
	FORMAT_YUYV_P10_32BIT_LSB,

	FORMAT_YVYU,
	FORMAT_YVYU_P10_16BIT_MSB,
	FORMAT_YVYU_P10_16BIT_LSB,
	FORMAT_YVYU_P10_32BIT_MSB,
	FORMAT_YVYU_P10_32BIT_LSB,

	FORMAT_UYVY,
	FORMAT_UYVY_P10_16BIT_MSB,
	FORMAT_UYVY_P10_16BIT_LSB,
	FORMAT_UYVY_P10_32BIT_MSB,
	FORMAT_UYVY_P10_32BIT_LSB,

	FORMAT_VYUY,
	FORMAT_VYUY_P10_16BIT_MSB,
	FORMAT_VYUY_P10_16BIT_LSB,
	FORMAT_VYUY_P10_32BIT_MSB,
	FORMAT_VYUY_P10_32BIT_LSB,

	FORMAT_RGB_32BIT_PACKED = 90,
	FORMAT_YUV444_32BIT_PACKED,
	FORMAT_RGB_P10_32BIT_PACKED,
	FORMAT_YUV444_P10_32BIT_PACKED,

	FORMAT_RGB_24BIT_PACKED = 95,
	FORMAT_YUV444_24BIT_PACKED,
	FORMAT_YUV444_24BIT,

	FORMAT_MAX,
};

enum pic_type {
	PIC_TYPE_I = 0,
	PIC_TYPE_P = 1,
	PIC_TYPE_B = 2,
	PIC_TYPE_IDR = 5,
	PIC_TYPE_MAX
};

enum enc_force_pic_type {
	ENC_FORCE_PIC_TYPE_I = 0,
	ENC_FORCE_PIC_TYPE_P = 1,
	ENC_FORCE_PIC_TYPE_B = 2,
	ENC_FORCE_PIC_TYPE_IDR = 3,
	ENC_FORCE_PIC_TYPE_DISABLED = 4,
};

enum display_mode {
	DISP_MODE_DISP_ORDER,
	DISP_MODE_DEC_ORDER,
};

enum sw_reset_mode {
	SW_RESET_SAFETY,
	SW_RESET_FORCE,
	SW_RESET_ON_BOOT
};

enum tiled_map_type {
	LINEAR_FRAME_MAP = 0,
	COMPRESSED_FRAME_MAP = 17,
};

enum aux_buffer_type {
	AUX_BUF_FBC_Y_TBL,
	AUX_BUF_FBC_C_TBL,
	AUX_BUF_MV_COL,
	AUX_BUF_SUB_SAMPLE,
	AUX_BUF_TYPE_MAX,
};

enum intra_refresh_mode {
	INTRA_REFRESH_NONE = 0,
	INTRA_REFRESH_ROW = 1,
	INTRA_REFRESH_COLUMN = 2,
};

struct vpu_attr {
	u32 product_id;
	char product_name[8];
	u32 product_code;
	u32 product_version;
	u32 fw_version;
	u32 fw_revision;
	u32 hw_version;
	u32 support_decoders;
	u32 support_encoders;
	bool support_avc10bit_dec;
	bool support_hevc10bit_dec;
	bool support_avc10bit_enc;
	bool support_hevc10bit_enc;
	bool support_dual_core;
};

struct frame_buffer {
	dma_addr_t buf_y;
	dma_addr_t buf_cb;
	dma_addr_t buf_cr;
	enum tiled_map_type map_type;
	unsigned int stride;
	unsigned int width;
	unsigned int height;
	int index;
	u32 luma_bit_depth: 4;
	u32 chroma_bit_depth: 4;
	enum chroma_format_idc c_fmt_idc;
};

struct vpu_rect {
	u32 left;
	u32 top;
	u32 right;
	u32 bottom;
};

struct sar_info {
	u32 enable;
	u32 idc;
	u32 width;
	u32 height;
};

struct aux_buffer {
	int index;
	int size;
	dma_addr_t addr;
};

struct aux_buffer_info {
	int num;
	struct aux_buffer *buf_array;
	enum aux_buffer_type type;
};

struct instance_buffer {
	dma_addr_t temp_base;
	u32 temp_size;
	dma_addr_t ar_base;
};

struct report_cycle {
	u32 host_cmd_s;
	u32 host_cmd_e;
	u32 proc_s;
	u32 proc_e;
	u32 vpu_s;
	u32 vpu_e;
	u32 frame_cycle;
	u32 proc_cycle;
	u32 vpu_cycle;
};

struct color_param {
	u32 color_range;
	u32 matrix_coefficients;
	u32 transfer_characteristics;
	u32 color_primaries;
	bool color_description_present;
	bool video_signal_type_present;
};

struct sec_axi_info {
	bool use_dec_ip;
	bool use_dec_lf_row;
	bool use_enc_rdo;
	bool use_enc_lf;
};

struct dec_aux_buffer_size_info {
	int width;
	int height;
	enum aux_buffer_type type;
};

struct dec_scaler_info {
	bool enable;
	int width;
	int height;
};

struct dec_open_param {
	enum display_mode disp_mode;
	u32 ext_addr_vcpu: 8;
	bool is_secure_inst;
	u32 inst_priority: 5;
	struct instance_buffer inst_buffer;
};

struct dec_seq_info {
	u32 pic_width;
	u32 pic_height;
	u32 f_rate_numerator;
	u32 f_rate_denominator;
	struct vpu_rect pic_crop_rect;
	u32 min_frame_buffer_count;
	u32 req_mv_buffer_count;
	u32 frame_buf_delay;
	u32 profile;
	u32 level;
	u32 tier;
	bool is_ext_sar;
	u32 aspect_rate_info;
	u32 bitrate;
	enum chroma_format_idc c_fmt_idc;
	u32 luma_bit_depth;
	u32 chroma_bit_depth;
	u32 err_reason;
	int warn_info;
	dma_addr_t rd_ptr;
	dma_addr_t wr_ptr;
	unsigned int sequence_no;
	struct color_param color;
};

struct dec_param {
	u64 timestamp;
};

struct dec_output_info {
	int nal_type;
	int pic_type;
	int disp_pic_width;
	int disp_pic_height;
	int dec_pic_width;
	int dec_pic_height;
	int decoded_poc;
	int display_poc;
	dma_addr_t rd_ptr;
	dma_addr_t wr_ptr;
	dma_addr_t byte_pos_frame_start;
	dma_addr_t byte_pos_frame_end;
	dma_addr_t frame_decoded_addr;
	dma_addr_t frame_display_addr;
	int error_reason;
	int warn_info;
	unsigned int sequence_no;
	struct report_cycle cycle;
	dma_addr_t release_disp_frame_addr[WAVE6_MAX_FBS];
	dma_addr_t disp_frame_addr[WAVE6_MAX_FBS];
	u64 timestamp;
	u32 notification_flags;
	u32 release_disp_frame_num: 5;
	u32 disp_frame_num: 5;
	u32 ctu_size: 2;
	bool frame_display;
	bool frame_decoded;
	bool stream_end;
	bool last_frame_in_au;
	bool decoding_success;
};

struct dec_info {
	struct dec_open_param open_param;
	struct dec_seq_info seq_info;
	dma_addr_t stream_wr_ptr;
	dma_addr_t stream_rd_ptr;
	bool stream_end;
	struct vpu_buf vb_mv[WAVE6_MAX_FBS];
	struct vpu_buf vb_fbc_y_tbl[WAVE6_MAX_FBS];
	struct vpu_buf vb_fbc_c_tbl[WAVE6_MAX_FBS];
	struct frame_buffer disp_buf[WAVE6_MAX_FBS];
	int stride;
	bool seq_info_obtained;
	struct sec_axi_info sec_axi;
	struct dec_output_info dec_out_info[WAVE6_MAX_FBS];
	int seq_change_mask;
	enum frame_buffer_format wtl_format;
};

#define MAX_CUSTOM_LAMBDA_NUM 52
#define MAX_NUM_TEMP_LAYER 7
#define MAX_CUSTOM_GOP_NUM 8
#define MAX_NUM_CHANGEABLE_TEMP_LAYER 4

struct custom_gop_pic_param {
	int pic_type;
	int poc_offset;
	int pic_qp;
	bool multi_ref_p;
	int ref_poc_l0;
	int ref_poc_l1;
	int temporal_id;
};

struct custom_gop_param {
	int size;
	struct custom_gop_pic_param pic[MAX_CUSTOM_GOP_NUM];
};

struct temporal_layer_param {
	bool change_qp;
	u32 qp_i;
	u32 qp_p;
	u32 qp_b;
};

struct enc_aux_buffer_size_info {
	int width;
	int height;
	enum aux_buffer_type type;
};

struct enc_scaler_info {
	bool enable;
	int width;
	int height;
	int coef_mode;
};

struct enc_codec_param {
	u32 internal_bit_depth;
	u32 decoding_refresh_type;
	u32 idr_period;
	u32 intra_period;
	u32 gop_preset_idx;
	u32 frame_rate;
	u32 bitrate;
	u32 cpb_size;
	u32 hvs_qp_scale_div2;
	u32 max_delta_qp;
	int rc_initial_qp;
	u32 rc_update_speed;
	u32 max_bitrate;
	u32 rc_mode;
	u32 rc_initial_level;
	u32 pic_rc_max_dqp;
	u32 bg_th_diff;
	u32 bg_th_mean_diff;
	int bg_delta_qp;
	u32 intra_refresh_mode;
	u32 intra_refresh_arg;
	int beta_offset_div2;
	int tc_offset_div2;
	u32 qp;
	u32 min_qp_i;
	u32 max_qp_i;
	u32 min_qp_p;
	u32 max_qp_p;
	u32 min_qp_b;
	u32 max_qp_b;
	int cb_qp_offset;
	int cr_qp_offset;
	u32 qround_intra;
	u32 qround_inter;
	int lambda_dqp_intra;
	int lambda_dqp_inter;
	u32 slice_mode;
	u32 slice_arg;
	u32 level;
	u32 tier;
	u32 profile;
	struct vpu_rect conf_win;
	u32 forced_idr_header;
	u16 lambda_ssd[MAX_CUSTOM_LAMBDA_NUM];
	u16 lambda_sad[MAX_CUSTOM_LAMBDA_NUM];
	struct custom_gop_param gop_param;
	struct temporal_layer_param temp_layer[MAX_NUM_CHANGEABLE_TEMP_LAYER];
	u32 temp_layer_cnt;
	u32 mv_histo_th0;
	u32 mv_histo_th1;
	u32 mv_histo_th2;
	u32 mv_histo_th3;
	u32 num_units_in_tick;
	u32 time_scale;
	u32 num_ticks_poc_diff_one;
	struct color_param color;
	struct sar_info sar;
	u32 max_intra_pic_bit;
	u32 max_inter_pic_bit;
	u32 intra_4x4;

	u32 en_const_intra_pred: 1;
	u32 en_longterm: 1;
	u32 en_rate_control: 1;
	u32 en_transform8x8: 1;
	u32 en_hvs_qp: 1;
	u32 en_bg_detect: 1;
	u32 en_temporal_mvp: 1;
	u32 en_cabac: 1;
	u32 en_dbk: 1;
	u32 en_sao: 1;
	u32 en_lf_slice_boundary: 1;
	u32 en_scaling_list: 1;
	u32 en_qround_offset: 1;
	u32 en_still_picture: 1;
	u32 en_intra_smooth: 1;
	u32 en_custom_lambda: 1;
	u32 en_report_mv_histo: 1;
	u32 en_cu_level_rate_control: 1;
	u32 en_skip_frame: 1;
};

struct enc_open_param {
	int pic_width;
	int pic_height;
	struct enc_codec_param codec_param;
	enum endian_mode source_endian;
	enum frame_buffer_format src_format;
	enum frame_buffer_format output_format;
	u32 ext_addr_vcpu: 8;
	bool is_secure_inst;
	u32 inst_priority: 5;
	struct instance_buffer inst_buffer;
	enum mirror_direction mir_dir;
	enum rotation_angle rot_angle;
};

struct enc_seq_info {
	u32 min_frame_buffer_count;
	u32 min_src_frame_count;
	u32 req_mv_buffer_count;
	int max_latency_pictures;
	int err_reason;
	int warn_info;
};

struct enc_csc_param {
	u32 fmt_order;
	s32 coef_ry;
	s32 coef_gy;
	s32 coef_by;
	s32 coef_rcb;
	s32 coef_gcb;
	s32 coef_bcb;
	s32 coef_rcr;
	s32 coef_gcr;
	s32 coef_bcr;
	u32 offset_y;
	u32 offset_cb;
	u32 offset_cr;
};

struct enc_param {
	struct frame_buffer *source_frame;
	bool skip_picture;
	dma_addr_t pic_stream_buffer_addr;
	int pic_stream_buffer_size;
	bool force_pic_qp;
	int force_pic_qp_i;
	int force_pic_qp_p;
	int force_pic_qp_b;
	bool force_pic;
	int force_pic_type;
	int src_idx;
	bool src_end;
	u32 bitrate;
	struct enc_csc_param csc;
	u64 timestamp;
};

struct enc_report_fme_sum {
	u32 lower_x0;
	u32 higher_x0;
	u32 lower_y0;
	u32 higher_y0;
	u32 lower_x1;
	u32 higher_x1;
	u32 lower_y1;
	u32 higher_y1;
};

struct enc_report_mv_histo {
	u32 cnt0;
	u32 cnt1;
	u32 cnt2;
	u32 cnt3;
	u32 cnt4;
};

struct enc_output_info {
	dma_addr_t bitstream_buffer;
	u32 bitstream_size;
	int pic_type;
	int num_of_slices;
	int recon_frame_index;
	dma_addr_t rd_ptr;
	dma_addr_t wr_ptr;
	int pic_skipped;
	int num_of_intra;
	int num_of_merge;
	int num_of_skip_block;
	int avg_ctu_qp;
	int enc_pic_byte;
	int enc_gop_pic_idx;
	int enc_pic_poc;
	int enc_src_idx;
	int enc_vcl_nut;
	int enc_pic_cnt;
	int error_reason;
	int warn_info;
	u32 pic_distortion_low;
	u32 pic_distortion_high;
	bool non_ref_pic;
	bool encoding_success;
	struct enc_report_fme_sum fme_sum;
	struct enc_report_mv_histo mv_histo;
	struct report_cycle cycle;
	u64 timestamp;
	dma_addr_t src_y_addr;
	dma_addr_t custom_map_addr;
	dma_addr_t prefix_sei_nal_addr;
	dma_addr_t suffix_sei_nal_addr;
};

enum gop_preset_idx {
	PRESET_IDX_CUSTOM_GOP = 0,
	PRESET_IDX_ALL_I = 1,
	PRESET_IDX_IPP = 2,
	PRESET_IDX_IBBB = 3,
	PRESET_IDX_IBPBP = 4,
	PRESET_IDX_IBBBP = 5,
	PRESET_IDX_IPPPP = 6,
	PRESET_IDX_IBBBB = 7,
	PRESET_IDX_RA_IB = 8,
	PRESET_IDX_IPP_SINGLE = 9,
	PRESET_IDX_MAX,
};

struct enc_info {
	struct enc_open_param open_param;
	struct enc_seq_info seq_info;
	int num_frame_buffers;
	int stride;
	enum mirror_direction mir_dir;
	enum rotation_angle rot_angle;
	bool seq_info_obtained;
	struct sec_axi_info sec_axi;
	struct vpu_buf vb_mv[WAVE6_MAX_FBS];
	struct vpu_buf vb_fbc_y_tbl[WAVE6_MAX_FBS];
	struct vpu_buf vb_fbc_c_tbl[WAVE6_MAX_FBS];
	struct vpu_buf vb_sub_sam_buf[WAVE6_MAX_FBS];
	u32 width;
	u32 height;
	struct enc_scaler_info scaler;
	enum chroma_format_idc c_fmt_idc;
};

struct h264_enc_controls {
	u32 profile;
	u32 level;
	u32 min_qp;
	u32 max_qp;
	u32 i_frame_qp;
	u32 p_frame_qp;
	u32 b_frame_qp;
	u32 loop_filter_mode;
	u32 loop_filter_beta;
	u32 loop_filter_alpha;
	u32 _8x8_transform;
	u32 constrained_intra_prediction;
	u32 chroma_qp_index_offset;
	u32 entropy_mode;
	u32 i_period;
	u32 vui_sar_enable;
	u32 vui_sar_idc;
	u32 vui_ext_sar_width;
	u32 vui_ext_sar_height;
	u32 cpb_size;
};

struct hevc_enc_controls {
	u32 profile;
	u32 level;
	u32 min_qp;
	u32 max_qp;
	u32 i_frame_qp;
	u32 p_frame_qp;
	u32 b_frame_qp;
	u32 loop_filter_mode;
	u32 lf_beta_offset_div2;
	u32 lf_tc_offset_div2;
	u32 refresh_type;
	u32 refresh_period;
	u32 const_intra_pred;
	u32 strong_smoothing;
	u32 tmv_prediction;
};

struct enc_controls {
	u32 frame_rate;
	u32 rotation_angle;
	u32 mirror_direction;
	u32 bitrate;
	u32 bitrate_mode;
	u32 gop_size;
	u32 frame_rc_enable;
	u32 mb_rc_enable;
	u32 slice_mode;
	u32 slice_max_mb;
	u32 prepend_spspps_to_idr;
	u32 intra_refresh_period;
	struct h264_enc_controls h264;
	struct hevc_enc_controls hevc;
	u32 force_key_frame;
	u32 frame_skip_mode;
};

/**
 * struct vpu_irq - VPU interrupt information
 * @status:	Value of the VPU interrupt status
 * @inst_idc:	Bitmask of instance IDs for this interrupt
 */
struct vpu_irq {
	u32 status;
	u32 inst_idc;
};

/**
 * struct vpu_core_device - VPU core driver structure
 * @dev:		Platform device pointer
 * @vpu:		Parent VPU driver structure
 * @v4l2_dev:		V4L2 device
 * @m2m_dev:		V4L2 mem2mem device
 * @instances:		List of VPU instances
 * @inst_lock:		Spinlock protecting instance list
 * @video_dev_dec:	Video device node for decoder
 * @video_dev_enc:	Video device node for encoder
 * @dev_lock:		Mutex protecting video_device, src, dst vb2_queue
 * @hw_lock:		Mutex protecting register access
 * @attr:		Hardware attributes retrieved after boot
 * @reg_base:		Base address of MMIO registers
 * @temp_vbuf:		TEMP buffer
 * @clks:		Array of clock handles
 * @num_clks:		Number of entries in @clks
 * @irq_fifo:		kfifo storing interrupt information
 * @task_timer:		Workqueue to detect hangs during frame processing
 * @res:		Device compatible data
 * @debugfs:		Debugfs entry
 * @active:		Decoder/encoder active flag
 */
struct vpu_core_device {
	struct device *dev;
	struct wave6_vpu_device *vpu;
	struct v4l2_device v4l2_dev;
	struct v4l2_m2m_dev *m2m_dev;
	struct list_head instances;
	spinlock_t inst_lock; /* Protects instance list */
	struct video_device *video_dev_dec;
	struct video_device *video_dev_enc;
	struct mutex dev_lock; /* Protects video_device, src, dst vb2_queue */
	struct mutex hw_lock; /* Protects register access */
	struct vpu_attr	attr;
	void __iomem *reg_base;

	/* Allocates per-core, used during decode/encode process. */
	struct vpu_buf temp_vbuf;

	struct clk_bulk_data *clks;
	int num_clks;

	/* Stores interrupt information quickly, consumed in irq_thread. */
	struct kfifo irq_fifo;

	/* Detects hangs during decode/encode and aborts if needed. */
	struct delayed_work task_timer;

	const struct wave6_vpu_core_resource *res;
	struct dentry *debugfs;
	bool active;
};

struct vpu_instance;

struct vpu_instance_ops {
	int (*prepare_process)(struct vpu_instance *inst);
	int (*start_process)(struct vpu_instance *inst);
	void (*finish_process)(struct vpu_instance *inst, bool error);
};

struct vpu_performance_info {
	ktime_t ts_start;
	ktime_t ts_first;
	ktime_t ts_last;
	s64 latency_first;
	s64 latency_max;
	s64 min_process_time;
	s64 max_process_time;
	u64 total_sw_time;
	u64 total_hw_time;
};

/**
 * struct vpu_instance - VPU instance structure
 * @list:		List head for VPU core device instance list
 * @v4l2_fh:		V4L2 file handler
 * @v4l2_ctrl_hdl:	V4L2 control handler
 * @dev:		VPU core driver structure
 * @irq_done:		Completion for command finish
 * @src_fmt:		V4L2 pixel format for source
 * @dst_fmt:		V4L2 pixel format for destination
 * @crop:		Crop rectangle
 * @codec_rect:		Actual encoding rectangle
 * @scaler_info:	Decoder scaler information
 * @colorspace:		V4L2 colorspace
 * @xfer_func:		V4L2 transfer function
 * @ycbcr_enc:		V4L2 YCbCr encoding
 * @quantization:	V4L2 quantization
 * @state:		Instance state
 * @state_in_seek:	Previous state before seek
 * @type:		Instance type (decoder or encoder)
 * @ops:		Instance operations
 * @std:		Codec standard
 * @id:			Instance id
 * @enc_info:		Encoder-specific information
 * @dec_info:		Decoder-specific information
 * @frame_buf:		Metadata for reference frame buffers
 * @frame_vbuf:		Reference frame buffers
 * @aux_vbuf:		Auxiliary buffers
 * @ar_vbuf:		Auxiliary AR buffer for encoder
 * @fbc_buf_registered:	Reference frame buffers registration flag
 * @queued_src_buf_num:	number of queued source buffers
 * @queued_dst_buf_num:	number of queued destination buffers
 * @processed_buf_num:	number of processed buffers
 * @error_buf_num:	number of error buffers
 * @sequence:		V4L2 buffers sequence number
 * @next_buf_last:	Next queued buffer last flag
 * @cbcr_interleave:	CbCr interleave format flag
 * @nv21:		NV21 format flag
 * @eos:		End of stream flag
 * @error_recovery:	Error recovery flag
 * @disp_mode:		Display mode for decoder V4L2 control
 * @enc_ctrls:		Encoder V4L2 control and streaming parameters
 * @performance:	Performance information
 * @debugfs:		Debugfs entry
 */
struct vpu_instance {
	struct list_head list;
	struct v4l2_fh v4l2_fh;
	struct v4l2_ctrl_handler v4l2_ctrl_hdl;
	struct vpu_core_device *dev;

	/* Signals when INIT_SEQ, SET_PARAM command is done. */
	struct completion irq_done;

	struct v4l2_pix_format_mplane src_fmt;
	struct v4l2_pix_format_mplane dst_fmt;
	struct v4l2_rect crop;
	struct v4l2_rect codec_rect;
	struct dec_scaler_info scaler_info;
	enum v4l2_colorspace colorspace;
	enum v4l2_xfer_func xfer_func;
	enum v4l2_ycbcr_encoding ycbcr_enc;
	enum v4l2_quantization quantization;

	enum vpu_instance_state state;
	enum vpu_instance_state state_in_seek;
	enum vpu_instance_type type;
	const struct vpu_instance_ops *ops;

	enum codec_std std;
	u32 id;
	union {
		struct enc_info enc_info;
		struct dec_info dec_info;
	} *codec_info;
	struct frame_buffer frame_buf[WAVE6_MAX_FBS];
	struct vpu_buf frame_vbuf[WAVE6_MAX_FBS];
	struct vpu_buf aux_vbuf[AUX_BUF_TYPE_MAX][WAVE6_MAX_FBS];
	struct vpu_buf ar_vbuf;
	u32 fbc_buf_registered;
	u32 queued_src_buf_num;
	u32 queued_dst_buf_num;
	u32 processed_buf_num;
	u32 error_buf_num;
	u32 sequence;
	bool next_buf_last;
	bool cbcr_interleave;
	bool nv21;
	bool eos;
	bool error_recovery;

	enum display_mode disp_mode;
	struct enc_controls enc_ctrls;

	struct vpu_performance_info performance;
	struct dentry *debugfs;
};

void wave6_vpu_writel(struct vpu_core_device *core, u32 addr, u32 data);
u32 wave6_vpu_readl(struct vpu_core_device *core, u32 addr);

int wave6_vpu_dec_open(struct vpu_instance *inst, struct dec_open_param *pop);
int wave6_vpu_dec_close(struct vpu_instance *inst, u32 *fail_res);
int wave6_vpu_dec_issue_seq_init(struct vpu_instance *inst);
int wave6_vpu_dec_complete_seq_init(struct vpu_instance *inst, struct dec_seq_info *info);
int wave6_vpu_dec_get_aux_buffer_size(struct vpu_instance *inst,
				      struct dec_aux_buffer_size_info info,
				      uint32_t *size);
int wave6_vpu_dec_register_aux_buffer(struct vpu_instance *inst, struct aux_buffer_info info);
int wave6_vpu_dec_register_frame_buffer_ex(struct vpu_instance *inst, int num_of_dec_fbs,
					   int stride, int height, int map_type);
int wave6_vpu_dec_register_display_buffer_ex(struct vpu_instance *inst, struct frame_buffer fb);
int wave6_vpu_dec_start_one_frame(struct vpu_instance *inst, struct dec_param *param,
				  u32 *res_fail);
int wave6_vpu_dec_get_output_info(struct vpu_instance *inst, struct dec_output_info *info);
void wave6_vpu_dec_set_rd_ptr(struct vpu_instance *inst, dma_addr_t addr,
			      bool update_wr_ptr);
void wave6_vpu_dec_reset_frame_buffer_info(struct vpu_instance *inst);
void wave6_vpu_dec_get_bitstream_buffer(struct vpu_instance *inst,
					dma_addr_t *p_rd_ptr,
					dma_addr_t *p_wr_ptr);
int wave6_vpu_dec_update_bitstream_buffer(struct vpu_instance *inst, int size);
int wave6_vpu_dec_flush_instance(struct vpu_instance *inst);

int wave6_vpu_enc_open(struct vpu_instance *inst, struct enc_open_param *enc_op_param);
int wave6_vpu_enc_close(struct vpu_instance *inst, u32 *fail_res);
int wave6_vpu_enc_issue_seq_init(struct vpu_instance *inst);
int wave6_vpu_enc_issue_seq_change(struct vpu_instance *inst, bool *changed);
int wave6_vpu_enc_complete_seq_init(struct vpu_instance *inst, struct enc_seq_info *info);
int wave6_vpu_enc_get_aux_buffer_size(struct vpu_instance *inst,
				      struct enc_aux_buffer_size_info info,
				      uint32_t *size);
int wave6_vpu_enc_register_aux_buffer(struct vpu_instance *inst, struct aux_buffer_info info);
int wave6_vpu_enc_register_frame_buffer_ex(struct vpu_instance *inst, int num, unsigned int stride,
					   int height, enum tiled_map_type map_type);
int wave6_vpu_enc_start_one_frame(struct vpu_instance *inst, struct enc_param *param,
				  u32 *fail_res);
int wave6_vpu_enc_get_output_info(struct vpu_instance *inst, struct enc_output_info *info);

#endif /* __WAVE6_VPUAPI_H__ */
