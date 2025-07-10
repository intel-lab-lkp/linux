/* SPDX-License-Identifier: GPL-2.0 */
/* input tracepoints
 *
 * Copyright (C) 2025 WangYuli <wangyuli@uniontech.com>
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM input

#if !defined(_TRACE_INPUT_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_INPUT_H

#include <linux/tracepoint.h>
#include <linux/input.h>

/**
 * input_event - called when an input event is processed
 * @dev: input device that generated the event
 * @type: event type (EV_KEY, EV_REL, EV_ABS, etc.)
 * @code: event code within the type
 * @value: event value
 *
 * This tracepoint fires for every input event processed by the input core.
 * It can be used to monitor input device activity and debug input issues.
 */
TRACE_EVENT(
	input_event,

	TP_PROTO(struct input_dev *dev, unsigned int type, unsigned int code,
		 int value),

	TP_ARGS(dev, type, code, value),

	TP_STRUCT__entry(__string(name, dev->name ?: "unknown") __field(
		unsigned int, type) __field(unsigned int, code)
				 __field(int, value) __field(u16, bustype)
					 __field(u16, vendor)
						 __field(u16, product)),

	TP_fast_assign(__assign_str(name); __entry->type = type;
		       __entry->code = code; __entry->value = value;
		       __entry->bustype = dev->id.bustype;
		       __entry->vendor = dev->id.vendor;
		       __entry->product = dev->id.product;),

	TP_printk(
		"dev=%s type=%u code=%u value=%d bus=%04x vendor=%04x product=%04x",
		__get_str(name), __entry->type, __entry->code, __entry->value,
		__entry->bustype, __entry->vendor, __entry->product));

/**
 * input_device_register - called when an input device is registered
 * @dev: input device being registered
 *
 * This tracepoint fires when a new input device is registered with the
 * input subsystem. Useful for monitoring device hotplug events.
 */
TRACE_EVENT(
	input_device_register,

	TP_PROTO(struct input_dev *dev),

	TP_ARGS(dev),

	TP_STRUCT__entry(__string(name, dev->name ?: "unknown") __string(
		phys, dev->phys ?: "") __field(u16, bustype)
				 __field(u16, vendor) __field(u16, product)
					 __field(u16, version)),

	TP_fast_assign(__assign_str(name); __assign_str(phys);
		       __entry->bustype = dev->id.bustype;
		       __entry->vendor = dev->id.vendor;
		       __entry->product = dev->id.product;
		       __entry->version = dev->id.version;),

	TP_printk(
		"dev=%s phys=%s bus=%04x vendor=%04x product=%04x version=%04x",
		__get_str(name), __get_str(phys), __entry->bustype,
		__entry->vendor, __entry->product, __entry->version));

/**
 * input_device_unregister - called when an input device is unregistered
 * @dev: input device being unregistered
 *
 * This tracepoint fires when an input device is unregistered from the
 * input subsystem. Useful for monitoring device hotplug events.
 */
TRACE_EVENT(input_device_unregister,

	    TP_PROTO(struct input_dev *dev),

	    TP_ARGS(dev),

	    TP_STRUCT__entry(__string(name, dev->name ?: "unknown") __string(
		    phys, dev->phys ?: "") __field(u16, bustype)
				     __field(u16, vendor)
					     __field(u16, product)),

	    TP_fast_assign(__assign_str(name); __assign_str(phys);
			   __entry->bustype = dev->id.bustype;
			   __entry->vendor = dev->id.vendor;
			   __entry->product = dev->id.product;),

	    TP_printk("dev=%s phys=%s bus=%04x vendor=%04x product=%04x",
		      __get_str(name), __get_str(phys), __entry->bustype,
		      __entry->vendor, __entry->product));

/**
 * input_handler_connect - called when an input handler connects to a device
 * @handler: input handler
 * @dev: input device
 * @minor: assigned minor number (if applicable)
 *
 * This tracepoint fires when an input handler (like evdev, mousedev) connects
 * to an input device, creating a new input handle.
 */
