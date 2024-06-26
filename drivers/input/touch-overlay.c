// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Helper functions for overlay objects on touchscreens
 *
 *  Copyright (c) 2023 Javier Carrasco <javier.carrasco@wolfvision.net>
 */

#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touch-overlay.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/property.h>

struct touch_overlay_segment {
	struct list_head list;
	u32 x_origin;
	u32 y_origin;
	u32 x_size;
	u32 y_size;
	u32 key;
	bool pressed;
	int slot;
};

static int touch_overlay_get_segment(struct fwnode_handle *segment_node,
				     struct touch_overlay_segment *segment,
				     struct input_dev *input)
{
	int error;

	error = fwnode_property_read_u32(segment_node, "x-origin",
					 &segment->x_origin);
	if (error)
		return error;

	error = fwnode_property_read_u32(segment_node, "y-origin",
					 &segment->y_origin);
	if (error)
		return error;

	error = fwnode_property_read_u32(segment_node, "x-size",
					 &segment->x_size);
	if (error)
		return error;

	error = fwnode_property_read_u32(segment_node, "y-size",
					 &segment->y_size);
	if (error)
		return error;

	error = fwnode_property_read_u32(segment_node, "linux,code",
					 &segment->key);
	if (!error)
		input_set_capability(input, EV_KEY, segment->key);
	else if (error != -EINVAL)
		return error;

	return 0;
}

/**
 * touch_overlay_map - map overlay objects from the device tree and set
 * key capabilities if buttons are defined.
 * @list: pointer to the list that will hold the segments
 * @input: pointer to the already allocated input_dev
 *
 * Returns 0 on success and error number otherwise.
 *
 * If buttons are defined, key capabilities are set accordingly.
 */
int touch_overlay_map(struct list_head *list, struct input_dev *input)
{
	struct fwnode_handle *overlay, *fw_segment;
	struct device *dev = input->dev.parent;
	struct touch_overlay_segment *segment;
	int error = 0;

	overlay = device_get_named_child_node(dev, "touch-overlay");
	if (!overlay)
		return 0;

	fwnode_for_each_available_child_node(overlay, fw_segment) {
		segment = devm_kzalloc(dev, sizeof(*segment), GFP_KERNEL);
		if (!segment) {
			fwnode_handle_put(fw_segment);
			error = -ENOMEM;
			break;
		}
		error = touch_overlay_get_segment(fw_segment, segment, input);
		if (error) {
			fwnode_handle_put(fw_segment);
			break;
		}
		list_add_tail(&segment->list, list);
	}
	fwnode_handle_put(overlay);

	return error;
}
EXPORT_SYMBOL(touch_overlay_map);

/**
 * touch_overlay_get_touchscreen_abs - get abs size from the touchscreen area.
 * @list: pointer to the list that holds the segments
 * @x: horizontal abs
 * @y: vertical abs
 */
void touch_overlay_get_touchscreen_abs(struct list_head *list, u16 *x, u16 *y)
{
	struct touch_overlay_segment *segment;
	struct list_head *ptr;

	list_for_each(ptr, list) {
		segment = list_entry(ptr, struct touch_overlay_segment, list);
		if (!segment->key) {
			*x = segment->x_size - 1;
			*y = segment->y_size - 1;
			break;
		}
	}
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
 * touch_overlay_mapped_touchscreen - check if a touchscreen area is mapped
 * @list: pointer to the list that holds the segments
 *
 * Returns true if a touchscreen area is mapped or false otherwise.
 */
bool touch_overlay_mapped_touchscreen(struct list_head *list)
{
	struct touch_overlay_segment *segment;
	struct list_head *ptr;

	list_for_each(ptr, list) {
		segment = list_entry(ptr, struct touch_overlay_segment, list);
		if (!segment->key)
			return true;
	}

	return false;
}
EXPORT_SYMBOL(touch_overlay_mapped_touchscreen);

static bool touch_overlay_event_on_ts(struct list_head *list, u32 *x, u32 *y)
{
	struct touch_overlay_segment *segment;
	struct list_head *ptr;
	bool valid_touch = true;

	if (!x || !y)
		return false;

	list_for_each(ptr, list) {
		segment = list_entry(ptr, struct touch_overlay_segment, list);
		if (segment->key)
			continue;

		if (touch_overlay_segment_event(segment, *x, *y)) {
			*x -= segment->x_origin;
			*y -= segment->y_origin;
			return true;
		}
		/* ignore touch events outside the defined area */
		valid_touch = false;
	}

	return valid_touch;
}

static bool touch_overlay_button_event(struct input_dev *input,
				       struct touch_overlay_segment *segment,
				       const u32 *x, const u32 *y, u32 slot)
{
	bool contact = x && y;

	if (segment->slot == slot && segment->pressed) {
		/* button release */
		if (!contact) {
			segment->pressed = false;
			input_report_key(input, segment->key, false);
			input_sync(input);
			return true;
		}

		/* sliding out of the button releases it */
		if (!touch_overlay_segment_event(segment, *x, *y)) {
			segment->pressed = false;
			input_report_key(input, segment->key, false);
			input_sync(input);
			/* keep available for a possible touch event */
			return false;
		}
		/* ignore sliding on the button while pressed */
		return true;
	} else if (contact && touch_overlay_segment_event(segment, *x, *y)) {
		segment->pressed = true;
		segment->slot = slot;
		input_report_key(input, segment->key, true);
		input_sync(input);
		return true;
	}

	return false;
}

/**
 * touch_overlay_process_event - process input events according to the overlay
 * mapping. This function acts as a filter to release the calling driver from
 * the events that are either related to overlay buttons or out of the overlay
 * touchscreen area, if defined.
 * @list: pointer to the list that holds the segments
 * @input: pointer to the input device associated to the event
 * @x: pointer to the x coordinate (NULL if not available - no contact)
 * @y: pointer to the y coordinate (NULL if not available - no contact)
 * @slot: slot associated to the event
 *
 * Returns true if the event was processed (reported for valid key events
 * and dropped for events outside the overlay touchscreen area) or false
 * if the event must be processed by the caller. In that case this function
 * shifts the (x,y) coordinates to the overlay touchscreen axis if required.
 */
bool touch_overlay_process_event(struct list_head *list,
				 struct input_dev *input,
				 u32 *x, u32 *y, u32 slot)
{
	struct touch_overlay_segment *segment;
	struct list_head *ptr;

	/*
	 * buttons must be prioritized over overlay touchscreens to account for
	 * overlappings e.g. a button inside the touchscreen area.
	 */
	list_for_each(ptr, list) {
		segment = list_entry(ptr, struct touch_overlay_segment, list);
		if (segment->key &&
		    touch_overlay_button_event(input, segment, x, y, slot))
			return true;
	}

	/*
	 * valid touch events on the overlay touchscreen are left for the client
	 * to be processed/reported according to its (possibly) unique features.
	 */
	return !touch_overlay_event_on_ts(list, x, y);
}
EXPORT_SYMBOL(touch_overlay_process_event);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Helper functions for overlay objects on touch devices");
