// SPDX-License-Identifier: GPL-2.0
/*
 * Support for Intel Camera Imaging ISP subsystem.
 * Copyright (c) 2015, Intel Corporation.
 */

#include "debug.h"

#ifndef __INLINE_INPUT_SYSTEM__
#define __INLINE_INPUT_SYSTEM__
#endif
#ifndef __INLINE_IBUF_CTRL__
#define __INLINE_IBUF_CTRL__
#endif
#ifndef __INLINE_CSI_RX__
#define __INLINE_CSI_RX__
#endif
#ifndef __INLINE_PIXELGEN__
#define __INLINE_PIXELGEN__
#endif
#ifndef __INLINE_STREAM2MMIO__
#define __INLINE_STREAM2MMIO__
#endif

#include <linux/args.h>
#include <linux/string.h> /* for strscpy() */

#include "ia_css_debug.h"
#include "ia_css_debug_pipe.h"
#include "ia_css_irq.h"
#include "ia_css_stream.h"
#include "ia_css_pipeline.h"
#include "ia_css_isp_param.h"
#include "sh_css_params.h"
#include "ia_css_bufq.h"
/* ISP2401 */
#include "ia_css_queue.h"

#include "ia_css_isp_params.h"

#include "system_local.h"
#include "assert_support.h"
#include "print_support.h"

#include "fifo_monitor.h"

#include "input_formatter.h"
#include "dma.h"
#include "irq.h"
#include "gp_device.h"
#include "sp.h"
#include "isp.h"
#include "type_support.h"
#include "input_system.h"	/* input_formatter_reg_load */
#include "ia_css_tagger_common.h"

#include "sh_css_internal.h"
#include "ia_css_isys.h"
#include "sh_css_sp.h"		/* sh_css_sp_get_debug_state() */

#include "css_trace.h"      /* tracer */

#include "device_access.h"	/* for ia_css_device_load_uint32 */

/* Include all kernel host interfaces for ISP1 */
#include "anr/anr_1.0/ia_css_anr.host.h"
#include "cnr/cnr_1.0/ia_css_cnr.host.h"
#include "csc/csc_1.0/ia_css_csc.host.h"
#include "de/de_1.0/ia_css_de.host.h"
#include "dp/dp_1.0/ia_css_dp.host.h"
#include "bnr/bnr_1.0/ia_css_bnr.host.h"
#include "fpn/fpn_1.0/ia_css_fpn.host.h"
#include "gc/gc_1.0/ia_css_gc.host.h"
#include "ob/ob_1.0/ia_css_ob.host.h"
#include "s3a/s3a_1.0/ia_css_s3a.host.h"
#include "sc/sc_1.0/ia_css_sc.host.h"
#include "tnr/tnr_1.0/ia_css_tnr.host.h"
#include "uds/uds_1.0/ia_css_uds_param.h"
#include "wb/wb_1.0/ia_css_wb.host.h"
#include "ynr/ynr_1.0/ia_css_ynr.host.h"

/* Include additional kernel host interfaces for ISP2 */
#include "aa/aa_2/ia_css_aa2.host.h"
#include "anr/anr_2/ia_css_anr2.host.h"
#include "cnr/cnr_2/ia_css_cnr2.host.h"
#include "de/de_2/ia_css_de2.host.h"
#include "gc/gc_2/ia_css_gc2.host.h"
#include "ynr/ynr_2/ia_css_ynr2.host.h"

#define DPG_START "ia_css_debug_pipe_graph_dump_start "
#define DPG_END   " ia_css_debug_pipe_graph_dump_end\n"

#define ENABLE_LINE_MAX_LENGTH (25)

static struct pipe_graph_class {
	bool do_init;
	int height;
	int width;
	int eff_height;
	int eff_width;
	enum atomisp_input_format stream_format;
} pg_inst = {true, 0, 0, 0, 0, N_ATOMISP_INPUT_FORMAT};

static const char *const queue_id_to_str[] = {
	/* [SH_CSS_QUEUE_A_ID]     =*/ "queue_A",
	/* [SH_CSS_QUEUE_B_ID]     =*/ "queue_B",
	/* [SH_CSS_QUEUE_C_ID]     =*/ "queue_C",
	/* [SH_CSS_QUEUE_D_ID]     =*/ "queue_D",
	/* [SH_CSS_QUEUE_E_ID]     =*/ "queue_E",
	/* [SH_CSS_QUEUE_F_ID]     =*/ "queue_F",
	/* [SH_CSS_QUEUE_G_ID]     =*/ "queue_G",
	/* [SH_CSS_QUEUE_H_ID]     =*/ "queue_H"
};

static const char *const pipe_id_to_str[] = {
	/* [IA_CSS_PIPE_ID_PREVIEW]   =*/ "preview",
	/* [IA_CSS_PIPE_ID_COPY]      =*/ "copy",
	/* [IA_CSS_PIPE_ID_VIDEO]     =*/ "video",
	/* [IA_CSS_PIPE_ID_CAPTURE]   =*/ "capture",
	/* [IA_CSS_PIPE_ID_YUVPP]     =*/ "yuvpp",
};

/* 27 is combined length of _stage%d(pipe%d)\0. */
static char dot_id_input_bin[SH_CSS_MAX_BINARY_NAME + 27];
static char ring_buffer[200];

void ia_css_debug_dtrace(unsigned int level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	ia_css_debug_vdtrace(level, fmt, ap);
	va_end(ap);
}

