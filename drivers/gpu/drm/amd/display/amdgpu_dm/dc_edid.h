/* SPDX-License-Identifier: MIT */

#ifndef __DC_EDID_H__
#define __DC_EDID_H__

#include "dc.h"

bool dc_edid_is_same_edid(struct dc_sink *prev_sink,
			  struct dc_sink *current_sink);

#endif /* __DC_EDID_H__ */
