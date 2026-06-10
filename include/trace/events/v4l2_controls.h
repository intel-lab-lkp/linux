/* SPDX-License-Identifier: GPL-2.0 */
#if !defined(_TRACE_V4L2_CONTROLS_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_V4L2_CONTROLS_H_

#include <linux/tracepoint.h>
#include <linux/v4l2-controls.h>

#undef TRACE_SYSTEM
#define TRACE_SYSTEM v4l2_controls

/* V4L2 controls tracing events.
 *
 * These events are used to trace each V4L2 control when they are set by userspace.
 * They can be identified by the name of the event. All control fields are copied in a TP_STRUCT
 * field so that they can be filtered separately in userspace.
 *
 * In addition to the controls fields, tgid and fd are also added in each trace events.
 * This allows to identify controls set by a specific process and to match them with other events
 * from the same process.
 * tgid contains the process id that opened the video device.
 * fd is the file descriptor in the tgid, used in case a process opens multiple video devices.
 *
 * Currently only the codec controls are supported.
 */

/* AV1 controls */
DECLARE_EVENT_CLASS(v4l2_ctrl_av1_seq_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_av1_sequence *s),
	TP_ARGS(tgid, fd, s),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field(__u32, flags)
			 __field(__u8, seq_profile)
			 __field(__u8, order_hint_bits)
			 __field(__u8, bit_depth)
			 __field(__u16, max_frame_width_minus_1)
			 __field(__u16, max_frame_height_minus_1)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->flags = s->flags;
		       __entry->seq_profile = s->seq_profile;
		       __entry->order_hint_bits = s->order_hint_bits;
		       __entry->bit_depth = s->bit_depth;
		       __entry->max_frame_width_minus_1 = s->max_frame_width_minus_1;
		       __entry->max_frame_height_minus_1 = s->max_frame_height_minus_1;),
	TP_printk("tgid = %u, fd = %u, "
		  "\nflags %s\nseq_profile: %u\norder_hint_bits: %u\nbit_depth: %u\n"
		  "max_frame_width_minus_1: %u\nmax_frame_height_minus_1: %u\n",
		  __entry->tgid, __entry->fd,
		  __print_flags(__entry->flags, "|",
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
		  __entry->seq_profile,
		  __entry->order_hint_bits,
		  __entry->bit_depth,
		  __entry->max_frame_width_minus_1,
		  __entry->max_frame_height_minus_1
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_av1_tge_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_av1_tile_group_entry *t),
	TP_ARGS(tgid, fd, t),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field(__u32, tile_offset)
			 __field(__u32, tile_size)
			 __field(__u32, tile_row)
			 __field(__u32, tile_col)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->tile_offset = t->tile_offset;
		       __entry->tile_size = t->tile_size;
		       __entry->tile_row = t->tile_row;
		       __entry->tile_col = t->tile_col;),
	TP_printk("tgid = %u, fd = %u, "
		  "\ntile_offset: %u\n tile_size: %u\n tile_row: %u\ntile_col: %u\n",
		  __entry->tgid, __entry->fd,
		  __entry->tile_offset,
		  __entry->tile_size,
		  __entry->tile_row,
		  __entry->tile_col
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_av1_frame_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_av1_frame *f),
	TP_ARGS(tgid, fd, f),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field(u8, tile_info_flags)
			 __field(u8, tile_info_context_update_tile_id)
			 __field(u8, tile_info_tile_cols)
			 __field(u8, tile_info_tile_rows)
			 __array(u32, tile_info_mi_col_starts, V4L2_AV1_MAX_TILE_COLS + 1)
			 __array(u32, tile_info_mi_row_starts, V4L2_AV1_MAX_TILE_ROWS + 1)
			 __array(u32, tile_info_width_in_sbs_minus_1, V4L2_AV1_MAX_TILE_COLS)
			 __array(u32, tile_info_height_in_sbs_minus_1, V4L2_AV1_MAX_TILE_ROWS)
			 __field(u8, tile_info_tile_size_bytes)
			 __field(u8, quantization_flags)
			 __field(u8, quantization_base_q_idx)
			 __field(s8, quantization_delta_q_y_dc)
			 __field(s8, quantization_delta_q_u_dc)
			 __field(s8, quantization_delta_q_u_ac)
			 __field(s8, quantization_delta_q_v_dc)
			 __field(s8, quantization_delta_q_v_ac)
			 __field(u8, quantization_qm_y)
			 __field(u8, quantization_qm_u)
			 __field(u8, quantization_qm_v)
			 __field(u8, quantization_delta_q_res)
			 __field(u8, superres_denom)
			 __field(u8, segmentation_flags)
			 __field(u8, segmentation_last_active_seg_id)
			 __array(u8, segmentation_feature_enabled, V4L2_AV1_MAX_SEGMENTS)
			 __field(u8, loop_filter_flags)
			 __array(u8, loop_filter_level, 4)
			 __field(u8, loop_filter_sharpness)
			 __array(s8, loop_filter_ref_deltas, V4L2_AV1_TOTAL_REFS_PER_FRAME)
			 __array(s8, loop_filter_mode_deltas, 2)
			 __field(u8, loop_filter_delta_lf_res)
			 __field(u8, cdef_damping_minus_3)
			 __field(u8, cdef_bits)
			 __array(u8, cdef_y_pri_strength, V4L2_AV1_CDEF_MAX)
			 __array(u8, cdef_y_sec_strength, V4L2_AV1_CDEF_MAX)
			 __array(u8, cdef_uv_pri_strength, V4L2_AV1_CDEF_MAX)
			 __array(u8, cdef_uv_sec_strength, V4L2_AV1_CDEF_MAX)
			 __array(u8, skip_mode_frame, 2)
			 __field(u8, primary_ref_frame)
			 __field(u8, loop_restoration_flags)
			 __field(u8, loop_restoration_lr_unit_shift)
			 __field(u8, loop_restoration_lr_uv_shift)
			 __array(int,
				 loop_restoration_frame_restoration_type, V4L2_AV1_NUM_PLANES_MAX)
			 __array(u32,
				 loop_restoration_loop_restoration_size, V4L2_AV1_MAX_NUM_PLANES)
			 __field(u32, flags)
			 __field(u32, order_hint)
			 __field(u32, upscaled_width)
			 __field(u32, frame_width_minus_1)
			 __field(u32, frame_height_minus_1)
			 __field(u16, render_width_minus_1)
			 __field(u16, render_height_minus_1)
			 __field(u32, current_frame_id)
			 __array(u32, buffer_removal_time, V4L2_AV1_MAX_OPERATING_POINTS)
			 __array(u32, order_hints, V4L2_AV1_TOTAL_REFS_PER_FRAME)
			 __array(u64, reference_frame_ts, V4L2_AV1_TOTAL_REFS_PER_FRAME)
			 __array(s8, ref_frame_idx, V4L2_AV1_REFS_PER_FRAME)
			 __field(u8, refresh_frame_flags)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->tile_info_flags = f->tile_info.flags;
		       __entry->tile_info_context_update_tile_id =
				f->tile_info.context_update_tile_id;
		       __entry->tile_info_tile_cols = f->tile_info.tile_cols;
		       __entry->tile_info_tile_rows = f->tile_info.tile_rows;
		       memcpy(__entry->tile_info_mi_col_starts, f->tile_info.mi_col_starts,
			      sizeof(__entry->tile_info_mi_col_starts));
		       memcpy(__entry->tile_info_mi_row_starts, f->tile_info.mi_row_starts,
			      sizeof(__entry->tile_info_mi_row_starts));
		       memcpy(__entry->tile_info_width_in_sbs_minus_1,
			      f->tile_info.width_in_sbs_minus_1,
			      sizeof(__entry->tile_info_width_in_sbs_minus_1));
		       memcpy(__entry->tile_info_height_in_sbs_minus_1,
			      f->tile_info.height_in_sbs_minus_1,
			      sizeof(__entry->tile_info_height_in_sbs_minus_1));
		       __entry->tile_info_tile_size_bytes = f->tile_info.tile_size_bytes;
		       __entry->quantization_flags = f->quantization.flags;
		       __entry->quantization_base_q_idx = f->quantization.base_q_idx;
		       __entry->quantization_delta_q_y_dc = f->quantization.delta_q_y_dc;
		       __entry->quantization_delta_q_u_dc = f->quantization.delta_q_u_dc;
		       __entry->quantization_delta_q_u_ac = f->quantization.delta_q_u_ac;
		       __entry->quantization_delta_q_v_dc = f->quantization.delta_q_v_dc;
		       __entry->quantization_delta_q_v_ac = f->quantization.delta_q_v_ac;
		       __entry->quantization_qm_y = f->quantization.qm_y;
		       __entry->quantization_qm_u = f->quantization.qm_u;
		       __entry->quantization_qm_v = f->quantization.qm_v;
		       __entry->quantization_delta_q_res = f->quantization.delta_q_res;
		       __entry->superres_denom = f->superres_denom;
		       __entry->segmentation_flags = f->segmentation.flags;
		       __entry->segmentation_last_active_seg_id =
				f->segmentation.last_active_seg_id;
		       memcpy(__entry->segmentation_feature_enabled,
			      f->segmentation.feature_enabled,
			      sizeof(__entry->segmentation_feature_enabled));
		       __entry->loop_filter_flags = f->loop_filter.flags;
		       memcpy(__entry->loop_filter_level, f->loop_filter.level,
			      sizeof(__entry->loop_filter_level));
		       __entry->loop_filter_sharpness = f->loop_filter.sharpness;
		       memcpy(__entry->loop_filter_ref_deltas, f->loop_filter.ref_deltas,
			      sizeof(__entry->loop_filter_ref_deltas));
		       memcpy(__entry->loop_filter_mode_deltas, f->loop_filter.mode_deltas,
			      sizeof(__entry->loop_filter_mode_deltas));
		       __entry->loop_filter_delta_lf_res = f->loop_filter.delta_lf_res;
		       __entry->cdef_damping_minus_3 = f->cdef.damping_minus_3;
		       __entry->cdef_bits = f->cdef.bits;
		       memcpy(__entry->cdef_y_pri_strength, f->cdef.y_pri_strength,
			      sizeof(__entry->cdef_y_pri_strength));
		       memcpy(__entry->cdef_y_sec_strength, f->cdef.y_sec_strength,
			      sizeof(__entry->cdef_y_sec_strength));
		       memcpy(__entry->cdef_uv_pri_strength, f->cdef.uv_pri_strength,
			      sizeof(__entry->cdef_uv_pri_strength));
		       memcpy(__entry->cdef_uv_sec_strength, f->cdef.uv_sec_strength,
			      sizeof(__entry->cdef_uv_sec_strength));
		       memcpy(__entry->skip_mode_frame, f->skip_mode_frame,
			      sizeof(__entry->skip_mode_frame));
		       __entry->primary_ref_frame = f->primary_ref_frame;
		       __entry->loop_restoration_flags = f->loop_restoration.flags;
		       __entry->loop_restoration_lr_unit_shift = f->loop_restoration.lr_unit_shift;
		       __entry->loop_restoration_lr_uv_shift = f->loop_restoration.lr_uv_shift;
		       memcpy(__entry->loop_restoration_frame_restoration_type,
			      f->loop_restoration.frame_restoration_type,
			      sizeof(__entry->loop_restoration_frame_restoration_type));
		       memcpy(__entry->loop_restoration_loop_restoration_size,
			      f->loop_restoration.loop_restoration_size,
			      sizeof(__entry->loop_restoration_loop_restoration_size));
		       __entry->flags = f->flags;
		       __entry->order_hint = f->order_hint;
		       __entry->upscaled_width = f->upscaled_width;
		       __entry->frame_width_minus_1 = f->frame_width_minus_1;
		       __entry->frame_height_minus_1 = f->frame_height_minus_1;
		       __entry->render_width_minus_1 = f->render_width_minus_1;
		       __entry->render_height_minus_1 = f->render_height_minus_1;
		       __entry->current_frame_id = f->current_frame_id;
		       memcpy(__entry->buffer_removal_time, f->buffer_removal_time,
			      sizeof(__entry->buffer_removal_time));
		       memcpy(__entry->order_hints, f->order_hints,
			      sizeof(__entry->order_hints));
		       memcpy(__entry->reference_frame_ts, f->reference_frame_ts,
			      sizeof(__entry->reference_frame_ts));
		       memcpy(__entry->ref_frame_idx, f->ref_frame_idx,
			      sizeof(__entry->ref_frame_idx));
		       __entry->refresh_frame_flags = f->refresh_frame_flags;),
	TP_printk("tgid = %u, fd = %u, "
		  "\ntile_info.flags: %s\ntile_info.context_update_tile_id: %u\n"
		  "tile_info.tile_cols: %u\ntile_info.tile_rows: %u\n"
		  "tile_info.mi_col_starts: %s\ntile_info.mi_row_starts: %s\n"
		  "tile_info.width_in_sbs_minus_1: %s\ntile_info.height_in_sbs_minus_1: %s\n"
		  "tile_info.tile_size_bytes: %u\nquantization.flags: %s\n"
		  "quantization.base_q_idx: %u\nquantization.delta_q_y_dc: %d\n"
		  "quantization.delta_q_u_dc: %d\nquantization.delta_q_u_ac: %d\n"
		  "quantization.delta_q_v_dc: %d\nquantization.delta_q_v_ac: %d\n"
		  "quantization.qm_y: %u\nquantization.qm_u: %u\nquantization.qm_v: %u\n"
		  "quantization.delta_q_res: %u\nsuperres_denom: %u\nsegmentation.flags: %s\n"
		  "segmentation.last_active_seg_id: %u\nsegmentation.feature_enabled:%s\n"
		  "loop_filter.flags: %s\nloop_filter.level: %s\nloop_filter.sharpness: %u\n"
		  "loop_filter.ref_deltas: %s\nloop_filter.mode_deltas: %s\n"
		  "loop_filter.delta_lf_res: %u\ncdef.damping_minus_3: %u\ncdef.bits: %u\n"
		  "cdef.y_pri_strength: %s\ncdef.y_sec_strength: %s\n"
		  "cdef.uv_pri_strength: %s\ncdef.uv_sec_strength:%s\nskip_mode_frame: %s\n"
		  "primary_ref_frame: %u\nloop_restoration.flags: %s\n"
		  "loop_restoration.lr_unit_shift: %u\nloop_restoration.lr_uv_shift: %u\n"
		  "loop_restoration.frame_restoration_type: %s\n"
		  "loop_restoration.loop_restoration_size: %s\nflags: %s\norder_hint: %u\n"
		  "upscaled_width: %u\nframe_width_minus_1: %u\nframe_height_minus_1: %u\n"
		  "render_width_minus_1: %u\nrender_height_minus_1: %u\ncurrent_frame_id: %u\n"
		  "buffer_removal_time: %s\norder_hints: %s\nreference_frame_ts: %s\n"
		  "ref_frame_idx: %s\nrefresh_frame_flags: %u\n",
		  __entry->tgid, __entry->fd,
		  __print_flags(__entry->tile_info_flags, "|",
		  {V4L2_AV1_TILE_INFO_FLAG_UNIFORM_TILE_SPACING, "UNIFORM_TILE_SPACING"}),
		  __entry->tile_info_context_update_tile_id,
		  __entry->tile_info_tile_cols,
		  __entry->tile_info_tile_rows,
		  __print_array(__entry->tile_info_mi_col_starts,
				ARRAY_SIZE(__entry->tile_info_mi_col_starts),
				sizeof(__entry->tile_info_mi_col_starts[0])),
		  __print_array(__entry->tile_info_mi_row_starts,
				ARRAY_SIZE(__entry->tile_info_mi_row_starts),
				sizeof(__entry->tile_info_mi_row_starts[0])),
		  __print_array(__entry->tile_info_width_in_sbs_minus_1,
				ARRAY_SIZE(__entry->tile_info_width_in_sbs_minus_1),
				sizeof(__entry->tile_info_width_in_sbs_minus_1[0])),
		  __print_array(__entry->tile_info_height_in_sbs_minus_1,
				ARRAY_SIZE(__entry->tile_info_height_in_sbs_minus_1),
				sizeof(__entry->tile_info_height_in_sbs_minus_1[0])),
		  __entry->tile_info_tile_size_bytes,
		  __print_flags(__entry->quantization_flags, "|",
		  {V4L2_AV1_QUANTIZATION_FLAG_DIFF_UV_DELTA, "DIFF_UV_DELTA"},
		  {V4L2_AV1_QUANTIZATION_FLAG_USING_QMATRIX, "USING_QMATRIX"},
		  {V4L2_AV1_QUANTIZATION_FLAG_DELTA_Q_PRESENT, "DELTA_Q_PRESENT"}),
		  __entry->quantization_base_q_idx,
		  __entry->quantization_delta_q_y_dc,
		  __entry->quantization_delta_q_u_dc,
		  __entry->quantization_delta_q_u_ac,
		  __entry->quantization_delta_q_v_dc,
		  __entry->quantization_delta_q_v_ac,
		  __entry->quantization_qm_y,
		  __entry->quantization_qm_u,
		  __entry->quantization_qm_v,
		  __entry->quantization_delta_q_res,
		  __entry->superres_denom,
		  __print_flags(__entry->segmentation_flags, "|",
		  {V4L2_AV1_SEGMENTATION_FLAG_ENABLED, "ENABLED"},
		  {V4L2_AV1_SEGMENTATION_FLAG_UPDATE_MAP, "UPDATE_MAP"},
		  {V4L2_AV1_SEGMENTATION_FLAG_TEMPORAL_UPDATE, "TEMPORAL_UPDATE"},
		  {V4L2_AV1_SEGMENTATION_FLAG_UPDATE_DATA, "UPDATE_DATA"},
		  {V4L2_AV1_SEGMENTATION_FLAG_SEG_ID_PRE_SKIP, "SEG_ID_PRE_SKIP"}),
		  __entry->segmentation_last_active_seg_id,
		  __print_array(__entry->segmentation_feature_enabled,
				ARRAY_SIZE(__entry->segmentation_feature_enabled),
				sizeof(__entry->segmentation_feature_enabled[0])),
		  __print_flags(__entry->loop_filter_flags, "|",
		  {V4L2_AV1_LOOP_FILTER_FLAG_DELTA_ENABLED, "DELTA_ENABLED"},
		  {V4L2_AV1_LOOP_FILTER_FLAG_DELTA_UPDATE, "DELTA_UPDATE"},
		  {V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_PRESENT, "DELTA_LF_PRESENT"},
		  {V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_MULTI, "DELTA_LF_MULTI"}),
		  __print_array(__entry->loop_filter_level,
				ARRAY_SIZE(__entry->loop_filter_level),
				sizeof(__entry->loop_filter_level[0])),
		  __entry->loop_filter_sharpness,
		  __print_array(__entry->loop_filter_ref_deltas,
				ARRAY_SIZE(__entry->loop_filter_ref_deltas),
				sizeof(__entry->loop_filter_ref_deltas[0])),
		  __print_array(__entry->loop_filter_mode_deltas,
				ARRAY_SIZE(__entry->loop_filter_mode_deltas),
				sizeof(__entry->loop_filter_mode_deltas[0])),
		  __entry->loop_filter_delta_lf_res,
		  __entry->cdef_damping_minus_3,
		  __entry->cdef_bits,
		  __print_array(__entry->cdef_y_pri_strength,
				ARRAY_SIZE(__entry->cdef_y_pri_strength),
				sizeof(__entry->cdef_y_pri_strength[0])),
		  __print_array(__entry->cdef_y_sec_strength,
				ARRAY_SIZE(__entry->cdef_y_sec_strength),
				sizeof(__entry->cdef_y_sec_strength[0])),
		  __print_array(__entry->cdef_uv_pri_strength,
				ARRAY_SIZE(__entry->cdef_uv_pri_strength),
				sizeof(__entry->cdef_uv_pri_strength[0])),
		  __print_array(__entry->cdef_uv_sec_strength,
				ARRAY_SIZE(__entry->cdef_uv_sec_strength),
				sizeof(__entry->cdef_uv_sec_strength[0])),
		  __print_array(__entry->skip_mode_frame,
				ARRAY_SIZE(__entry->skip_mode_frame),
				sizeof(__entry->skip_mode_frame[0])),
		  __entry->primary_ref_frame,
		  __print_flags(__entry->loop_restoration_flags, "|",
		  {V4L2_AV1_LOOP_RESTORATION_FLAG_USES_LR, "USES_LR"},
		  {V4L2_AV1_LOOP_RESTORATION_FLAG_USES_CHROMA_LR, "USES_CHROMA_LR"}),
		  __entry->loop_restoration_lr_unit_shift,
		  __entry->loop_restoration_lr_uv_shift,
		  __print_array(__entry->loop_restoration_frame_restoration_type,
				ARRAY_SIZE(__entry->loop_restoration_frame_restoration_type),
				sizeof(__entry->loop_restoration_frame_restoration_type[0])),
		  __print_array(__entry->loop_restoration_loop_restoration_size,
				ARRAY_SIZE(__entry->loop_restoration_loop_restoration_size),
				sizeof(__entry->loop_restoration_loop_restoration_size[0])),
		  __print_flags(__entry->flags, "|",
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
		  __entry->order_hint,
		  __entry->upscaled_width,
		  __entry->frame_width_minus_1,
		  __entry->frame_height_minus_1,
		  __entry->render_width_minus_1,
		  __entry->render_height_minus_1,
		  __entry->current_frame_id,
		  __print_array(__entry->buffer_removal_time,
				ARRAY_SIZE(__entry->buffer_removal_time),
				sizeof(__entry->buffer_removal_time[0])),
		  __print_array(__entry->order_hints,
				ARRAY_SIZE(__entry->order_hints),
				sizeof(__entry->order_hints[0])),
		  __print_array(__entry->reference_frame_ts,
				ARRAY_SIZE(__entry->reference_frame_ts),
				sizeof(__entry->reference_frame_ts[0])),
		  __print_array(__entry->ref_frame_idx,
				ARRAY_SIZE(__entry->ref_frame_idx),
				sizeof(__entry->ref_frame_idx[0])),
		  __entry->refresh_frame_flags
	)
);