static const char *debug_stream_format2str(const enum atomisp_input_format
	stream_format)
{
	switch (stream_format) {
	case ATOMISP_INPUT_FORMAT_YUV420_8_LEGACY:
		return "yuv420-8-legacy";
	case ATOMISP_INPUT_FORMAT_YUV420_8:
		return "yuv420-8";
	case ATOMISP_INPUT_FORMAT_YUV420_10:
		return "yuv420-10";
	case ATOMISP_INPUT_FORMAT_YUV420_16:
		return "yuv420-16";
	case ATOMISP_INPUT_FORMAT_YUV422_8:
		return "yuv422-8";
	case ATOMISP_INPUT_FORMAT_YUV422_10:
		return "yuv422-10";
	case ATOMISP_INPUT_FORMAT_YUV422_16:
		return "yuv422-16";
	case ATOMISP_INPUT_FORMAT_RGB_444:
		return "rgb444";
	case ATOMISP_INPUT_FORMAT_RGB_555:
		return "rgb555";
	case ATOMISP_INPUT_FORMAT_RGB_565:
		return "rgb565";
	case ATOMISP_INPUT_FORMAT_RGB_666:
		return "rgb666";
	case ATOMISP_INPUT_FORMAT_RGB_888:
		return "rgb888";
	case ATOMISP_INPUT_FORMAT_RAW_6:
		return "raw6";
	case ATOMISP_INPUT_FORMAT_RAW_7:
		return "raw7";
	case ATOMISP_INPUT_FORMAT_RAW_8:
		return "raw8";
	case ATOMISP_INPUT_FORMAT_RAW_10:
		return "raw10";
	case ATOMISP_INPUT_FORMAT_RAW_12:
		return "raw12";
	case ATOMISP_INPUT_FORMAT_RAW_14:
		return "raw14";
	case ATOMISP_INPUT_FORMAT_RAW_16:
		return "raw16";
	case ATOMISP_INPUT_FORMAT_BINARY_8:
		return "binary8";
	case ATOMISP_INPUT_FORMAT_GENERIC_SHORT1:
		return "generic-short1";
	case ATOMISP_INPUT_FORMAT_GENERIC_SHORT2:
		return "generic-short2";
	case ATOMISP_INPUT_FORMAT_GENERIC_SHORT3:
		return "generic-short3";
	case ATOMISP_INPUT_FORMAT_GENERIC_SHORT4:
		return "generic-short4";
	case ATOMISP_INPUT_FORMAT_GENERIC_SHORT5:
		return "generic-short5";
	case ATOMISP_INPUT_FORMAT_GENERIC_SHORT6:
		return "generic-short6";
	case ATOMISP_INPUT_FORMAT_GENERIC_SHORT7:
		return "generic-short7";
	case ATOMISP_INPUT_FORMAT_GENERIC_SHORT8:
		return "generic-short8";
	case ATOMISP_INPUT_FORMAT_YUV420_8_SHIFT:
		return "yuv420-8-shift";
	case ATOMISP_INPUT_FORMAT_YUV420_10_SHIFT:
		return "yuv420-10-shift";
	case ATOMISP_INPUT_FORMAT_EMBEDDED:
		return "embedded-8";
	case ATOMISP_INPUT_FORMAT_USER_DEF1:
		return "user-def-8-type-1";
	case ATOMISP_INPUT_FORMAT_USER_DEF2:
		return "user-def-8-type-2";
	case ATOMISP_INPUT_FORMAT_USER_DEF3:
		return "user-def-8-type-3";
	case ATOMISP_INPUT_FORMAT_USER_DEF4:
		return "user-def-8-type-4";
	case ATOMISP_INPUT_FORMAT_USER_DEF5:
		return "user-def-8-type-5";
	case ATOMISP_INPUT_FORMAT_USER_DEF6:
		return "user-def-8-type-6";
	case ATOMISP_INPUT_FORMAT_USER_DEF7:
		return "user-def-8-type-7";
	case ATOMISP_INPUT_FORMAT_USER_DEF8:
		return "user-def-8-type-8";

	default:
		assert(!"Unknown stream format");
		return "unknown-stream-format";
	}
};

static const char *debug_frame_format2str(const enum ia_css_frame_format
	frame_format)
{
	switch (frame_format) {
	case IA_CSS_FRAME_FORMAT_NV11:
		return "NV11";
	case IA_CSS_FRAME_FORMAT_NV12:
		return "NV12";
	case IA_CSS_FRAME_FORMAT_NV12_16:
		return "NV12_16";
	case IA_CSS_FRAME_FORMAT_NV12_TILEY:
		return "NV12_TILEY";
	case IA_CSS_FRAME_FORMAT_NV16:
		return "NV16";
	case IA_CSS_FRAME_FORMAT_NV21:
		return "NV21";
	case IA_CSS_FRAME_FORMAT_NV61:
		return "NV61";
	case IA_CSS_FRAME_FORMAT_YV12:
		return "YV12";
	case IA_CSS_FRAME_FORMAT_YV16:
		return "YV16";
	case IA_CSS_FRAME_FORMAT_YUV420:
		return "YUV420";
	case IA_CSS_FRAME_FORMAT_YUV420_16:
		return "YUV420_16";
	case IA_CSS_FRAME_FORMAT_YUV422:
		return "YUV422";
	case IA_CSS_FRAME_FORMAT_YUV422_16:
		return "YUV422_16";
	case IA_CSS_FRAME_FORMAT_UYVY:
		return "UYVY";
	case IA_CSS_FRAME_FORMAT_YUYV:
		return "YUYV";
	case IA_CSS_FRAME_FORMAT_YUV444:
		return "YUV444";
	case IA_CSS_FRAME_FORMAT_YUV_LINE:
		return "YUV_LINE";
	case IA_CSS_FRAME_FORMAT_RAW:
		return "RAW";
	case IA_CSS_FRAME_FORMAT_RGB565:
		return "RGB565";
	case IA_CSS_FRAME_FORMAT_PLANAR_RGB888:
		return "PLANAR_RGB888";
	case IA_CSS_FRAME_FORMAT_RGBA888:
		return "RGBA888";
	case IA_CSS_FRAME_FORMAT_QPLANE6:
		return "QPLANE6";
	case IA_CSS_FRAME_FORMAT_BINARY_8:
		return "BINARY_8";
	case IA_CSS_FRAME_FORMAT_MIPI:
		return "MIPI";
	case IA_CSS_FRAME_FORMAT_RAW_PACKED:
		return "RAW_PACKED";
	case IA_CSS_FRAME_FORMAT_CSI_MIPI_YUV420_8:
		return "CSI_MIPI_YUV420_8";
	case IA_CSS_FRAME_FORMAT_CSI_MIPI_LEGACY_YUV420_8:
		return "CSI_MIPI_LEGACY_YUV420_8";
	case IA_CSS_FRAME_FORMAT_CSI_MIPI_YUV420_10:
		return "CSI_MIPI_YUV420_10";

	default:
		assert(!"Unknown frame format");
		return "unknown-frame-format";
	}
}

