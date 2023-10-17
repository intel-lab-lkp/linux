/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2023 Javier Carrasco <javier.carrasco@wolfvision.net>
 */

#ifndef _TOUCH_OVERLAY
#define _TOUCH_OVERLAY

#include <linux/types.h>

struct input_dev;
struct device;

struct touch_overlay_map {
	struct touch_overlay_segment *touchscreen;
	bool overlay_touchscreen;
	struct touch_overlay_button *buttons;
	u32 button_count;
};

struct touch_overlay_map *touch_overlay_map_overlay(struct input_dev *keypad);

void touch_overlay_get_touchscreen_abs(struct touch_overlay_map *map,
				       u16 *x, u16 *y);

bool touch_overlay_mapped_touchscreen(struct touch_overlay_map *map);

bool touch_overlay_mapped_buttons(struct touch_overlay_map *map);

bool touch_overlay_process_event(struct touch_overlay_map *map,
				 struct input_dev *input,
				 u32 *x, u32 *y, u32 slot);

#endif