DECLARE_EVENT_CLASS(v4l2_ctrl_av1_film_grain_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_av1_film_grain *f),
	TP_ARGS(tgid, fd, f),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field(__u8, flags)
			 __field(__u8, cr_mult)
			 __field(__u16, grain_seed)
			 __field(__u8, film_grain_params_ref_idx)
			 __field(__u8, num_y_points)
			 __array(__u8, point_y_value, V4L2_AV1_MAX_NUM_Y_POINTS)
			 __array(__u8, point_y_scaling, V4L2_AV1_MAX_NUM_Y_POINTS)
			 __field(__u8, num_cb_points)
			 __array(__u8, point_cb_value, V4L2_AV1_MAX_NUM_CB_POINTS)
			 __array(__u8, point_cb_scaling, V4L2_AV1_MAX_NUM_CB_POINTS)
			 __field(__u8, num_cr_points)
			 __array(__u8, point_cr_value, V4L2_AV1_MAX_NUM_CR_POINTS)
			 __array(__u8, point_cr_scaling, V4L2_AV1_MAX_NUM_CR_POINTS)
			 __field(__u8, grain_scaling_minus_8)
			 __field(__u8, ar_coeff_lag)
			 __array(__u8, ar_coeffs_y_plus_128, V4L2_AV1_AR_COEFFS_SIZE)
			 __array(__u8, ar_coeffs_cb_plus_128, V4L2_AV1_AR_COEFFS_SIZE)
			 __array(__u8, ar_coeffs_cr_plus_128, V4L2_AV1_AR_COEFFS_SIZE)
			 __field(__u8, ar_coeff_shift_minus_6)
			 __field(__u8, grain_scale_shift)
			 __field(__u8, cb_mult)
			 __field(__u8, cb_luma_mult)
			 __field(__u8, cr_luma_mult)
			 __field(__u16, cb_offset)
			 __field(__u16, cr_offset)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->flags = f->flags;
		       __entry->cr_mult = f->cr_mult;
		       __entry->grain_seed = f->grain_seed;
		       __entry->film_grain_params_ref_idx = f->film_grain_params_ref_idx;
		       __entry->num_y_points = f->num_y_points;
		       memcpy(__entry->point_y_value, f->point_y_value,
			      sizeof(__entry->point_y_value));
		       memcpy(__entry->point_y_scaling, f->point_y_scaling,
			      sizeof(__entry->point_y_scaling));
		       __entry->num_cb_points = f->num_cb_points;
		       memcpy(__entry->point_cb_value, f->point_cb_value,
			      sizeof(__entry->point_cb_value));
		       memcpy(__entry->point_cb_scaling, f->point_cb_scaling,
			      sizeof(__entry->point_cb_scaling));
		       __entry->num_cr_points = f->num_cr_points;
		       memcpy(__entry->point_cr_value, f->point_cr_value,
			      sizeof(__entry->point_cr_value));
		       memcpy(__entry->point_cr_scaling, f->point_cr_scaling,
			      sizeof(__entry->point_cr_scaling));
		       __entry->grain_scaling_minus_8 = f->grain_scaling_minus_8;
		       __entry->ar_coeff_lag = f->ar_coeff_lag;
		       memcpy(__entry->ar_coeffs_y_plus_128, f->ar_coeffs_y_plus_128,
			      sizeof(__entry->ar_coeffs_y_plus_128));
		       memcpy(__entry->ar_coeffs_cb_plus_128, f->ar_coeffs_cb_plus_128,
			      sizeof(__entry->ar_coeffs_cb_plus_128));
		       memcpy(__entry->ar_coeffs_cr_plus_128, f->ar_coeffs_cr_plus_128,
			      sizeof(__entry->ar_coeffs_cr_plus_128));
		       __entry->ar_coeff_shift_minus_6 = f->ar_coeff_shift_minus_6;
		       __entry->grain_scale_shift = f->grain_scale_shift;
		       __entry->cb_mult = f->cb_mult;
		       __entry->cb_luma_mult = f->cb_luma_mult;
		       __entry->cr_luma_mult = f->cr_luma_mult;
		       __entry->cb_offset = f->cb_offset;
		       __entry->cr_offset = f->cr_offset;),
	TP_printk("tgid = %u, fd = %u, "
		  "\nflags %s\ncr_mult: %u\ngrain_seed: %u\n"
		  "film_grain_params_ref_idx: %u\nnum_y_points: %u\npoint_y_value: %s\n"
		  "point_y_scaling: %s\nnum_cb_points: %u\npoint_cb_value: %s\n"
		  "point_cb_scaling: %s\nnum_cr_points: %u\npoint_cr_value: %s\n"
		  "point_cr_scaling: %s\ngrain_scaling_minus_8: %u\nar_coeff_lag: %u\n"
		  "ar_coeffs_y_plus_128: %s\nar_coeffs_cb_plus_128: %s\n"
		  "ar_coeffs_cr_plus_128: %s\nar_coeff_shift_minus_6: %u\n"
		  "grain_scale_shift: %u\ncb_mult: %u\ncb_luma_mult: %u\ncr_luma_mult: %u\n"
		  "cb_offset: %u\ncr_offset: %u\n",
		  __entry->tgid, __entry->fd,
		  __print_flags(__entry->flags, "|",
		  {V4L2_AV1_FILM_GRAIN_FLAG_APPLY_GRAIN, "APPLY_GRAIN"},
		  {V4L2_AV1_FILM_GRAIN_FLAG_UPDATE_GRAIN, "UPDATE_GRAIN"},
		  {V4L2_AV1_FILM_GRAIN_FLAG_CHROMA_SCALING_FROM_LUMA, "CHROMA_SCALING_FROM_LUMA"},
		  {V4L2_AV1_FILM_GRAIN_FLAG_OVERLAP, "OVERLAP"},
		  {V4L2_AV1_FILM_GRAIN_FLAG_CLIP_TO_RESTRICTED_RANGE, "CLIP_TO_RESTRICTED_RANGE"}),
		  __entry->cr_mult,
		  __entry->grain_seed,
		  __entry->film_grain_params_ref_idx,
		  __entry->num_y_points,
		  __print_array(__entry->point_y_value,
				ARRAY_SIZE(__entry->point_y_value),
				sizeof(__entry->point_y_value[0])),
		  __print_array(__entry->point_y_scaling,
				ARRAY_SIZE(__entry->point_y_scaling),
				sizeof(__entry->point_y_scaling[0])),
		  __entry->num_cb_points,
		  __print_array(__entry->point_cb_value,
				ARRAY_SIZE(__entry->point_cb_value),
				sizeof(__entry->point_cb_value[0])),
		  __print_array(__entry->point_cb_scaling,
				ARRAY_SIZE(__entry->point_cb_scaling),
				sizeof(__entry->point_cb_scaling[0])),
		  __entry->num_cr_points,
		  __print_array(__entry->point_cr_value,
				ARRAY_SIZE(__entry->point_cr_value),
				sizeof(__entry->point_cr_value[0])),
		  __print_array(__entry->point_cr_scaling,
				ARRAY_SIZE(__entry->point_cr_scaling),
				sizeof(__entry->point_cr_scaling[0])),
		  __entry->grain_scaling_minus_8,
		  __entry->ar_coeff_lag,
		  __print_array(__entry->ar_coeffs_y_plus_128,
				ARRAY_SIZE(__entry->ar_coeffs_y_plus_128),
				sizeof(__entry->ar_coeffs_y_plus_128[0])),
		  __print_array(__entry->ar_coeffs_cb_plus_128,
				ARRAY_SIZE(__entry->ar_coeffs_cb_plus_128),
				sizeof(__entry->ar_coeffs_cb_plus_128[0])),
		  __print_array(__entry->ar_coeffs_cr_plus_128,
				ARRAY_SIZE(__entry->ar_coeffs_cr_plus_128),
				sizeof(__entry->ar_coeffs_cr_plus_128[0])),
		  __entry->ar_coeff_shift_minus_6,
		  __entry->grain_scale_shift,
		  __entry->cb_mult,
		  __entry->cb_luma_mult,
		  __entry->cr_luma_mult,
		  __entry->cb_offset,
		  __entry->cr_offset
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
		  "backward_ref_ts %llu version %u width %u height %u flags %s colorspace %u "
		  "xfer_func %u ycbcr_enc %u quantization %u",
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
			 __field(u8, profile_idc)
			 __field(u8, constraint_set_flags)
			 __field(u8, level_idc)
			 __field(u8, seq_parameter_set_id)
			 __field(u8, chroma_format_idc)
			 __field(u8, bit_depth_luma_minus8)
			 __field(u8, bit_depth_chroma_minus8)
			 __field(u8, log2_max_frame_num_minus4)
			 __field(u8, pic_order_cnt_type)
			 __field(u8, log2_max_pic_order_cnt_lsb_minus4)
			 __field(u8, max_num_ref_frames)
			 __field(u8, num_ref_frames_in_pic_order_cnt_cycle)
			 __array(__s32, offset_for_ref_frame, 255)
			 __field(__s32, offset_for_non_ref_pic)
			 __field(__s32, offset_for_top_to_bottom_field)
			 __field(u16, pic_width_in_mbs_minus1)
			 __field(u16, pic_height_in_map_units_minus1)
			 __field(u32, flags)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->profile_idc = s->profile_idc;
		       __entry->constraint_set_flags = s->constraint_set_flags;
		       __entry->level_idc = s->level_idc;
		       __entry->seq_parameter_set_id = s->seq_parameter_set_id;
		       __entry->chroma_format_idc = s->chroma_format_idc;
		       __entry->bit_depth_luma_minus8 = s->bit_depth_luma_minus8;
		       __entry->bit_depth_chroma_minus8 = s->bit_depth_chroma_minus8;
		       __entry->log2_max_frame_num_minus4 = s->log2_max_frame_num_minus4;
		       __entry->pic_order_cnt_type = s->pic_order_cnt_type;
		       __entry->log2_max_pic_order_cnt_lsb_minus4 =
				s->log2_max_pic_order_cnt_lsb_minus4;
		       __entry->max_num_ref_frames = s->max_num_ref_frames;
		       __entry->num_ref_frames_in_pic_order_cnt_cycle =
				s->num_ref_frames_in_pic_order_cnt_cycle;
		       memcpy(__entry->offset_for_ref_frame, s->offset_for_ref_frame,
			      sizeof(__entry->offset_for_ref_frame));
		       __entry->offset_for_non_ref_pic = s->offset_for_non_ref_pic;
		       __entry->offset_for_top_to_bottom_field = s->offset_for_top_to_bottom_field;
		       __entry->pic_width_in_mbs_minus1 = s->pic_width_in_mbs_minus1;
		       __entry->pic_height_in_map_units_minus1 = s->pic_height_in_map_units_minus1;
		       __entry->flags = s->flags),
	TP_printk("tgid = %u, fd = %u, "
		  "\nprofile_idc %u\n"
		  "constraint_set_flags %s\n"
		  "level_idc %u\n"
		  "seq_parameter_set_id %u\n"
		  "chroma_format_idc %u\n"
		  "bit_depth_luma_minus8 %u\n"
		  "bit_depth_chroma_minus8 %u\n"
		  "log2_max_frame_num_minus4 %u\n"
		  "pic_order_cnt_type %u\n"
		  "log2_max_pic_order_cnt_lsb_minus4 %u\n"
		  "max_num_ref_frames %u\n"
		  "num_ref_frames_in_pic_order_cnt_cycle %u\n"
		  "offset_for_ref_frame %s\n"
		  "offset_for_non_ref_pic %d\n"
		  "offset_for_top_to_bottom_field %d\n"
		  "pic_width_in_mbs_minus1 %u\n"
		  "pic_height_in_map_units_minus1 %u\n"
		  "flags %s",
		  __entry->tgid, __entry->fd,
		  __entry->profile_idc,
		  __print_flags(__entry->constraint_set_flags, "|",
		  {V4L2_H264_SPS_CONSTRAINT_SET0_FLAG, "CONSTRAINT_SET0_FLAG"},
		  {V4L2_H264_SPS_CONSTRAINT_SET1_FLAG, "CONSTRAINT_SET1_FLAG"},
		  {V4L2_H264_SPS_CONSTRAINT_SET2_FLAG, "CONSTRAINT_SET2_FLAG"},
		  {V4L2_H264_SPS_CONSTRAINT_SET3_FLAG, "CONSTRAINT_SET3_FLAG"},
		  {V4L2_H264_SPS_CONSTRAINT_SET4_FLAG, "CONSTRAINT_SET4_FLAG"},
		  {V4L2_H264_SPS_CONSTRAINT_SET5_FLAG, "CONSTRAINT_SET5_FLAG"}),
		  __entry->level_idc,
		  __entry->seq_parameter_set_id,
		  __entry->chroma_format_idc,
		  __entry->bit_depth_luma_minus8,
		  __entry->bit_depth_chroma_minus8,
		  __entry->log2_max_frame_num_minus4,
		  __entry->pic_order_cnt_type,
		  __entry->log2_max_pic_order_cnt_lsb_minus4,
		  __entry->max_num_ref_frames,
		  __entry->num_ref_frames_in_pic_order_cnt_cycle,
		  __print_array(__entry->offset_for_ref_frame,
				ARRAY_SIZE(__entry->offset_for_ref_frame),
				sizeof(__entry->offset_for_ref_frame[0])),
		  __entry->offset_for_non_ref_pic,
		  __entry->offset_for_top_to_bottom_field,
		  __entry->pic_width_in_mbs_minus1,
		  __entry->pic_height_in_map_units_minus1,
		  __print_flags(__entry->flags, "|",
		  {V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE, "SEPARATE_COLOUR_PLANE"},
		  {V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS,
		   "QPPRIME_Y_ZERO_TRANSFORM_BYPASS"},
		  {V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO, "DELTA_PIC_ORDER_ALWAYS_ZERO"},
		  {V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED,
		   "GAPS_IN_FRAME_NUM_VALUE_ALLOWED"},
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
			 __field(u8, pic_parameter_set_id)
			 __field(u8, seq_parameter_set_id)
			 __field(u8, num_slice_groups_minus1)
			 __field(u8, num_ref_idx_l0_default_active_minus1)
			 __field(u8, num_ref_idx_l1_default_active_minus1)
			 __field(u8, weighted_bipred_idc)
			 __field(__s8, pic_init_qp_minus26)
			 __field(__s8, pic_init_qs_minus26)
			 __field(__s8, chroma_qp_index_offset)
			 __field(__s8, second_chroma_qp_index_offset)
			 __field(u16, flags)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->pic_parameter_set_id = p->pic_parameter_set_id;
		       __entry->seq_parameter_set_id = p->seq_parameter_set_id;
		       __entry->num_slice_groups_minus1 = p->num_slice_groups_minus1;
		       __entry->num_ref_idx_l0_default_active_minus1 =
				p->num_ref_idx_l0_default_active_minus1;
		       __entry->num_ref_idx_l1_default_active_minus1 =
				p->num_ref_idx_l1_default_active_minus1;
		       __entry->weighted_bipred_idc = p->weighted_bipred_idc;
		       __entry->pic_init_qp_minus26 = p->pic_init_qp_minus26;
		       __entry->pic_init_qs_minus26 = p->pic_init_qs_minus26;
		       __entry->chroma_qp_index_offset = p->chroma_qp_index_offset;
		       __entry->second_chroma_qp_index_offset = p->second_chroma_qp_index_offset;
		       __entry->flags = p->flags),
	TP_printk("tgid = %u, fd = %u, "
		  "\npic_parameter_set_id %u\n"
		  "seq_parameter_set_id %u\n"
		  "num_slice_groups_minus1 %u\n"
		  "num_ref_idx_l0_default_active_minus1 %u\n"
		  "num_ref_idx_l1_default_active_minus1 %u\n"
		  "weighted_bipred_idc %u\n"
		  "pic_init_qp_minus26 %d\n"
		  "pic_init_qs_minus26 %d\n"
		  "chroma_qp_index_offset %d\n"
		  "second_chroma_qp_index_offset %d\n"
		  "flags %s",
		  __entry->tgid, __entry->fd,
		  __entry->pic_parameter_set_id,
		  __entry->seq_parameter_set_id,
		  __entry->num_slice_groups_minus1,
		  __entry->num_ref_idx_l0_default_active_minus1,
		  __entry->num_ref_idx_l1_default_active_minus1,
		  __entry->weighted_bipred_idc,
		  __entry->pic_init_qp_minus26,
		  __entry->pic_init_qs_minus26,
		  __entry->chroma_qp_index_offset,
		  __entry->second_chroma_qp_index_offset,
		  __print_flags(__entry->flags, "|",
		  {V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE, "ENTROPY_CODING_MODE"},
		  {V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT,
		   "BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT"},
		  {V4L2_H264_PPS_FLAG_WEIGHTED_PRED, "WEIGHTED_PRED"},
		  {V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT,
		   "DEBLOCKING_FILTER_CONTROL_PRESENT"},
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
			 __array(u8, scaling_list_4x4, 6 * 16)
			 __array(u8, scaling_list_8x8, 6 * 64)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       memcpy(__entry->scaling_list_4x4, s->scaling_list_4x4,
			      sizeof(__entry->scaling_list_4x4));
		       memcpy(__entry->scaling_list_8x8, s->scaling_list_8x8,
			      sizeof(__entry->scaling_list_8x8))),
	TP_printk("tgid = %u, fd = %u, "
		  "\nscaling_list_4x4 {%s}\nscaling_list_8x8 {%s}",
		  __entry->tgid, __entry->fd,
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->scaling_list_4x4,
				   sizeof(__entry->scaling_list_4x4),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->scaling_list_8x8,
				   sizeof(__entry->scaling_list_8x8),
				   false)
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_h264_pred_weights_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_h264_pred_weights *p),
	TP_ARGS(tgid, fd, p),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field(u16, luma_log2_weight_denom)
			 __field(u16, chroma_log2_weight_denom)
			 __array(__s16, weight_factors_0_luma_weight, 32)
			 __array(__s16, weight_factors_0_luma_offset, 32)
			 __array(__s16, weight_factors_0_chroma_weight, 32 * 2)
			 __array(__s16, weight_factors_0_chroma_offset, 32 * 2)
			 __array(__s16, weight_factors_1_luma_weight, 32)
			 __array(__s16, weight_factors_1_luma_offset, 32)
			 __array(__s16, weight_factors_1_chroma_weight, 32 * 2)
			 __array(__s16, weight_factors_1_chroma_offset, 32 * 2)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->luma_log2_weight_denom = p->luma_log2_weight_denom;
		       __entry->chroma_log2_weight_denom = p->chroma_log2_weight_denom;
		       memcpy(__entry->weight_factors_0_luma_weight,
			      p->weight_factors[0].luma_weight,
			      sizeof(__entry->weight_factors_0_luma_weight));
		       memcpy(__entry->weight_factors_0_luma_offset,
			      p->weight_factors[0].luma_offset,
			      sizeof(__entry->weight_factors_0_luma_offset));
		       memcpy(__entry->weight_factors_0_chroma_weight,
			      p->weight_factors[0].chroma_weight,
			      sizeof(__entry->weight_factors_0_chroma_weight));
		       memcpy(__entry->weight_factors_0_chroma_offset,
			      p->weight_factors[0].chroma_offset,
			      sizeof(__entry->weight_factors_0_chroma_offset));
		       memcpy(__entry->weight_factors_1_luma_weight,
			      p->weight_factors[1].luma_weight,
			      sizeof(__entry->weight_factors_1_luma_weight));
		       memcpy(__entry->weight_factors_1_luma_offset,
			      p->weight_factors[1].luma_offset,
			      sizeof(__entry->weight_factors_1_luma_offset));
		       memcpy(__entry->weight_factors_1_chroma_weight,
			      p->weight_factors[1].chroma_weight,
			      sizeof(__entry->weight_factors_1_chroma_weight));
		       memcpy(__entry->weight_factors_1_chroma_offset,
			      p->weight_factors[1].chroma_offset,
			      sizeof(__entry->weight_factors_1_chroma_offset))),
	TP_printk("tgid = %u, fd = %u, "
		  "\nluma_log2_weight_denom %u\n"
		  "chroma_log2_weight_denom %u\n"
		  "weight_factor[0].luma_weight %s\n"
		  "weight_factor[0].luma_offset %s\n"
		  "weight_factor[0].chroma_weight {%s}\n"
		  "weight_factor[0].chroma_offset {%s}\n"
		  "weight_factor[1].luma_weight %s\n"
		  "weight_factor[1].luma_offset %s\n"
		  "weight_factor[1].chroma_weight {%s}\n"
		  "weight_factor[1].chroma_offset {%s}\n",
		  __entry->tgid, __entry->fd,
		  __entry->luma_log2_weight_denom,
		  __entry->chroma_log2_weight_denom,
		  __print_array(__entry->weight_factors_0_luma_weight,
				ARRAY_SIZE(__entry->weight_factors_0_luma_weight),
				sizeof(__entry->weight_factors_0_luma_weight[0])),
		  __print_array(__entry->weight_factors_0_luma_offset,
				ARRAY_SIZE(__entry->weight_factors_0_luma_offset),
				sizeof(__entry->weight_factors_0_luma_offset[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->weight_factors_0_chroma_weight,
				   sizeof(__entry->weight_factors_0_chroma_weight),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->weight_factors_0_chroma_offset,
				   sizeof(__entry->weight_factors_0_chroma_offset),
				   false),
		  __print_array(__entry->weight_factors_1_luma_weight,
				ARRAY_SIZE(__entry->weight_factors_1_luma_weight),
				sizeof(__entry->weight_factors_1_luma_weight[0])),
		  __print_array(__entry->weight_factors_1_luma_offset,
				ARRAY_SIZE(__entry->weight_factors_1_luma_offset),
				sizeof(__entry->weight_factors_1_luma_offset[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->weight_factors_1_chroma_weight,
				   sizeof(__entry->weight_factors_1_chroma_weight),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->weight_factors_1_chroma_offset,
				   sizeof(__entry->weight_factors_1_chroma_offset),
				   false)
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_h264_slice_params_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_h264_slice_params *s),
	TP_ARGS(tgid, fd, s),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field(u32, header_bit_size)
			 __field(u32, first_mb_in_slice)
			 __field(u8, slice_type)
			 __field(u8, colour_plane_id)
			 __field(u8, redundant_pic_cnt)
			 __field(u8, cabac_init_idc)
			 __field(__s8, slice_qp_delta)
			 __field(__s8, slice_qs_delta)
			 __field(u8, disable_deblocking_filter_idc)
			 __field(__s8, slice_alpha_c0_offset_div2)
			 __field(__s8, slice_beta_offset_div2)
			 __field(u8, num_ref_idx_l0_active_minus1)
			 __field(u8, num_ref_idx_l1_active_minus1)
			 __field(u32, flags)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->header_bit_size = s->header_bit_size;
		       __entry->first_mb_in_slice = s->first_mb_in_slice;
		       __entry->slice_type = s->slice_type;
		       __entry->colour_plane_id = s->colour_plane_id;
		       __entry->redundant_pic_cnt = s->redundant_pic_cnt;
		       __entry->cabac_init_idc = s->cabac_init_idc;
		       __entry->slice_qp_delta = s->slice_qp_delta;
		       __entry->slice_qs_delta = s->slice_qs_delta;
		       __entry->disable_deblocking_filter_idc = s->disable_deblocking_filter_idc;
		       __entry->slice_alpha_c0_offset_div2 = s->slice_alpha_c0_offset_div2;
		       __entry->slice_beta_offset_div2 = s->slice_beta_offset_div2;
		       __entry->num_ref_idx_l0_active_minus1 = s->num_ref_idx_l0_active_minus1;
		       __entry->num_ref_idx_l1_active_minus1 = s->num_ref_idx_l1_active_minus1;
		       __entry->flags = s->flags),
	TP_printk("tgid = %u, fd = %u, "
		  "\nheader_bit_size %u\n"
		  "first_mb_in_slice %u\n"
		  "slice_type %s\n"
		  "colour_plane_id %u\n"
		  "redundant_pic_cnt %u\n"
		  "cabac_init_idc %u\n"
		  "slice_qp_delta %d\n"
		  "slice_qs_delta %d\n"
		  "disable_deblocking_filter_idc %u\n"
		  "slice_alpha_c0_offset_div2 %u\n"
		  "slice_beta_offset_div2 %u\n"
		  "num_ref_idx_l0_active_minus1 %u\n"
		  "num_ref_idx_l1_active_minus1 %u\n"
		  "flags %s",
		  __entry->tgid, __entry->fd,
		  __entry->header_bit_size,
		  __entry->first_mb_in_slice,
		  __print_symbolic(__entry->slice_type,
		  {V4L2_H264_SLICE_TYPE_P, "P"},
		  {V4L2_H264_SLICE_TYPE_B, "B"},
		  {V4L2_H264_SLICE_TYPE_I, "I"},
		  {V4L2_H264_SLICE_TYPE_SP, "SP"},
		  {V4L2_H264_SLICE_TYPE_SI, "SI"}),
		  __entry->colour_plane_id,
		  __entry->redundant_pic_cnt,
		  __entry->cabac_init_idc,
		  __entry->slice_qp_delta,
		  __entry->slice_qs_delta,
		  __entry->disable_deblocking_filter_idc,
		  __entry->slice_alpha_c0_offset_div2,
		  __entry->slice_beta_offset_div2,
		  __entry->num_ref_idx_l0_active_minus1,
		  __entry->num_ref_idx_l1_active_minus1,
		  __print_flags(__entry->flags, "|",
		  {V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED, "DIRECT_SPATIAL_MV_PRED"},
		  {V4L2_H264_SLICE_FLAG_SP_FOR_SWITCH, "SP_FOR_SWITCH"})
	)
);

DECLARE_EVENT_CLASS(v4l2_h264_reference_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_h264_reference *r, int i),
	TP_ARGS(tgid, fd, r, i),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field(u8, fields)
			 __field(u8, index)
			 __field(int, i)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->fields = r->fields;
		       __entry->index = r->index;
		       __entry->i = i;),
	TP_printk("tgid = %u, fd = %u, "
		  "[%d]: fields %s index %u",
		  __entry->tgid, __entry->fd,
		  __entry->i,
		  __print_flags(__entry->fields, "|",
		  {V4L2_H264_TOP_FIELD_REF, "TOP_FIELD_REF"},
		  {V4L2_H264_BOTTOM_FIELD_REF, "BOTTOM_FIELD_REF"},
		  {V4L2_H264_FRAME_REF, "FRAME_REF"}),
		  __entry->index
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_h264_decode_params_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_h264_decode_params *d),
	TP_ARGS(tgid, fd, d),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field(u16, nal_ref_idc)
			 __field(u16, frame_num)
			 __field(__s32, top_field_order_cnt)
			 __field(__s32, bottom_field_order_cnt)
			 __field(u16, idr_pic_id)
			 __field(u16, pic_order_cnt_lsb)
			 __field(__s32, delta_pic_order_cnt_bottom)
			 __field(__s32, delta_pic_order_cnt0)
			 __field(__s32, delta_pic_order_cnt1)
			 __field(u32, dec_ref_pic_marking_bit_size)
			 __field(u32, pic_order_cnt_bit_size)
			 __field(u32, slice_group_change_cycle)
			 __field(u32, flags)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->nal_ref_idc = d->nal_ref_idc;
		       __entry->frame_num = d->frame_num;
		       __entry->top_field_order_cnt = d->top_field_order_cnt;
		       __entry->bottom_field_order_cnt = d->bottom_field_order_cnt;
		       __entry->idr_pic_id = d->idr_pic_id;
		       __entry->pic_order_cnt_lsb = d->pic_order_cnt_lsb;
		       __entry->delta_pic_order_cnt_bottom = d->delta_pic_order_cnt_bottom;
		       __entry->delta_pic_order_cnt0 = d->delta_pic_order_cnt0;
		       __entry->delta_pic_order_cnt1 = d->delta_pic_order_cnt1;
		       __entry->dec_ref_pic_marking_bit_size = d->dec_ref_pic_marking_bit_size;
		       __entry->pic_order_cnt_bit_size = d->pic_order_cnt_bit_size;
		       __entry->slice_group_change_cycle = d->slice_group_change_cycle;
		       __entry->flags = d->flags),
	TP_printk("tgid = %u, fd = %u, "
		  "\nnal_ref_idc %u\n"
		  "frame_num %u\n"
		  "top_field_order_cnt %d\n"
		  "bottom_field_order_cnt %d\n"
		  "idr_pic_id %u\n"
		  "pic_order_cnt_lsb %u\n"
		  "delta_pic_order_cnt_bottom %d\n"
		  "delta_pic_order_cnt0 %d\n"
		  "delta_pic_order_cnt1 %d\n"
		  "dec_ref_pic_marking_bit_size %u\n"
		  "pic_order_cnt_bit_size %u\n"
		  "slice_group_change_cycle %u\n"
		  "flags %s\n",
		  __entry->tgid, __entry->fd,
		  __entry->nal_ref_idc,
		  __entry->frame_num,
		  __entry->top_field_order_cnt,
		  __entry->bottom_field_order_cnt,
		  __entry->idr_pic_id,
		  __entry->pic_order_cnt_lsb,
		  __entry->delta_pic_order_cnt_bottom,
		  __entry->delta_pic_order_cnt0,
		  __entry->delta_pic_order_cnt1,
		  __entry->dec_ref_pic_marking_bit_size,
		  __entry->pic_order_cnt_bit_size,
		  __entry->slice_group_change_cycle,
		  __print_flags(__entry->flags, "|",
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
			 __field(u64, reference_ts)
			 __field(u32, pic_num)
			 __field(u16, frame_num)
			 __field(u8, fields)
			 __field(__s32, top_field_order_cnt)
			 __field(__s32, bottom_field_order_cnt)
			 __field(u32, flags)
			 __field(int, i)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->reference_ts = e->reference_ts;
		       __entry->pic_num = e->pic_num;
		       __entry->frame_num = e->frame_num;
		       __entry->fields = e->fields;
		       __entry->top_field_order_cnt = e->top_field_order_cnt;
		       __entry->bottom_field_order_cnt = e->bottom_field_order_cnt;
		       __entry->flags = e->flags;
		       __entry->i = i;),
	TP_printk("tgid = %u, fd = %u, "
		  "[%d]: reference_ts %llu, pic_num %u frame_num %u fields %s "
		  "top_field_order_cnt %d bottom_field_order_cnt %d flags %s",
		  __entry->tgid, __entry->fd,
		  __entry->i,
		  __entry->reference_ts,
		  __entry->pic_num,
		  __entry->frame_num,
		  __print_flags(__entry->fields, "|",
		  {V4L2_H264_TOP_FIELD_REF, "TOP_FIELD_REF"},
		  {V4L2_H264_BOTTOM_FIELD_REF, "BOTTOM_FIELD_REF"},
		  {V4L2_H264_FRAME_REF, "FRAME_REF"}),
		  __entry->top_field_order_cnt,
		  __entry->bottom_field_order_cnt,
		  __print_flags(__entry->flags, "|",
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
			 __field(__u8, video_parameter_set_id)
			 __field(__u8, seq_parameter_set_id)
			 __field(__u16, pic_width_in_luma_samples)
			 __field(__u16, pic_height_in_luma_samples)
			 __field(__u8, bit_depth_luma_minus8)
			 __field(__u8, bit_depth_chroma_minus8)
			 __field(__u8, log2_max_pic_order_cnt_lsb_minus4)
			 __field(__u8, sps_max_dec_pic_buffering_minus1)
			 __field(__u8, sps_max_num_reorder_pics)
			 __field(__u8, sps_max_latency_increase_plus1)
			 __field(__u8, log2_min_luma_coding_block_size_minus3)
			 __field(__u8, log2_diff_max_min_luma_coding_block_size)
			 __field(__u8, log2_min_luma_transform_block_size_minus2)
			 __field(__u8, log2_diff_max_min_luma_transform_block_size)
			 __field(__u8, max_transform_hierarchy_depth_inter)
			 __field(__u8, max_transform_hierarchy_depth_intra)
			 __field(__u8, pcm_sample_bit_depth_luma_minus1)
			 __field(__u8, pcm_sample_bit_depth_chroma_minus1)
			 __field(__u8, log2_min_pcm_luma_coding_block_size_minus3)
			 __field(__u8, log2_diff_max_min_pcm_luma_coding_block_size)
			 __field(__u8, num_short_term_ref_pic_sets)
			 __field(__u8, num_long_term_ref_pics_sps)
			 __field(__u8, chroma_format_idc)
			 __field(__u8, sps_max_sub_layers_minus1)
			 __field(__u64, flags)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->video_parameter_set_id = s->video_parameter_set_id;
		       __entry->seq_parameter_set_id = s->seq_parameter_set_id;
		       __entry->pic_width_in_luma_samples = s->pic_width_in_luma_samples;
		       __entry->pic_height_in_luma_samples = s->pic_height_in_luma_samples;
		       __entry->bit_depth_luma_minus8 = s->bit_depth_luma_minus8;
		       __entry->bit_depth_chroma_minus8 = s->bit_depth_chroma_minus8;
		       __entry->log2_max_pic_order_cnt_lsb_minus4 =
				s->log2_max_pic_order_cnt_lsb_minus4;
		       __entry->sps_max_dec_pic_buffering_minus1 =
				s->sps_max_dec_pic_buffering_minus1;
		       __entry->sps_max_num_reorder_pics = s->sps_max_num_reorder_pics;
		       __entry->sps_max_latency_increase_plus1 = s->sps_max_latency_increase_plus1;
		       __entry->log2_min_luma_coding_block_size_minus3 =
				s->log2_min_luma_coding_block_size_minus3;
		       __entry->log2_diff_max_min_luma_coding_block_size =
				s->log2_diff_max_min_luma_coding_block_size;
		       __entry->log2_min_luma_transform_block_size_minus2 =
				s->log2_min_luma_transform_block_size_minus2;
		       __entry->log2_diff_max_min_luma_transform_block_size =
				s->log2_diff_max_min_luma_transform_block_size;
		       __entry->max_transform_hierarchy_depth_inter =
				s->max_transform_hierarchy_depth_inter;
		       __entry->max_transform_hierarchy_depth_intra =
				s->max_transform_hierarchy_depth_intra;
		       __entry->pcm_sample_bit_depth_luma_minus1 =
				s->pcm_sample_bit_depth_luma_minus1;
		       __entry->pcm_sample_bit_depth_chroma_minus1 =
				s->pcm_sample_bit_depth_chroma_minus1;
		       __entry->log2_min_pcm_luma_coding_block_size_minus3 =
				s->log2_min_pcm_luma_coding_block_size_minus3;
		       __entry->log2_diff_max_min_pcm_luma_coding_block_size =
				s->log2_diff_max_min_pcm_luma_coding_block_size;
		       __entry->num_short_term_ref_pic_sets = s->num_short_term_ref_pic_sets;
		       __entry->num_long_term_ref_pics_sps = s->num_long_term_ref_pics_sps;
		       __entry->chroma_format_idc = s->chroma_format_idc;
		       __entry->sps_max_sub_layers_minus1 = s->sps_max_sub_layers_minus1;
		       __entry->flags = s->flags;),
	TP_printk("tgid = %u, fd = %u, "
		  "\nvideo_parameter_set_id %u\n"
		  "seq_parameter_set_id %u\n"
		  "pic_width_in_luma_samples %u\n"
		  "pic_height_in_luma_samples %u\n"
		  "bit_depth_luma_minus8 %u\n"
		  "bit_depth_chroma_minus8 %u\n"
		  "log2_max_pic_order_cnt_lsb_minus4 %u\n"
		  "sps_max_dec_pic_buffering_minus1 %u\n"
		  "sps_max_num_reorder_pics %u\n"
		  "sps_max_latency_increase_plus1 %u\n"
		  "log2_min_luma_coding_block_size_minus3 %u\n"
		  "log2_diff_max_min_luma_coding_block_size %u\n"
		  "log2_min_luma_transform_block_size_minus2 %u\n"
		  "log2_diff_max_min_luma_transform_block_size %u\n"
		  "max_transform_hierarchy_depth_inter %u\n"
		  "max_transform_hierarchy_depth_intra %u\n"
		  "pcm_sample_bit_depth_luma_minus1 %u\n"
		  "pcm_sample_bit_depth_chroma_minus1 %u\n"
		  "log2_min_pcm_luma_coding_block_size_minus3 %u\n"
		  "log2_diff_max_min_pcm_luma_coding_block_size %u\n"
		  "num_short_term_ref_pic_sets %u\n"
		  "num_long_term_ref_pics_sps %u\n"
		  "chroma_format_idc %u\n"
		  "sps_max_sub_layers_minus1 %u\n"
		  "flags %s",
		  __entry->tgid, __entry->fd,
		  __entry->video_parameter_set_id,
		  __entry->seq_parameter_set_id,
		  __entry->pic_width_in_luma_samples,
		  __entry->pic_height_in_luma_samples,
		  __entry->bit_depth_luma_minus8,
		  __entry->bit_depth_chroma_minus8,
		  __entry->log2_max_pic_order_cnt_lsb_minus4,
		  __entry->sps_max_dec_pic_buffering_minus1,
		  __entry->sps_max_num_reorder_pics,
		  __entry->sps_max_latency_increase_plus1,
		  __entry->log2_min_luma_coding_block_size_minus3,
		  __entry->log2_diff_max_min_luma_coding_block_size,
		  __entry->log2_min_luma_transform_block_size_minus2,
		  __entry->log2_diff_max_min_luma_transform_block_size,
		  __entry->max_transform_hierarchy_depth_inter,
		  __entry->max_transform_hierarchy_depth_intra,
		  __entry->pcm_sample_bit_depth_luma_minus1,
		  __entry->pcm_sample_bit_depth_chroma_minus1,
		  __entry->log2_min_pcm_luma_coding_block_size_minus3,
		  __entry->log2_diff_max_min_pcm_luma_coding_block_size,
		  __entry->num_short_term_ref_pic_sets,
		  __entry->num_long_term_ref_pics_sps,
		  __entry->chroma_format_idc,
		  __entry->sps_max_sub_layers_minus1,
		  __print_flags(__entry->flags, "|",
		  {V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE, "SEPARATE_COLOUR_PLANE"},
		  {V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED, "SCALING_LIST_ENABLED"},
		  {V4L2_HEVC_SPS_FLAG_AMP_ENABLED, "AMP_ENABLED"},
		  {V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET, "SAMPLE_ADAPTIVE_OFFSET"},
		  {V4L2_HEVC_SPS_FLAG_PCM_ENABLED, "PCM_ENABLED"},
		  {V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED,
		   "V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED"},
		  {V4L2_HEVC_SPS_FLAG_LONG_TERM_REF_PICS_PRESENT, "LONG_TERM_REF_PICS_PRESENT"},
		  {V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED, "TEMPORAL_MVP_ENABLED"},
		  {V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED,
		   "STRONG_INTRA_SMOOTHING_ENABLED"}
	))

);


DECLARE_EVENT_CLASS(v4l2_ctrl_hevc_pps_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_pps *p),
	TP_ARGS(tgid, fd, p),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field(__u8, pic_parameter_set_id)
			 __field(__u8, num_extra_slice_header_bits)
			 __field(__u8, num_ref_idx_l0_default_active_minus1)
			 __field(__u8, num_ref_idx_l1_default_active_minus1)
			 __field(__s8, init_qp_minus26)
			 __field(__u8, diff_cu_qp_delta_depth)
			 __field(__s8, pps_cb_qp_offset)
			 __field(__s8, pps_cr_qp_offset)
			 __field(__u8, num_tile_columns_minus1)
			 __field(__u8, num_tile_rows_minus1)
			 __array(__u8, column_width_minus1, 20)
			 __array(__u8, row_height_minus1, 22)
			 __field(__s8, pps_beta_offset_div2)
			 __field(__s8, pps_tc_offset_div2)
			 __field(__u8, log2_parallel_merge_level_minus2)
			 __field(__u64, flags)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->pic_parameter_set_id = p->pic_parameter_set_id;
		       __entry->num_extra_slice_header_bits = p->num_extra_slice_header_bits;
		       __entry->num_ref_idx_l0_default_active_minus1 =
				p->num_ref_idx_l0_default_active_minus1;
		       __entry->num_ref_idx_l1_default_active_minus1 =
				p->num_ref_idx_l1_default_active_minus1;
		       __entry->init_qp_minus26 = p->init_qp_minus26;
		       __entry->diff_cu_qp_delta_depth = p->diff_cu_qp_delta_depth;
		       __entry->pps_cb_qp_offset = p->pps_cb_qp_offset;
		       __entry->pps_cr_qp_offset = p->pps_cr_qp_offset;
		       __entry->num_tile_columns_minus1 = p->num_tile_columns_minus1;
		       __entry->num_tile_rows_minus1 = p->num_tile_rows_minus1;
		       memcpy(__entry->column_width_minus1, p->column_width_minus1,
			      sizeof(__entry->column_width_minus1));
		       memcpy(__entry->row_height_minus1, p->row_height_minus1,
			      sizeof(__entry->row_height_minus1));
		       __entry->pps_beta_offset_div2 = p->pps_beta_offset_div2;
		       __entry->pps_tc_offset_div2 = p->pps_tc_offset_div2;
		       __entry->log2_parallel_merge_level_minus2 =
				p->log2_parallel_merge_level_minus2;
		       __entry->flags = p->flags;),
	TP_printk("tgid = %u, fd = %u, "
		  "\npic_parameter_set_id %u\n"
		  "num_extra_slice_header_bits %u\n"
		  "num_ref_idx_l0_default_active_minus1 %u\n"
		  "num_ref_idx_l1_default_active_minus1 %u\n"
		  "init_qp_minus26 %d\n"
		  "diff_cu_qp_delta_depth %u\n"
		  "pps_cb_qp_offset %d\n"
		  "pps_cr_qp_offset %d\n"
		  "num_tile_columns_minus1 %d\n"
		  "num_tile_rows_minus1 %d\n"
		  "column_width_minus1 %s\n"
		  "row_height_minus1 %s\n"
		  "pps_beta_offset_div2 %d\n"
		  "pps_tc_offset_div2 %d\n"
		  "log2_parallel_merge_level_minus2 %u\n"
		  "flags %s",
		  __entry->tgid, __entry->fd,
		  __entry->pic_parameter_set_id,
		  __entry->num_extra_slice_header_bits,
		  __entry->num_ref_idx_l0_default_active_minus1,
		  __entry->num_ref_idx_l1_default_active_minus1,
		  __entry->init_qp_minus26,
		  __entry->diff_cu_qp_delta_depth,
		  __entry->pps_cb_qp_offset,
		  __entry->pps_cr_qp_offset,
		  __entry->num_tile_columns_minus1,
		  __entry->num_tile_rows_minus1,
		  __print_array(__entry->column_width_minus1,
				ARRAY_SIZE(__entry->column_width_minus1),
				sizeof(__entry->column_width_minus1[0])),
		  __print_array(__entry->row_height_minus1,
				ARRAY_SIZE(__entry->row_height_minus1),
				sizeof(__entry->row_height_minus1[0])),
		  __entry->pps_beta_offset_div2,
		  __entry->pps_tc_offset_div2,
		  __entry->log2_parallel_merge_level_minus2,
		  __print_flags(__entry->flags, "|",
		  {V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED,
		   "DEPENDENT_SLICE_SEGMENT_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT, "OUTPUT_FLAG_PRESENT"},
		  {V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED, "SIGN_DATA_HIDING_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT, "CABAC_INIT_PRESENT"},
		  {V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED, "CONSTRAINED_INTRA_PRED"},
		  {V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED, "CU_QP_DELTA_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT,
		   "PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT"},
		  {V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED, "WEIGHTED_PRED"},
		  {V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED, "WEIGHTED_BIPRED"},
		  {V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED, "TRANSQUANT_BYPASS_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_TILES_ENABLED, "TILES_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED, "ENTROPY_CODING_SYNC_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED,
		   "LOOP_FILTER_ACROSS_TILES_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED,
		   "PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED,
		   "DEBLOCKING_FILTER_OVERRIDE_ENABLED"},
		  {V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER, "DISABLE_DEBLOCKING_FILTER"},
		  {V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT, "LISTS_MODIFICATION_PRESENT"},
		  {V4L2_HEVC_PPS_FLAG_SLICE_SEGMENT_HEADER_EXTENSION_PRESENT,
		   "SLICE_SEGMENT_HEADER_EXTENSION_PRESENT"},
		  {V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT,
		   "DEBLOCKING_FILTER_CONTROL_PRESENT"},
		  {V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING, "UNIFORM_SPACING"}
	))

);



DECLARE_EVENT_CLASS(v4l2_ctrl_hevc_slice_params_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_slice_params *s),
	TP_ARGS(tgid, fd, s),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field(__u32, bit_size)
			 __field(__u32, data_byte_offset)
			 __field(__u32, num_entry_point_offsets)
			 __field(__u8, nal_unit_type)
			 __field(__u8, nuh_temporal_id_plus1)
			 __field(__u8, slice_type)
			 __field(__u8, colour_plane_id)
			 __field(__s32, slice_pic_order_cnt)
			 __field(__u8, num_ref_idx_l0_active_minus1)
			 __field(__u8, num_ref_idx_l1_active_minus1)
			 __field(__u8, collocated_ref_idx)
			 __field(__u8, five_minus_max_num_merge_cand)
			 __field(__s8, slice_qp_delta)
			 __field(__s8, slice_cb_qp_offset)
			 __field(__s8, slice_cr_qp_offset)
			 __field(__s8, slice_act_y_qp_offset)
			 __field(__s8, slice_act_cb_qp_offset)
			 __field(__s8, slice_act_cr_qp_offset)
			 __field(__s8, slice_beta_offset_div2)
			 __field(__s8, slice_tc_offset_div2)
			 __field(__u8, pic_struct)
			 __field(__u32, slice_segment_addr)
			 __array(__u8, ref_idx_l0, V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
			 __array(__u8, ref_idx_l1, V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
			 __field(__u16, short_term_ref_pic_set_size)
			 __field(__u16, long_term_ref_pic_set_size)
			 __field(__u64, flags)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->bit_size = s->bit_size;
		       __entry->data_byte_offset = s->data_byte_offset;
		       __entry->num_entry_point_offsets = s->num_entry_point_offsets;
		       __entry->nal_unit_type = s->nal_unit_type;
		       __entry->nuh_temporal_id_plus1 = s->nuh_temporal_id_plus1;
		       __entry->slice_type = s->slice_type;
		       __entry->colour_plane_id = s->colour_plane_id;
		       __entry->slice_pic_order_cnt = s->slice_pic_order_cnt;
		       __entry->num_ref_idx_l0_active_minus1 = s->num_ref_idx_l0_active_minus1;
		       __entry->num_ref_idx_l1_active_minus1 = s->num_ref_idx_l1_active_minus1;
		       __entry->collocated_ref_idx = s->collocated_ref_idx;
		       __entry->five_minus_max_num_merge_cand = s->five_minus_max_num_merge_cand;
		       __entry->slice_qp_delta = s->slice_qp_delta;
		       __entry->slice_cb_qp_offset = s->slice_cb_qp_offset;
		       __entry->slice_cr_qp_offset = s->slice_cr_qp_offset;
		       __entry->slice_act_y_qp_offset = s->slice_act_y_qp_offset;
		       __entry->slice_act_cb_qp_offset = s->slice_act_cb_qp_offset;
		       __entry->slice_act_cr_qp_offset = s->slice_act_cr_qp_offset;
		       __entry->slice_beta_offset_div2 = s->slice_beta_offset_div2;
		       __entry->slice_tc_offset_div2 = s->slice_tc_offset_div2;
		       __entry->pic_struct = s->pic_struct;
		       __entry->slice_segment_addr = s->slice_segment_addr;
		       memcpy(__entry->ref_idx_l0, s->ref_idx_l0, sizeof(__entry->ref_idx_l0));
		       memcpy(__entry->ref_idx_l1, s->ref_idx_l1, sizeof(__entry->ref_idx_l1));
		       __entry->short_term_ref_pic_set_size = s->short_term_ref_pic_set_size;
		       __entry->long_term_ref_pic_set_size = s->long_term_ref_pic_set_size;
		       __entry->flags = s->flags;),
	TP_printk("tgid = %u, fd = %u, "
		  "\nbit_size %u\n"
		  "data_byte_offset %u\n"
		  "num_entry_point_offsets %u\n"
		  "nal_unit_type %u\n"
		  "nuh_temporal_id_plus1 %u\n"
		  "slice_type %u\n"
		  "colour_plane_id %u\n"
		  "slice_pic_order_cnt %d\n"
		  "num_ref_idx_l0_active_minus1 %u\n"
		  "num_ref_idx_l1_active_minus1 %u\n"
		  "collocated_ref_idx %u\n"
		  "five_minus_max_num_merge_cand %u\n"
		  "slice_qp_delta %d\n"
		  "slice_cb_qp_offset %d\n"
		  "slice_cr_qp_offset %d\n"
		  "slice_act_y_qp_offset %d\n"
		  "slice_act_cb_qp_offset %d\n"
		  "slice_act_cr_qp_offset %d\n"
		  "slice_beta_offset_div2 %d\n"
		  "slice_tc_offset_div2 %d\n"
		  "pic_struct %u\n"
		  "slice_segment_addr %u\n"
		  "ref_idx_l0 %s\n"
		  "ref_idx_l1 %s\n"
		  "short_term_ref_pic_set_size %u\n"
		  "long_term_ref_pic_set_size %u\n"
		  "flags %s",
		  __entry->tgid, __entry->fd,
		  __entry->bit_size,
		  __entry->data_byte_offset,
		  __entry->num_entry_point_offsets,
		  __entry->nal_unit_type,
		  __entry->nuh_temporal_id_plus1,
		  __entry->slice_type,
		  __entry->colour_plane_id,
		  __entry->slice_pic_order_cnt,
		  __entry->num_ref_idx_l0_active_minus1,
		  __entry->num_ref_idx_l1_active_minus1,
		  __entry->collocated_ref_idx,
		  __entry->five_minus_max_num_merge_cand,
		  __entry->slice_qp_delta,
		  __entry->slice_cb_qp_offset,
		  __entry->slice_cr_qp_offset,
		  __entry->slice_act_y_qp_offset,
		  __entry->slice_act_cb_qp_offset,
		  __entry->slice_act_cr_qp_offset,
		  __entry->slice_beta_offset_div2,
		  __entry->slice_tc_offset_div2,
		  __entry->pic_struct,
		  __entry->slice_segment_addr,
		  __print_array(__entry->ref_idx_l0,
				ARRAY_SIZE(__entry->ref_idx_l0),
				sizeof(__entry->ref_idx_l0[0])),
		  __print_array(__entry->ref_idx_l1,
				ARRAY_SIZE(__entry->ref_idx_l1),
				sizeof(__entry->ref_idx_l1[0])),
		  __entry->short_term_ref_pic_set_size,
		  __entry->long_term_ref_pic_set_size,
		  __print_flags(__entry->flags, "|",
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA, "SLICE_SAO_LUMA"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA, "SLICE_SAO_CHROMA"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED,
		   "SLICE_TEMPORAL_MVP_ENABLED"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO, "MVD_L1_ZERO"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT, "CABAC_INIT"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0, "COLLOCATED_FROM_L0"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_USE_INTEGER_MV, "USE_INTEGER_MV"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED,
		   "SLICE_DEBLOCKING_FILTER_DISABLED"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED,
		   "SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED"},
		  {V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT, "DEPENDENT_SLICE_SEGMENT"}

	))
);

