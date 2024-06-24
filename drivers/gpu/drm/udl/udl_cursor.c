// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * udl_cursor.c
 *
 * Copyright (c) 2015 The Chromium OS Authors
 * Copyright (c) 2024 Synaptics Incorporated. All Rights Reserved.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/iosys-map.h>
#include <drm/drm_crtc_helper.h>

#include "udl_cursor.h"
#include "udl_drv.h"

void udl_cursor_get_hline(struct udl_cursor *cursor, int x, int y,
		struct udl_cursor_hline *hline)
{
	if (!cursor || !cursor->enabled ||
		x >= cursor->x + UDL_CURSOR_W ||
		y < cursor->y || y >= cursor->y + UDL_CURSOR_H) {
		hline->buffer = NULL;
		return;
	}

	hline->buffer = &cursor->buffer[UDL_CURSOR_W * (y - cursor->y)];
	hline->width = UDL_CURSOR_W;
	hline->offset = x - cursor->x;
}

/*
 * Return pre-computed cursor blend value defined as:
 * R: 5 bits (bit 0:4)
 * G: 6 bits (bit 5:10)
 * B: 5 bits (bit 11:15)
 * A: 7 bits (bit 16:22)
 */
static uint32_t cursor_blend_val32(uint32_t pix)
{
	/* range of alpha_scaled is 0..64 */
	uint32_t alpha_scaled = ((pix >> 24) * 65) >> 8;

	return ((pix >> 3) & 0x1f) |
		((pix >> 5) & 0x7e0) |
		((pix >> 8) & 0xf800) |
		(alpha_scaled << 16);
}

int udl_cursor_download(struct udl_cursor *cursor,
		const struct iosys_map *map)
{
	uint32_t *src_ptr, *dst_ptr;
	size_t i;

	src_ptr = map->vaddr;
	dst_ptr = cursor->buffer;
	for (i = 0; i < UDL_CURSOR_BUF; ++i)
		dst_ptr[i] = cursor_blend_val32(le32_to_cpu(src_ptr[i]));
	return 0;
}

int udl_cursor_move(struct udl_cursor *cursor, int x, int y)
{
	cursor->x = x;
	cursor->y = y;
	return 0;
}

void udl_cursor_damage_clear(struct udl_cursor *cursor)
{
	cursor->damage.x1 = INT_MAX;
	cursor->damage.y1 = INT_MAX;
	cursor->damage.x2 = 0;
	cursor->damage.y2 = 0;
}

void udl_rect_merge(struct drm_rect *rect, struct drm_rect *rect2)
{
	rect->x1 = min(rect->x1, rect2->x1);
	rect->y1 = min(rect->y1, rect2->y1);
	rect->x2 = max(rect->x2, rect2->x2);
	rect->y2 = max(rect->y2, rect2->y2);
}

void udl_cursor_mark_damage_from_plane(struct udl_cursor *cursor, struct drm_plane_state *state)
{
	struct drm_rect rect;

	rect.x1 = (state->crtc_x < 0) ? 0 : state->crtc_x;
	rect.y1 = (state->crtc_y < 0) ? 0 : state->crtc_y;
	rect.x2 = state->crtc_x + state->crtc_w;
	rect.y2 = state->crtc_y + state->crtc_h;

	udl_rect_merge(&cursor->damage, &rect);
}


