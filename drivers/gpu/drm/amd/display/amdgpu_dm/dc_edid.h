/* SPDX-License-Identifier: MIT */

#ifndef __DC_EDID_H__
#define __DC_EDID_H__

#include "dc.h"

bool dc_edid_is_same_edid(struct dc_sink *prev_sink,
			  struct dc_sink *current_sink);
void dc_edid_copy_edid_to_dc(struct dc_sink *dc_sink,
			     const void *edid, int len);
void dc_edid_sink_edid_free(struct dc_sink *sink);

#endif /* __DC_EDID_H__ */
