// SPDX-License-Identifier: MIT
#include "amdgpu_dm/dc_edid.h"
#include "dc.h"

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
