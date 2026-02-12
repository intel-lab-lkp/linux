/* SPDX-License-Identifier: GPL-2.0 */
#if !defined(_TRACE_V4L2_REQUESTS_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_V4L2_REQUESTS_H_

#include <linux/tracepoint.h>
#include <linux/v4l2-controls.h>

#undef TRACE_SYSTEM
#define TRACE_SYSTEM v4l2_requests

/* AV1 controls */
DECLARE_EVENT_CLASS(v4l2_ctrl_av1_seq_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_av1_sequence *s),
	TP_ARGS(tgid, fd, s),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_av1_sequence, s)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->s = *s;),
	TP_printk("tgid = %u, fd = %u, "
		  "flags = %s, seq_profile = %u, order_hint_bits = %u, bit_depth = %u, "
		  "max_frame_width_minus_1 = %u, max_frame_height_minus_1 = %u",
		  __entry->tgid, __entry->fd,
		  __print_flags(__entry->s.flags, "|",
		  {V4L2_AV1_SEQUENCE_FLAG_STILL_PICTURE, "STILL_PICTURE"},
		  {V4L2_AV1_SEQUENCE_FLAG_USE_128X128_SUPERBLOCK, "USE_128X128_SUPERBLOCK"},
		  {V4L2_AV1_SEQUENCE_FLAG_ENABLE_FILTER_INTRA, "ENABLE_FILTER_INTRA"},
		  {V4L2_AV1_SEQUENCE_FLAG_ENABLE_INTRA_EDGE_FILTER, "ENABLE_INTRA_EDGE_FILTER"},
		  {V4L2_AV1_SEQUENCE_FLAG_ENABLE_INTERINTRA_COMPOUND, "ENABLE_INTERINTRA_COMPOUND"},
		  {V4L2_AV1_SEQUENCE_FLAG_ENABLE_MASKED_COMPOUND, "ENABLE_MASKED_COMPOUND"},
		  {V4L2_AV1_SEQUENCE_FLAG_ENABLE_WARPED_MOTION, "ENABLE_WARPED_MOTION"},
		  {V4L2_AV1_SEQUENCE_FLAG_ENABLE_DUAL_FILTER, "ENABLE_DUAL_FILTER"},
		  {V4L2_AV1_SEQUENCE_FLAG_ENABLE_ORDER_HINT, "ENABLE_ORDER_HINT"},
		  {V4L2_AV1_SEQUENCE_FLAG_ENABLE_JNT_COMP, "ENABLE_JNT_COMP"},
		  {V4L2_AV1_SEQUENCE_FLAG_ENABLE_REF_FRAME_MVS, "ENABLE_REF_FRAME_MVS"},
		  {V4L2_AV1_SEQUENCE_FLAG_ENABLE_SUPERRES, "ENABLE_SUPERRES"},
		  {V4L2_AV1_SEQUENCE_FLAG_ENABLE_CDEF, "ENABLE_CDEF"},
		  {V4L2_AV1_SEQUENCE_FLAG_ENABLE_RESTORATION, "ENABLE_RESTORATION"},
		  {V4L2_AV1_SEQUENCE_FLAG_MONO_CHROME, "MONO_CHROME"},
		  {V4L2_AV1_SEQUENCE_FLAG_COLOR_RANGE, "COLOR_RANGE"},
		  {V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_X, "SUBSAMPLING_X"},
		  {V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_Y, "SUBSAMPLING_Y"},
		  {V4L2_AV1_SEQUENCE_FLAG_FILM_GRAIN_PARAMS_PRESENT, "FILM_GRAIN_PARAMS_PRESENT"},
		  {V4L2_AV1_SEQUENCE_FLAG_SEPARATE_UV_DELTA_Q, "SEPARATE_UV_DELTA_Q"}),
		  __entry->s.seq_profile,
		  __entry->s.order_hint_bits,
		  __entry->s.bit_depth,
		  __entry->s.max_frame_width_minus_1,
		  __entry->s.max_frame_height_minus_1
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_av1_tge_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_av1_tile_group_entry *t),
	TP_ARGS(tgid, fd, t),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_av1_tile_group_entry, t)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->t = *t;),
	TP_printk("tgid = %u, fd = %u, "
		  "tile_offset = %u, tile_size = %u, tile_row = %u, tile_col = %u",
		  __entry->tgid, __entry->fd,
		  __entry->t.tile_offset,
		  __entry->t.tile_size,
		  __entry->t.tile_row,
		  __entry->t.tile_col
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_av1_frame_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_av1_frame *f),
	TP_ARGS(tgid, fd, f),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_av1_frame, f)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->f = *f;),
	TP_printk("tgid = %u, fd = %u, "
		  "tile_info.flags = %s, tile_info.context_update_tile_id = %u, "
		  "tile_info.tile_cols = %u, tile_info.tile_rows = %u, "
		  "tile_info.mi_col_starts = %s, tile_info.mi_row_starts = %s, "
		  "tile_info.width_in_sbs_minus_1 = %s, tile_info.height_in_sbs_minus_1 = %s, "
		  "tile_info.tile_size_bytes = %u, quantization.flags = %s, "
		  "quantization.base_q_idx = %u, quantization.delta_q_y_dc = %d, "
		  "quantization.delta_q_u_dc = %d, quantization.delta_q_u_ac = %d, "
		  "quantization.delta_q_v_dc = %d, quantization.delta_q_v_ac = %d, "
		  "quantization.qm_y = %u, quantization.qm_u = %u, quantization.qm_v = %u, "
		  "quantization.delta_q_res = %u, superres_denom = %u, segmentation.flags = %s, "
		  "segmentation.last_active_seg_id = %u, segmentation.feature_enabled = %s, "
		  "loop_filter.flags = %s, loop_filter.level = %s, loop_filter.sharpness = %u, "
		  "loop_filter.ref_deltas = %s, loop_filter.mode_deltas = %s, "
		  "loop_filter.delta_lf_res = %u, cdef.damping_minus_3 = %u, cdef.bits = %u, "
		  "cdef.y_pri_strength = %s, cdef.y_sec_strength = %s, "
		  "cdef.uv_pri_strength = %s, cdef.uv_sec_strength = %s, skip_mode_frame = %s, "
		  "primary_ref_frame = %u, loop_restoration.flags = %s, "
		  "loop_restoration.lr_unit_shift = %u, loop_restoration.lr_uv_shift = %u, "
		  "loop_restoration.frame_restoration_type = %s, "
		  "loop_restoration.loop_restoration_size = %s, flags = %s, order_hint = %u, "
		  "upscaled_width = %u, frame_width_minus_1 = %u, frame_height_minus_1 = %u, "
		  "render_width_minus_1 = %u, render_height_minus_1 = %u, current_frame_id = %u, "
		  "buffer_removal_time = %s, order_hints = %s, reference_frame_ts = %s, "
		  "ref_frame_idx = %s, refresh_frame_flags = %u",
		  __entry->tgid, __entry->fd,
		  __print_flags(__entry->f.tile_info.flags, "|",
		  {V4L2_AV1_TILE_INFO_FLAG_UNIFORM_TILE_SPACING, "UNIFORM_TILE_SPACING"}),
		  __entry->f.tile_info.context_update_tile_id,
		  __entry->f.tile_info.tile_cols,
		  __entry->f.tile_info.tile_rows,
		  __print_array(__entry->f.tile_info.mi_col_starts,
				ARRAY_SIZE(__entry->f.tile_info.mi_col_starts),
				sizeof(__entry->f.tile_info.mi_col_starts[0])),
		  __print_array(__entry->f.tile_info.mi_row_starts,
				ARRAY_SIZE(__entry->f.tile_info.mi_row_starts),
				sizeof(__entry->f.tile_info.mi_row_starts[0])),
		  __print_array(__entry->f.tile_info.width_in_sbs_minus_1,
				ARRAY_SIZE(__entry->f.tile_info.width_in_sbs_minus_1),
				sizeof(__entry->f.tile_info.width_in_sbs_minus_1[0])),
		  __print_array(__entry->f.tile_info.height_in_sbs_minus_1,
				ARRAY_SIZE(__entry->f.tile_info.height_in_sbs_minus_1),
				sizeof(__entry->f.tile_info.height_in_sbs_minus_1[0])),
		  __entry->f.tile_info.tile_size_bytes,
		  __print_flags(__entry->f.quantization.flags, "|",
		  {V4L2_AV1_QUANTIZATION_FLAG_DIFF_UV_DELTA, "DIFF_UV_DELTA"},
		  {V4L2_AV1_QUANTIZATION_FLAG_USING_QMATRIX, "USING_QMATRIX"},
		  {V4L2_AV1_QUANTIZATION_FLAG_DELTA_Q_PRESENT, "DELTA_Q_PRESENT"}),
		  __entry->f.quantization.base_q_idx,
		  __entry->f.quantization.delta_q_y_dc,
		  __entry->f.quantization.delta_q_u_dc,
		  __entry->f.quantization.delta_q_u_ac,
		  __entry->f.quantization.delta_q_v_dc,
		  __entry->f.quantization.delta_q_v_ac,
		  __entry->f.quantization.qm_y,
		  __entry->f.quantization.qm_u,
		  __entry->f.quantization.qm_v,
		  __entry->f.quantization.delta_q_res,
		  __entry->f.superres_denom,
		  __print_flags(__entry->f.segmentation.flags, "|",
		  {V4L2_AV1_SEGMENTATION_FLAG_ENABLED, "ENABLED"},
		  {V4L2_AV1_SEGMENTATION_FLAG_UPDATE_MAP, "UPDATE_MAP"},
		  {V4L2_AV1_SEGMENTATION_FLAG_TEMPORAL_UPDATE, "TEMPORAL_UPDATE"},
		  {V4L2_AV1_SEGMENTATION_FLAG_UPDATE_DATA, "UPDATE_DATA"},
		  {V4L2_AV1_SEGMENTATION_FLAG_SEG_ID_PRE_SKIP, "SEG_ID_PRE_SKIP"}),
		  __entry->f.segmentation.last_active_seg_id,
		  __print_array(__entry->f.segmentation.feature_enabled,
				ARRAY_SIZE(__entry->f.segmentation.feature_enabled),
				sizeof(__entry->f.segmentation.feature_enabled[0])),
		  __print_flags(__entry->f.loop_filter.flags, "|",
		  {V4L2_AV1_LOOP_FILTER_FLAG_DELTA_ENABLED, "DELTA_ENABLED"},
		  {V4L2_AV1_LOOP_FILTER_FLAG_DELTA_UPDATE, "DELTA_UPDATE"},
		  {V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_PRESENT, "DELTA_LF_PRESENT"},
		  {V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_MULTI, "DELTA_LF_MULTI"}),
		  __print_array(__entry->f.loop_filter.level,
				ARRAY_SIZE(__entry->f.loop_filter.level),
				sizeof(__entry->f.loop_filter.level[0])),
		  __entry->f.loop_filter.sharpness,
		  __print_array(__entry->f.loop_filter.ref_deltas,
				ARRAY_SIZE(__entry->f.loop_filter.ref_deltas),
				sizeof(__entry->f.loop_filter.ref_deltas[0])),
		  __print_array(__entry->f.loop_filter.mode_deltas,
				ARRAY_SIZE(__entry->f.loop_filter.mode_deltas),
				sizeof(__entry->f.loop_filter.mode_deltas[0])),
		  __entry->f.loop_filter.delta_lf_res,
		  __entry->f.cdef.damping_minus_3,
		  __entry->f.cdef.bits,
		  __print_array(__entry->f.cdef.y_pri_strength,
				ARRAY_SIZE(__entry->f.cdef.y_pri_strength),
				sizeof(__entry->f.cdef.y_pri_strength[0])),
		  __print_array(__entry->f.cdef.y_sec_strength,
				ARRAY_SIZE(__entry->f.cdef.y_sec_strength),
				sizeof(__entry->f.cdef.y_sec_strength[0])),
		  __print_array(__entry->f.cdef.uv_pri_strength,
				ARRAY_SIZE(__entry->f.cdef.uv_pri_strength),
				sizeof(__entry->f.cdef.uv_pri_strength[0])),
		  __print_array(__entry->f.cdef.uv_sec_strength,
				ARRAY_SIZE(__entry->f.cdef.uv_sec_strength),
				sizeof(__entry->f.cdef.uv_sec_strength[0])),
		  __print_array(__entry->f.skip_mode_frame,
				ARRAY_SIZE(__entry->f.skip_mode_frame),
				sizeof(__entry->f.skip_mode_frame[0])),
		  __entry->f.primary_ref_frame,
		  __print_flags(__entry->f.loop_restoration.flags, "|",
		  {V4L2_AV1_LOOP_RESTORATION_FLAG_USES_LR, "USES_LR"},
		  {V4L2_AV1_LOOP_RESTORATION_FLAG_USES_CHROMA_LR, "USES_CHROMA_LR"}),
		  __entry->f.loop_restoration.lr_unit_shift,
		  __entry->f.loop_restoration.lr_uv_shift,
		  __print_array(__entry->f.loop_restoration.frame_restoration_type,
				ARRAY_SIZE(__entry->f.loop_restoration.frame_restoration_type),
				sizeof(__entry->f.loop_restoration.frame_restoration_type[0])),
		  __print_array(__entry->f.loop_restoration.loop_restoration_size,
				ARRAY_SIZE(__entry->f.loop_restoration.loop_restoration_size),
				sizeof(__entry->f.loop_restoration.loop_restoration_size[0])),
		  __print_flags(__entry->f.flags, "|",
		  {V4L2_AV1_FRAME_FLAG_SHOW_FRAME, "SHOW_FRAME"},
		  {V4L2_AV1_FRAME_FLAG_SHOWABLE_FRAME, "SHOWABLE_FRAME"},
		  {V4L2_AV1_FRAME_FLAG_ERROR_RESILIENT_MODE, "ERROR_RESILIENT_MODE"},
		  {V4L2_AV1_FRAME_FLAG_DISABLE_CDF_UPDATE, "DISABLE_CDF_UPDATE"},
		  {V4L2_AV1_FRAME_FLAG_ALLOW_SCREEN_CONTENT_TOOLS, "ALLOW_SCREEN_CONTENT_TOOLS"},
		  {V4L2_AV1_FRAME_FLAG_FORCE_INTEGER_MV, "FORCE_INTEGER_MV"},
		  {V4L2_AV1_FRAME_FLAG_ALLOW_INTRABC, "ALLOW_INTRABC"},
		  {V4L2_AV1_FRAME_FLAG_USE_SUPERRES, "USE_SUPERRES"},
		  {V4L2_AV1_FRAME_FLAG_ALLOW_HIGH_PRECISION_MV, "ALLOW_HIGH_PRECISION_MV"},
		  {V4L2_AV1_FRAME_FLAG_IS_MOTION_MODE_SWITCHABLE, "IS_MOTION_MODE_SWITCHABLE"},
		  {V4L2_AV1_FRAME_FLAG_USE_REF_FRAME_MVS, "USE_REF_FRAME_MVS"},
		  {V4L2_AV1_FRAME_FLAG_DISABLE_FRAME_END_UPDATE_CDF,
		   "DISABLE_FRAME_END_UPDATE_CDF"},
		  {V4L2_AV1_FRAME_FLAG_ALLOW_WARPED_MOTION, "ALLOW_WARPED_MOTION"},
		  {V4L2_AV1_FRAME_FLAG_REFERENCE_SELECT, "REFERENCE_SELECT"},
		  {V4L2_AV1_FRAME_FLAG_REDUCED_TX_SET, "REDUCED_TX_SET"},
		  {V4L2_AV1_FRAME_FLAG_SKIP_MODE_ALLOWED, "SKIP_MODE_ALLOWED"},
		  {V4L2_AV1_FRAME_FLAG_SKIP_MODE_PRESENT, "SKIP_MODE_PRESENT"},
		  {V4L2_AV1_FRAME_FLAG_FRAME_SIZE_OVERRIDE, "FRAME_SIZE_OVERRIDE"},
		  {V4L2_AV1_FRAME_FLAG_BUFFER_REMOVAL_TIME_PRESENT, "BUFFER_REMOVAL_TIME_PRESENT"},
		  {V4L2_AV1_FRAME_FLAG_FRAME_REFS_SHORT_SIGNALING, "FRAME_REFS_SHORT_SIGNALING"}),
		  __entry->f.order_hint,
		  __entry->f.upscaled_width,
		  __entry->f.frame_width_minus_1,
		  __entry->f.frame_height_minus_1,
		  __entry->f.render_width_minus_1,
		  __entry->f.render_height_minus_1,
		  __entry->f.current_frame_id,
		  __print_array(__entry->f.buffer_removal_time,
				ARRAY_SIZE(__entry->f.buffer_removal_time),
				sizeof(__entry->f.buffer_removal_time[0])),
		  __print_array(__entry->f.order_hints,
				ARRAY_SIZE(__entry->f.order_hints),
				sizeof(__entry->f.order_hints[0])),
		  __print_array(__entry->f.reference_frame_ts,
				ARRAY_SIZE(__entry->f.reference_frame_ts),
				sizeof(__entry->f.reference_frame_ts[0])),
		  __print_array(__entry->f.ref_frame_idx,
				ARRAY_SIZE(__entry->f.ref_frame_idx),
				sizeof(__entry->f.ref_frame_idx[0])),
		  __entry->f.refresh_frame_flags
	)
);