#if SP_DEBUG != SP_DEBUG_NONE

void ia_css_debug_print_sp_debug_state(const struct sh_css_sp_debug_state
				       *state)
{
#endif

#if SP_DEBUG == SP_DEBUG_DUMP

	assert(state);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "current SP software counter: %d\n",
			    state->debug[0]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty output buffer queue head: 0x%x\n",
			    state->debug[1]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty output buffer queue tail: 0x%x\n",
			    state->debug[2]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty s3a buffer queue head: 0x%x\n",
			    state->debug[3]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty s3a buffer queue tail: 0x%x\n",
			    state->debug[4]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "full output buffer queue head: 0x%x\n",
			    state->debug[5]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "full output buffer queue tail: 0x%x\n",
			    state->debug[6]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "full s3a buffer queue head: 0x%x\n",
			    state->debug[7]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "full s3a buffer queue tail: 0x%x\n",
			    state->debug[8]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE, "event queue head: 0x%x\n",
			    state->debug[9]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE, "event queue tail: 0x%x\n",
			    state->debug[10]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "num of stages of current pipeline: 0x%x\n",
			    state->debug[11]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE, "DDR address of stage 1: 0x%x\n",
			    state->debug[12]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE, "DDR address of stage 2: 0x%x\n",
			    state->debug[13]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "current stage out_vf buffer idx: 0x%x\n",
			    state->debug[14]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "current stage output buffer idx: 0x%x\n",
			    state->debug[15]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "current stage s3a buffer idx: 0x%x\n",
			    state->debug[16]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "first char of current stage name: 0x%x\n",
			    state->debug[17]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE, "current SP thread id: 0x%x\n",
			    state->debug[18]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty output buffer address 1: 0x%x\n",
			    state->debug[19]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty output buffer address 2: 0x%x\n",
			    state->debug[20]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty out_vf buffer address 1: 0x%x\n",
			    state->debug[21]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty out_vf buffer address 2: 0x%x\n",
			    state->debug[22]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty s3a_hi buffer address 1: 0x%x\n",
			    state->debug[23]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty s3a_hi buffer address 2: 0x%x\n",
			    state->debug[24]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty s3a_lo buffer address 1: 0x%x\n",
			    state->debug[25]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty s3a_lo buffer address 2: 0x%x\n",
			    state->debug[26]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty dis_hor buffer address 1: 0x%x\n",
			    state->debug[27]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty dis_hor buffer address 2: 0x%x\n",
			    state->debug[28]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty dis_ver buffer address 1: 0x%x\n",
			    state->debug[29]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty dis_ver buffer address 2: 0x%x\n",
			    state->debug[30]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "empty param buffer address: 0x%x\n",
			    state->debug[31]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "first incorrect frame address: 0x%x\n",
			    state->debug[32]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "first incorrect frame container address: 0x%x\n",
			    state->debug[33]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "first incorrect frame container payload: 0x%x\n",
			    state->debug[34]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "first incorrect s3a_hi address: 0x%x\n",
			    state->debug[35]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "first incorrect s3a_hi container address: 0x%x\n",
			    state->debug[36]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "first incorrect s3a_hi container payload: 0x%x\n",
			    state->debug[37]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "first incorrect s3a_lo address: 0x%x\n",
			    state->debug[38]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "first incorrect s3a_lo container address: 0x%x\n",
			    state->debug[39]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "first incorrect s3a_lo container payload: 0x%x\n",
			    state->debug[40]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "number of calling flash start function: 0x%x\n",
			    state->debug[41]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "number of calling flash close function: 0x%x\n",
			    state->debug[42]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE, "number of flashed frame: 0x%x\n",
			    state->debug[43]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE, "flash in use flag: 0x%x\n",
			    state->debug[44]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "number of update frame flashed flag: 0x%x\n",
			    state->debug[46]);
	ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
			    "number of active threads: 0x%x\n",
			    state->debug[45]);

