/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Support for Intel Camera Imaging ISP subsystem.
 * Copyright (c) 2015, Intel Corporation.
 */

#ifndef _IA_CSS_DEBUG_H_
#define _IA_CSS_DEBUG_H_

/*! \file */

#include <type_support.h>
#include <linux/stdarg.h>
#include <linux/bits.h>
#include "ia_css_types.h"
#include "ia_css_binary.h"
#include "ia_css_frame_public.h"
#include "ia_css_pipe_public.h"
#include "ia_css_stream_public.h"
#include "ia_css_metadata.h"
#include "sh_css_internal.h"
/* ISP2500 */
#include "ia_css_pipe.h"

/* available levels */
/*! Level for tracing errors */
#define IA_CSS_DEBUG_ERROR   1
/*! Level for tracing warnings */
#define IA_CSS_DEBUG_WARNING 3
/*! Level for tracing debug messages */
#define IA_CSS_DEBUG_VERBOSE   5
/*! Level for tracing trace messages a.o. ia_css public function calls */
#define IA_CSS_DEBUG_TRACE   6
/*! Level for tracing trace messages a.o. ia_css private function calls */
#define IA_CSS_DEBUG_TRACE_PRIVATE   7
/*! Level for tracing parameter messages e.g. in and out params of functions */
#define IA_CSS_DEBUG_PARAM   8
/*! Level for tracing info messages */
#define IA_CSS_DEBUG_INFO    9

/* Global variable which controls the verbosity levels of the debug tracing */
extern int dbg_level;

/*! @brief Enum defining the different isp parameters to dump.
 *  Values can be combined to dump a combination of sets.
 */
enum ia_css_debug_enable_param_dump {
	IA_CSS_DEBUG_DUMP_FPN = BIT(0),  /** FPN table */
	IA_CSS_DEBUG_DUMP_OB  = BIT(1),  /** OB table */
	IA_CSS_DEBUG_DUMP_SC  = BIT(2),  /** Shading table */
	IA_CSS_DEBUG_DUMP_WB  = BIT(3),  /** White balance */
	IA_CSS_DEBUG_DUMP_DP  = BIT(4),  /** Defect Pixel */
	IA_CSS_DEBUG_DUMP_BNR = BIT(5),  /** Bayer Noise Reductions */
	IA_CSS_DEBUG_DUMP_S3A = BIT(6),  /** 3A Statistics */
	IA_CSS_DEBUG_DUMP_DE  = BIT(7),  /** De Mosaicing */
	IA_CSS_DEBUG_DUMP_YNR = BIT(8),  /** Luma Noise Reduction */
	IA_CSS_DEBUG_DUMP_CSC = BIT(9),  /** Color Space Conversion */
	IA_CSS_DEBUG_DUMP_GC  = BIT(10), /** Gamma Correction */
	IA_CSS_DEBUG_DUMP_TNR = BIT(11), /** Temporal Noise Reduction */
	IA_CSS_DEBUG_DUMP_ANR = BIT(12), /** Advanced Noise Reduction */
	IA_CSS_DEBUG_DUMP_CE  = BIT(13), /** Chroma Enhancement */
	IA_CSS_DEBUG_DUMP_ALL = BIT(14), /** Dump all device parameters */
};

#define IA_CSS_ERROR(fmt, ...) \
	ia_css_debug_dtrace(IA_CSS_DEBUG_ERROR, \
		"%s() %d: error: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

#define IA_CSS_WARNING(fmt, ...) \
	ia_css_debug_dtrace(IA_CSS_DEBUG_WARNING, \
		"%s() %d: warning: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

/* Logging macros for public functions (API functions) */
#define IA_CSS_ENTER(fmt, ...) \
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, \
		"%s(): enter: " fmt "\n", __func__, ##__VA_ARGS__)

/* Use this macro for small functions that do not call other functions. */
#define IA_CSS_ENTER_LEAVE(fmt, ...) \
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, \
		"%s(): enter: leave: " fmt "\n", __func__, ##__VA_ARGS__)

#define IA_CSS_LEAVE(fmt, ...) \
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, \
		"%s(): leave: " fmt "\n", __func__, ##__VA_ARGS__)

/* Shorthand for returning an int return value */
#define IA_CSS_LEAVE_ERR(__err) \
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE, \
		"%s() %d: leave: return_err=%d\n", __func__, __LINE__, __err)

/* Use this macro for logging other than enter/leave.
 * Note that this macro always uses the PRIVATE logging level.
 */
#define IA_CSS_LOG(fmt, ...) \
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE_PRIVATE, \
		"%s(): " fmt "\n", __func__, ##__VA_ARGS__)

/* Logging macros for non-API functions. These have a lower trace level */
#define IA_CSS_ENTER_PRIVATE(fmt, ...) \
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE_PRIVATE, \
		"%s(): enter: " fmt "\n", __func__, ##__VA_ARGS__)

#define IA_CSS_LEAVE_PRIVATE(fmt, ...) \
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE_PRIVATE, \
		"%s(): leave: " fmt "\n", __func__, ##__VA_ARGS__)

/* Shorthand for returning an int return value */
#define IA_CSS_LEAVE_ERR_PRIVATE(__err) \
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE_PRIVATE, \
		"%s() %d: leave: return_err=%d\n", __func__, __LINE__, __err)