DECLARE_EVENT_CLASS(v4l2_ctrl_av1_film_grain_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_av1_film_grain *f),
	TP_ARGS(tgid, fd, f),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_av1_film_grain, f)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->f = *f;),
	TP_printk("tgid = %u, fd = %u, "
		  "flags = %s, cr_mult = %u, grain_seed = %u, "
		  "film_grain_params_ref_idx = %u, num_y_points = %u, point_y_value = %s, "
		  "point_y_scaling = %s, num_cb_points = %u, point_cb_value = %s, "
		  "point_cb_scaling = %s, num_cr_points = %u, point_cr_value = %s, "
		  "point_cr_scaling = %s, grain_scaling_minus_8 = %u, ar_coeff_lag = %u, "
		  "ar_coeffs_y_plus_128 = %s, ar_coeffs_cb_plus_128 = %s, "
		  "ar_coeffs_cr_plus_128 = %s, ar_coeff_shift_minus_6 = %u, "
		  "grain_scale_shift = %u, cb_mult = %u, cb_luma_mult = %u, cr_luma_mult = %u, "
		  "cb_offset = %u, cr_offset = %u",
		  __entry->tgid, __entry->fd,
		  __print_flags(__entry->f.flags, "|",
		  {V4L2_AV1_FILM_GRAIN_FLAG_APPLY_GRAIN, "APPLY_GRAIN"},
		  {V4L2_AV1_FILM_GRAIN_FLAG_UPDATE_GRAIN, "UPDATE_GRAIN"},
		  {V4L2_AV1_FILM_GRAIN_FLAG_CHROMA_SCALING_FROM_LUMA, "CHROMA_SCALING_FROM_LUMA"},
		  {V4L2_AV1_FILM_GRAIN_FLAG_OVERLAP, "OVERLAP"},
		  {V4L2_AV1_FILM_GRAIN_FLAG_CLIP_TO_RESTRICTED_RANGE, "CLIP_TO_RESTRICTED_RANGE"}),
		  __entry->f.cr_mult,
		  __entry->f.grain_seed,
		  __entry->f.film_grain_params_ref_idx,
		  __entry->f.num_y_points,
		  __print_array(__entry->f.point_y_value,
				ARRAY_SIZE(__entry->f.point_y_value),
				sizeof(__entry->f.point_y_value[0])),
		  __print_array(__entry->f.point_y_scaling,
				ARRAY_SIZE(__entry->f.point_y_scaling),
				sizeof(__entry->f.point_y_scaling[0])),
		  __entry->f.num_cb_points,
		  __print_array(__entry->f.point_cb_value,
				ARRAY_SIZE(__entry->f.point_cb_value),
				sizeof(__entry->f.point_cb_value[0])),
		  __print_array(__entry->f.point_cb_scaling,
				ARRAY_SIZE(__entry->f.point_cb_scaling),
				sizeof(__entry->f.point_cb_scaling[0])),
		  __entry->f.num_cr_points,
		  __print_array(__entry->f.point_cr_value,
				ARRAY_SIZE(__entry->f.point_cr_value),
				sizeof(__entry->f.point_cr_value[0])),
		  __print_array(__entry->f.point_cr_scaling,
				ARRAY_SIZE(__entry->f.point_cr_scaling),
				sizeof(__entry->f.point_cr_scaling[0])),
		  __entry->f.grain_scaling_minus_8,
		  __entry->f.ar_coeff_lag,
		  __print_array(__entry->f.ar_coeffs_y_plus_128,
				ARRAY_SIZE(__entry->f.ar_coeffs_y_plus_128),
				sizeof(__entry->f.ar_coeffs_y_plus_128[0])),
		  __print_array(__entry->f.ar_coeffs_cb_plus_128,
				ARRAY_SIZE(__entry->f.ar_coeffs_cb_plus_128),
				sizeof(__entry->f.ar_coeffs_cb_plus_128[0])),
		  __print_array(__entry->f.ar_coeffs_cr_plus_128,
				ARRAY_SIZE(__entry->f.ar_coeffs_cr_plus_128),
				sizeof(__entry->f.ar_coeffs_cr_plus_128[0])),
		  __entry->f.ar_coeff_shift_minus_6,
		  __entry->f.grain_scale_shift,
		  __entry->f.cb_mult,
		  __entry->f.cb_luma_mult,
		  __entry->f.cr_luma_mult,
		  __entry->f.cb_offset,
		  __entry->f.cr_offset
	)
)

DEFINE_EVENT(v4l2_ctrl_av1_seq_tmpl, v4l2_ctrl_av1_sequence,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_av1_sequence *s),
	TP_ARGS(tgid, fd, s)
);

DEFINE_EVENT(v4l2_ctrl_av1_frame_tmpl, v4l2_ctrl_av1_frame,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_av1_frame *f),
	TP_ARGS(tgid, fd, f)
);

DEFINE_EVENT(v4l2_ctrl_av1_tge_tmpl, v4l2_ctrl_av1_tile_group_entry,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_av1_tile_group_entry *t),
	TP_ARGS(tgid, fd, t)
);

DEFINE_EVENT(v4l2_ctrl_av1_film_grain_tmpl, v4l2_ctrl_av1_film_grain,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_av1_film_grain *f),
	TP_ARGS(tgid, fd, f)
);