#elif SP_DEBUG == SP_DEBUG_COPY

	/* Remember last_index because we only want to print new entries */
	static int last_index;
	int sp_index = state->index;
	int n;

	assert(state);
	if (sp_index < last_index) {
		/* SP has been reset */
		last_index = 0;
	}

	if (last_index == 0) {
		ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
				    "copy-trace init: sp_dbg_if_start_line=%d, sp_dbg_if_start_column=%d, sp_dbg_if_cropped_height=%d, sp_debg_if_cropped_width=%d\n",
				    state->if_start_line,
				    state->if_start_column,
				    state->if_cropped_height,
				    state->if_cropped_width);
	}

	if ((last_index + SH_CSS_SP_DBG_TRACE_DEPTH) < sp_index) {
		/* last index can be multiple rounds behind */
		/* while trace size is only SH_CSS_SP_DBG_TRACE_DEPTH */
		last_index = sp_index - SH_CSS_SP_DBG_TRACE_DEPTH;
	}

	for (n = last_index; n < sp_index; n++) {
		int i = n % SH_CSS_SP_DBG_TRACE_DEPTH;

		if (state->trace[i].frame != 0) {
			ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
					    "copy-trace: frame=%d, line=%d, pixel_distance=%d, mipi_used_dword=%d, sp_index=%d\n",
					    state->trace[i].frame,
					    state->trace[i].line,
					    state->trace[i].pixel_distance,
					    state->trace[i].mipi_used_dword,
					    state->trace[i].sp_index);
		}
	}

	last_index = sp_index;

#elif SP_DEBUG == SP_DEBUG_TRACE

	/*
	 * This is just an example how TRACE_FILE_ID (see ia_css_debug.sp.h) will
	 * me mapped on the file name string.
	 *
	 * Adjust this to your trace case!
	 */
	static char const *const id2filename[8] = {
		"param_buffer.sp.c | tagger.sp.c | pipe_data.sp.c",
		"isp_init.sp.c",
		"sp_raw_copy.hive.c",
		"dma_configure.sp.c",
		"sp.hive.c",
		"event_proxy_sp.hive.c",
		"circular_buffer.sp.c",
		"frame_buffer.sp.c"
	};

	/* Example SH_CSS_SP_DBG_NR_OF_TRACES==1 */
	/* Adjust this to your trace case */
	static char const *trace_name[SH_CSS_SP_DBG_NR_OF_TRACES] = {
		"default"
	};

	/* Remember host_index_last because we only want to print new entries */
	static int host_index_last[SH_CSS_SP_DBG_NR_OF_TRACES] = { 0 };
	int t, n;

	assert(state);

	for (t = 0; t < SH_CSS_SP_DBG_NR_OF_TRACES; t++) {
		int sp_index_last = state->index_last[t];

		if (sp_index_last < host_index_last[t]) {
			/* SP has been reset */
			host_index_last[t] = 0;
		}

		if ((host_index_last[t] + SH_CSS_SP_DBG_TRACE_DEPTH) <
		    sp_index_last) {
			/* last index can be multiple rounds behind */
			/* while trace size is only SH_CSS_SP_DBG_TRACE_DEPTH */
			ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
					    "Warning: trace %s has gap of %d traces\n",
					    trace_name[t],
					    (sp_index_last -
					     (host_index_last[t] +
					      SH_CSS_SP_DBG_TRACE_DEPTH)));

			host_index_last[t] =
			    sp_index_last - SH_CSS_SP_DBG_TRACE_DEPTH;
		}

		for (n = host_index_last[t]; n < sp_index_last; n++) {
			int i = n % SH_CSS_SP_DBG_TRACE_DEPTH;
			int l = state->trace[t][i].location &
				((1 << SH_CSS_SP_DBG_TRACE_FILE_ID_BIT_POS) - 1);
			int fid = state->trace[t][i].location >>
				  SH_CSS_SP_DBG_TRACE_FILE_ID_BIT_POS;
			int ts = state->trace[t][i].time_stamp;

			if (ts) {
				ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
						    "%05d trace=%s, file=%s:%d, data=0x%08x\n",
						    ts,
						    trace_name[t],
						    id2filename[fid], l,
						    state->trace[t][i].data);
			}
		}
		host_index_last[t] = sp_index_last;
	}

#elif SP_DEBUG == SP_DEBUG_MINIMAL
	int i;
	int base = 0;
	int limit = SH_CSS_NUM_SP_DEBUG;
	int step = 1;

	assert(state);

	for (i = base; i < limit; i += step) {
		ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
				    "sp_dbg_trace[%d] = %d\n",
				    i, state->debug[i]);
	}
#endif

#if SP_DEBUG != SP_DEBUG_NONE

	return;
}
#endif

void ia_css_debug_dump_sp_sw_debug_info(void)
{
#if SP_DEBUG != SP_DEBUG_NONE
	struct sh_css_sp_debug_state state;

	sh_css_sp_get_debug_state(&state);
	ia_css_debug_print_sp_debug_state(&state);
#endif
	ia_css_bufq_dump_queue_info();
	ia_css_pipeline_dump_thread_map_info();
	return;
}

void sh_css_dump_sp_raw_copy_linecount(bool reduced)
{
	const struct ia_css_fw_info *fw;
	unsigned int HIVE_ADDR_raw_copy_line_count;
	s32 raw_copy_line_count;
	static s32 prev_raw_copy_line_count = -1;

	fw = &sh_css_sp_fw;
	HIVE_ADDR_raw_copy_line_count =
	    fw->info.sp.raw_copy_line_count;

	(void)HIVE_ADDR_raw_copy_line_count;

	sp_dmem_load(SP0_ID,
		     (unsigned int)sp_address_of(raw_copy_line_count),
		     &raw_copy_line_count,
		     sizeof(raw_copy_line_count));

	/* only indicate if copy loop is active */
	if (reduced)
		raw_copy_line_count = (raw_copy_line_count < 0) ? raw_copy_line_count : 1;
	/* do the handling */
	if (prev_raw_copy_line_count != raw_copy_line_count) {
		ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
				    "sh_css_dump_sp_raw_copy_linecount() line_count=%d\n",
				    raw_copy_line_count);
		prev_raw_copy_line_count = raw_copy_line_count;
	}
}