TRACE_EVENT(input_handler_connect,

	    TP_PROTO(struct input_handler *handler, struct input_dev *dev,
		     int minor),

	    TP_ARGS(handler, dev, minor),

	    TP_STRUCT__entry(__string(handler_name, handler->name)
				     __string(dev_name, dev->name ?: "unknown")
					     __field(int, minor)),

	    TP_fast_assign(__assign_str(handler_name); __assign_str(dev_name);
			   __entry->minor = minor;),

	    TP_printk("handler=%s dev=%s minor=%d", __get_str(handler_name),
		      __get_str(dev_name), __entry->minor));

/**
 * input_grab_device - called when a device is grabbed by a handler
 * @dev: input device being grabbed
 * @handle: input handle doing the grab
 *
 * This tracepoint fires when an input device is grabbed exclusively
 * by an input handler, typically for security or special processing.
 */
TRACE_EVENT(input_grab_device,

	    TP_PROTO(struct input_dev *dev, struct input_handle *handle),

	    TP_ARGS(dev, handle),

	    TP_STRUCT__entry(__string(dev_name, dev->name ?: "unknown")
				     __string(handler_name,
					      handle->handler->name)),

	    TP_fast_assign(__assign_str(dev_name); __assign_str(handler_name);),

	    TP_printk("dev=%s grabbed_by=%s", __get_str(dev_name),
		      __get_str(handler_name)));

/**
 * input_release_device - called when a grabbed device is released
 * @dev: input device being released
 * @handle: input handle releasing the grab
 *
 * This tracepoint fires when an input device grab is released.
 */
TRACE_EVENT(input_release_device,

	    TP_PROTO(struct input_dev *dev, struct input_handle *handle),

	    TP_ARGS(dev, handle),

	    TP_STRUCT__entry(__string(dev_name, dev->name ?: "unknown")
				     __string(handler_name,
					      handle->handler->name)),

	    TP_fast_assign(__assign_str(dev_name); __assign_str(handler_name);),

	    TP_printk("dev=%s released_by=%s", __get_str(dev_name),
		      __get_str(handler_name)));

DECLARE_EVENT_CLASS(input_device_state,

		    TP_PROTO(struct input_dev *dev),

		    TP_ARGS(dev),

		    TP_STRUCT__entry(__string(name, dev->name ?: "unknown")
					     __field(unsigned int, users)
						     __field(bool, inhibited)),

		    TP_fast_assign(__assign_str(name);
				   __entry->users = dev->users;
				   __entry->inhibited = dev->inhibited;),

		    TP_printk("dev=%s users=%u inhibited=%s", __get_str(name),
			      __entry->users,
			      __entry->inhibited ? "true" : "false"));

/**
 * input_open_device - called when an input device is opened
 * @dev: input device being opened
 *
 * This tracepoint fires when the user count for an input device increases,
 * typically when a userspace application opens the device.
 */
DEFINE_EVENT(input_device_state, input_open_device,

	     TP_PROTO(struct input_dev *dev),

	     TP_ARGS(dev));

/**
 * input_close_device - called when an input device is closed
 * @dev: input device being closed
 *
 * This tracepoint fires when the user count for an input device decreases,
 * typically when a userspace application closes the device.
 */
DEFINE_EVENT(input_device_state, input_close_device,

	     TP_PROTO(struct input_dev *dev),

	     TP_ARGS(dev));

/**
 * input_inhibit_device - called when an input device is inhibited
 * @dev: input device being inhibited
 *
 * This tracepoint fires when an input device is inhibited (disabled),
 * usually for power management or security reasons.
 */
DEFINE_EVENT(input_device_state, input_inhibit_device,

	     TP_PROTO(struct input_dev *dev),

	     TP_ARGS(dev));

/**
 * input_uninhibit_device - called when an input device is uninhibited
 * @dev: input device being uninhibited
 *
 * This tracepoint fires when an input device is uninhibited (re-enabled)
 * after being previously inhibited.
 */
DEFINE_EVENT(input_device_state, input_uninhibit_device,

	     TP_PROTO(struct input_dev *dev),

	     TP_ARGS(dev));

#endif /* _TRACE_INPUT_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
