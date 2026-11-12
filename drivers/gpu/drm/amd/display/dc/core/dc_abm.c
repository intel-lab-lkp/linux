// SPDX-License-Identifier: MIT
//
// Copyright 2026 Advanced Micro Devices, Inc.
//
/* FILE POLICY AND INTENDED USAGE:
 * DC-side ABM (Adaptive Backlight Management) hardware primitives. DC
 * owns the ABM and DMCU objects in the resource pool, so the dispatch to
 * their function tables lives here; the ABM curve and iRAM table baking
 * is policy and stays in modules/power, which hands over opaque blobs
 * through these entry points.
 */
#include "dm_services.h"
#include "dc.h"
#include "dc_priv.h"
#include "core_types.h"
#include "hw/dmcu.h"
#include "hw/abm.h"

bool dc_has_dmcu(const struct dc *dc)
{
	return dc->res_pool && dc->res_pool->dmcu != NULL;
}

bool dc_dmcu_is_initialized(struct dc *dc)
{
	if (!dc->res_pool || !dc->res_pool->dmcu)
		return false;

	return dc->res_pool->dmcu->funcs->is_dmcu_initialized(
			dc->res_pool->dmcu);
}

bool dc_dmcu_load_iram(struct dc *dc, unsigned int start_offset,
		const char *src, unsigned int bytes)
{
	if (!dc->res_pool || !dc->res_pool->dmcu)
		return false;

	return dc->res_pool->dmcu->funcs->load_iram(
			dc->res_pool->dmcu, start_offset, src, bytes);
}

bool dc_abm_is_supported(struct dc *dc, unsigned int inst)
{
	if (dc->res_pool->abm == NULL && dc->res_pool->multiple_abms[inst] == NULL)
		return false;

	return true;
}

bool dc_abm_program_config(struct dc *dc, const char *config,
		unsigned int size, unsigned int inst)
{
	if (dc->res_pool->abm == NULL && dc->res_pool->multiple_abms[inst] == NULL)
		return false;

	if (dc->res_pool->multiple_abms[inst])
		return dc->res_pool->multiple_abms[inst]->funcs->init_abm_config(
				dc->res_pool->multiple_abms[inst], config, size, inst);

	return dc->res_pool->abm->funcs->init_abm_config(
			dc->res_pool->abm, config, size, 0);
}

bool dc_abm_set_event(struct dc *dc,
		unsigned int full_screen, unsigned int trans_info,
		unsigned int hdr_mode, unsigned int scaling_enable,
		unsigned int scaling_strength_map,
		unsigned int inst)
{
	struct resource_pool *res_pool = dc->res_pool;
	bool result = false;

	if (res_pool->abm == NULL && res_pool->multiple_abms[inst] == NULL)
		return false;

	if (res_pool->multiple_abms[inst]) {
		if (res_pool->multiple_abms[inst]->funcs->set_abm_event)
			result = res_pool->multiple_abms[inst]->funcs->set_abm_event(
				res_pool->multiple_abms[inst], full_screen, trans_info,
					hdr_mode, scaling_enable, scaling_strength_map, inst);
	} else {
		if (res_pool->abm->funcs->set_abm_event)
			result = res_pool->abm->funcs->set_abm_event(
				res_pool->abm, full_screen, trans_info,
					hdr_mode, scaling_enable, scaling_strength_map, inst);
	}

	return result;
}

bool dc_abm_set_strength(struct dc *dc,
		unsigned int strength,
		unsigned int inst)
{
	struct resource_pool *res_pool = dc->res_pool;
	bool result = false;

	if (res_pool->abm == NULL && res_pool->multiple_abms[inst] == NULL)
		return false;

	if (res_pool->multiple_abms[inst]) {
		if (res_pool->multiple_abms[inst]->funcs->set_abm_level)
			result = res_pool->multiple_abms[inst]->funcs->set_abm_level(
				res_pool->multiple_abms[inst], strength);
	} else {
		if (res_pool->abm->funcs->set_abm_level)
			result = res_pool->abm->funcs->set_abm_level(
					res_pool->abm, strength);
	}

	return result;
}

unsigned int dc_get_dmcu_abm_version(const struct dc *dc)
{
	if (!dc->res_pool || !dc->res_pool->dmcu)
		return 0;

	return dc->res_pool->dmcu->dmcu_version.abm_version;
}