void ia_css_debug_dump_isp_binary(void)
{
	const struct ia_css_fw_info *fw;
	unsigned int HIVE_ADDR_pipeline_sp_curr_binary_id;
	u32 curr_binary_id;
	static u32 prev_binary_id = 0xFFFFFFFF;
	static u32 sample_count;

	fw = &sh_css_sp_fw;
	HIVE_ADDR_pipeline_sp_curr_binary_id = fw->info.sp.curr_binary_id;

	(void)HIVE_ADDR_pipeline_sp_curr_binary_id;

	sp_dmem_load(SP0_ID,
		     (unsigned int)sp_address_of(pipeline_sp_curr_binary_id),
		     &curr_binary_id,
		     sizeof(curr_binary_id));

	/* do the handling */
	sample_count++;
	if (prev_binary_id != curr_binary_id) {
		ia_css_debug_dtrace(IA_CSS_DEBUG_VERBOSE,
				    "sh_css_dump_isp_binary() pipe_id=%d, binary_id=%d, sample_count=%d\n",
				    (curr_binary_id >> 16),
				    (curr_binary_id & 0x0ffff),
				    sample_count);
		sample_count = 0;
		prev_binary_id = curr_binary_id;
	}
}

static void __printf(1, 2) dtrace_dot(const char *fmt, ...)
{
	va_list ap;

	assert(fmt);
	va_start(ap, fmt);

	ia_css_debug_dtrace(IA_CSS_DEBUG_INFO, "%s", DPG_START);
	ia_css_debug_vdtrace(IA_CSS_DEBUG_INFO, fmt, ap);
	ia_css_debug_dtrace(IA_CSS_DEBUG_INFO, "%s", DPG_END);
	va_end(ap);
}

static void
ia_css_debug_pipe_graph_dump_frame(
    const struct ia_css_frame *frame,
    enum ia_css_pipe_id id,
    char const *blob_name,
    char const *frame_name,
    bool in_frame)
{
	char bufinfo[100];

	if (frame->dynamic_queue_id == SH_CSS_INVALID_QUEUE_ID) {
		snprintf(bufinfo, sizeof(bufinfo), "Internal");
	} else {
		snprintf(bufinfo, sizeof(bufinfo), "Queue: %s %s",
			 pipe_id_to_str[id],
			 queue_id_to_str[frame->dynamic_queue_id]);
	}
	dtrace_dot(
	    "node [shape = box, fixedsize=true, width=2, height=0.7]; \"%p\" [label = \"%s\\n%d(%d) x %d, %dbpp\\n%s\"];",
	    frame,
	    debug_frame_format2str(frame->frame_info.format),
	    frame->frame_info.res.width,
	    frame->frame_info.padded_width,
	    frame->frame_info.res.height,
	    frame->frame_info.raw_bit_depth,
	    bufinfo);

	if (in_frame) {
		dtrace_dot(
		    "\"%p\"->\"%s(pipe%d)\" [label = %s_frame];",
		    frame,
		    blob_name, id, frame_name);
	} else {
		dtrace_dot(
		    "\"%s(pipe%d)\"->\"%p\" [label = %s_frame];",
		    blob_name, id,
		    frame,
		    frame_name);
	}
}

void
ia_css_debug_pipe_graph_dump_prologue(void)
{
	dtrace_dot("digraph sh_css_pipe_graph {");
	dtrace_dot("rankdir=LR;");

	dtrace_dot("fontsize=9;");
	dtrace_dot("label = \"\\nEnable options: rp=reduced pipe, vfve=vf_veceven, dvse=dvs_envelope, dvs6=dvs_6axis, bo=block_out, fbds=fixed_bayer_ds, bf6=bayer_fir_6db, rawb=raw_binning, cont=continuous, disc=dis_crop\\n"
		   "dp2a=dp_2adjacent, outp=output, outt=out_table, reff=ref_frame, par=params, gam=gamma, cagdc=ca_gdc, ispa=isp_addresses, inf=in_frame, outf=out_frame, hs=high_speed, inpc=input_chunking\"");
}

void ia_css_debug_pipe_graph_dump_epilogue(void)
{
	if (strlen(ring_buffer) > 0)
		dtrace_dot(ring_buffer);

	if (pg_inst.stream_format != N_ATOMISP_INPUT_FORMAT) {
		/* An input stream format has been set so assume we have
		 * an input system and sensor
		 */

		dtrace_dot(
		    "node [shape = doublecircle, fixedsize=true, width=2.5]; \"input_system\" [label = \"Input system\"];");

		dtrace_dot(
		    "\"input_system\"->\"%s\" [label = \"%s\"];",
		    dot_id_input_bin, debug_stream_format2str(pg_inst.stream_format));

		dtrace_dot(
		    "node [shape = doublecircle, fixedsize=true, width=2.5]; \"sensor\" [label = \"Sensor\"];");

		dtrace_dot(
		    "\"sensor\"->\"input_system\" [label = \"%s\\n%d x %d\\n(%d x %d)\"];",
		    debug_stream_format2str(pg_inst.stream_format),
		    pg_inst.width, pg_inst.height,
		    pg_inst.eff_width, pg_inst.eff_height);
	}

	dtrace_dot("}");

	/* Reset temp strings */
	memset(dot_id_input_bin, 0, sizeof(dot_id_input_bin));
	memset(ring_buffer, 0, sizeof(ring_buffer));

	pg_inst.do_init = true;
	pg_inst.width = 0;
	pg_inst.height = 0;
	pg_inst.eff_width = 0;
	pg_inst.eff_height = 0;
	pg_inst.stream_format = N_ATOMISP_INPUT_FORMAT;
}