/* FWHT controls */

DECLARE_EVENT_CLASS(v4l2_ctrl_fwht_params_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_fwht_params *p),
	TP_ARGS(tgid, fd, p),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field(u64, backward_ref_ts)
			 __field(u32, version)
			 __field(u32, width)
			 __field(u32, height)
			 __field(u32, flags)
			 __field(u32, colorspace)
			 __field(u32, xfer_func)
			 __field(u32, ycbcr_enc)
			 __field(u32, quantization)
			 ),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->backward_ref_ts = p->backward_ref_ts;
		       __entry->version = p->version;
		       __entry->width = p->width;
		       __entry->height = p->height;
		       __entry->flags = p->flags;
		       __entry->colorspace = p->colorspace;
		       __entry->xfer_func = p->xfer_func;
		       __entry->ycbcr_enc = p->ycbcr_enc;
		       __entry->quantization = p->quantization;
		       ),
	TP_printk("tgid = %u, fd = %u, "
		  "backward_ref_ts = %llu, "
		  "version = %u, "
		  "width = %u, "
		  "height = %u, "
		  "flags = %s, "
		  "colorspace = %u, "
		  "xfer_func = %u, "
		  "ycbcr_enc = %u, "
		  "quantization = %u",
		  __entry->tgid, __entry->fd,
		  __entry->backward_ref_ts, __entry->version, __entry->width, __entry->height,
		  __print_flags(__entry->flags, "|",
		  {V4L2_FWHT_FL_IS_INTERLACED, "IS_INTERLACED"},
		  {V4L2_FWHT_FL_IS_BOTTOM_FIRST, "IS_BOTTOM_FIRST"},
		  {V4L2_FWHT_FL_IS_ALTERNATE, "IS_ALTERNATE"},
		  {V4L2_FWHT_FL_IS_BOTTOM_FIELD, "IS_BOTTOM_FIELD"},
		  {V4L2_FWHT_FL_LUMA_IS_UNCOMPRESSED, "LUMA_IS_UNCOMPRESSED"},
		  {V4L2_FWHT_FL_CB_IS_UNCOMPRESSED, "CB_IS_UNCOMPRESSED"},
		  {V4L2_FWHT_FL_CR_IS_UNCOMPRESSED, "CR_IS_UNCOMPRESSED"},
		  {V4L2_FWHT_FL_ALPHA_IS_UNCOMPRESSED, "ALPHA_IS_UNCOMPRESSED"},
		  {V4L2_FWHT_FL_I_FRAME, "I_FRAME"},
		  {V4L2_FWHT_FL_PIXENC_HSV, "PIXENC_HSV"},
		  {V4L2_FWHT_FL_PIXENC_RGB, "PIXENC_RGB"},
		  {V4L2_FWHT_FL_PIXENC_YUV, "PIXENC_YUV"}),
		  __entry->colorspace, __entry->xfer_func, __entry->ycbcr_enc,
		  __entry->quantization)
);

DEFINE_EVENT(v4l2_ctrl_fwht_params_tmpl, v4l2_ctrl_fwht_params,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_fwht_params *p),
	TP_ARGS(tgid, fd, p)
);

/* H264 controls */

DECLARE_EVENT_CLASS(v4l2_ctrl_h264_sps_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_h264_sps *s),
	TP_ARGS(tgid, fd, s),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_h264_sps, s)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->s = *s),
	TP_printk("tgid = %u, fd = %u, "
		  "profile_idc = %u, "
		  "constraint_set_flags = %s, "
		  "level_idc = %u, "
		  "seq_parameter_set_id = %u, "
		  "chroma_format_idc = %u, "
		  "bit_depth_luma_minus8 = %u, "
		  "bit_depth_chroma_minus8 = %u, "
		  "log2_max_frame_num_minus4 = %u, "
		  "pic_order_cnt_type = %u, "
		  "log2_max_pic_order_cnt_lsb_minus4 = %u, "
		  "max_num_ref_frames = %u, "
		  "num_ref_frames_in_pic_order_cnt_cycle = %u, "
		  "offset_for_ref_frame = %s, "
		  "offset_for_non_ref_pic = %d, "
		  "offset_for_top_to_bottom_field = %d, "
		  "pic_width_in_mbs_minus1 = %u, "
		  "pic_height_in_map_units_minus1 = %u, "
		  "flags = %s",
		  __entry->tgid, __entry->fd,
		  __entry->s.profile_idc,
		  __print_flags(__entry->s.constraint_set_flags, "|",
		  {V4L2_H264_SPS_CONSTRAINT_SET0_FLAG, "CONSTRAINT_SET0_FLAG"},
		  {V4L2_H264_SPS_CONSTRAINT_SET1_FLAG, "CONSTRAINT_SET1_FLAG"},
		  {V4L2_H264_SPS_CONSTRAINT_SET2_FLAG, "CONSTRAINT_SET2_FLAG"},
		  {V4L2_H264_SPS_CONSTRAINT_SET3_FLAG, "CONSTRAINT_SET3_FLAG"},
		  {V4L2_H264_SPS_CONSTRAINT_SET4_FLAG, "CONSTRAINT_SET4_FLAG"},
		  {V4L2_H264_SPS_CONSTRAINT_SET5_FLAG, "CONSTRAINT_SET5_FLAG"}),
		  __entry->s.level_idc,
		  __entry->s.seq_parameter_set_id,
		  __entry->s.chroma_format_idc,
		  __entry->s.bit_depth_luma_minus8,
		  __entry->s.bit_depth_chroma_minus8,
		  __entry->s.log2_max_frame_num_minus4,
		  __entry->s.pic_order_cnt_type,
		  __entry->s.log2_max_pic_order_cnt_lsb_minus4,
		  __entry->s.max_num_ref_frames,
		  __entry->s.num_ref_frames_in_pic_order_cnt_cycle,
		  __print_array(__entry->s.offset_for_ref_frame,
				ARRAY_SIZE(__entry->s.offset_for_ref_frame),
				sizeof(__entry->s.offset_for_ref_frame[0])),
		  __entry->s.offset_for_non_ref_pic,
		  __entry->s.offset_for_top_to_bottom_field,
		  __entry->s.pic_width_in_mbs_minus1,
		  __entry->s.pic_height_in_map_units_minus1,
		  __print_flags(__entry->s.flags, "|",
		  {V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE, "SEPARATE_COLOUR_PLANE"},
		  {V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS, "QPPRIME_Y_ZERO_TRANSFORM_BYPASS"},
		  {V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO, "DELTA_PIC_ORDER_ALWAYS_ZERO"},
		  {V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED, "GAPS_IN_FRAME_NUM_VALUE_ALLOWED"},
		  {V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY, "FRAME_MBS_ONLY"},
		  {V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD, "MB_ADAPTIVE_FRAME_FIELD"},
		  {V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE, "DIRECT_8X8_INFERENCE"}
		  ))
);

DECLARE_EVENT_CLASS(v4l2_ctrl_h264_pps_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_h264_pps *p),
	TP_ARGS(tgid, fd, p),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_h264_pps, p)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->p = *p),
	TP_printk("tgid = %u, fd = %u, "
		  "pic_parameter_set_id = %u, "
		  "seq_parameter_set_id = %u, "
		  "num_slice_groups_minus1 = %u, "
		  "num_ref_idx_l0_default_active_minus1 = %u, "
		  "num_ref_idx_l1_default_active_minus1 = %u, "
		  "weighted_bipred_idc = %u, "
		  "pic_init_qp_minus26 = %d, "
		  "pic_init_qs_minus26 = %d, "
		  "chroma_qp_index_offset = %d, "
		  "second_chroma_qp_index_offset = %d, "
		  "flags = %s",
		  __entry->tgid, __entry->fd,
		  __entry->p.pic_parameter_set_id,
		  __entry->p.seq_parameter_set_id,
		  __entry->p.num_slice_groups_minus1,
		  __entry->p.num_ref_idx_l0_default_active_minus1,
		  __entry->p.num_ref_idx_l1_default_active_minus1,
		  __entry->p.weighted_bipred_idc,
		  __entry->p.pic_init_qp_minus26,
		  __entry->p.pic_init_qs_minus26,
		  __entry->p.chroma_qp_index_offset,
		  __entry->p.second_chroma_qp_index_offset,
		  __print_flags(__entry->p.flags, "|",
		  {V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE, "ENTROPY_CODING_MODE"},
		  {V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT, "BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT"},
		  {V4L2_H264_PPS_FLAG_WEIGHTED_PRED, "WEIGHTED_PRED"},
		  {V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT, "DEBLOCKING_FILTER_CONTROL_PRESENT"},
		  {V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED, "CONSTRAINED_INTRA_PRED"},
		  {V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT, "REDUNDANT_PIC_CNT_PRESENT"},
		  {V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE, "TRANSFORM_8X8_MODE"},
		  {V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT, "SCALING_MATRIX_PRESENT"}
		  ))
);

