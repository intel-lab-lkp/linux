// SPDX-License-Identifier: MIT
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
 * Authors: AMD
 *
 */

/* FILE POLICY AND INTENDED USAGE:
 * This file implements the struct dc accessors declared in dc.h. struct dc
 * is private to DC; this file is the single place that maps public accessor
 * names onto struct dc fields. Keep it free of functional behavior so the
 * mapping stays obvious and auditable.
 */
#include "dc.h"
#include "dc_priv.h"
#include "core_types.h"
#include "hw/dchubbub.h"
#include "dm_helpers.h"	/* EXPORT_IF_KUNIT */

uint8_t dc_get_link_count(const struct dc *dc)
{
	return dc->link_count;
}

struct dc_context *dc_get_ctx(const struct dc *dc)
{
	return dc->ctx;
}
EXPORT_IF_KUNIT(dc_get_ctx);

enum dce_version dc_get_dce_version(const struct dc *dc)
{
	return dc->ctx->dce_version;
}

const struct dc_caps *dc_get_caps(const struct dc *dc)
{
	return &dc->caps;
}

unsigned int dc_get_disable_ips(const struct dc *dc)
{
	return dc->config.disable_ips;
}
EXPORT_IF_KUNIT(dc_get_disable_ips);

bool dc_get_enable_frl(const struct dc *dc)
{
	return dc->config.enable_frl;
}

bool dc_get_consolidated_dpia_dp_lt(const struct dc *dc)
{
	return dc->config.consolidated_dpia_dp_lt;
}

bool dc_get_usb4_bw_alloc_support(const struct dc *dc)
{
	return dc->config.usb4_bw_alloc_support;
}

void dc_set_allow_edp_hotplug_detection(struct dc *dc,
		enum dc_hpd_enable_select hpd_select)
{
	dc->config.allow_edp_hotplug_detection = hpd_select;
}

unsigned int dc_get_allow_edp_hotplug_detection(const struct dc *dc)
{
	return dc->config.allow_edp_hotplug_detection;
}

const struct dc_debug_options *dc_get_debug_options(const struct dc *dc)
{
	return &dc->debug;
}

struct dc_debug_options *dc_get_debug_options_mutable(struct dc *dc)
{
	return &dc->debug;
}

struct dc_state *dc_get_current_state(const struct dc *dc)
{
	return dc->current_state;
}
EXPORT_IF_KUNIT(dc_get_current_state);

bool dc_is_idle_optimizations_allowed(const struct dc *dc)
{
	return dc->idle_optimizations_allowed;
}
EXPORT_IF_KUNIT(dc_is_idle_optimizations_allowed);

unsigned int dc_get_audio_instances(const struct dc *dc,
		unsigned int *inst_array)
{
	unsigned int i;

	for (i = 0; i < dc->res_pool->audio_count; i++)
		inst_array[i] = dc->res_pool->audios[i]->inst;

	return dc->res_pool->audio_count;
}
EXPORT_IF_KUNIT(dc_get_audio_instances);

unsigned int dc_get_pipe_count(const struct dc *dc)
{
	return dc->res_pool->pipe_count;
}
EXPORT_IF_KUNIT(dc_get_pipe_count);

unsigned int dc_get_usb4_dpia_count(const struct dc *dc)
{
	return dc->res_pool->usb4_dpia_count;
}
EXPORT_IF_KUNIT(dc_get_usb4_dpia_count);

unsigned int dc_get_dmcu_psp_version(const struct dc *dc)
{
	if (!dc->res_pool || !dc->res_pool->dmcu)
		return 0;

	return dc->res_pool->dmcu->psp_version;
}

void dc_get_mall_en(struct dc *dc, unsigned int *mall_in_use)
{
	struct hubbub *hubbub = dc->res_pool ? dc->res_pool->hubbub : NULL;

	*mall_in_use = false;

	if (hubbub && hubbub->funcs->get_mall_en)
		hubbub->funcs->get_mall_en(hubbub, mall_in_use);
}


bool dc_get_dcc_compression_cap(const struct dc *dc,
		const struct dc_dcc_surface_param *input,
		struct dc_surface_dcc_cap *output)
{
	if (!dc->cap_funcs.get_dcc_compression_cap)
		return false;

	return dc->cap_funcs.get_dcc_compression_cap(dc, input, output);
}

bool dc_get_subvp_en(struct dc *dc, struct dc_state *context)
{
	if (!dc->cap_funcs.get_subvp_en)
		return false;

	return dc->cap_funcs.get_subvp_en(dc, context);
}

bool dc_log_hw_state(struct dc *dc, struct dc_log_buffer_ctx *log_ctx)
{
	if (!dc->hwss.log_hw_state)
		return false;

	dc->hwss.log_hw_state(dc, log_ctx);
	return true;
}

bool dc_get_dcc_en_bits(struct dc *dc, int *dcc_en_bits)
{
	if (!dc->hwss.get_dcc_en_bits)
		return false;

	dc->hwss.get_dcc_en_bits(dc, dcc_en_bits);
	return true;
}

void dc_prepare_ddc(struct dc_link *link)
{
	if (link->dc->hwss.prepare_ddc)
		link->dc->hwss.prepare_ddc(link);
}

bool dc_is_fbc_compressor_available(const struct dc *dc)
{
	return dc->fbc_compressor != NULL;
}
EXPORT_IF_KUNIT(dc_is_fbc_compressor_available);

void dc_set_fbc_gpu_addr(struct dc *dc, uint64_t gpu_addr)
{
	dc->ctx->fbc_gpu_addr = gpu_addr;
}

void dc_set_dml2_alt_ch_cfg(struct dc *dc,
		const struct dml2_soc_alt_ch_info *alt_ch_cfg)
{
	dc->dml2_options.alt_ch_cfg = *alt_ch_cfg;
}
