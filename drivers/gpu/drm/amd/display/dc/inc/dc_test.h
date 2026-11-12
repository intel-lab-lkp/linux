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

#ifndef __DC_TEST_H__
#define __DC_TEST_H__

/*
 * KUnit support for building DC test doubles.
 *
 * struct dc is private to DC, so test code outside the dc directory cannot
 * allocate one or poke its members directly. These helpers construct an
 * empty fake DC instance and let tests install the pieces they need. They
 * exist only when the amdgpu_dm KUnit tests are enabled and must never be
 * used outside tests.
 */

struct dc;
struct dc_context;
struct dc_state;
struct dc_link;
struct dc_caps;
struct dc_config;
struct dc_debug_options;
struct dc_cap_funcs;
struct link_service;
struct resource_pool;
struct clk_mgr;
struct compressor;
struct hw_sequencer_funcs;

#if IS_ENABLED(CONFIG_DRM_AMD_DC_KUNIT_TEST)

/*
 * Allocate a zeroed fake DC instance. Free it with dc_test_destroy() (or
 * register that as a KUnit action).
 */
struct dc *dc_test_create(void);
void dc_test_destroy(struct dc *dc);

void dc_test_set_ctx(struct dc *dc, struct dc_context *ctx);
void dc_test_set_current_state(struct dc *dc, struct dc_state *state);
void dc_test_install_link_service(struct dc *dc,
		struct link_service *link_srv);
void dc_test_set_res_pool(struct dc *dc, struct resource_pool *res_pool);
void dc_test_set_clk_mgr(struct dc *dc, struct clk_mgr *clk_mgr);
void dc_test_set_fbc_compressor(struct dc *dc, struct compressor *compressor);
void dc_test_set_link(struct dc *dc, unsigned int index, struct dc_link *link);
void dc_test_set_link_count(struct dc *dc, unsigned int count);
void dc_test_set_idle_optimizations_allowed(struct dc *dc, bool allowed);

/* Mutable views of embedded sub-structures, for test faking only. */
struct dc_caps *dc_test_caps(struct dc *dc);
struct dc_config *dc_test_config(struct dc *dc);
struct dc_debug_options *dc_test_debug_options(struct dc *dc);
struct dc_cap_funcs *dc_test_cap_funcs(struct dc *dc);
struct hw_sequencer_funcs *dc_test_hwss(struct dc *dc);

#endif /* IS_ENABLED(CONFIG_DRM_AMD_DC_KUNIT_TEST) */

#endif /* __DC_TEST_H__ */
