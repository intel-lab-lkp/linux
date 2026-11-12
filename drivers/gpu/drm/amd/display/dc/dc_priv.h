/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef __DC_PRIV_H__
#define __DC_PRIV_H__

/* FILE POLICY AND INTENDED USAGE:
 * This header defines struct dc, the DC core state. It is private to the dc
 * directory and must never be included from DM, DAL modules or any other
 * code outside DC. Public DC interfaces are declared in dc.h; use the
 * accessors implemented in dc/core/dc_accessors.c instead of reaching into
 * struct dc.
 */

#include "dc.h"

/* Bandwidth validation trace macros. Usable only from DC-internal code
 * because they dereference the dc-local variable of the including function.
 */
#define BW_VAL_TRACE_SETUP() \
		unsigned long long end_tick = 0; \
		unsigned long long voltage_level_tick = 0; \
		unsigned long long watermark_tick = 0; \
		unsigned long long start_tick = dc->debug.bw_val_profile.enable ? \
				dm_get_timestamp(dc->ctx) : 0

#define BW_VAL_TRACE_COUNT() \
		do { \
			if (dc->debug.bw_val_profile.enable) \
				dc->debug.bw_val_profile.total_count++; \
		} while (0)

#define BW_VAL_TRACE_SKIP(status) \
		do { \
			if (dc->debug.bw_val_profile.enable) { \
				if (!voltage_level_tick) \
					voltage_level_tick = dm_get_timestamp(dc->ctx); \
				dc->debug.bw_val_profile.skip_ ## status ## _count++; \
			} \
		} while (0)

#define BW_VAL_TRACE_END_VOLTAGE_LEVEL() \
		do { \
			if (dc->debug.bw_val_profile.enable) \
				voltage_level_tick = dm_get_timestamp(dc->ctx); \
		} while (0)

#define BW_VAL_TRACE_END_WATERMARKS() \
		do { \
			if (dc->debug.bw_val_profile.enable) \
				watermark_tick = dm_get_timestamp(dc->ctx); \
		} while (0)

#define BW_VAL_TRACE_FINISH() \
		do { \
			if (dc->debug.bw_val_profile.enable) { \
				end_tick = dm_get_timestamp(dc->ctx); \
				dc->debug.bw_val_profile.total_ticks += end_tick - start_tick; \
				dc->debug.bw_val_profile.voltage_level_ticks += \
						voltage_level_tick - start_tick; \
				if (watermark_tick) { \
					dc->debug.bw_val_profile.watermark_ticks += \
							watermark_tick - voltage_level_tick; \
					dc->debug.bw_val_profile.rq_dlg_ticks += \
							end_tick - watermark_tick; \
				} \
			} \
		} while (0)

struct dc {
	struct dc_debug_options debug;
	struct dc_versions versions;
	struct dc_caps caps;
	struct dc_check_config check_config;
	struct dc_cap_funcs cap_funcs;
	struct dc_config config;
	struct dc_bounding_box_overrides bb_overrides;
	struct dc_bug_wa work_arounds;
	struct dc_context *ctx;
	struct dc_phy_addr_space_config vm_pa_config;

	uint8_t link_count;
	struct dc_link *links[MAX_LINKS];
	uint8_t lowest_dpia_link_index;
	struct link_service *link_srv;

	struct dc_state *current_state;
	/**
	 * @scratch_dc_context: Pre-allocated states for the no-allocation update
	 * path, created only when dc_config.preallocate_scratch_state is set.
	 * Two are kept because the commit path installs the new context as
	 * @current_state, so a single buffer would alias it and self-copy.
	 */
	struct dc_state *scratch_dc_context[2];
	struct resource_pool *res_pool;

	/**
	 * @update_scratch_pool: Per-commit scratch buffers for dc_update_state.
	 */
	struct dc_memory_pool *update_scratch_pool;  // MAX_STREAMS + 1

	struct clk_mgr *clk_mgr;

	/* Display Engine Clock levels */
	struct dm_pp_clock_levels sclk_lvls;

	/* Inputs into BW and WM calculations. */
	struct bw_calcs_dceip *bw_dceip;
	struct bw_calcs_vbios *bw_vbios;
	struct dcn_soc_bounding_box *dcn_soc;
	struct dcn_ip_params *dcn_ip;
	struct display_mode_lib dml;

	/* HW functions */
	struct hw_sequencer_funcs hwss;
	struct dce_hwseq *hwseq;

	/* Require to optimize clocks and bandwidth for added/removed planes */
	bool optimized_required;
	bool idle_optimizations_allowed;
	bool enable_c20_dtm_b0;

	/* Require to maintain clocks and bandwidth for UEFI enabled HW */

	/* For eDP to know the switching state of SmartMux */
	bool is_switch_in_progress_orig;
	bool is_switch_in_progress_dest;

	/* FBC compressor */
	struct compressor *fbc_compressor;

	struct dc_debug_data debug_data;
	struct dpcd_vendor_signature vendor_signature;

	const char *build_id;
	struct vm_helper *vm_helper;

	uint32_t *dcn_reg_offsets;
	uint32_t *nbio_reg_offsets;
	uint32_t *clk_reg_offsets;

	/* Scratch memory */
	struct {
		struct {
			/*
			 * For matching clock_limits table in driver with table
			 * from PMFW.
			 */
			struct _vcs_dpi_voltage_scaling_st clock_limits[DC__VOLTAGE_STATES];
		} update_bw_bounding_box;
		struct dc_scratch_space current_state;
		struct dc_scratch_space new_state;
		/* Used so we don't need to allocate stream on the stack */
		struct dc_stream_state temp_stream;
		struct dc_link temp_link;
		/* Any of the pipes indicated here should be unlocked first */
		bool pipes_to_unlock_first[MAX_PIPES];
	} scratch;

	struct dml2_configuration_options dml2_options;
	struct dml2_configuration_options dml2_dc_power_options;
	enum dc_acpi_cm_power_state power_state;
	struct soc_and_ip_translator *soc_and_ip_translator;
};
#endif /* __DC_PRIV_H__ */