void
ia_css_debug_pipe_graph_dump_stage(
    struct ia_css_pipeline_stage *stage,
    enum ia_css_pipe_id id)
{
	char blob_name[SH_CSS_MAX_BINARY_NAME + 10] = "<unknown type>";
	char const *bin_type = "<unknown type>";
	int i;

	assert(stage);
	if (stage->sp_func != IA_CSS_PIPELINE_NO_FUNC)
		return;

	if (pg_inst.do_init) {
		ia_css_debug_pipe_graph_dump_prologue();
		pg_inst.do_init = false;
	}

	if (stage->binary) {
		bin_type = "binary";
		if (stage->binary->info->blob)
			snprintf(blob_name, sizeof(blob_name), "%s_stage%d",
				 stage->binary->info->blob->name, stage->stage_num);
	} else if (stage->firmware) {
		bin_type = "firmware";

		strscpy(blob_name, IA_CSS_EXT_ISP_PROG_NAME(stage->firmware),
			sizeof(blob_name));
	}

	/* Guard in case of binaries that don't have any binary_info */
	if (stage->binary_info) {
		char enable_info1[100];
		char enable_info2[100];
		char enable_info3[100];
		char enable_info[302];
		struct ia_css_binary_info *bi = stage->binary_info;

		/* Split it in 2 function-calls to keep the amount of
		 * parameters per call "reasonable"
		 */
		snprintf(enable_info1, sizeof(enable_info1),
			 "%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
			 bi->enable.reduced_pipe ?	"rp," : "",
			 bi->enable.vf_veceven ?		"vfve," : "",
			 bi->enable.dis ?		"dis," : "",
			 bi->enable.dvs_envelope ?	"dvse," : "",
			 bi->enable.uds ?		"uds," : "",
			 bi->enable.dvs_6axis ?		"dvs6," : "",
			 bi->enable.block_output ?	"bo," : "",
			 bi->enable.ds ?			"ds," : "",
			 bi->enable.bayer_fir_6db ?	"bf6," : "",
			 bi->enable.raw_binning ?	"rawb," : "",
			 bi->enable.continuous ?		"cont," : "",
			 bi->enable.s3a ?		"s3a," : "",
			 bi->enable.fpnr ?		"fpnr," : "",
			 bi->enable.sc ?			"sc," : ""
			);

		snprintf(enable_info2, sizeof(enable_info2),
			 "%s%s%s%s%s%s%s%s%s%s%s",
			 bi->enable.macc ?		"macc," : "",
			 bi->enable.output ?		"outp," : "",
			 bi->enable.ref_frame ?		"reff," : "",
			 bi->enable.tnr ?		"tnr," : "",
			 bi->enable.xnr ?		"xnr," : "",
			 bi->enable.params ?		"par," : "",
			 bi->enable.ca_gdc ?		"cagdc," : "",
			 bi->enable.isp_addresses ?	"ispa," : "",
			 bi->enable.in_frame ?		"inf," : "",
			 bi->enable.out_frame ?		"outf," : "",
			 bi->enable.high_speed ?		"hs," : ""
			);

		/* And merge them into one string */
		snprintf(enable_info, sizeof(enable_info), "%s%s",
			 enable_info1, enable_info2);
		{
			int l, p;
			char *ei = enable_info;

			l = strlen(ei);

			/* Replace last ',' with \0 if present */
			if (l && enable_info[l - 1] == ',')
				enable_info[--l] = '\0';

			if (l > ENABLE_LINE_MAX_LENGTH) {
				/* Too big for one line, find last comma */
				p = ENABLE_LINE_MAX_LENGTH;
				while (ei[p] != ',')
					p--;
				/* Last comma found, copy till that comma */
				strscpy(enable_info1, ei, umin(p, sizeof(enable_info1)));

				ei += p + 1;
				l = strlen(ei);

				if (l <= ENABLE_LINE_MAX_LENGTH) {
					/* The 2nd line fits */
					/* we cannot use ei as argument because
					 * it is not guaranteed dword aligned
					 */

					strscpy(enable_info2, ei, umin(l, sizeof(enable_info2)));

					snprintf(enable_info, sizeof(enable_info), "%s\\n%s",
						 enable_info1, enable_info2);

				} else {
					/* 2nd line is still too long */
					p = ENABLE_LINE_MAX_LENGTH;
					while (ei[p] != ',')
						p--;

					strscpy(enable_info2, ei, umin(p, sizeof(enable_info2)));

					ei += p + 1;
					l = strlen(ei);

					if (l <= ENABLE_LINE_MAX_LENGTH) {
						/* The 3rd line fits */
						/* we cannot use ei as argument because
						* it is not guaranteed dword aligned
						*/
						strscpy(enable_info3, ei,
							sizeof(enable_info3));
						snprintf(enable_info, sizeof(enable_info),
							 "%s\\n%s\\n%s",
							 enable_info1, enable_info2,
							 enable_info3);
					} else {
						/* 3rd line is still too long */
						p = ENABLE_LINE_MAX_LENGTH;
						while (ei[p] != ',')
							p--;
						strscpy(enable_info3, ei,
							umin(p, sizeof(enable_info3)));
						ei += p + 1;
						strscpy(enable_info3, ei,
							sizeof(enable_info3));
						snprintf(enable_info, sizeof(enable_info),
							 "%s\\n%s\\n%s",
							 enable_info1, enable_info2,
							 enable_info3);
					}
				}
			}
		}

		dtrace_dot("node [shape = circle, fixedsize=true, width=2.5, label=\"%s\\n%s\\n\\n%s\"]; \"%s(pipe%d)\"",
			   bin_type, blob_name, enable_info, blob_name, id);
	} else {
		dtrace_dot("node [shape = circle, fixedsize=true, width=2.5, label=\"%s\\n%s\\n\"]; \"%s(pipe%d)\"",
			   bin_type, blob_name, blob_name, id);
	}

