// SPDX-License-Identifier: MIT
#include "dc.h"
#include "dc_edid.h"
#include <drm/drm_edid.h>

bool dc_edid_is_same_edid(struct dc_sink *prev_sink,
			  struct dc_sink *current_sink)
{
	struct dc_edid *old_edid = &prev_sink->dc_edid;
	struct dc_edid *new_edid = &current_sink->dc_edid;

       if (old_edid->length != new_edid->length)
               return false;

       if (new_edid->length == 0)
               return false;

       return (memcmp(old_edid->raw_edid,
                      new_edid->raw_edid, new_edid->length) == 0);
}

void dc_edid_copy_edid_to_dc(struct dc_sink *dc_sink,
			     const void *edid,
			     int len)
{
	memmove(dc_sink->dc_edid.raw_edid, edid, len);
	dc_sink->dc_edid.length = len;
}

void dc_edid_sink_edid_free(struct dc_sink *sink)
{
	drm_edid_free(sink->drm_edid);
}
