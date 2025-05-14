// SPDX-License-Identifier: MIT
#include "amdgpu_dm/dc_edid.h"
#include "dc.h"
#include <drm/drm_edid.h>

bool dc_edid_is_same_edid(struct dc_sink *prev_sink,
			  struct dc_sink *current_sink)
{
	return drm_edid_eq(prev_sink->drm_edid, current_sink->drm_edid);
}

void dc_edid_copy_edid_to_dc(struct dc_sink *dc_sink,
			     const void *edid,
			     int len)
{
	dc_sink->drm_edid = drm_edid_dup((const struct drm_edid *) edid);
}

void dc_edid_copy_edid_to_sink(struct dc_sink *sink)
{
	const struct edid *edid;
	uint32_t edid_length;

	edid = drm_edid_raw(sink->drm_edid); // FIXME: Get rid of drm_edid_raw()
	edid_length = EDID_LENGTH * (edid->extensions + 1);
	memcpy(sink->dc_edid.raw_edid, (uint8_t *) edid, edid_length);
	sink->dc_edid.length = edid_length;
}

void dc_edid_sink_edid_free(struct dc_sink *sink)
{
	drm_edid_free(sink->drm_edid);
}
