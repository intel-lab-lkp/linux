// SPDX-License-Identifier: GPL-2.0
#include <media/v4l2-common.h>
#include <media/v4l2-fh.h>
#include <media/videobuf2-v4l2.h>

#define CREATE_TRACE_POINTS
#include <trace/events/v4l2.h>
#include <trace/events/v4l2_requests.h>

EXPORT_TRACEPOINT_SYMBOL_GPL(vb2_v4l2_buf_done);
EXPORT_TRACEPOINT_SYMBOL_GPL(vb2_v4l2_buf_queue);
EXPORT_TRACEPOINT_SYMBOL_GPL(vb2_v4l2_dqbuf);
EXPORT_TRACEPOINT_SYMBOL_GPL(vb2_v4l2_qbuf);

/* Export AV1 controls */
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_av1_sequence);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_av1_frame);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_av1_tile_group_entry);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_av1_film_grain);

/* Export FWHT controls */
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_fwht_params);

/* Export H264 controls */
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_h264_sps);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_h264_pps);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_h264_scaling_matrix);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_h264_pred_weights);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_h264_slice_params);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_h264_ref_pic_list0);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_h264_ref_pic_list1);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_h264_decode_params);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_h264_dpb_entry);

/* Export HEVC controls */
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_hevc_sps);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_hevc_pps);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_hevc_slice_params);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_hevc_pred_weight_table);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_hevc_scaling_matrix);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_hevc_decode_params);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_hevc_dpb_entry);

/* Export MPEG2 controls */
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_mpeg2_sequence);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_mpeg2_picture);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_mpeg2_quantisation);

/* Export VP8 controls */
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_vp8_frame);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_vp8_entropy);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_vp9_frame);

/* Export VP9 controls */
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_vp9_compressed_hdr);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_ctrl_vp9_compressed_coeff);
EXPORT_TRACEPOINT_SYMBOL_GPL(v4l2_vp9_mv_probs);
