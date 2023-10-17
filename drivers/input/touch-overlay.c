// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Helper functions for overlay objects on touchscreens
 *
 *  Copyright (c) 2023 Javier Carrasco <javier.carrasco@wolfvision.net>
 */

#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touch-overlay.h>
#include <linux/module.h>
#include <linux/property.h>

enum touch_overlay_valid_objects {
	TOUCH_OVERLAY_TS,
	TOUCH_OVERLAY_BTN,
};

static const char *const object_names[] = {
	[TOUCH_OVERLAY_TS] = "overlay-touchscreen",
	[TOUCH_OVERLAY_BTN] = "overlay-buttons",
};

struct touch_overlay_segment {
	u32 x_origin;
	u32 y_origin;
	u32 x_size;
	u32 y_size;
};

struct touch_overlay_button {
	struct touch_overlay_segment segment;
	u32 key;
	bool pressed;
	int slot;
};

static int touch_overlay_get_segment_props(struct fwnode_handle *segment_node,
					   struct touch_overlay_segment *segment)
{
	int error;

	error = fwnode_property_read_u32(segment_node, "x-origin",
					 &segment->x_origin);
	if (error < 0)
		return error;

	error = fwnode_property_read_u32(segment_node, "y-origin",
					 &segment->y_origin);
	if (error < 0)
		return error;

	error = fwnode_property_read_u32(segment_node, "x-size",
					 &segment->x_size);
	if (error < 0)
		return error;

	error = fwnode_property_read_u32(segment_node, "y-size",
					 &segment->y_size);
	if (error < 0)
		return error;

	return 0;
}

static int
touch_overlay_get_button_properties(struct device *dev,
				    struct fwnode_handle *overlay_node,
				    struct touch_overlay_button *btn)
{
	struct fwnode_handle *btn_node;
	int error;
	int j = 0;

	fwnode_for_each_child_node(overlay_node, btn_node) {
		error = touch_overlay_get_segment_props(btn_node,
							&btn[j].segment);
		if (error < 0)
			goto button_put;

		error = fwnode_property_read_u32(btn_node, "linux,code",
						 &btn[j].key);
		if (error < 0)
			goto button_put;

		dev_dbg(dev, "Added button at (%u, %u), size %ux%u, code=%u\n",
			btn[j].segment.x_origin, btn[j].segment.y_origin,
			btn[j].segment.x_size, btn[j].segment.y_size, btn[j].key);
		j++;
	}

	return 0;

button_put:
	fwnode_handle_put(btn_node);
	return error;
}

static void touch_overlay_set_button_caps(struct touch_overlay_map *map,
					  struct input_dev *dev)
{
	int i;

	for (i = 0; i < map->button_count; i++)
		input_set_capability(dev, EV_KEY, map->buttons[i].key);
}

static int touch_overlay_count_buttons(struct device *dev)
{
	struct fwnode_handle *overlay;
	struct fwnode_handle *button;
	int count = 0;

	overlay = device_get_named_child_node(dev,
					      object_names[TOUCH_OVERLAY_BTN]);
	if (!overlay)
		return 0;

	fwnode_for_each_child_node(overlay, button)
		count++;
	fwnode_handle_put(overlay);

	return count;
}

static int touch_overlay_map_touchscreen(struct device *dev,
					 struct touch_overlay_map *map)
{
	struct fwnode_handle *ts_node;
	int error = 0;

	ts_node = device_get_named_child_node(dev,
					      object_names[TOUCH_OVERLAY_TS]);
	if (!ts_node)
		return 0;

	map->touchscreen =
		devm_kzalloc(dev, sizeof(*map->touchscreen), GFP_KERNEL);
	if (!map->touchscreen) {
		error = -ENOMEM;
		goto handle_put;
	}
	error = touch_overlay_get_segment_props(ts_node, map->touchscreen);
	if (error < 0)
		goto handle_put;

	map->overlay_touchscreen = true;
	dev_dbg(dev, "Added overlay touchscreen at (%u, %u), size %u x %u\n",
		map->touchscreen->x_origin, map->touchscreen->y_origin,
		map->touchscreen->x_size, map->touchscreen->y_size);

handle_put:
	fwnode_handle_put(ts_node);

	return error;
}

static int touch_overlay_map_buttons(struct device *dev,
				     struct touch_overlay_map *map)
{
	struct fwnode_handle *button;
	u32 count;
	int error = 0;

	count = touch_overlay_count_buttons(dev);
	if (!count)
		return 0;

	map->buttons = devm_kcalloc(dev, count,
				    sizeof(*map->buttons), GFP_KERNEL);
	if (!map->buttons) {
		error = -ENOMEM;
		goto map_buttons_ret;
	}
	button = device_get_named_child_node(dev,
					     object_names[TOUCH_OVERLAY_BTN]);
	if (unlikely(!button)) {
		error = -ENODEV;
		goto map_buttons_ret;
	}

	error = touch_overlay_get_button_properties(dev, button,
						    map->buttons);

	if (error < 0)
		goto map_buttons_put;

	map->button_count = count;

map_buttons_put:
	fwnode_handle_put(button);
map_buttons_ret:
	return error;
}

static bool touch_overlay_defined_objects(struct device *dev)
{
	struct fwnode_handle *obj_node;
	int i;

	for (i = 0; i < ARRAY_SIZE(object_names); i++) {
		obj_node = device_get_named_child_node(dev, object_names[i]);
		if (obj_node) {
			fwnode_handle_put(obj_node);
			return true;
		}
		fwnode_handle_put(obj_node);
	}

	return false;
}