/* Use this macro for small functions that do not call other functions. */
#define IA_CSS_ENTER_LEAVE_PRIVATE(fmt, ...) \
	ia_css_debug_dtrace(IA_CSS_DEBUG_TRACE_PRIVATE, \
		"%s(): enter: leave: " fmt "\n", __func__, ##__VA_ARGS__)

/*! @brief Function for tracing to the provided printf function in the
 *	environment.
 * @param[in]	level		Level of the message.
 * @param[in]	fmt		printf like format string
 * @param[in]	args		arguments for the format string
 */
static inline void __printf(2, 0) ia_css_debug_vdtrace(unsigned int level,
						       const char *fmt,
						       va_list args)
{
	if (dbg_level >= level)
		sh_css_vprint(fmt, args);
}

__printf(2, 3) void ia_css_debug_dtrace(unsigned int level,
					const char *fmt, ...);

/*! @brief Dump internal sp software state.
 * Dumps the sp software state to tracing output.
 * @return	None
 */
void ia_css_debug_dump_sp_sw_debug_info(void);

#if SP_DEBUG != SP_DEBUG_NONE
void ia_css_debug_print_sp_debug_state(
    const struct sh_css_sp_debug_state *state);
#endif

void ia_css_debug_dump_isp_binary(void);

void sh_css_dump_sp_raw_copy_linecount(bool reduced);

/*! @brief Dump the resolution info to the trace output
 * Dumps the resolution info to the trace output.
 * @param[in]	res	pointer to struct ia_css_resolution
 * @param[in]	label	description of resolution output
 * @return	None
 */
void ia_css_debug_dump_resolution(
    const struct ia_css_resolution *res,
    const char *label);

/*! @brief Dump the frame info to the trace output
 * Dumps the frame info to the trace output.
 * @param[in]	info	pointer to struct ia_css_frame_info
 * @param[in]	label	description of frame_info output
 * @return	None
 */
void ia_css_debug_dump_frame_info(
    const struct ia_css_frame_info *info,
    const char *label);

/*! @brief Dump the capture config info to the trace output
 * Dumps the capture config info to the trace output.
 * @param[in]	config	pointer to struct ia_css_capture_config
 * @return	None
 */
void ia_css_debug_dump_capture_config(
    const struct ia_css_capture_config *config);

/*! @brief Dump the pipe extra config info to the trace output
 * Dumps the pipe extra config info to the trace output.
 * @param[in]	extra_config	pointer to struct ia_css_pipe_extra_config
 * @return	None
 */
void ia_css_debug_dump_pipe_extra_config(
    const struct ia_css_pipe_extra_config *extra_config);

/*! @brief Dump the pipe config info to the trace output
 * Dumps the pipe config info to the trace output.
 * @param[in]	config	pointer to struct ia_css_pipe_config
 * @return	None
 */
void ia_css_debug_dump_pipe_config(
    const struct ia_css_pipe_config *config);

/*! @brief Dump the stream config source info to the trace output
 * Dumps the stream config source info to the trace output.
 * @param[in]	config	pointer to struct ia_css_stream_config
 * @return	None
 */
void ia_css_debug_dump_stream_config_source(
    const struct ia_css_stream_config *config);

/*! @brief Dump the mipi buffer config info to the trace output
 * Dumps the mipi buffer config info to the trace output.
 * @param[in]	config	pointer to struct ia_css_mipi_buffer_config
 * @return	None
 */
void ia_css_debug_dump_mipi_buffer_config(
    const struct ia_css_mipi_buffer_config *config);

/*! @brief Dump the metadata config info to the trace output
 * Dumps the metadata config info to the trace output.
 * @param[in]	config	pointer to struct ia_css_metadata_config
 * @return	None
 */
void ia_css_debug_dump_metadata_config(
    const struct ia_css_metadata_config *config);

/*! @brief Dump the stream config info to the trace output
 * Dumps the stream config info to the trace output.
 * @param[in]	config		pointer to struct ia_css_stream_config
 * @param[in]	num_pipes	number of pipes for the stream
 * @return	None
 */
void ia_css_debug_dump_stream_config(
    const struct ia_css_stream_config *config,
    int num_pipes);

#endif /* _IA_CSS_DEBUG_H_ */
