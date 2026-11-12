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
 */

/* FILE POLICY AND INTENDED USAGE:
 * Implementation of the KUnit test double API declared in dc_test.h.
 * struct dc is private to DC; this file is the only place outside of the
 * accessors that maps test operations onto struct dc members. It carries
 * no functional behavior.
 */
#include <linux/slab.h>
#include <linux/export.h>

#include "dc.h"
#include "dc_test.h"

struct dc *dc_test_create(void)
{
	return kvzalloc(sizeof(struct dc), GFP_KERNEL);
}
EXPORT_SYMBOL(dc_test_create);

void dc_test_destroy(struct dc *dc)
{
	kvfree(dc);
}
EXPORT_SYMBOL(dc_test_destroy);

void dc_test_set_ctx(struct dc *dc, struct dc_context *ctx)
{
	dc->ctx = ctx;
}
EXPORT_SYMBOL(dc_test_set_ctx);

void dc_test_set_current_state(struct dc *dc, struct dc_state *state)
{
	dc->current_state = state;
}
EXPORT_SYMBOL(dc_test_set_current_state);

void dc_test_install_link_service(struct dc *dc,
		struct link_service *link_srv)
{
	dc->link_srv = link_srv;
}
EXPORT_SYMBOL(dc_test_install_link_service);

void dc_test_set_res_pool(struct dc *dc, struct resource_pool *res_pool)
{
	dc->res_pool = res_pool;
}
EXPORT_SYMBOL(dc_test_set_res_pool);

void dc_test_set_clk_mgr(struct dc *dc, struct clk_mgr *clk_mgr)
{
	dc->clk_mgr = clk_mgr;
}
EXPORT_SYMBOL(dc_test_set_clk_mgr);

void dc_test_set_fbc_compressor(struct dc *dc, struct compressor *compressor)
{
	dc->fbc_compressor = compressor;
}
EXPORT_SYMBOL(dc_test_set_fbc_compressor);

void dc_test_set_link(struct dc *dc, unsigned int index, struct dc_link *link)
{
	if (index >= MAX_LINKS)
		return;

	dc->links[index] = link;
}
EXPORT_SYMBOL(dc_test_set_link);

void dc_test_set_link_count(struct dc *dc, unsigned int count)
{
	dc->link_count = count;
}
EXPORT_SYMBOL(dc_test_set_link_count);

void dc_test_set_idle_optimizations_allowed(struct dc *dc, bool allowed)
{
	dc->idle_optimizations_allowed = allowed;
}
EXPORT_SYMBOL(dc_test_set_idle_optimizations_allowed);

struct dc_caps *dc_test_caps(struct dc *dc)
{
	return &dc->caps;
}
EXPORT_SYMBOL(dc_test_caps);

struct dc_config *dc_test_config(struct dc *dc)
{
	return &dc->config;
}
EXPORT_SYMBOL(dc_test_config);

struct dc_debug_options *dc_test_debug_options(struct dc *dc)
{
	return &dc->debug;
}
EXPORT_SYMBOL(dc_test_debug_options);

struct dc_cap_funcs *dc_test_cap_funcs(struct dc *dc)
{
	return &dc->cap_funcs;
}
EXPORT_SYMBOL(dc_test_cap_funcs);

struct hw_sequencer_funcs *dc_test_hwss(struct dc *dc)
{
	return &dc->hwss;
}
EXPORT_SYMBOL(dc_test_hwss);