DECLARE_EVENT_CLASS(v4l2_ctrl_h264_scaling_matrix_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_h264_scaling_matrix *s),
	TP_ARGS(tgid, fd, s),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_h264_scaling_matrix, s)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->s = *s),
	TP_printk("tgid = %u, fd = %u, "
		  "scaling_list_4x4 = {%s}, scaling_list_8x8 = {%s}",
		  __entry->tgid, __entry->fd,
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->s.scaling_list_4x4,
				   sizeof(__entry->s.scaling_list_4x4),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->s.scaling_list_8x8,
				   sizeof(__entry->s.scaling_list_8x8),
				   false)
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_h264_pred_weights_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_h264_pred_weights *p),
	TP_ARGS(tgid, fd, p),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_h264_pred_weights, p)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->p = *p),
	TP_printk("tgid = %u, fd = %u, "
		  "luma_log2_weight_denom = %u, "
		  "chroma_log2_weight_denom = %u, "
		  "weight_factor[0].luma_weight = %s, "
		  "weight_factor[0].luma_offset = %s, "
		  "weight_factor[0].chroma_weight = {%s}, "
		  "weight_factor[0].chroma_offset = {%s}, "
		  "weight_factor[1].luma_weight = %s, "
		  "weight_factor[1].luma_offset = %s, "
		  "weight_factor[1].chroma_weight = {%s}, "
		  "weight_factor[1].chroma_offset = {%s}",
		  __entry->tgid, __entry->fd,
		  __entry->p.luma_log2_weight_denom,
		  __entry->p.chroma_log2_weight_denom,
		  __print_array(__entry->p.weight_factors[0].luma_weight,
				ARRAY_SIZE(__entry->p.weight_factors[0].luma_weight),
				sizeof(__entry->p.weight_factors[0].luma_weight[0])),
		  __print_array(__entry->p.weight_factors[0].luma_offset,
				ARRAY_SIZE(__entry->p.weight_factors[0].luma_offset),
				sizeof(__entry->p.weight_factors[0].luma_offset[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->p.weight_factors[0].chroma_weight,
				   sizeof(__entry->p.weight_factors[0].chroma_weight),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->p.weight_factors[0].chroma_offset,
				   sizeof(__entry->p.weight_factors[0].chroma_offset),
				   false),
		  __print_array(__entry->p.weight_factors[1].luma_weight,
				ARRAY_SIZE(__entry->p.weight_factors[1].luma_weight),
				sizeof(__entry->p.weight_factors[1].luma_weight[0])),
		  __print_array(__entry->p.weight_factors[1].luma_offset,
				ARRAY_SIZE(__entry->p.weight_factors[1].luma_offset),
				sizeof(__entry->p.weight_factors[1].luma_offset[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->p.weight_factors[1].chroma_weight,
				   sizeof(__entry->p.weight_factors[1].chroma_weight),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->p.weight_factors[1].chroma_offset,
				   sizeof(__entry->p.weight_factors[1].chroma_offset),
				   false)
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_h264_slice_params_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_h264_slice_params *s),
	TP_ARGS(tgid, fd, s),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_h264_slice_params, s)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->s = *s),
	TP_printk("tgid = %u, fd = %u, "
		  "header_bit_size = %u, "
		  "first_mb_in_slice = %u, "
		  "slice_type = %s, "
		  "colour_plane_id = %u, "
		  "redundant_pic_cnt = %u, "
		  "cabac_init_idc = %u, "
		  "slice_qp_delta = %d, "
		  "slice_qs_delta = %d, "
		  "disable_deblocking_filter_idc = %u, "
		  "slice_alpha_c0_offset_div2 = %u, "
		  "slice_beta_offset_div2 = %u, "
		  "num_ref_idx_l0_active_minus1 = %u, "
		  "num_ref_idx_l1_active_minus1 = %u, "
		  "flags = %s",
		  __entry->tgid, __entry->fd,
		  __entry->s.header_bit_size,
		  __entry->s.first_mb_in_slice,
		  __print_symbolic(__entry->s.slice_type,
		  {V4L2_H264_SLICE_TYPE_P, "P"},
		  {V4L2_H264_SLICE_TYPE_B, "B"},
		  {V4L2_H264_SLICE_TYPE_I, "I"},
		  {V4L2_H264_SLICE_TYPE_SP, "SP"},
		  {V4L2_H264_SLICE_TYPE_SI, "SI"}),
		  __entry->s.colour_plane_id,
		  __entry->s.redundant_pic_cnt,
		  __entry->s.cabac_init_idc,
		  __entry->s.slice_qp_delta,
		  __entry->s.slice_qs_delta,
		  __entry->s.disable_deblocking_filter_idc,
		  __entry->s.slice_alpha_c0_offset_div2,
		  __entry->s.slice_beta_offset_div2,
		  __entry->s.num_ref_idx_l0_active_minus1,
		  __entry->s.num_ref_idx_l1_active_minus1,
		  __print_flags(__entry->s.flags, "|",
		  {V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED, "DIRECT_SPATIAL_MV_PRED"},
		  {V4L2_H264_SLICE_FLAG_SP_FOR_SWITCH, "SP_FOR_SWITCH"})
	)
);

DECLARE_EVENT_CLASS(v4l2_h264_reference_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_h264_reference *r, int i),
	TP_ARGS(tgid, fd, r, i),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_h264_reference, r)
			 __field(int, i)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->r = *r; __entry->i = i;),
	TP_printk("tgid = %u, fd = %u, "
		  "[%d]: fields = %s, index = %u",
		  __entry->tgid, __entry->fd,
		  __entry->i,
		  __print_flags(__entry->r.fields, "|",
		  {V4L2_H264_TOP_FIELD_REF, "TOP_FIELD_REF"},
		  {V4L2_H264_BOTTOM_FIELD_REF, "BOTTOM_FIELD_REF"},
		  {V4L2_H264_FRAME_REF, "FRAME_REF"}),
		  __entry->r.index
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_h264_decode_params_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_h264_decode_params *d),
	TP_ARGS(tgid, fd, d),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_h264_decode_params, d)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->d = *d),
	TP_printk("tgid = %u, fd = %u, "
		  "nal_ref_idc = %u, "
		  "frame_num = %u, "
		  "top_field_order_cnt = %d, "
		  "bottom_field_order_cnt = %d, "
		  "idr_pic_id = %u, "
		  "pic_order_cnt_lsb = %u, "
		  "delta_pic_order_cnt_bottom = %d, "
		  "delta_pic_order_cnt0 = %d, "
		  "delta_pic_order_cnt1 = %d, "
		  "dec_ref_pic_marking_bit_size = %u, "
		  "pic_order_cnt_bit_size = %u, "
		  "slice_group_change_cycle = %u, "
		  "flags = %s",
		  __entry->tgid, __entry->fd,
		  __entry->d.nal_ref_idc,
		  __entry->d.frame_num,
		  __entry->d.top_field_order_cnt,
		  __entry->d.bottom_field_order_cnt,
		  __entry->d.idr_pic_id,
		  __entry->d.pic_order_cnt_lsb,
		  __entry->d.delta_pic_order_cnt_bottom,
		  __entry->d.delta_pic_order_cnt0,
		  __entry->d.delta_pic_order_cnt1,
		  __entry->d.dec_ref_pic_marking_bit_size,
		  __entry->d.pic_order_cnt_bit_size,
		  __entry->d.slice_group_change_cycle,
		  __print_flags(__entry->d.flags, "|",
		  {V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC, "IDR_PIC"},
		  {V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC, "FIELD_PIC"},
		  {V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD, "BOTTOM_FIELD"},
		  {V4L2_H264_DECODE_PARAM_FLAG_PFRAME, "PFRAME"},
		  {V4L2_H264_DECODE_PARAM_FLAG_BFRAME, "BFRAME"})
	)
);

DECLARE_EVENT_CLASS(v4l2_h264_dpb_entry_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_h264_dpb_entry *e, int i),
	TP_ARGS(tgid, fd, e, i),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_h264_dpb_entry, e)
			 __field(int, i)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->e = *e; __entry->i = i;),
	TP_printk("tgid = %u, fd = %u, "
		  "[%d]: reference_ts = %llu, pic_num = %u, frame_num = %u, fields = %s, "
		  "top_field_order_cnt = %d, bottom_field_order_cnt = %d, flags = %s",
		  __entry->tgid, __entry->fd,
		  __entry->i,
		  __entry->e.reference_ts,
		  __entry->e.pic_num,
		  __entry->e.frame_num,
		  __print_flags(__entry->e.fields, "|",
		  {V4L2_H264_TOP_FIELD_REF, "TOP_FIELD_REF"},
		  {V4L2_H264_BOTTOM_FIELD_REF, "BOTTOM_FIELD_REF"},
		  {V4L2_H264_FRAME_REF, "FRAME_REF"}),
		  __entry->e.top_field_order_cnt,
		  __entry->e.bottom_field_order_cnt,
		  __print_flags(__entry->e.flags, "|",
		  {V4L2_H264_DPB_ENTRY_FLAG_VALID, "VALID"},
		  {V4L2_H264_DPB_ENTRY_FLAG_ACTIVE, "ACTIVE"},
		  {V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM, "LONG_TERM"},
		  {V4L2_H264_DPB_ENTRY_FLAG_FIELD, "FIELD"})
	)
);

DEFINE_EVENT(v4l2_ctrl_h264_sps_tmpl, v4l2_ctrl_h264_sps,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_h264_sps *s),
	TP_ARGS(tgid, fd, s)
);

DEFINE_EVENT(v4l2_ctrl_h264_pps_tmpl, v4l2_ctrl_h264_pps,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_h264_pps *p),
	TP_ARGS(tgid, fd, p)
);

DEFINE_EVENT(v4l2_ctrl_h264_scaling_matrix_tmpl, v4l2_ctrl_h264_scaling_matrix,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_h264_scaling_matrix *s),
	TP_ARGS(tgid, fd, s)
);

DEFINE_EVENT(v4l2_ctrl_h264_pred_weights_tmpl, v4l2_ctrl_h264_pred_weights,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_h264_pred_weights *p),
	TP_ARGS(tgid, fd, p)
);

DEFINE_EVENT(v4l2_ctrl_h264_slice_params_tmpl, v4l2_ctrl_h264_slice_params,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_h264_slice_params *s),
	TP_ARGS(tgid, fd, s)
);

DEFINE_EVENT(v4l2_h264_reference_tmpl, v4l2_h264_ref_pic_list0,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_h264_reference *r, int i),
	TP_ARGS(tgid, fd, r, i)
);

DEFINE_EVENT(v4l2_h264_reference_tmpl, v4l2_h264_ref_pic_list1,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_h264_reference *r, int i),
	TP_ARGS(tgid, fd, r, i)
);

DEFINE_EVENT(v4l2_ctrl_h264_decode_params_tmpl, v4l2_ctrl_h264_decode_params,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_h264_decode_params *d),
	TP_ARGS(tgid, fd, d)
);

DEFINE_EVENT(v4l2_h264_dpb_entry_tmpl, v4l2_h264_dpb_entry,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_h264_dpb_entry *e, int i),
	TP_ARGS(tgid, fd, e, i)
);

/* HEVC controls */

DECLARE_EVENT_CLASS(v4l2_ctrl_hevc_sps_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_sps *s),
	TP_ARGS(tgid, fd, s),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_hevc_sps, s)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->s = *s),
	TP_printk("tgid = %u, fd = %u, "
		  "video_parameter_set_id = %u, "
		  "seq_parameter_set_id = %u, "
		  "pic_width_in_luma_samples = %u, "
		  "pic_height_in_luma_samples = %u, "
		  "bit_depth_luma_minus8 = %u, "
		  "bit_depth_chroma_minus8 = %u, "
		  "log2_max_pic_order_cnt_lsb_minus4 = %u, "
		  "sps_max_dec_pic_buffering_minus1 = %u, "
		  "sps_max_num_reorder_pics = %u, "
		  "sps_max_latency_increase_plus1 = %u, "
		  "log2_min_luma_coding_block_size_minus3 = %u, "
		  "log2_diff_max_min_luma_coding_block_size = %u, "
		  "log2_min_luma_transform_block_size_minus2 = %u, "
		  "log2_diff_max_min_luma_transform_block_size = %u, "
		  "max_transform_hierarchy_depth_inter = %u, "
		  "max_transform_hierarchy_depth_intra = %u, "
		  "pcm_sample_bit_depth_luma_minus1 = %u, "
		  "pcm_sample_bit_depth_chroma_minus1 = %u, "
		  "log2_min_pcm_luma_coding_block_size_minus3 = %u, "
		  "log2_diff_max_min_pcm_luma_coding_block_size = %u, "
		  "num_short_term_ref_pic_sets = %u, "
		  "num_long_term_ref_pics_sps = %u, "
		  "chroma_format_idc = %u, "
		  "sps_max_sub_layers_minus1 = %u, "
		  "flags = %s",
		  __entry->tgid, __entry->fd,
		  __entry->s.video_parameter_set_id,
		  __entry->s.seq_parameter_set_id,
		  __entry->s.pic_width_in_luma_samples,
		  __entry->s.pic_height_in_luma_samples,
		  __entry->s.bit_depth_luma_minus8,
		  __entry->s.bit_depth_chroma_minus8,
		  __entry->s.log2_max_pic_order_cnt_lsb_minus4,
		  __entry->s.sps_max_dec_pic_buffering_minus1,
		  __entry->s.sps_max_num_reorder_pics,
		  __entry->s.sps_max_latency_increase_plus1,
		  __entry->s.log2_min_luma_coding_block_size_minus3,
		  __entry->s.log2_diff_max_min_luma_coding_block_size,
		  __entry->s.log2_min_luma_transform_block_size_minus2,
		  __entry->s.log2_diff_max_min_luma_transform_block_size,
		  __entry->s.max_transform_hierarchy_depth_inter,
		  __entry->s.max_transform_hierarchy_depth_intra,
		  __entry->s.pcm_sample_bit_depth_luma_minus1,
		  __entry->s.pcm_sample_bit_depth_chroma_minus1,
		  __entry->s.log2_min_pcm_luma_coding_block_size_minus3,
		  __entry->s.log2_diff_max_min_pcm_luma_coding_block_size,
		  __entry->s.num_short_term_ref_pic_sets,
		  __entry->s.num_long_term_ref_pics_sps,
		  __entry->s.chroma_format_idc,
		  __entry->s.sps_max_sub_layers_minus1,
		  __print_flags(__entry->s.flags, "|",
		  {V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE, "SEPARATE_COLOUR_PLANE"},
		  {V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED, "SCALING_LIST_ENABLED"},
		  {V4L2_HEVC_SPS_FLAG_AMP_ENABLED, "AMP_ENABLED"},
		  {V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET, "SAMPLE_ADAPTIVE_OFFSET"},
		  {V4L2_HEVC_SPS_FLAG_PCM_ENABLED, "PCM_ENABLED"},
		  {V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED, "V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED"},
		  {V4L2_HEVC_SPS_FLAG_LONG_TERM_REF_PICS_PRESENT, "LONG_TERM_REF_PICS_PRESENT"},
		  {V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED, "TEMPORAL_MVP_ENABLED"},
		  {V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED, "STRONG_INTRA_SMOOTHING_ENABLED"}
	))

);