DECLARE_EVENT_CLASS(v4l2_hevc_pred_weight_table_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_hevc_pred_weight_table *p),
	TP_ARGS(tgid, fd, p),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __array(__s8, delta_luma_weight_l0, V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
			 __array(__s8, luma_offset_l0, V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
			 __array(__s8, delta_chroma_weight_l0, V4L2_HEVC_DPB_ENTRIES_NUM_MAX * 2)
			 __array(__s8, chroma_offset_l0, V4L2_HEVC_DPB_ENTRIES_NUM_MAX * 2)
			 __array(__s8, delta_luma_weight_l1, V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
			 __array(__s8, luma_offset_l1, V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
			 __array(__s8, delta_chroma_weight_l1, V4L2_HEVC_DPB_ENTRIES_NUM_MAX * 2)
			 __array(__s8, chroma_offset_l1, V4L2_HEVC_DPB_ENTRIES_NUM_MAX * 2)
			 __field(__u8, luma_log2_weight_denom)
			 __field(__s8, delta_chroma_log2_weight_denom)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       memcpy(__entry->delta_luma_weight_l0, p->delta_luma_weight_l0,
			      sizeof(__entry->delta_luma_weight_l0));
		       memcpy(__entry->luma_offset_l0, p->luma_offset_l0,
			      sizeof(__entry->luma_offset_l0));
		       memcpy(__entry->delta_chroma_weight_l0, p->delta_chroma_weight_l0,
			      sizeof(__entry->delta_chroma_weight_l0));
		       memcpy(__entry->chroma_offset_l0, p->chroma_offset_l0,
			      sizeof(__entry->chroma_offset_l0));
		       memcpy(__entry->delta_luma_weight_l1, p->delta_luma_weight_l1,
			      sizeof(__entry->delta_luma_weight_l1));
		       memcpy(__entry->luma_offset_l1, p->luma_offset_l1,
			      sizeof(__entry->luma_offset_l1));
		       memcpy(__entry->delta_chroma_weight_l1, p->delta_chroma_weight_l1,
			      sizeof(__entry->delta_chroma_weight_l1));
		       memcpy(__entry->chroma_offset_l1, p->chroma_offset_l1,
			      sizeof(__entry->chroma_offset_l1));
		       __entry->luma_log2_weight_denom = p->luma_log2_weight_denom;
		       __entry->delta_chroma_log2_weight_denom =
				p->delta_chroma_log2_weight_denom;),
	TP_printk("tgid = %u, fd = %u, "
		  "\ndelta_luma_weight_l0 %s\n"
		  "luma_offset_l0 %s\n"
		  "delta_chroma_weight_l0 {%s}\n"
		  "chroma_offset_l0 {%s}\n"
		  "delta_luma_weight_l1 %s\n"
		  "luma_offset_l1 %s\n"
		  "delta_chroma_weight_l1 {%s}\n"
		  "chroma_offset_l1 {%s}\n"
		  "luma_log2_weight_denom %d\n"
		  "delta_chroma_log2_weight_denom %d\n",
		  __entry->tgid, __entry->fd,
		  __print_array(__entry->delta_luma_weight_l0,
				ARRAY_SIZE(__entry->delta_luma_weight_l0),
				sizeof(__entry->delta_luma_weight_l0[0])),
		  __print_array(__entry->luma_offset_l0,
				ARRAY_SIZE(__entry->luma_offset_l0),
				sizeof(__entry->luma_offset_l0[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->delta_chroma_weight_l0,
				   sizeof(__entry->delta_chroma_weight_l0),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->chroma_offset_l0,
				   sizeof(__entry->chroma_offset_l0),
				   false),
		  __print_array(__entry->delta_luma_weight_l1,
				ARRAY_SIZE(__entry->delta_luma_weight_l1),
				sizeof(__entry->delta_luma_weight_l1[0])),
		  __print_array(__entry->luma_offset_l1,
				ARRAY_SIZE(__entry->luma_offset_l1),
				sizeof(__entry->luma_offset_l1[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->delta_chroma_weight_l1,
				   sizeof(__entry->delta_chroma_weight_l1),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->chroma_offset_l1,
				   sizeof(__entry->chroma_offset_l1),
				   false),
		__entry->luma_log2_weight_denom,
		__entry->delta_chroma_log2_weight_denom

	))

DECLARE_EVENT_CLASS(v4l2_ctrl_hevc_scaling_matrix_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_scaling_matrix *s),
	TP_ARGS(tgid, fd, s),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __array(__u8, scaling_list_4x4, 6 * 16)
			 __array(__u8, scaling_list_8x8, 6 * 64)
			 __array(__u8, scaling_list_16x16, 6 * 64)
			 __array(__u8, scaling_list_32x32, 2 * 64)
			 __array(__u8, scaling_list_dc_coef_16x16, 6)
			 __array(__u8, scaling_list_dc_coef_32x32, 2)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       memcpy(__entry->scaling_list_4x4,
			      s->scaling_list_4x4, sizeof(__entry->scaling_list_4x4));
		       memcpy(__entry->scaling_list_8x8,
			      s->scaling_list_8x8, sizeof(__entry->scaling_list_8x8));
		       memcpy(__entry->scaling_list_16x16,
			      s->scaling_list_16x16, sizeof(__entry->scaling_list_16x16));
		       memcpy(__entry->scaling_list_32x32,
			      s->scaling_list_32x32, sizeof(__entry->scaling_list_32x32));
		       memcpy(__entry->scaling_list_dc_coef_16x16,
			      s->scaling_list_dc_coef_16x16,
			      sizeof(__entry->scaling_list_dc_coef_16x16));
		       memcpy(__entry->scaling_list_dc_coef_32x32,
			      s->scaling_list_dc_coef_32x32,
			      sizeof(__entry->scaling_list_dc_coef_32x32));),
	TP_printk("tgid = %u, fd = %u, "
		  "\nscaling_list_4x4 {%s}\n"
		  "scaling_list_8x8 {%s}\n"
		  "scaling_list_16x16 {%s}\n"
		  "scaling_list_32x32 {%s}\n"
		  "scaling_list_dc_coef_16x16 %s\n"
		  "scaling_list_dc_coef_32x32 %s\n",
		  __entry->tgid, __entry->fd,
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->scaling_list_4x4,
				   sizeof(__entry->scaling_list_4x4),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->scaling_list_8x8,
				   sizeof(__entry->scaling_list_8x8),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->scaling_list_16x16,
				   sizeof(__entry->scaling_list_16x16),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->scaling_list_32x32,
				   sizeof(__entry->scaling_list_32x32),
				   false),
		  __print_array(__entry->scaling_list_dc_coef_16x16,
				ARRAY_SIZE(__entry->scaling_list_dc_coef_16x16),
				sizeof(__entry->scaling_list_dc_coef_16x16[0])),
		  __print_array(__entry->scaling_list_dc_coef_32x32,
				ARRAY_SIZE(__entry->scaling_list_dc_coef_32x32),
				sizeof(__entry->scaling_list_dc_coef_32x32[0]))
	))

DECLARE_EVENT_CLASS(v4l2_ctrl_hevc_decode_params_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_decode_params *d),
	TP_ARGS(tgid, fd, d),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field(__s32, pic_order_cnt_val)
			 __field(__u16, short_term_ref_pic_set_size)
			 __field(__u16, long_term_ref_pic_set_size)
			 __field(__u8, num_active_dpb_entries)
			 __field(__u8, num_poc_st_curr_before)
			 __field(__u8, num_poc_st_curr_after)
			 __field(__u8, num_poc_lt_curr)
			 __array(__u8, poc_st_curr_before, V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
			 __array(__u8, poc_st_curr_after, V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
			 __array(__u8, poc_lt_curr, V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
			 __field(__u64, flags)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->pic_order_cnt_val = d->pic_order_cnt_val;
		       __entry->short_term_ref_pic_set_size = d->short_term_ref_pic_set_size;
		       __entry->long_term_ref_pic_set_size = d->long_term_ref_pic_set_size;
		       __entry->num_active_dpb_entries = d->num_active_dpb_entries;
		       __entry->num_poc_st_curr_before = d->num_poc_st_curr_before;
		       __entry->num_poc_st_curr_after = d->num_poc_st_curr_after;
		       __entry->num_poc_lt_curr = d->num_poc_lt_curr;
		       memcpy(__entry->poc_st_curr_before, d->poc_st_curr_before,
			      sizeof(__entry->poc_st_curr_before));
		       memcpy(__entry->poc_st_curr_after, d->poc_st_curr_after,
			      sizeof(__entry->poc_st_curr_after));
		       memcpy(__entry->poc_lt_curr, d->poc_lt_curr, sizeof(__entry->poc_lt_curr));
		       __entry->flags = d->flags;),
	TP_printk("tgid = %u, fd = %u, "
		  "\npic_order_cnt_val %d\n"
		  "short_term_ref_pic_set_size %u\n"
		  "long_term_ref_pic_set_size %u\n"
		  "num_active_dpb_entries %u\n"
		  "num_poc_st_curr_before %u\n"
		  "num_poc_st_curr_after %u\n"
		  "num_poc_lt_curr %u\n"
		  "poc_st_curr_before %s\n"
		  "poc_st_curr_after %s\n"
		  "poc_lt_curr %s\n"
		  "flags %s",
		  __entry->tgid, __entry->fd,
		  __entry->pic_order_cnt_val,
		  __entry->short_term_ref_pic_set_size,
		  __entry->long_term_ref_pic_set_size,
		  __entry->num_active_dpb_entries,
		  __entry->num_poc_st_curr_before,
		  __entry->num_poc_st_curr_after,
		  __entry->num_poc_lt_curr,
		  __print_array(__entry->poc_st_curr_before,
				ARRAY_SIZE(__entry->poc_st_curr_before),
				sizeof(__entry->poc_st_curr_before[0])),
		  __print_array(__entry->poc_st_curr_after,
				ARRAY_SIZE(__entry->poc_st_curr_after),
				sizeof(__entry->poc_st_curr_after[0])),
		  __print_array(__entry->poc_lt_curr,
				ARRAY_SIZE(__entry->poc_lt_curr),
				sizeof(__entry->poc_lt_curr[0])),
		  __print_flags(__entry->flags, "|",
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
			 __field(__u8, flags)
			 __field(__u32, lt_ref_pic_poc_lsb_sps)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->flags = lt->flags;
		       __entry->lt_ref_pic_poc_lsb_sps = lt->lt_ref_pic_poc_lsb_sps;),
	TP_printk("tgid = %u, fd = %u, "
		  "\nflags %s\n"
		  "lt_ref_pic_poc_lsb_sps %x\n",
		  __entry->tgid, __entry->fd,
		  __print_flags(__entry->flags, "|",
		  {V4L2_HEVC_EXT_SPS_LT_RPS_FLAG_USED_LT, "USED_LT"}
		  ),
		  __entry->lt_ref_pic_poc_lsb_sps
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_hevc_ext_sps_st_rps_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_hevc_ext_sps_st_rps *st),
	TP_ARGS(tgid, fd, st),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field(__u8, flags)
			 __field(__u8, delta_idx_minus1)
			 __field(__u8, delta_rps_sign)
			 __field(__u16, abs_delta_rps_minus1)
			 __field(__u8, num_negative_pics)
			 __field(__u8, num_positive_pics)
			 __field(__u32, used_by_curr_pic)
			 __field(__u32, use_delta_flag)
			 __array(__u32, delta_poc_s0_minus1, 16)
			 __array(__u32, delta_poc_s1_minus1, 16)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->flags = st->flags;
		       __entry->delta_idx_minus1 = st->delta_idx_minus1;
		       __entry->delta_rps_sign = st->delta_rps_sign;
		       __entry->abs_delta_rps_minus1 = st->abs_delta_rps_minus1;
		       __entry->num_negative_pics = st->num_negative_pics;
		       __entry->num_positive_pics = st->num_positive_pics;
		       __entry->used_by_curr_pic = st->used_by_curr_pic;
		       __entry->use_delta_flag = st->use_delta_flag;
		       memcpy(__entry->delta_poc_s0_minus1, st->delta_poc_s0_minus1,
			      sizeof(__entry->delta_poc_s0_minus1));
		       memcpy(__entry->delta_poc_s1_minus1, st->delta_poc_s1_minus1,
			      sizeof(__entry->delta_poc_s1_minus1));),
	TP_printk("tgid = %u, fd = %u, "
		  "\nflags %s\n"
		  "delta_idx_minus1: %u\n"
		  "delta_rps_sign: %u\n"
		  "abs_delta_rps_minus1: %u\n"
		  "num_negative_pics: %u\n"
		  "num_positive_pics: %u\n"
		  "used_by_curr_pic: %08x\n"
		  "use_delta_flag: %08x\n"
		  "delta_poc_s0_minus1: %s\n"
		  "delta_poc_s1_minus1: %s\n",
		  __entry->tgid, __entry->fd,
		  __print_flags(__entry->flags, "|",
		  {V4L2_HEVC_EXT_SPS_ST_RPS_FLAG_INTER_REF_PIC_SET_PRED, "INTER_REF_PIC_SET_PRED"}
		  ),
		  __entry->delta_idx_minus1,
		  __entry->delta_rps_sign,
		  __entry->abs_delta_rps_minus1,
		  __entry->num_negative_pics,
		  __entry->num_positive_pics,
		  __entry->used_by_curr_pic,
		  __entry->use_delta_flag,
		  __print_array(__entry->delta_poc_s0_minus1,
				ARRAY_SIZE(__entry->delta_poc_s0_minus1),
				sizeof(__entry->delta_poc_s0_minus1[0])),
		  __print_array(__entry->delta_poc_s1_minus1,
				ARRAY_SIZE(__entry->delta_poc_s1_minus1),
				sizeof(__entry->delta_poc_s1_minus1[0]))
	)
);

DECLARE_EVENT_CLASS(v4l2_hevc_dpb_entry_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_hevc_dpb_entry *e),
	TP_ARGS(tgid, fd, e),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field(__u64, timestamp)
			 __field(__u8, flags)
			 __field(__u8, field_pic)
			 __field(__s32, pic_order_cnt_val)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->timestamp = e->timestamp;
		       __entry->flags = e->flags;
		       __entry->field_pic = e->field_pic;
		       __entry->pic_order_cnt_val = e->pic_order_cnt_val;),
	TP_printk("tgid = %u, fd = %u, "
		  "\ntimestamp %llu\n"
		  "flags %s\n"
		  "field_pic %u\n"
		  "pic_order_cnt_val %d\n",
		  __entry->tgid, __entry->fd,
		__entry->timestamp,
		__print_flags(__entry->flags, "|",
		{V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE, "LONG_TERM_REFERENCE"}
		  ),
		__entry->field_pic,
		__entry->pic_order_cnt_val
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
			 __field(__u16, horizontal_size)
			 __field(__u16, vertical_size)
			 __field(__u32, vbv_buffer_size)
			 __field(__u16, profile_and_level_indication)
			 __field(__u8, chroma_format)
			 __field(__u8, flags)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->horizontal_size = s->horizontal_size;
		       __entry->vertical_size = s->vertical_size;
		       __entry->vbv_buffer_size = s->vbv_buffer_size;
		       __entry->profile_and_level_indication = s->profile_and_level_indication;
		       __entry->chroma_format = s->chroma_format;
		       __entry->flags = s->flags;),
	TP_printk("tgid = %u, fd = %u, "
		  "\nhorizontal_size %u\nvertical_size %u\nvbv_buffer_size %u\n"
		  "profile_and_level_indication %u\nchroma_format %u\nflags %s\n",
		  __entry->tgid, __entry->fd,
		  __entry->horizontal_size,
		  __entry->vertical_size,
		  __entry->vbv_buffer_size,
		  __entry->profile_and_level_indication,
		  __entry->chroma_format,
		  __print_flags(__entry->flags, "|",
		  {V4L2_MPEG2_SEQ_FLAG_PROGRESSIVE, "PROGRESSIVE"})
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_mpeg2_pic_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_mpeg2_picture *p),
	TP_ARGS(tgid, fd, p),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field(__u64, backward_ref_ts)
			 __field(__u64, forward_ref_ts)
			 __field(__u32, flags)
			 __array(__u8, f_code, 4)
			 __field(__u8, picture_coding_type)
			 __field(__u8, picture_structure)
			 __field(__u8, intra_dc_precision)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->backward_ref_ts = p->backward_ref_ts;
		       __entry->forward_ref_ts = p->forward_ref_ts;
		       __entry->flags = p->flags;
		       memcpy(__entry->f_code, p->f_code, sizeof(__entry->f_code));
		       __entry->picture_coding_type = p->picture_coding_type;
		       __entry->picture_structure = p->picture_structure;
		       __entry->intra_dc_precision = p->intra_dc_precision;),
	TP_printk("tgid = %u, fd = %u, "
		  "\nbackward_ref_ts %llu\nforward_ref_ts %llu\nflags %s\nf_code {%s}\n"
		  "picture_coding_type: %u\npicture_structure %u\nintra_dc_precision %u\n",
		  __entry->tgid, __entry->fd,
		  __entry->backward_ref_ts,
		  __entry->forward_ref_ts,
		  __print_flags(__entry->flags, "|",
		  {V4L2_MPEG2_PIC_FLAG_TOP_FIELD_FIRST, "TOP_FIELD_FIRST"},
		  {V4L2_MPEG2_PIC_FLAG_FRAME_PRED_DCT, "FRAME_PRED_DCT"},
		  {V4L2_MPEG2_PIC_FLAG_CONCEALMENT_MV, "CONCEALMENT_MV"},
		  {V4L2_MPEG2_PIC_FLAG_Q_SCALE_TYPE, "Q_SCALE_TYPE"},
		  {V4L2_MPEG2_PIC_FLAG_INTRA_VLC, "INTA_VLC"},
		  {V4L2_MPEG2_PIC_FLAG_ALT_SCAN, "ALT_SCAN"},
		  {V4L2_MPEG2_PIC_FLAG_REPEAT_FIRST, "REPEAT_FIRST"},
		  {V4L2_MPEG2_PIC_FLAG_PROGRESSIVE, "PROGRESSIVE"}),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->f_code,
				   sizeof(__entry->f_code),
				   false),
		  __entry->picture_coding_type,
		  __entry->picture_structure,
		  __entry->intra_dc_precision
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_mpeg2_quant_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_mpeg2_quantisation *q),
	TP_ARGS(tgid, fd, q),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __array(__u8, intra_quantiser_matrix, 64)
			 __array(__u8, non_intra_quantiser_matrix, 64)
			 __array(__u8, chroma_intra_quantiser_matrix, 64)
			 __array(__u8, chroma_non_intra_quantiser_matrix, 64)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       memcpy(__entry->intra_quantiser_matrix, q->intra_quantiser_matrix,
			      sizeof(__entry->intra_quantiser_matrix));
		       memcpy(__entry->non_intra_quantiser_matrix, q->non_intra_quantiser_matrix,
			      sizeof(__entry->non_intra_quantiser_matrix));
		       memcpy(__entry->chroma_intra_quantiser_matrix,
			      q->chroma_intra_quantiser_matrix,
			      sizeof(__entry->chroma_intra_quantiser_matrix));
		       memcpy(__entry->chroma_non_intra_quantiser_matrix,
			      q->chroma_non_intra_quantiser_matrix,
			      sizeof(__entry->chroma_non_intra_quantiser_matrix));),
	TP_printk("tgid = %u, fd = %u, "
		  "\nintra_quantiser_matrix %s\nnon_intra_quantiser_matrix %s\n"
		  "chroma_intra_quantiser_matrix %s\nchroma_non_intra_quantiser_matrix %s\n",
		  __entry->tgid, __entry->fd,
		  __print_array(__entry->intra_quantiser_matrix,
				ARRAY_SIZE(__entry->intra_quantiser_matrix),
				sizeof(__entry->intra_quantiser_matrix[0])),
		  __print_array(__entry->non_intra_quantiser_matrix,
				ARRAY_SIZE(__entry->non_intra_quantiser_matrix),
				sizeof(__entry->non_intra_quantiser_matrix[0])),
		  __print_array(__entry->chroma_intra_quantiser_matrix,
				ARRAY_SIZE(__entry->chroma_intra_quantiser_matrix),
				sizeof(__entry->chroma_intra_quantiser_matrix[0])),
		  __print_array(__entry->chroma_non_intra_quantiser_matrix,
				ARRAY_SIZE(__entry->chroma_non_intra_quantiser_matrix),
				sizeof(__entry->chroma_non_intra_quantiser_matrix[0]))
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
			 __array(__u8, entropy_coeff_probs, 4 * 8 * 3 * V4L2_VP8_COEFF_PROB_CNT)
			 __array(__u8, entropy_y_mode_probs, 4)
			 __array(__u8, entropy_uv_mode_probs, 3)
			 __array(__u8, entropy_mv_probs, 2 * 19)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       memcpy(__entry->entropy_coeff_probs, f->entropy.coeff_probs,
			      sizeof(__entry->entropy_coeff_probs));
		       memcpy(__entry->entropy_y_mode_probs, f->entropy.y_mode_probs,
			      sizeof(__entry->entropy_y_mode_probs));
		       memcpy(__entry->entropy_uv_mode_probs, f->entropy.uv_mode_probs,
			      sizeof(__entry->entropy_uv_mode_probs));
		       memcpy(__entry->entropy_mv_probs, f->entropy.mv_probs,
			      sizeof(__entry->entropy_mv_probs));),
	TP_printk("tgid = %u, fd = %u, "
		  "\nentropy.coeff_probs {%s}\n"
		  "entropy.y_mode_probs %s\n"
		  "entropy.uv_mode_probs %s\n"
		  "entropy.mv_probs {%s}",
		  __entry->tgid, __entry->fd,
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->entropy_coeff_probs,
				   sizeof(__entry->entropy_coeff_probs),
				   false),
		  __print_array(__entry->entropy_y_mode_probs,
				ARRAY_SIZE(__entry->entropy_y_mode_probs),
				sizeof(__entry->entropy_y_mode_probs[0])),
		  __print_array(__entry->entropy_uv_mode_probs,
				ARRAY_SIZE(__entry->entropy_uv_mode_probs),
				sizeof(__entry->entropy_uv_mode_probs[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->entropy_mv_probs,
				   sizeof(__entry->entropy_mv_probs),
				   false)
		  )
)