	if (stage->stage_num == 0) {
		/*
		 * There are some implicit assumptions about which bin is the
		 * input binary e.g. which one is connected to the input system
		 * Priority:
		 * 1) sp_raw_copy bin has highest priority
		 * 2) First stage==0 binary of preview, video or capture
		 */
		if (strlen(dot_id_input_bin) == 0) {
			snprintf(dot_id_input_bin, sizeof(dot_id_input_bin),
				 "%s(pipe%d)", blob_name, id);
		}
	}

	if (stage->args.in_frame) {
		ia_css_debug_pipe_graph_dump_frame(
		    stage->args.in_frame, id, blob_name,
		    "in", true);
	}

	for (i = 0; i < NUM_VIDEO_TNR_FRAMES; i++) {
		if (stage->args.tnr_frames[i]) {
			ia_css_debug_pipe_graph_dump_frame(
			    stage->args.tnr_frames[i], id,
			    blob_name, "tnr_frame", true);
		}
	}

	for (i = 0; i < MAX_NUM_VIDEO_DELAY_FRAMES; i++) {
		if (stage->args.delay_frames[i]) {
			ia_css_debug_pipe_graph_dump_frame(
			    stage->args.delay_frames[i], id,
			    blob_name, "delay_frame", true);
		}
	}

	for (i = 0; i < IA_CSS_BINARY_MAX_OUTPUT_PORTS; i++) {
		if (stage->args.out_frame[i]) {
			ia_css_debug_pipe_graph_dump_frame(
			    stage->args.out_frame[i], id, blob_name,
			    "out", false);
		}
	}

	if (stage->args.out_vf_frame) {
		ia_css_debug_pipe_graph_dump_frame(
		    stage->args.out_vf_frame, id, blob_name,
		    "out_vf", false);
	}
}

void
ia_css_debug_pipe_graph_dump_sp_raw_copy(
    struct ia_css_frame *out_frame)
{
	assert(out_frame);
	if (pg_inst.do_init) {
		ia_css_debug_pipe_graph_dump_prologue();
		pg_inst.do_init = false;
	}

	dtrace_dot("node [shape = circle, fixedsize=true, width=2.5, label=\"%s\\n%s\"]; \"%s(pipe%d)\"",
		   "sp-binary", "sp_raw_copy", "sp_raw_copy", 1);

	snprintf(ring_buffer, sizeof(ring_buffer),
		 "node [shape = box, fixedsize=true, width=2, height=0.7]; \"%p\" [label = \"%s\\n%d(%d) x %d\\nRingbuffer\"];",
		 out_frame,
		 debug_frame_format2str(out_frame->frame_info.format),
		 out_frame->frame_info.res.width,
		 out_frame->frame_info.padded_width,
		 out_frame->frame_info.res.height);

	dtrace_dot(ring_buffer);

	dtrace_dot(
	    "\"%s(pipe%d)\"->\"%p\" [label = out_frame];",
	    "sp_raw_copy", 1, out_frame);

	snprintf(dot_id_input_bin, sizeof(dot_id_input_bin), "%s(pipe%d)",
		 "sp_raw_copy", 1);
}

void
ia_css_debug_pipe_graph_dump_stream_config(
    const struct ia_css_stream_config *stream_config)
{
	pg_inst.width = stream_config->input_config.input_res.width;
	pg_inst.height = stream_config->input_config.input_res.height;
	pg_inst.eff_width = stream_config->input_config.effective_res.width;
	pg_inst.eff_height = stream_config->input_config.effective_res.height;
	pg_inst.stream_format = stream_config->input_config.format;
}

void
ia_css_debug_dump_resolution(
    const struct ia_css_resolution *res,
    const char *label)
{
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "%s: =%d x =%d\n",
			    label, res->width, res->height);
}

void
ia_css_debug_dump_frame_info(
    const struct ia_css_frame_info *info,
    const char *label)
{
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "%s\n", label);
	ia_css_debug_dump_resolution(&info->res, "res");
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "padded_width: %d\n",
			    info->padded_width);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "format: %d\n", info->format);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "raw_bit_depth: %d\n",
			    info->raw_bit_depth);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "raw_bayer_order: %d\n",
			    info->raw_bayer_order);
}

void
ia_css_debug_dump_capture_config(
    const struct ia_css_capture_config *config)
{
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "%s\n", __func__);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "mode: %d\n", config->mode);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "enable_xnr:  %d\n",
			    config->enable_xnr);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "enable_raw_output: %d\n",
			    config->enable_raw_output);
}