DECLARE_EVENT_CLASS(v4l2_ctrl_hevc_pps_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_pps *p),
	TP_ARGS(tgid, fd, p),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_hevc_pps, p)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->p = *p),
	TP_printk("tgid = %u, fd = %u, "
		  "pic_parameter_set_id = %u, "
		  "num_extra_slice_header_bits = %u, "
		  "num_ref_idx_l0_default_active_minus1 = %u, "
		  "num_ref_idx_l1_default_active_minus1 = %u, "
		  "init_qp_minus26 = %d, "
		  "diff_cu_qp_delta_depth = %u, "
		  "pps_cb_qp_offset = %d, "
		  "pps_cr_qp_offset = %d, "
		  "num_tile_columns_minus1 = %d, "
		  "num_tile_rows_minus1 = %d, "
		  "column_width_minus1 = %s, "
		  "row_height_minus1 = %s, "
		  "pps_beta_offset_div2 = %d, "
		  "pps_tc_offset_div2 = %d, "
		  "log2_parallel_merge_level_minus2 = %u, "
		  "flags = %s",
		  __entry->tgid, __entry->fd,
		  __entry->p.pic_parameter_set_id,
		  __entry->p.num_extra_slice_header_bits,
		  __entry->p.num_ref_idx_l0_default_active_minus1,
		  __entry->p.num_ref_idx_l1_default_active_minus1,
		  __entry->p.init_qp_minus26,
		  __entry->p.diff_cu_qp_delta_depth,
		  __entry->p.pps_cb_qp_offset,
		  __entry->p.pps_cr_qp_offset,
		  __entry->p.num_tile_columns_minus1,
		  __entry->p.num_tile_rows_minus1,
		  __print_array(__entry->p.column_width_minus1,
				ARRAY_SIZE(__entry->p.column_width_minus1),
				sizeof(__entry->p.column_width_minus1[0])),
		  __print_array(__entry->p.row_height_minus1,
				ARRAY_SIZE(__entry->p.row_height_minus1),
				sizeof(__entry->p.row_height_minus1[0])),
		  __entry->p.pps_beta_offset_div2,
		  __entry->p.pps_tc_offset_div2,
		  __entry->p.log2_parallel_merge_level_minus2,
		  __print_flags(__entry->p.flags, "|",
		  {V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED, "DEPENDENT_SLICE_SEGMENT_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT, "OUTPUT_FLAG_PRESENT"},
		  {V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED, "SIGN_DATA_HIDING_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT, "CABAC_INIT_PRESENT"},
		  {V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED, "CONSTRAINED_INTRA_PRED"},
		  {V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED, "CU_QP_DELTA_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT, "PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT"},
		  {V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED, "WEIGHTED_PRED"},
		  {V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED, "WEIGHTED_BIPRED"},
		  {V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED, "TRANSQUANT_BYPASS_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_TILES_ENABLED, "TILES_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED, "ENTROPY_CODING_SYNC_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED, "LOOP_FILTER_ACROSS_TILES_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED, "PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED, "DEBLOCKING_FILTER_OVERRIDE_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER, "DISABLE_DEBLOCKING_FILTER"},
		  {V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT, "LISTS_MODIFICATION_PRESENT"},
		  {V4L2_HEVC_PPS_FLAG_SLICE_SEGMENT_HEADER_EXTENSION_PRESENT, "SLICE_SEGMENT_HEADER_EXTENSION_PRESENT"},
		  {V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT, "DEBLOCKING_FILTER_CONTROL_PRESENT"},
		  {V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING, "UNIFORM_SPACING"}
	))

);



DECLARE_EVENT_CLASS(v4l2_ctrl_hevc_slice_params_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_slice_params *s),
	TP_ARGS(tgid, fd, s),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_hevc_slice_params, s)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->s = *s),
	TP_printk("tgid = %u, fd = %u, "
		  "bit_size = %u, "
		  "data_byte_offset = %u, "
		  "num_entry_point_offsets = %u, "
		  "nal_unit_type = %u, "
		  "nuh_temporal_id_plus1 = %u, "
		  "slice_type = %u, "
		  "colour_plane_id = %u, "
		  "slice_pic_order_cnt = %d, "
		  "num_ref_idx_l0_active_minus1 = %u, "
		  "num_ref_idx_l1_active_minus1 = %u, "
		  "collocated_ref_idx = %u, "
		  "five_minus_max_num_merge_cand = %u, "
		  "slice_qp_delta = %d, "
		  "slice_cb_qp_offset = %d, "
		  "slice_cr_qp_offset = %d, "
		  "slice_act_y_qp_offset = %d, "
		  "slice_act_cb_qp_offset = %d, "
		  "slice_act_cr_qp_offset = %d, "
		  "slice_beta_offset_div2 = %d, "
		  "slice_tc_offset_div2 = %d, "
		  "pic_struct = %u, "
		  "slice_segment_addr = %u, "
		  "ref_idx_l0 = %s, "
		  "ref_idx_l1 = %s, "
		  "short_term_ref_pic_set_size = %u, "
		  "long_term_ref_pic_set_size = %u, "
		  "flags = %s",
		  __entry->tgid, __entry->fd,
		  __entry->s.bit_size,
		  __entry->s.data_byte_offset,
		  __entry->s.num_entry_point_offsets,
		  __entry->s.nal_unit_type,
		  __entry->s.nuh_temporal_id_plus1,
		  __entry->s.slice_type,
		  __entry->s.colour_plane_id,
		  __entry->s.slice_pic_order_cnt,
		  __entry->s.num_ref_idx_l0_active_minus1,
		  __entry->s.num_ref_idx_l1_active_minus1,
		  __entry->s.collocated_ref_idx,
		  __entry->s.five_minus_max_num_merge_cand,
		  __entry->s.slice_qp_delta,
		  __entry->s.slice_cb_qp_offset,
		  __entry->s.slice_cr_qp_offset,
		  __entry->s.slice_act_y_qp_offset,
		  __entry->s.slice_act_cb_qp_offset,
		  __entry->s.slice_act_cr_qp_offset,
		  __entry->s.slice_beta_offset_div2,
		  __entry->s.slice_tc_offset_div2,
		  __entry->s.pic_struct,
		  __entry->s.slice_segment_addr,
		  __print_array(__entry->s.ref_idx_l0,
				ARRAY_SIZE(__entry->s.ref_idx_l0),
				sizeof(__entry->s.ref_idx_l0[0])),
		  __print_array(__entry->s.ref_idx_l1,
				ARRAY_SIZE(__entry->s.ref_idx_l1),
				sizeof(__entry->s.ref_idx_l1[0])),
		  __entry->s.short_term_ref_pic_set_size,
		  __entry->s.long_term_ref_pic_set_size,
		  __print_flags(__entry->s.flags, "|",
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA, "SLICE_SAO_LUMA"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA, "SLICE_SAO_CHROMA"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED, "SLICE_TEMPORAL_MVP_ENABLED"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO, "MVD_L1_ZERO"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT, "CABAC_INIT"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0, "COLLOCATED_FROM_L0"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_USE_INTEGER_MV, "USE_INTEGER_MV"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED, "SLICE_DEBLOCKING_FILTER_DISABLED"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED, "SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT, "DEPENDENT_SLICE_SEGMENT"}

	))
);

DECLARE_EVENT_CLASS(v4l2_hevc_pred_weight_table_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_hevc_pred_weight_table *p),
	TP_ARGS(tgid, fd, p),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_hevc_pred_weight_table, p)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->p = *p),
	TP_printk("tgid = %u, fd = %u, "
		  "delta_luma_weight_l0 = %s, "
		  "luma_offset_l0 = %s, "
		  "delta_chroma_weight_l0 = {%s}, "
		  "chroma_offset_l0 = {%s}, "
		  "delta_luma_weight_l1 = %s, "
		  "luma_offset_l1 = %s, "
		  "delta_chroma_weight_l1 = {%s}, "
		  "chroma_offset_l1 = {%s}, "
		  "luma_log2_weight_denom = %d, "
		  "delta_chroma_log2_weight_denom = %d",
		  __entry->tgid, __entry->fd,
		  __print_array(__entry->p.delta_luma_weight_l0,
				ARRAY_SIZE(__entry->p.delta_luma_weight_l0),
				sizeof(__entry->p.delta_luma_weight_l0[0])),
		  __print_array(__entry->p.luma_offset_l0,
				ARRAY_SIZE(__entry->p.luma_offset_l0),
				sizeof(__entry->p.luma_offset_l0[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->p.delta_chroma_weight_l0,
				   sizeof(__entry->p.delta_chroma_weight_l0),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->p.chroma_offset_l0,
				   sizeof(__entry->p.chroma_offset_l0),
				   false),
		  __print_array(__entry->p.delta_luma_weight_l1,
				ARRAY_SIZE(__entry->p.delta_luma_weight_l1),
				sizeof(__entry->p.delta_luma_weight_l1[0])),
		  __print_array(__entry->p.luma_offset_l1,
				ARRAY_SIZE(__entry->p.luma_offset_l1),
				sizeof(__entry->p.luma_offset_l1[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->p.delta_chroma_weight_l1,
				   sizeof(__entry->p.delta_chroma_weight_l1),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->p.chroma_offset_l1,
				   sizeof(__entry->p.chroma_offset_l1),
				   false),
		__entry->p.luma_log2_weight_denom,
		__entry->p.delta_chroma_log2_weight_denom

	))

DECLARE_EVENT_CLASS(v4l2_ctrl_hevc_scaling_matrix_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_scaling_matrix *s),
	TP_ARGS(tgid, fd, s),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_hevc_scaling_matrix, s)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->s = *s),
	TP_printk("tgid = %u, fd = %u, "
		  "scaling_list_4x4 = {%s}, "
		  "scaling_list_8x8 = {%s}, "
		  "scaling_list_16x16 = {%s}, "
		  "scaling_list_32x32 = {%s}, "
		  "scaling_list_dc_coef_16x16 = %s, "
		  "scaling_list_dc_coef_32x32 = %s",
		  __entry->tgid, __entry->fd,
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->s.scaling_list_4x4,
				   sizeof(__entry->s.scaling_list_4x4),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->s.scaling_list_8x8,
				   sizeof(__entry->s.scaling_list_8x8),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->s.scaling_list_16x16,
				   sizeof(__entry->s.scaling_list_16x16),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->s.scaling_list_32x32,
				   sizeof(__entry->s.scaling_list_32x32),
				   false),
		  __print_array(__entry->s.scaling_list_dc_coef_16x16,
				ARRAY_SIZE(__entry->s.scaling_list_dc_coef_16x16),
				sizeof(__entry->s.scaling_list_dc_coef_16x16[0])),
		  __print_array(__entry->s.scaling_list_dc_coef_32x32,
				ARRAY_SIZE(__entry->s.scaling_list_dc_coef_32x32),
				sizeof(__entry->s.scaling_list_dc_coef_32x32[0]))
	))