DECLARE_EVENT_CLASS(v4l2_ctrl_vp8_frame_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_vp8_frame *f),
	TP_ARGS(tgid, fd, f),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __array(__s8, segment_quant_update, 4)
			 __array(__s8, segment_lf_update, 4)
			 __array(__u8, segment_segment_probs, 3)
			 __field(__u32, segment_flags)
			 __array(__s8, lf_ref_frm_delta, 4)
			 __array(__s8, lf_mb_mode_delta, 4)
			 __field(__u8, lf_sharpness_level)
			 __field(__u8, lf_level)
			 __field(__u32, lf_flags)
			 __field(__u8, quant_y_ac_qi)
			 __field(__s8, quant_y_dc_delta)
			 __field(__s8, quant_y2_dc_delta)
			 __field(__s8, quant_y2_ac_delta)
			 __field(__s8, quant_uv_dc_delta)
			 __field(__s8, quant_uv_ac_delta)
			 __field(__u8, coder_state_range)
			 __field(__u8, coder_state_value)
			 __field(__u8, coder_state_bit_count)
			 __field(__u16, width)
			 __field(__u16, height)
			 __field(__u8, horizontal_scale)
			 __field(__u8, vertical_scale)
			 __field(__u8, version)
			 __field(__u8, prob_skip_false)
			 __field(__u8, prob_intra)
			 __field(__u8, prob_last)
			 __field(__u8, prob_gf)
			 __field(__u8, num_dct_parts)
			 __field(__u32, first_part_size)
			 __field(__u32, first_part_header_bits)
			 __array(__u32, dct_part_sizes, 8)
			 __field(__u64, last_frame_ts)
			 __field(__u64, golden_frame_ts)
			 __field(__u64, alt_frame_ts)
			 __field(__u64, flags)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       memcpy(__entry->segment_quant_update, f->segment.quant_update,
			      sizeof(__entry->segment_quant_update));
		       memcpy(__entry->segment_lf_update, f->segment.lf_update,
			      sizeof(__entry->segment_lf_update));
		       memcpy(__entry->segment_segment_probs, f->segment.segment_probs,
			      sizeof(__entry->segment_segment_probs));
		       __entry->segment_flags = f->segment.flags;
		       memcpy(__entry->lf_ref_frm_delta, f->lf.ref_frm_delta,
			      sizeof(__entry->lf_ref_frm_delta));
		       memcpy(__entry->lf_mb_mode_delta, f->lf.mb_mode_delta,
			      sizeof(__entry->lf_mb_mode_delta));
		       __entry->lf_sharpness_level = f->lf.sharpness_level;
		       __entry->lf_level = f->lf.level;
		       __entry->lf_flags = f->lf.flags;
		       __entry->quant_y_ac_qi = f->quant.y_ac_qi;
		       __entry->quant_y_dc_delta = f->quant.y_dc_delta;
		       __entry->quant_y2_dc_delta = f->quant.y2_dc_delta;
		       __entry->quant_y2_ac_delta = f->quant.y2_ac_delta;
		       __entry->quant_uv_dc_delta = f->quant.uv_dc_delta;
		       __entry->quant_uv_ac_delta = f->quant.uv_ac_delta;
		       __entry->coder_state_range = f->coder_state.range;
		       __entry->coder_state_value = f->coder_state.value;
		       __entry->coder_state_bit_count = f->coder_state.bit_count;
		       __entry->width = f->width;
		       __entry->height = f->height;
		       __entry->horizontal_scale = f->horizontal_scale;
		       __entry->vertical_scale = f->vertical_scale;
		       __entry->version = f->version;
		       __entry->prob_skip_false = f->prob_skip_false;
		       __entry->prob_intra = f->prob_intra;
		       __entry->prob_last = f->prob_last;
		       __entry->prob_gf = f->prob_gf;
		       __entry->num_dct_parts = f->num_dct_parts;
		       __entry->first_part_size = f->first_part_size;
		       __entry->first_part_header_bits = f->first_part_header_bits;
		       memcpy(__entry->dct_part_sizes, f->dct_part_sizes,
			      sizeof(__entry->dct_part_sizes));
		       __entry->last_frame_ts = f->last_frame_ts;
		       __entry->golden_frame_ts = f->golden_frame_ts;
		       __entry->alt_frame_ts = f->alt_frame_ts;
		       __entry->flags = f->flags;),
	TP_printk("tgid = %u, fd = %u, "
		  "\nsegment.quant_update %s\n"
		  "segment.lf_update %s\n"
		  "segment.segment_probs %s\n"
		  "segment.flags %s\n"
		  "lf.ref_frm_delta %s\n"
		  "lf.mb_mode_delta %s\n"
		  "lf.sharpness_level %u\n"
		  "lf.level %u\n"
		  "lf.flags %s\n"
		  "quant.y_ac_qi %u\n"
		  "quant.y_dc_delta %d\n"
		  "quant.y2_dc_delta %d\n"
		  "quant.y2_ac_delta %d\n"
		  "quant.uv_dc_delta %d\n"
		  "quant.uv_ac_delta %d\n"
		  "coder_state.range %u\n"
		  "coder_state.value %u\n"
		  "coder_state.bit_count %u\n"
		  "width %u\n"
		  "height %u\n"
		  "horizontal_scale %u\n"
		  "vertical_scale %u\n"
		  "version %u\n"
		  "prob_skip_false %u\n"
		  "prob_intra %u\n"
		  "prob_last %u\n"
		  "prob_gf %u\n"
		  "num_dct_parts %u\n"
		  "first_part_size %u\n"
		  "first_part_header_bits %u\n"
		  "dct_part_sizes %s\n"
		  "last_frame_ts %llu\n"
		  "golden_frame_ts %llu\n"
		  "alt_frame_ts %llu\n"
		  "flags %s",
		  __entry->tgid, __entry->fd,
		  __print_array(__entry->segment_quant_update,
				ARRAY_SIZE(__entry->segment_quant_update),
				sizeof(__entry->segment_quant_update[0])),
		  __print_array(__entry->segment_lf_update,
				ARRAY_SIZE(__entry->segment_lf_update),
				sizeof(__entry->segment_lf_update[0])),
		  __print_array(__entry->segment_segment_probs,
				ARRAY_SIZE(__entry->segment_segment_probs),
				sizeof(__entry->segment_segment_probs[0])),
		  __print_flags(__entry->segment_flags, "|",
		  {V4L2_VP8_SEGMENT_FLAG_ENABLED, "SEGMENT_ENABLED"},
		  {V4L2_VP8_SEGMENT_FLAG_UPDATE_MAP, "SEGMENT_UPDATE_MAP"},
		  {V4L2_VP8_SEGMENT_FLAG_UPDATE_FEATURE_DATA, "SEGMENT_UPDATE_FEATURE_DATA"},
		  {V4L2_VP8_SEGMENT_FLAG_DELTA_VALUE_MODE, "SEGMENT_DELTA_VALUE_MODE"}),
		  __print_array(__entry->lf_ref_frm_delta,
				ARRAY_SIZE(__entry->lf_ref_frm_delta),
				sizeof(__entry->lf_ref_frm_delta[0])),
		  __print_array(__entry->lf_mb_mode_delta,
				ARRAY_SIZE(__entry->lf_mb_mode_delta),
				sizeof(__entry->lf_mb_mode_delta[0])),
		  __entry->lf_sharpness_level,
		  __entry->lf_level,
		  __print_flags(__entry->lf_flags, "|",
		  {V4L2_VP8_LF_ADJ_ENABLE, "LF_ADJ_ENABLED"},
		  {V4L2_VP8_LF_DELTA_UPDATE, "LF_DELTA_UPDATE"},
		  {V4L2_VP8_LF_FILTER_TYPE_SIMPLE, "LF_FILTER_TYPE_SIMPLE"}),
		  __entry->quant_y_ac_qi,
		  __entry->quant_y_dc_delta,
		  __entry->quant_y2_dc_delta,
		  __entry->quant_y2_ac_delta,
		  __entry->quant_uv_dc_delta,
		  __entry->quant_uv_ac_delta,
		  __entry->coder_state_range,
		  __entry->coder_state_value,
		  __entry->coder_state_bit_count,
		  __entry->width,
		  __entry->height,
		  __entry->horizontal_scale,
		  __entry->vertical_scale,
		  __entry->version,
		  __entry->prob_skip_false,
		  __entry->prob_intra,
		  __entry->prob_last,
		  __entry->prob_gf,
		  __entry->num_dct_parts,
		  __entry->first_part_size,
		  __entry->first_part_header_bits,
		  __print_array(__entry->dct_part_sizes,
				ARRAY_SIZE(__entry->dct_part_sizes),
				sizeof(__entry->dct_part_sizes[0])),
		  __entry->last_frame_ts,
		  __entry->golden_frame_ts,
		  __entry->alt_frame_ts,
		  __print_flags(__entry->flags, "|",
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
			 __array(__s8, lf_ref_deltas, 4)
			 __array(__s8, lf_mode_deltas, 2)
			 __field(__u8, lf_level)
			 __field(__u8, lf_sharpness)
			 __field(__u8, lf_flags)
			 __field(__u8, quant_base_q_idx)
			 __field(__s8, quant_delta_q_y_dc)
			 __field(__s8, quant_delta_q_uv_dc)
			 __field(__s8, quant_delta_q_uv_ac)
			 __array(__u8, seg_feature_data, sizeof(__s16) * 8 * 4)
			 __array(__u8, seg_feature_enabled, 8)
			 __array(__u8, seg_tree_probs, 7)
			 __array(__u8, seg_pred_probs, 3)
			 __field(__u8, seg_flags)
			 __field(__u32, flags)
			 __field(__u16, compressed_header_size)
			 __field(__u16, uncompressed_header_size)
			 __field(__u16, frame_width_minus_1)
			 __field(__u16, frame_height_minus_1)
			 __field(__u16, render_width_minus_1)
			 __field(__u16, render_height_minus_1)
			 __field(__u64, last_frame_ts)
			 __field(__u64, golden_frame_ts)
			 __field(__u64, alt_frame_ts)
			 __field(__u8, ref_frame_sign_bias)
			 __field(__u8, reset_frame_context)
			 __field(__u8, frame_context_idx)
			 __field(__u8, profile)
			 __field(__u8, bit_depth)
			 __field(__u8, interpolation_filter)
			 __field(__u8, tile_cols_log2)
			 __field(__u8, tile_rows_log2)
			 __field(__u8, reference_mode)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       memcpy(__entry->lf_ref_deltas, f->lf.ref_deltas,
			      sizeof(__entry->lf_ref_deltas));
		       memcpy(__entry->lf_mode_deltas, f->lf.mode_deltas,
			      sizeof(__entry->lf_mode_deltas));
		       __entry->lf_level = f->lf.level;
		       __entry->lf_sharpness = f->lf.sharpness;
		       __entry->lf_flags = f->lf.flags;
		       __entry->quant_base_q_idx = f->quant.base_q_idx;
		       __entry->quant_delta_q_y_dc = f->quant.delta_q_y_dc;
		       __entry->quant_delta_q_uv_dc = f->quant.delta_q_uv_dc;
		       __entry->quant_delta_q_uv_ac = f->quant.delta_q_uv_ac;
		       memcpy(__entry->seg_feature_data, f->seg.feature_data,
			      sizeof(__entry->seg_feature_data));
		       memcpy(__entry->seg_feature_enabled, f->seg.feature_enabled,
			      sizeof(__entry->seg_feature_enabled));
		       memcpy(__entry->seg_tree_probs, f->seg.tree_probs,
			      sizeof(__entry->seg_tree_probs));
		       memcpy(__entry->seg_pred_probs, f->seg.pred_probs,
			      sizeof(__entry->seg_pred_probs));
		       __entry->seg_flags = f->seg.flags;
		       __entry->flags = f->flags;
		       __entry->compressed_header_size = f->compressed_header_size;
		       __entry->uncompressed_header_size = f->uncompressed_header_size;
		       __entry->frame_width_minus_1 = f->frame_width_minus_1;
		       __entry->frame_height_minus_1 = f->frame_height_minus_1;
		       __entry->render_width_minus_1 = f->render_width_minus_1;
		       __entry->render_height_minus_1 = f->render_height_minus_1;
		       __entry->last_frame_ts = f->last_frame_ts;
		       __entry->golden_frame_ts = f->golden_frame_ts;
		       __entry->alt_frame_ts = f->alt_frame_ts;
		       __entry->ref_frame_sign_bias = f->ref_frame_sign_bias;
		       __entry->reset_frame_context = f->reset_frame_context;
		       __entry->frame_context_idx = f->frame_context_idx;
		       __entry->profile = f->profile;
		       __entry->bit_depth = f->bit_depth;
		       __entry->interpolation_filter = f->interpolation_filter;
		       __entry->tile_cols_log2 = f->tile_cols_log2;
		       __entry->tile_rows_log2 = f->tile_rows_log2;
		       __entry->reference_mode = f->reference_mode;),
	TP_printk("tgid = %u, fd = %u, "
		  "\nlf.ref_deltas %s\n"
		  "lf.mode_deltas %s\n"
		  "lf.level %u\n"
		  "lf.sharpness %u\n"
		  "lf.flags %s\n"
		  "quant.base_q_idx %u\n"
		  "quant.delta_q_y_dc %d\n"
		  "quant.delta_q_uv_dc %d\n"
		  "quant.delta_q_uv_ac %d\n"
		  "seg.feature_data {%s}\n"
		  "seg.feature_enabled %s\n"
		  "seg.tree_probs %s\n"
		  "seg.pred_probs %s\n"
		  "seg.flags %s\n"
		  "flags %s\n"
		  "compressed_header_size %u\n"
		  "uncompressed_header_size %u\n"
		  "frame_width_minus_1 %u\n"
		  "frame_height_minus_1 %u\n"
		  "render_width_minus_1 %u\n"
		  "render_height_minus_1 %u\n"
		  "last_frame_ts %llu\n"
		  "golden_frame_ts %llu\n"
		  "alt_frame_ts %llu\n"
		  "ref_frame_sign_bias %s\n"
		  "reset_frame_context %s\n"
		  "frame_context_idx %u\n"
		  "profile %u\n"
		  "bit_depth %u\n"
		  "interpolation_filter %s\n"
		  "tile_cols_log2 %u\n"
		  "tile_rows_log_2 %u\n"
		  "reference_mode %s\n",
		  __entry->tgid, __entry->fd,
		  __print_array(__entry->lf_ref_deltas,
				ARRAY_SIZE(__entry->lf_ref_deltas),
				sizeof(__entry->lf_ref_deltas[0])),
		  __print_array(__entry->lf_mode_deltas,
				ARRAY_SIZE(__entry->lf_mode_deltas),
				sizeof(__entry->lf_mode_deltas[0])),
		  __entry->lf_level,
		  __entry->lf_sharpness,
		  __print_flags(__entry->lf_flags, "|",
		  {V4L2_VP9_LOOP_FILTER_FLAG_DELTA_ENABLED, "DELTA_ENABLED"},
		  {V4L2_VP9_LOOP_FILTER_FLAG_DELTA_UPDATE, "DELTA_UPDATE"}),
		  __entry->quant_base_q_idx,
		  __entry->quant_delta_q_y_dc,
		  __entry->quant_delta_q_uv_dc,
		  __entry->quant_delta_q_uv_ac,
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->seg_feature_data,
				   sizeof(__entry->seg_feature_data),
				   false),
		  __print_array(__entry->seg_feature_enabled,
				ARRAY_SIZE(__entry->seg_feature_enabled),
				sizeof(__entry->seg_feature_enabled[0])),
		  __print_array(__entry->seg_tree_probs,
				ARRAY_SIZE(__entry->seg_tree_probs),
				sizeof(__entry->seg_tree_probs[0])),
		  __print_array(__entry->seg_pred_probs,
				ARRAY_SIZE(__entry->seg_pred_probs),
				sizeof(__entry->seg_pred_probs[0])),
		  __print_flags(__entry->seg_flags, "|",
		  {V4L2_VP9_SEGMENTATION_FLAG_ENABLED, "ENABLED"},
		  {V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP, "UPDATE_MAP"},
		  {V4L2_VP9_SEGMENTATION_FLAG_TEMPORAL_UPDATE, "TEMPORAL_UPDATE"},
		  {V4L2_VP9_SEGMENTATION_FLAG_UPDATE_DATA, "UPDATE_DATA"},
		  {V4L2_VP9_SEGMENTATION_FLAG_ABS_OR_DELTA_UPDATE, "ABS_OR_DELTA_UPDATE"}),
		  __print_flags(__entry->flags, "|",
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
		  __entry->compressed_header_size,
		  __entry->uncompressed_header_size,
		  __entry->frame_width_minus_1,
		  __entry->frame_height_minus_1,
		  __entry->render_width_minus_1,
		  __entry->render_height_minus_1,
		  __entry->last_frame_ts,
		  __entry->golden_frame_ts,
		  __entry->alt_frame_ts,
		  __print_symbolic(__entry->ref_frame_sign_bias,
		  {V4L2_VP9_SIGN_BIAS_LAST, "SIGN_BIAS_LAST"},
		  {V4L2_VP9_SIGN_BIAS_GOLDEN, "SIGN_BIAS_GOLDEN"},
		  {V4L2_VP9_SIGN_BIAS_ALT, "SIGN_BIAS_ALT"}),
		  __print_symbolic(__entry->reset_frame_context,
		  {V4L2_VP9_RESET_FRAME_CTX_NONE, "RESET_FRAME_CTX_NONE"},
		  {V4L2_VP9_RESET_FRAME_CTX_SPEC, "RESET_FRAME_CTX_SPEC"},
		  {V4L2_VP9_RESET_FRAME_CTX_ALL, "RESET_FRAME_CTX_ALL"}),
		  __entry->frame_context_idx,
		  __entry->profile,
		  __entry->bit_depth,
		  __print_symbolic(__entry->interpolation_filter,
		  {V4L2_VP9_INTERP_FILTER_EIGHTTAP, "INTERP_FILTER_EIGHTTAP"},
		  {V4L2_VP9_INTERP_FILTER_EIGHTTAP_SMOOTH, "INTERP_FILTER_EIGHTTAP_SMOOTH"},
		  {V4L2_VP9_INTERP_FILTER_EIGHTTAP_SHARP, "INTERP_FILTER_EIGHTTAP_SHARP"},
		  {V4L2_VP9_INTERP_FILTER_BILINEAR, "INTERP_FILTER_BILINEAR"},
		  {V4L2_VP9_INTERP_FILTER_SWITCHABLE, "INTERP_FILTER_SWITCHABLE"}),
		  __entry->tile_cols_log2,
		  __entry->tile_rows_log2,
		  __print_symbolic(__entry->reference_mode,
		  {V4L2_VP9_REFERENCE_MODE_SINGLE_REFERENCE, "REFERENCE_MODE_SINGLE_REFERENCE"},
		  {V4L2_VP9_REFERENCE_MODE_COMPOUND_REFERENCE, "REFERENCE_MODE_COMPOUND_REFERENCE"},
		  {V4L2_VP9_REFERENCE_MODE_SELECT, "REFERENCE_MODE_SELECT"}))
);