/**
 * touch_overlay_map_overlay - map overlay objects from the device tree and set
 * key capabilities if buttons are defined.
 * @keypad: pointer to the already allocated input_dev.
 *
 * Returns a pointer to the object mapping struct.
 *
 * If a keypad input device is provided and overlay buttons are defined,
 * its button capabilities are set accordingly.
 */
struct touch_overlay_map *touch_overlay_map_overlay(struct input_dev *keypad)
{
	struct device *dev = keypad->dev.parent;
	struct touch_overlay_map *map = NULL;
	int error;

	if (!touch_overlay_defined_objects(dev))
		return NULL;

	map = devm_kzalloc(dev, sizeof(*map), GFP_KERNEL);
	if (!map) {
		error = -ENOMEM;
		goto map_error;
	}
	error = touch_overlay_map_touchscreen(dev, map);
	if (error < 0)
		goto map_error;

	error = touch_overlay_map_buttons(dev, map);
	if (error < 0)
		goto map_error;

	touch_overlay_set_button_caps(map, keypad);

	return map;

map_error:
	return ERR_PTR(error);
}
EXPORT_SYMBOL(touch_overlay_map_overlay);

/**
 * touch_overlay_get_touchscreen_abs - get abs size from the overlay node
 * @map: pointer to the struct that holds the object mapping
 * @x: horizontal abs
 * @y: vertical abs
 *
 */
void touch_overlay_get_touchscreen_abs(struct touch_overlay_map *map, u16 *x,
				       u16 *y)
{
	*x = map->touchscreen->x_size - 1;
	*y = map->touchscreen->y_size - 1;
}
EXPORT_SYMBOL(touch_overlay_get_touchscreen_abs);

static bool touch_overlay_segment_event(struct touch_overlay_segment *seg,
					u32 x, u32 y)
{
	if (!seg)
		return false;

	if (x >= seg->x_origin && x < (seg->x_origin + seg->x_size) &&
	    y >= seg->y_origin && y < (seg->y_origin + seg->y_size))
		return true;

	return false;
}

/**
 * touch_overlay_mapped_touchscreen - check if an overlay touchscreen is mapped
 * @map: pointer to the struct that holds the object mapping
 *
 * Returns true if an overlay touchscreen is mapped or false otherwise.
 */
bool touch_overlay_mapped_touchscreen(struct touch_overlay_map *map)
{
	if (!map || !map->overlay_touchscreen)
		return false;

	return true;
}
EXPORT_SYMBOL(touch_overlay_mapped_touchscreen);

/**
 * touch_overlay_mapped_buttons - check if overlay buttons are mapped
 * @map: pointer to the struct that holds the object mapping
 *
 * Returns true if overlay buttons mapped or false otherwise.
 */
bool touch_overlay_mapped_buttons(struct touch_overlay_map *map)
{
	if (!map || !map->button_count)
		return false;

	return true;
}
EXPORT_SYMBOL(touch_overlay_mapped_buttons);

static bool touch_overlay_mt_on_touchscreen(struct touch_overlay_map *map,
					    u32 *x, u32 *y)
{
	bool contact = x && y;

	if (!touch_overlay_mapped_touchscreen(map))
		return true;

	/* Let the caller handle events with no coordinates (release) */
	if (!contact)
		return false;

	if (touch_overlay_segment_event(map->touchscreen, *x, *y)) {
		*x -= map->touchscreen->x_origin;
		*y -= map->touchscreen->y_origin;
		return true;
	}

	return false;
}

static bool touch_overlay_button_event(struct input_dev *input,
				       struct touch_overlay_button *button,
				       const u32 *x, const u32 *y, u32 slot)
{
	bool contact = x && y;

	if (!contact && button->pressed && button->slot == slot) {
		button->pressed = false;
		input_report_key(input, button->key, false);
		input_sync(input);
		return true;
	} else if (contact && touch_overlay_segment_event(&button->segment,
							  *x, *y)) {
		button->pressed = true;
		button->slot = slot;
		input_report_key(input, button->key, true);
		input_sync(input);
		return true;
	}

	return false;
}

/**
 * touch_overlay_process_event - process input events according to the overlay
 * mapping. This function acts as a filter to release the calling driver from
 * the events that are either related to overlay buttons or out of the overlay
 * touchscreen area if defined.
 * @map: pointer to the struct that holds the object mapping
 * @input: pointer to the input device associated to the event
 * @x: pointer to the x coordinate (NULL if not available - no contact)
 * @y: pointer to the y coordinate (NULL if not available - no contact)
 * @slot: slot associated to the event
 *
 * Returns true if the event was processed (reported for valid key events
 * and dropped for events outside the overlay touchscreen area) or false
 * if the event must be processed by the caller. In that case this function
 * shifts the (x,y) coordinates to the overlay touchscreen axis if required
 */
bool touch_overlay_process_event(struct touch_overlay_map *map,
				 struct input_dev *input,
				 u32 *x, u32 *y, u32 slot)
{
	int i;

	if (!map)
		return false;

	/* buttons must be prioritized over overlay touchscreens to account for
	 * overlappings e.g. a button inside the touchscreen area
	 */
	if (touch_overlay_mapped_buttons(map)) {
		for (i = 0; i < map->button_count; i++) {
			if (touch_overlay_button_event(input, &map->buttons[i],
						       x, y, slot))
				return true;
		}
	}
	/* valid touch events on the overlay touchscreen are left for the client
	 * to be processed/reported according to its (possibly) unique features
	 */
	return !touch_overlay_mt_on_touchscreen(map, x, y);
}
EXPORT_SYMBOL(touch_overlay_process_event);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Helper functions for overlay objects on touch devices");