DECLARE_EVENT_CLASS(v4l2_ctrl_hevc_decode_params_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_decode_params *d),
	TP_ARGS(tgid, fd, d),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_hevc_decode_params, d)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->d = *d),
	TP_printk("tgid = %u, fd = %u, "
		  "pic_order_cnt_val = %d, "
		  "short_term_ref_pic_set_size = %u, "
		  "long_term_ref_pic_set_size = %u, "
		  "num_active_dpb_entries = %u, "
		  "num_poc_st_curr_before = %u, "
		  "num_poc_st_curr_after = %u, "
		  "num_poc_lt_curr = %u, "
		  "poc_st_curr_before = %s, "
		  "poc_st_curr_after = %s, "
		  "poc_lt_curr = %s, "
		  "flags = %s",
		  __entry->tgid, __entry->fd,
		  __entry->d.pic_order_cnt_val,
		  __entry->d.short_term_ref_pic_set_size,
		  __entry->d.long_term_ref_pic_set_size,
		  __entry->d.num_active_dpb_entries,
		  __entry->d.num_poc_st_curr_before,
		  __entry->d.num_poc_st_curr_after,
		  __entry->d.num_poc_lt_curr,
		  __print_array(__entry->d.poc_st_curr_before,
				ARRAY_SIZE(__entry->d.poc_st_curr_before),
				sizeof(__entry->d.poc_st_curr_before[0])),
		  __print_array(__entry->d.poc_st_curr_after,
				ARRAY_SIZE(__entry->d.poc_st_curr_after),
				sizeof(__entry->d.poc_st_curr_after[0])),
		  __print_array(__entry->d.poc_lt_curr,
				ARRAY_SIZE(__entry->d.poc_lt_curr),
				sizeof(__entry->d.poc_lt_curr[0])),
		  __print_flags(__entry->d.flags, "|",
		  {V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC, "IRAP_PIC"},
		  {V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC, "IDR_PIC"},
		  {V4L2_HEVC_DECODE_PARAM_FLAG_NO_OUTPUT_OF_PRIOR, "NO_OUTPUT_OF_PRIOR"}
	))
);

DECLARE_EVENT_CLASS(v4l2_ctrl_hevc_ext_sps_lt_rps_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_ext_sps_lt_rps *lt),
	TP_ARGS(tgid, fd, lt),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_hevc_ext_sps_lt_rps, lt)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->lt = *lt),
	TP_printk("tgid = %u, fd = %u, "
		  "flags = %s, "
		  "lt_ref_pic_poc_lsb_sps = %x",
		  __entry->tgid, __entry->fd,
		  __print_flags(__entry->lt.flags, "|",
		  {V4L2_HEVC_EXT_SPS_LT_RPS_FLAG_USED_LT, "USED_LT"}
		  ),
		  __entry->lt.lt_ref_pic_poc_lsb_sps
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_hevc_ext_sps_st_rps_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_ext_sps_st_rps *st),
	TP_ARGS(tgid, fd, st),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_hevc_ext_sps_st_rps, st)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->st = *st),
	TP_printk("tgid = %u, fd = %u, "
		  "flags = %s, "
		  "delta_idx_minus1 = %u, "
		  "delta_rps_sign = %u, "
		  "abs_delta_rps_minus1 = %u, "
		  "num_negative_pics = %u, "
		  "num_positive_pics = %u, "
		  "used_by_curr_pic = %08x, "
		  "use_delta_flag = %08x, "
		  "delta_poc_s0_minus1 = %s, "
		  "delta_poc_s1_minus1 = %s",
		  __entry->tgid, __entry->fd,
		  __print_flags(__entry->st.flags, "|",
		  {V4L2_HEVC_EXT_SPS_ST_RPS_FLAG_INTER_REF_PIC_SET_PRED, "INTER_REF_PIC_SET_PRED"}
		  ),
		  __entry->st.delta_idx_minus1,
		  __entry->st.delta_rps_sign,
		  __entry->st.abs_delta_rps_minus1,
		  __entry->st.num_negative_pics,
		  __entry->st.num_positive_pics,
		  __entry->st.used_by_curr_pic,
		  __entry->st.use_delta_flag,
		  __print_array(__entry->st.delta_poc_s0_minus1,
				ARRAY_SIZE(__entry->st.delta_poc_s0_minus1),
				sizeof(__entry->st.delta_poc_s0_minus1[0])),
		  __print_array(__entry->st.delta_poc_s1_minus1,
				ARRAY_SIZE(__entry->st.delta_poc_s1_minus1),
				sizeof(__entry->st.delta_poc_s1_minus1[0]))
	)
);

DECLARE_EVENT_CLASS(v4l2_hevc_dpb_entry_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_hevc_dpb_entry *e),
	TP_ARGS(tgid, fd, e),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_hevc_dpb_entry, e)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->e = *e),
	TP_printk("tgid = %u, fd = %u, "
		  "timestamp = %llu, "
		  "flags = %s, "
		  "field_pic = %u, "
		  "pic_order_cnt_val = %d",
		  __entry->tgid, __entry->fd,
		__entry->e.timestamp,
		__print_flags(__entry->e.flags, "|",
		{V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE, "LONG_TERM_REFERENCE"}
		  ),
		__entry->e.field_pic,
		__entry->e.pic_order_cnt_val
	))

DEFINE_EVENT(v4l2_ctrl_hevc_sps_tmpl, v4l2_ctrl_hevc_sps,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_sps *s),
	TP_ARGS(tgid, fd, s)
);

DEFINE_EVENT(v4l2_ctrl_hevc_pps_tmpl, v4l2_ctrl_hevc_pps,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_pps *p),
	TP_ARGS(tgid, fd, p)
);

DEFINE_EVENT(v4l2_ctrl_hevc_slice_params_tmpl, v4l2_ctrl_hevc_slice_params,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_slice_params *s),
	TP_ARGS(tgid, fd, s)
);

DEFINE_EVENT(v4l2_hevc_pred_weight_table_tmpl, v4l2_hevc_pred_weight_table,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_hevc_pred_weight_table *p),
	TP_ARGS(tgid, fd, p)
);

DEFINE_EVENT(v4l2_ctrl_hevc_scaling_matrix_tmpl, v4l2_ctrl_hevc_scaling_matrix,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_scaling_matrix *s),
	TP_ARGS(tgid, fd, s)
);

DEFINE_EVENT(v4l2_ctrl_hevc_decode_params_tmpl, v4l2_ctrl_hevc_decode_params,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_decode_params *d),
	TP_ARGS(tgid, fd, d)
);

DEFINE_EVENT(v4l2_ctrl_hevc_ext_sps_lt_rps_tmpl, v4l2_ctrl_hevc_ext_sps_lt_rps,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_ext_sps_lt_rps *lt),
	TP_ARGS(tgid, fd, lt)
);

DEFINE_EVENT(v4l2_ctrl_hevc_ext_sps_st_rps_tmpl, v4l2_ctrl_hevc_ext_sps_st_rps,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_ext_sps_st_rps *st),
	TP_ARGS(tgid, fd, st)
);

DEFINE_EVENT(v4l2_hevc_dpb_entry_tmpl, v4l2_hevc_dpb_entry,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_hevc_dpb_entry *e),
	TP_ARGS(tgid, fd, e)
);

/* MPEG2 controls */

DECLARE_EVENT_CLASS(v4l2_ctrl_mpeg2_seq_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_mpeg2_sequence *s),
	TP_ARGS(tgid, fd, s),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_mpeg2_sequence, s)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->s = *s;),
	TP_printk("tgid = %u, fd = %u, "
		  "horizontal_size = %u, vertical_size = %u, vbv_buffer_size = %u, "
		  "profile_and_level_indication = %u, chroma_format = %u, flags = %s",
		  __entry->tgid, __entry->fd,
		  __entry->s.horizontal_size,
		  __entry->s.vertical_size,
		  __entry->s.vbv_buffer_size,
		  __entry->s.profile_and_level_indication,
		  __entry->s.chroma_format,
		  __print_flags(__entry->s.flags, "|",
		  {V4L2_MPEG2_SEQ_FLAG_PROGRESSIVE, "PROGRESSIVE"})
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_mpeg2_pic_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_mpeg2_picture *p),
	TP_ARGS(tgid, fd, p),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_mpeg2_picture, p)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->p = *p;),
	TP_printk("tgid = %u, fd = %u, "
		  "backward_ref_ts = %llu, forward_ref_ts = %llu, flags = %s, f_code = {%s}, "
		  "picture_coding_type = %u, picture_structure = %u, intra_dc_precision = %u",
		  __entry->tgid, __entry->fd,
		  __entry->p.backward_ref_ts,
		  __entry->p.forward_ref_ts,
		  __print_flags(__entry->p.flags, "|",
		  {V4L2_MPEG2_PIC_FLAG_TOP_FIELD_FIRST, "TOP_FIELD_FIRST"},
		  {V4L2_MPEG2_PIC_FLAG_FRAME_PRED_DCT, "FRAME_PRED_DCT"},
		  {V4L2_MPEG2_PIC_FLAG_CONCEALMENT_MV, "CONCEALMENT_MV"},
		  {V4L2_MPEG2_PIC_FLAG_Q_SCALE_TYPE, "Q_SCALE_TYPE"},
		  {V4L2_MPEG2_PIC_FLAG_INTRA_VLC, "INTA_VLC"},
		  {V4L2_MPEG2_PIC_FLAG_ALT_SCAN, "ALT_SCAN"},
		  {V4L2_MPEG2_PIC_FLAG_REPEAT_FIRST, "REPEAT_FIRST"},
		  {V4L2_MPEG2_PIC_FLAG_PROGRESSIVE, "PROGRESSIVE"}),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->p.f_code,
				   sizeof(__entry->p.f_code),
				   false),
		  __entry->p.picture_coding_type,
		  __entry->p.picture_structure,
		  __entry->p.intra_dc_precision
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_mpeg2_quant_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_mpeg2_quantisation *q),
	TP_ARGS(tgid, fd, q),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_mpeg2_quantisation, q)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->q = *q;),
	TP_printk("tgid = %u, fd = %u, "
		  "intra_quantiser_matrix = %s, non_intra_quantiser_matrix = %s, "
		  "chroma_intra_quantiser_matrix = %s, chroma_non_intra_quantiser_matrix = %s",
		  __entry->tgid, __entry->fd,
		  __print_array(__entry->q.intra_quantiser_matrix,
				ARRAY_SIZE(__entry->q.intra_quantiser_matrix),
				sizeof(__entry->q.intra_quantiser_matrix[0])),
		  __print_array(__entry->q.non_intra_quantiser_matrix,
				ARRAY_SIZE(__entry->q.non_intra_quantiser_matrix),
				sizeof(__entry->q.non_intra_quantiser_matrix[0])),
		  __print_array(__entry->q.chroma_intra_quantiser_matrix,
				ARRAY_SIZE(__entry->q.chroma_intra_quantiser_matrix),
				sizeof(__entry->q.chroma_intra_quantiser_matrix[0])),
		  __print_array(__entry->q.chroma_non_intra_quantiser_matrix,
				ARRAY_SIZE(__entry->q.chroma_non_intra_quantiser_matrix),
				sizeof(__entry->q.chroma_non_intra_quantiser_matrix[0]))
		  )
)