void
ia_css_debug_dump_pipe_extra_config(
    const struct ia_css_pipe_extra_config *extra_config)
{
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "%s\n", __func__);
	if (extra_config) {
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE,
				    "enable_raw_binning: %d\n",
				    extra_config->enable_raw_binning);
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "enable_yuv_ds: %d\n",
				    extra_config->enable_yuv_ds);
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE,
				    "enable_high_speed:  %d\n",
				    extra_config->enable_high_speed);
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE,
				    "enable_dvs_6axis: %d\n",
				    extra_config->enable_dvs_6axis);
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE,
				    "enable_reduced_pipe: %d\n",
				    extra_config->enable_reduced_pipe);
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE,
				    "enable_fractional_ds: %d\n",
				    extra_config->enable_fractional_ds);
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "disable_vf_pp: %d\n",
				    extra_config->disable_vf_pp);
	}
}

void
ia_css_debug_dump_pipe_config(
    const struct ia_css_pipe_config *config)
{
	unsigned int i;

	IA_CSS_ENTER_PRIVATE("config = %p", config);
	if (!config) {
		IA_CSS_ERROR("NULL input parameter");
		IA_CSS_LEAVE_PRIVATE("");
		return;
	}
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "mode: %d\n", config->mode);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "isp_pipe_version: %d\n",
			    config->isp_pipe_version);
	ia_css_debug_dump_resolution(&config->bayer_ds_out_res,
				     "bayer_ds_out_res");
	ia_css_debug_dump_resolution(&config->capt_pp_in_res,
				     "capt_pp_in_res");
	ia_css_debug_dump_resolution(&config->vf_pp_in_res, "vf_pp_in_res");

	if (IS_ISP2401) {
		ia_css_debug_dump_resolution(&config->output_system_in_res,
					    "output_system_in_res");
	}
	ia_css_debug_dump_resolution(&config->dvs_crop_out_res,
				     "dvs_crop_out_res");
	for (i = 0; i < IA_CSS_PIPE_MAX_OUTPUT_STAGE; i++) {
		ia_css_debug_dump_frame_info(&config->output_info[i], "output_info");
		ia_css_debug_dump_frame_info(&config->vf_output_info[i],
					     "vf_output_info");
	}
	ia_css_debug_dump_capture_config(&config->default_capture_config);
	ia_css_debug_dump_resolution(&config->dvs_envelope, "dvs_envelope");
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "dvs_frame_delay: %d\n",
			    config->dvs_frame_delay);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "enable_dz: %d\n",
			    config->enable_dz);
	IA_CSS_LEAVE_PRIVATE("");
}

void
ia_css_debug_dump_stream_config_source(
    const struct ia_css_stream_config *config)
{
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "%s()\n", __func__);
	switch (config->mode) {
	case IA_CSS_INPUT_MODE_SENSOR:
	case IA_CSS_INPUT_MODE_BUFFERED_SENSOR:
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "source.port\n");
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "port: %d\n",
				    config->source.port.port);
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "num_lanes: %d\n",
				    config->source.port.num_lanes);
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "timeout: %d\n",
				    config->source.port.timeout);
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "compression: %d\n",
				    config->source.port.compression.type);
		break;
	case IA_CSS_INPUT_MODE_PRBS:
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "source.prbs\n");
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "id: %d\n",
				    config->source.prbs.id);
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "h_blank: %d\n",
				    config->source.prbs.h_blank);
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "v_blank: %d\n",
				    config->source.prbs.v_blank);
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "seed: 0x%x\n",
				    config->source.prbs.seed);
		ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "seed1: 0x%x\n",
				    config->source.prbs.seed1);
		break;
	default:
	case IA_CSS_INPUT_MODE_FIFO:
	case IA_CSS_INPUT_MODE_MEMORY:
		break;
	}
}

void
ia_css_debug_dump_mipi_buffer_config(
    const struct ia_css_mipi_buffer_config *config)
{
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "%s()\n", __func__);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "size_mem_words: %d\n",
			    config->size_mem_words);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "nof_mipi_buffers: %d\n",
			    config->nof_mipi_buffers);
}

void
ia_css_debug_dump_metadata_config(
    const struct ia_css_metadata_config *config)
{
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "%s()\n", __func__);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "data_type: %d\n",
			    config->data_type);
	ia_css_debug_dump_resolution(&config->resolution, "resolution");
}

void
ia_css_debug_dump_stream_config(
    const struct ia_css_stream_config *config,
    int num_pipes)
{
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "%s()\n", __func__);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "num_pipes: %d\n", num_pipes);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "mode: %d\n", config->mode);
	ia_css_debug_dump_stream_config_source(config);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "channel_id: %d\n",
			    config->channel_id);
	ia_css_debug_dump_resolution(&config->input_config.input_res, "input_res");
	ia_css_debug_dump_resolution(&config->input_config.effective_res,
				     "effective_res");
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "format: %d\n",
			    config->input_config.format);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "bayer_order: %d\n",
			    config->input_config.bayer_order);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "sensor_binning_factor: %d\n",
			    config->sensor_binning_factor);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "pixels_per_clock: %d\n",
			    config->pixels_per_clock);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "online: %d\n",
			    config->online);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "init_num_cont_raw_buf: %d\n",
			    config->init_num_cont_raw_buf);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE,
			    "target_num_cont_raw_buf: %d\n",
			    config->target_num_cont_raw_buf);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "pack_raw_pixels: %d\n",
			    config->pack_raw_pixels);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "continuous: %d\n",
			    config->continuous);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "flash_gpio_pin: %d\n",
			    config->flash_gpio_pin);
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, "left_padding: %d\n",
			    config->left_padding);
	ia_css_debug_dump_mipi_buffer_config(&config->mipi_buffer_config);
	ia_css_debug_dump_metadata_config(&config->metadata_config);
}