DECLARE_EVENT_CLASS(v4l2_ctrl_vp9_compressed_hdr_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_vp9_compressed_hdr *h),
	TP_ARGS(tgid, fd, h),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __field(__u8, tx_mode)
			 __array(__u8, tx8, 2 * 1)
			 __array(__u8, tx16, 2 * 2)
			 __array(__u8, tx32, 2 * 3)
			 __array(__u8, skip, 3)
			 __array(__u8, inter_mode, 7 * 3)
			 __array(__u8, interp_filter, 4 * 2)
			 __array(__u8, is_inter, 4)
			 __array(__u8, comp_mode, 5)
			 __array(__u8, single_ref, 5 * 2)
			 __array(__u8, comp_ref, 5)
			 __array(__u8, y_mode, 4 * 9)
			 __array(__u8, uv_mode, 10 * 9)
			 __array(__u8, partition, 16 * 3)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       __entry->tx_mode = h->tx_mode;
		       memcpy(__entry->tx8, h->tx8, sizeof(__entry->tx8));
		       memcpy(__entry->tx16, h->tx16, sizeof(__entry->tx16));
		       memcpy(__entry->tx32, h->tx32, sizeof(__entry->tx32));
		       memcpy(__entry->skip, h->skip, sizeof(__entry->skip));
		       memcpy(__entry->inter_mode, h->inter_mode, sizeof(__entry->inter_mode));
		       memcpy(__entry->interp_filter, h->interp_filter,
			      sizeof(__entry->interp_filter));
		       memcpy(__entry->is_inter, h->is_inter, sizeof(__entry->is_inter));
		       memcpy(__entry->comp_mode, h->comp_mode, sizeof(__entry->comp_mode));
		       memcpy(__entry->single_ref, h->single_ref, sizeof(__entry->single_ref));
		       memcpy(__entry->comp_ref, h->comp_ref, sizeof(__entry->comp_ref));
		       memcpy(__entry->y_mode, h->y_mode, sizeof(__entry->y_mode));
		       memcpy(__entry->uv_mode, h->uv_mode, sizeof(__entry->uv_mode));
		       memcpy(__entry->partition, h->partition, sizeof(__entry->partition));),
	TP_printk("tgid = %u, fd = %u, "
		  "\ntx_mode %s\n"
		  "tx8 {%s}\n"
		  "tx16 {%s}\n"
		  "tx32 {%s}\n"
		  "skip %s\n"
		  "inter_mode {%s}\n"
		  "interp_filter {%s}\n"
		  "is_inter %s\n"
		  "comp_mode %s\n"
		  "single_ref {%s}\n"
		  "comp_ref %s\n"
		  "y_mode {%s}\n"
		  "uv_mode {%s}\n"
		  "partition {%s}\n",
		  __entry->tgid, __entry->fd,
		  __print_symbolic(__entry->tx_mode,
		  {V4L2_VP9_TX_MODE_ONLY_4X4, "TX_MODE_ONLY_4X4"},
		  {V4L2_VP9_TX_MODE_ALLOW_8X8, "TX_MODE_ALLOW_8X8"},
		  {V4L2_VP9_TX_MODE_ALLOW_16X16, "TX_MODE_ALLOW_16X16"},
		  {V4L2_VP9_TX_MODE_ALLOW_32X32, "TX_MODE_ALLOW_32X32"},
		  {V4L2_VP9_TX_MODE_SELECT, "TX_MODE_SELECT"}),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->tx8,
				   sizeof(__entry->tx8),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->tx16,
				   sizeof(__entry->tx16),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->tx32,
				   sizeof(__entry->tx32),
				   false),
		  __print_array(__entry->skip,
				ARRAY_SIZE(__entry->skip),
				sizeof(__entry->skip[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->inter_mode,
				   sizeof(__entry->inter_mode),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->interp_filter,
				   sizeof(__entry->interp_filter),
				   false),
		  __print_array(__entry->is_inter,
				ARRAY_SIZE(__entry->is_inter),
				sizeof(__entry->is_inter[0])),
		  __print_array(__entry->comp_mode,
				ARRAY_SIZE(__entry->comp_mode),
				sizeof(__entry->comp_mode[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->single_ref,
				   sizeof(__entry->single_ref),
				   false),
		  __print_array(__entry->comp_ref,
				ARRAY_SIZE(__entry->comp_ref),
				sizeof(__entry->comp_ref[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->y_mode,
				   sizeof(__entry->y_mode),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->uv_mode,
				   sizeof(__entry->uv_mode),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->partition,
				   sizeof(__entry->partition),
				   false)
	)
);

DECLARE_EVENT_CLASS(v4l2_ctrl_vp9_compressed_coef_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_ctrl_vp9_compressed_hdr *h),
	TP_ARGS(tgid, fd, h),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __array(__u8, coef, 4 * 2 * 2 * 6 * 6 * 3)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       memcpy(__entry->coef, h->coef, sizeof(__entry->coef));),
	TP_printk("tgid = %u, fd = %u, "
		  "\n coef {%s}",
		  __entry->tgid, __entry->fd,
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->coef,
				   sizeof(__entry->coef),
				   false)
	)
);

DECLARE_EVENT_CLASS(v4l2_vp9_mv_probs_tmpl,
	TP_PROTO(u32 tgid, u32 fd, const struct v4l2_vp9_mv_probs *p),
	TP_ARGS(tgid, fd, p),
	TP_STRUCT__entry(__field(u32, tgid)
			 __field(u32, fd)
			 __array(__u8, joint, 3)
			 __array(__u8, sign, 2)
			 __array(__u8, classes, 2 * 10)
			 __array(__u8, class0_bit, 2)
			 __array(__u8, bits, 2 * 10)
			 __array(__u8, class0_fr, 2 * 2 * 3)
			 __array(__u8, fr, 2 * 3)
			 __array(__u8, class0_hp, 2)
			 __array(__u8, hp, 2)),
	TP_fast_assign(__entry->tgid = tgid;
		       __entry->fd = fd;
		       memcpy(__entry->joint, p->joint, sizeof(__entry->joint));
		       memcpy(__entry->sign, p->sign, sizeof(__entry->sign));
		       memcpy(__entry->classes, p->classes, sizeof(__entry->classes));
		       memcpy(__entry->class0_bit, p->class0_bit, sizeof(__entry->class0_bit));
		       memcpy(__entry->bits, p->bits, sizeof(__entry->bits));
		       memcpy(__entry->class0_fr, p->class0_fr, sizeof(__entry->class0_fr));
		       memcpy(__entry->fr, p->fr, sizeof(__entry->fr));
		       memcpy(__entry->class0_hp, p->class0_hp, sizeof(__entry->class0_hp));
		       memcpy(__entry->hp, p->hp, sizeof(__entry->hp));),
	TP_printk("tgid = %u, fd = %u, "
		  "\n joint %s\n"
		  "sign %s\n"
		  "classes {%s}\n"
		  "class0_bit %s\n"
		  "bits {%s}\n"
		  "class0_fr {%s}\n"
		  "fr {%s}\n"
		  "class0_hp %s\n"
		  "hp %s\n",
		  __entry->tgid, __entry->fd,
		  __print_array(__entry->joint,
				ARRAY_SIZE(__entry->joint),
				sizeof(__entry->joint[0])),
		  __print_array(__entry->sign,
				ARRAY_SIZE(__entry->sign),
				sizeof(__entry->sign[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->classes,
				   sizeof(__entry->classes),
				   false),
		  __print_array(__entry->class0_bit,
				ARRAY_SIZE(__entry->class0_bit),
				sizeof(__entry->class0_bit[0])),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->bits,
				   sizeof(__entry->bits),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->class0_fr,
				   sizeof(__entry->class0_fr),
				   false),
		  __print_hex_dump("", DUMP_PREFIX_NONE, 32, 1,
				   __entry->fr,
				   sizeof(__entry->fr),
				   false),
		  __print_array(__entry->class0_hp,
				ARRAY_SIZE(__entry->class0_hp),
				sizeof(__entry->class0_hp[0])),
		  __print_array(__entry->hp,
				ARRAY_SIZE(__entry->hp),
				sizeof(__entry->hp[0]))
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

#endif /* if !defined(_TRACE_V4L2_CONTROLS_H_) || defined(TRACE_HEADER_MULTI_READ) */

#include <trace/define_trace.h>