DEFINE_EVENT(v4l2_ctrl_mpeg2_seq_tmpl, v4l2_ctrl_mpeg2_sequence,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_mpeg2_sequence *s),
	TP_ARGS(tgid, fd, s)
);

DEFINE_EVENT(v4l2_ctrl_mpeg2_pic_tmpl, v4l2_ctrl_mpeg2_picture,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_mpeg2_picture *p),
	TP_ARGS(tgid, fd, p)
);

DEFINE_EVENT(v4l2_ctrl_mpeg2_quant_tmpl, v4l2_ctrl_mpeg2_quantisation,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_mpeg2_quantisation *q),
	TP_ARGS(tgid, fd, q)
);

/* VP8 controls */

DECLARE_EVENT_CLASS(v4l2_ctrl_vp8_entropy_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_vp8_frame *f),
	TP_ARGS(tgid, fd, f),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_vp8_frame, f)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->f = *f;),
	TP_printk("tgid = %u, fd = %u, "
		  "entropy.coeff_probs = {%s}, "
		  "entropy.y_mode_probs = %s, "
		  "entropy.uv_mode_probs = %s, "
		  "entropy.mv_probs = {%s}",
		  __entry->tgid, __entry->fd,
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->f.entropy.coeff_probs,
				   sizeof(__entry->f.entropy.coeff_probs),
				   false),
		  __print_array(__entry->f.entropy.y_mode_probs,
				ARRAY_SIZE(__entry->f.entropy.y_mode_probs),
				sizeof(__entry->f.entropy.y_mode_probs[0])),
		  __print_array(__entry->f.entropy.uv_mode_probs,
				ARRAY_SIZE(__entry->f.entropy.uv_mode_probs),
				sizeof(__entry->f.entropy.uv_mode_probs[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->f.entropy.mv_probs,
				   sizeof(__entry->f.entropy.mv_probs),
				   false)
		  )
)

DECLARE_EVENT_CLASS(v4l2_ctrl_vp8_frame_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_vp8_frame *f),
	TP_ARGS(tgid, fd, f),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_vp8_frame, f)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->f = *f;),
	TP_printk("tgid = %u, fd = %u, "
		  "segment.quant_update = %s, "
		  "segment.lf_update = %s, "
		  "segment.segment_probs = %s, "
		  "segment.flags = %s, "
		  "lf.ref_frm_delta = %s, "
		  "lf.mb_mode_delta = %s, "
		  "lf.sharpness_level = %u, "
		  "lf.level = %u, "
		  "lf.flags = %s, "
		  "quant.y_ac_qi = %u, "
		  "quant.y_dc_delta = %d, "
		  "quant.y2_dc_delta = %d, "
		  "quant.y2_ac_delta = %d, "
		  "quant.uv_dc_delta = %d, "
		  "quant.uv_ac_delta = %d, "
		  "coder_state.range = %u, "
		  "coder_state.value = %u, "
		  "coder_state.bit_count = %u, "
		  "width = %u, "
		  "height = %u, "
		  "horizontal_scale = %u, "
		  "vertical_scale = %u, "
		  "version = %u, "
		  "prob_skip_false = %u, "
		  "prob_intra = %u, "
		  "prob_last = %u, "
		  "prob_gf = %u, "
		  "num_dct_parts = %u, "
		  "first_part_size = %u, "
		  "first_part_header_bits = %u, "
		  "dct_part_sizes = %s, "
		  "last_frame_ts = %llu, "
		  "golden_frame_ts = %llu, "
		  "alt_frame_ts = %llu, "
		  "flags = %s",
		  __entry->tgid, __entry->fd,
		  __print_array(__entry->f.segment.quant_update,
				ARRAY_SIZE(__entry->f.segment.quant_update),
				sizeof(__entry->f.segment.quant_update[0])),
		  __print_array(__entry->f.segment.lf_update,
				ARRAY_SIZE(__entry->f.segment.lf_update),
				sizeof(__entry->f.segment.lf_update[0])),
		  __print_array(__entry->f.segment.segment_probs,
				ARRAY_SIZE(__entry->f.segment.segment_probs),
				sizeof(__entry->f.segment.segment_probs[0])),
		  __print_flags(__entry->f.segment.flags, "|",
		  {V4L2_VP8_SEGMENT_FLAG_ENABLED, "SEGMENT_ENABLED"},
		  {V4L2_VP8_SEGMENT_FLAG_UPDATE_MAP, "SEGMENT_UPDATE_MAP"},
		  {V4L2_VP8_SEGMENT_FLAG_UPDATE_FEATURE_DATA, "SEGMENT_UPDATE_FEATURE_DATA"},
		  {V4L2_VP8_SEGMENT_FLAG_DELTA_VALUE_MODE, "SEGMENT_DELTA_VALUE_MODE"}),
		  __print_array(__entry->f.lf.ref_frm_delta,
				ARRAY_SIZE(__entry->f.lf.ref_frm_delta),
				sizeof(__entry->f.lf.ref_frm_delta[0])),
		  __print_array(__entry->f.lf.mb_mode_delta,
				ARRAY_SIZE(__entry->f.lf.mb_mode_delta),
				sizeof(__entry->f.lf.mb_mode_delta[0])),
		  __entry->f.lf.sharpness_level,
		  __entry->f.lf.level,
		  __print_flags(__entry->f.lf.flags, "|",
		  {V4L2_VP8_LF_ADJ_ENABLE, "LF_ADJ_ENABLED"},
		  {V4L2_VP8_LF_DELTA_UPDATE, "LF_DELTA_UPDATE"},
		  {V4L2_VP8_LF_FILTER_TYPE_SIMPLE, "LF_FILTER_TYPE_SIMPLE"}),
		  __entry->f.quant.y_ac_qi,
		  __entry->f.quant.y_dc_delta,
		  __entry->f.quant.y2_dc_delta,
		  __entry->f.quant.y2_ac_delta,
		  __entry->f.quant.uv_dc_delta,
		  __entry->f.quant.uv_ac_delta,
		  __entry->f.coder_state.range,
		  __entry->f.coder_state.value,
		  __entry->f.coder_state.bit_count,
		  __entry->f.width,
		  __entry->f.height,
		  __entry->f.horizontal_scale,
		  __entry->f.vertical_scale,
		  __entry->f.version,
		  __entry->f.prob_skip_false,
		  __entry->f.prob_intra,
		  __entry->f.prob_last,
		  __entry->f.prob_gf,
		  __entry->f.num_dct_parts,
		  __entry->f.first_part_size,
		  __entry->f.first_part_header_bits,
		  __print_array(__entry->f.dct_part_sizes,
				ARRAY_SIZE(__entry->f.dct_part_sizes),
				sizeof(__entry->f.dct_part_sizes[0])),
		  __entry->f.last_frame_ts,
		  __entry->f.golden_frame_ts,
		  __entry->f.alt_frame_ts,
		  __print_flags(__entry->f.flags, "|",
		  {V4L2_VP8_FRAME_FLAG_KEY_FRAME, "KEY_FRAME"},
		  {V4L2_VP8_FRAME_FLAG_EXPERIMENTAL, "EXPERIMENTAL"},
		  {V4L2_VP8_FRAME_FLAG_SHOW_FRAME, "SHOW_FRAME"},
		  {V4L2_VP8_FRAME_FLAG_MB_NO_SKIP_COEFF, "MB_NO_SKIP_COEFF"},
		  {V4L2_VP8_FRAME_FLAG_SIGN_BIAS_GOLDEN, "SIGN_BIAS_GOLDEN"},
		  {V4L2_VP8_FRAME_FLAG_SIGN_BIAS_ALT, "SIGN_BIAS_ALT"})
		  )
);

DEFINE_EVENT(v4l2_ctrl_vp8_frame_tmpl, v4l2_ctrl_vp8_frame,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_vp8_frame *f),
	TP_ARGS(tgid, fd, f)
);

DEFINE_EVENT(v4l2_ctrl_vp8_entropy_tmpl, v4l2_ctrl_vp8_entropy,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_vp8_frame *f),
	TP_ARGS(tgid, fd, f)
);

/* VP9 controls */

DECLARE_EVENT_CLASS(v4l2_ctrl_vp9_frame_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_vp9_frame *f),
	TP_ARGS(tgid, fd, f),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_vp9_frame, f)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->f = *f;),
	TP_printk("tgid = %u, fd = %u, "
		  "lf.ref_deltas = %s, "
		  "lf.mode_deltas = %s, "
		  "lf.level = %u, "
		  "lf.sharpness = %u, "
		  "lf.flags = %s, "
		  "quant.base_q_idx = %u, "
		  "quant.delta_q_y_dc = %d, "
		  "quant.delta_q_uv_dc = %d, "
		  "quant.delta_q_uv_ac = %d, "
		  "seg.feature_data = {%s}, "
		  "seg.feature_enabled = %s, "
		  "seg.tree_probs = %s, "
		  "seg.pred_probs = %s, "
		  "seg.flags = %s, "
		  "flags = %s, "
		  "compressed_header_size = %u, "
		  "uncompressed_header_size = %u, "
		  "frame_width_minus_1 = %u, "
		  "frame_height_minus_1 = %u, "
		  "render_width_minus_1 = %u, "
		  "render_height_minus_1 = %u, "
		  "last_frame_ts = %llu, "
		  "golden_frame_ts = %llu, "
		  "alt_frame_ts = %llu, "
		  "ref_frame_sign_bias = %s, "
		  "reset_frame_context = %s, "
		  "frame_context_idx = %u, "
		  "profile = %u, "
		  "bit_depth = %u, "
		  "interpolation_filter = %s, "
		  "tile_cols_log2 = %u, "
		  "tile_rows_log_2 = %u, "
		  "reference_mode = %s",
		  __entry->tgid, __entry->fd,
		  __print_array(__entry->f.lf.ref_deltas,
				ARRAY_SIZE(__entry->f.lf.ref_deltas),
				sizeof(__entry->f.lf.ref_deltas[0])),
		  __print_array(__entry->f.lf.mode_deltas,
				ARRAY_SIZE(__entry->f.lf.mode_deltas),
				sizeof(__entry->f.lf.mode_deltas[0])),
		  __entry->f.lf.level,
		  __entry->f.lf.sharpness,
		  __print_flags(__entry->f.lf.flags, "|",
		  {V4L2_VP9_LOOP_FILTER_FLAG_DELTA_ENABLED, "DELTA_ENABLED"},
		  {V4L2_VP9_LOOP_FILTER_FLAG_DELTA_UPDATE, "DELTA_UPDATE"}),
		  __entry->f.quant.base_q_idx,
		  __entry->f.quant.delta_q_y_dc,
		  __entry->f.quant.delta_q_uv_dc,
		  __entry->f.quant.delta_q_uv_ac,
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->f.seg.feature_data,
				   sizeof(__entry->f.seg.feature_data),
				   false),
		  __print_array(__entry->f.seg.feature_enabled,
				ARRAY_SIZE(__entry->f.seg.feature_enabled),
				sizeof(__entry->f.seg.feature_enabled[0])),
		  __print_array(__entry->f.seg.tree_probs,
				ARRAY_SIZE(__entry->f.seg.tree_probs),
				sizeof(__entry->f.seg.tree_probs[0])),
		  __print_array(__entry->f.seg.pred_probs,
				ARRAY_SIZE(__entry->f.seg.pred_probs),
				sizeof(__entry->f.seg.pred_probs[0])),
		  __print_flags(__entry->f.seg.flags, "|",
		  {V4L2_VP9_SEGMENTATION_FLAG_ENABLED, "ENABLED"},
		  {V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP, "UPDATE_MAP"},
		  {V4L2_VP9_SEGMENTATION_FLAG_TEMPORAL_UPDATE, "TEMPORAL_UPDATE"},
		  {V4L2_VP9_SEGMENTATION_FLAG_UPDATE_DATA, "UPDATE_DATA"},
		  {V4L2_VP9_SEGMENTATION_FLAG_ABS_OR_DELTA_UPDATE, "ABS_OR_DELTA_UPDATE"}),
		  __print_flags(__entry->f.flags, "|",
		  {V4L2_VP9_FRAME_FLAG_KEY_FRAME, "KEY_FRAME"},
		  {V4L2_VP9_FRAME_FLAG_SHOW_FRAME, "SHOW_FRAME"},
		  {V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT, "ERROR_RESILIENT"},
		  {V4L2_VP9_FRAME_FLAG_INTRA_ONLY, "INTRA_ONLY"},
		  {V4L2_VP9_FRAME_FLAG_ALLOW_HIGH_PREC_MV, "ALLOW_HIGH_PREC_MV"},
		  {V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX, "REFRESH_FRAME_CTX"},
		  {V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE, "PARALLEL_DEC_MODE"},
		  {V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING, "X_SUBSAMPLING"},
		  {V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING, "Y_SUBSAMPLING"},
		  {V4L2_VP9_FRAME_FLAG_COLOR_RANGE_FULL_SWING, "COLOR_RANGE_FULL_SWING"}),
		  __entry->f.compressed_header_size,
		  __entry->f.uncompressed_header_size,
		  __entry->f.frame_width_minus_1,
		  __entry->f.frame_height_minus_1,
		  __entry->f.render_width_minus_1,
		  __entry->f.render_height_minus_1,
		  __entry->f.last_frame_ts,
		  __entry->f.golden_frame_ts,
		  __entry->f.alt_frame_ts,
		  __print_symbolic(__entry->f.ref_frame_sign_bias,
		  {V4L2_VP9_SIGN_BIAS_LAST, "SIGN_BIAS_LAST"},
		  {V4L2_VP9_SIGN_BIAS_GOLDEN, "SIGN_BIAS_GOLDEN"},
		  {V4L2_VP9_SIGN_BIAS_ALT, "SIGN_BIAS_ALT"}),
		  __print_symbolic(__entry->f.reset_frame_context,
		  {V4L2_VP9_RESET_FRAME_CTX_NONE, "RESET_FRAME_CTX_NONE"},
		  {V4L2_VP9_RESET_FRAME_CTX_SPEC, "RESET_FRAME_CTX_SPEC"},
		  {V4L2_VP9_RESET_FRAME_CTX_ALL, "RESET_FRAME_CTX_ALL"}),
		  __entry->f.frame_context_idx,
		  __entry->f.profile,
		  __entry->f.bit_depth,
		  __print_symbolic(__entry->f.interpolation_filter,
		  {V4L2_VP9_INTERP_FILTER_EIGHTTAP, "INTERP_FILTER_EIGHTTAP"},
		  {V4L2_VP9_INTERP_FILTER_EIGHTTAP_SMOOTH, "INTERP_FILTER_EIGHTTAP_SMOOTH"},
		  {V4L2_VP9_INTERP_FILTER_EIGHTTAP_SHARP, "INTERP_FILTER_EIGHTTAP_SHARP"},
		  {V4L2_VP9_INTERP_FILTER_BILINEAR, "INTERP_FILTER_BILINEAR"},
		  {V4L2_VP9_INTERP_FILTER_SWITCHABLE, "INTERP_FILTER_SWITCHABLE"}),
		  __entry->f.tile_cols_log2,
		  __entry->f.tile_rows_log2,
		  __print_symbolic(__entry->f.reference_mode,
		  {V4L2_VP9_REFERENCE_MODE_SINGLE_REFERENCE, "REFERENCE_MODE_SINGLE_REFERENCE"},
		  {V4L2_VP9_REFERENCE_MODE_COMPOUND_REFERENCE, "REFERENCE_MODE_COMPOUND_REFERENCE"},
		  {V4L2_VP9_REFERENCE_MODE_SELECT, "REFERENCE_MODE_SELECT"}))
);

DECLARE_EVENT_CLASS(v4l2_ctrl_vp9_compressed_hdr_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_vp9_compressed_hdr *h),
	TP_ARGS(tgid, fd, h),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_vp9_compressed_hdr, h)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->h = *h;),
	TP_printk("tgid = %u, fd = %u, "
		  "tx_mode = %s, "
		  "tx8 = {%s}, "
		  "tx16 = {%s}, "
		  "tx32 = {%s}, "
		  "skip = %s, "
		  "inter_mode = {%s}, "
		  "interp_filter = {%s}, "
		  "is_inter = %s, "
		  "comp_mode = %s, "
		  "single_ref = {%s}, "
		  "comp_ref = %s, "
		  "y_mode = {%s}, "
		  "uv_mode = {%s}, "
		  "partition = {%s}",
		  __entry->tgid, __entry->fd,
		  __print_symbolic(__entry->h.tx_mode,
		  {V4L2_VP9_TX_MODE_ONLY_4X4, "TX_MODE_ONLY_4X4"},
		  {V4L2_VP9_TX_MODE_ALLOW_8X8, "TX_MODE_ALLOW_8X8"},
		  {V4L2_VP9_TX_MODE_ALLOW_16X16, "TX_MODE_ALLOW_16X16"},
		  {V4L2_VP9_TX_MODE_ALLOW_32X32, "TX_MODE_ALLOW_32X32"},
		  {V4L2_VP9_TX_MODE_SELECT, "TX_MODE_SELECT"}),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->h.tx8,
				   sizeof(__entry->h.tx8),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->h.tx16,
				   sizeof(__entry->h.tx16),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->h.tx32,
				   sizeof(__entry->h.tx32),
				   false),
		  __print_array(__entry->h.skip,
				ARRAY_SIZE(__entry->h.skip),
				sizeof(__entry->h.skip[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->h.inter_mode,
				   sizeof(__entry->h.inter_mode),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->h.interp_filter,
				   sizeof(__entry->h.interp_filter),
				   false),
		  __print_array(__entry->h.is_inter,
				ARRAY_SIZE(__entry->h.is_inter),
				sizeof(__entry->h.is_inter[0])),
		  __print_array(__entry->h.comp_mode,
				ARRAY_SIZE(__entry->h.comp_mode),
				sizeof(__entry->h.comp_mode[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->h.single_ref,
				   sizeof(__entry->h.single_ref),
				   false),
		  __print_array(__entry->h.comp_ref,
				ARRAY_SIZE(__entry->h.comp_ref),
				sizeof(__entry->h.comp_ref[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->h.y_mode,
				   sizeof(__entry->h.y_mode),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->h.uv_mode,
				   sizeof(__entry->h.uv_mode),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->h.partition,
				   sizeof(__entry->h.partition),
				   false)
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_vp9_compressed_coef_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_vp9_compressed_hdr *h),
	TP_ARGS(tgid, fd, h),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_ctrl_vp9_compressed_hdr, h)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->h = *h;),
	TP_printk("tgid = %u, fd = %u, "
		  "coef = {%s}",
		  __entry->tgid, __entry->fd,
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->h.coef,
				   sizeof(__entry->h.coef),
				   false)
	)
);

DECLARE_EVENT_CLASS(v4l2_vp9_mv_probs_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_vp9_mv_probs *p),
	TP_ARGS(tgid, fd, p),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field_struct(struct v4l2_vp9_mv_probs, p)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->p = *p;),
	TP_printk("tgid = %u, fd = %u, "
		  "joint = %s, "
		  "sign = %s, "
		  "classes = {%s}, "
		  "class0_bit = %s, "
		  "bits = {%s}, "
		  "class0_fr = {%s}, "
		  "fr = {%s}, "
		  "class0_hp = %s, "
		  "hp = %s",
		  __entry->tgid, __entry->fd,
		  __print_array(__entry->p.joint,
				ARRAY_SIZE(__entry->p.joint),
				sizeof(__entry->p.joint[0])),
		  __print_array(__entry->p.sign,
				ARRAY_SIZE(__entry->p.sign),
				sizeof(__entry->p.sign[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->p.classes,
				   sizeof(__entry->p.classes),
				   false),
		  __print_array(__entry->p.class0_bit,
				ARRAY_SIZE(__entry->p.class0_bit),
				sizeof(__entry->p.class0_bit[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->p.bits,
				   sizeof(__entry->p.bits),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->p.class0_fr,
				   sizeof(__entry->p.class0_fr),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->p.fr,
				   sizeof(__entry->p.fr),
				   false),
		  __print_array(__entry->p.class0_hp,
				ARRAY_SIZE(__entry->p.class0_hp),
				sizeof(__entry->p.class0_hp[0])),
		  __print_array(__entry->p.hp,
				ARRAY_SIZE(__entry->p.hp),
				sizeof(__entry->p.hp[0]))
	)
);

DEFINE_EVENT(v4l2_ctrl_vp9_frame_tmpl, v4l2_ctrl_vp9_frame,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_vp9_frame *f),
	TP_ARGS(tgid, fd, f)
);

DEFINE_EVENT(v4l2_ctrl_vp9_compressed_hdr_tmpl, v4l2_ctrl_vp9_compressed_hdr,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_vp9_compressed_hdr *h),
	TP_ARGS(tgid, fd, h)
);

DEFINE_EVENT(v4l2_ctrl_vp9_compressed_coef_tmpl, v4l2_ctrl_vp9_compressed_coeff,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_vp9_compressed_hdr *h),
	TP_ARGS(tgid, fd, h)
);


DEFINE_EVENT(v4l2_vp9_mv_probs_tmpl, v4l2_vp9_mv_probs,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_vp9_mv_probs *p),
	TP_ARGS(tgid, fd, p)
);

#endif /* if !defined(_TRACE_V4L2_REQUESTS_H_) || defined(TRACE_HEADER_MULTI_READ) */

#include <trace/define_trace.h>
