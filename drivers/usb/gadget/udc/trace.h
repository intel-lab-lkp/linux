// SPDX-License-Identifier: GPL-2.0
/*
 * udc.c - Core UDC Framework
 *
 * Copyright (C) 2016 Intel Corporation
 * Author: Felipe Balbi <felipe.balbi@linux.intel.com>
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM gadget

#if !defined(__UDC_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define __UDC_TRACE_H

#include <linux/types.h>
#include <linux/tracepoint.h>
#include <asm/byteorder.h>
#include <linux/usb/gadget.h>

DECLARE_EVENT_CLASS(udc_log_gadget,
	TP_PROTO(struct usb_gadget *g, int ret),
	TP_ARGS(g, ret),
	TP_STRUCT__entry(
		__field(enum usb_device_speed, speed)
		__field(enum usb_device_speed, max_speed)
		__field(enum usb_device_state, state)
		__field(unsigned, mA)
		__field(u32, gdw1)
		__field(int, ret)
	),
	TP_fast_assign(
		__entry->speed = g->speed;
		__entry->max_speed = g->max_speed;
		__entry->state = g->state;
		__entry->mA = g->mA;
		__entry->gdw1 = g->dw1;
		__entry->ret = ret;
	),
	TP_printk("speed %d/%d state %d %dmA [%s] --> %d",
		__entry->speed, __entry->max_speed, __entry->state, __entry->mA,
		__print_flags(__entry->gdw1, ":", USB_GADGET_FLAGS),
		__entry->ret)
);

DEFINE_EVENT(udc_log_gadget, usb_gadget_frame_number,
	TP_PROTO(struct usb_gadget *g, int ret),
	TP_ARGS(g, ret)
);

DEFINE_EVENT(udc_log_gadget, usb_gadget_wakeup,
	TP_PROTO(struct usb_gadget *g, int ret),
	TP_ARGS(g, ret)
);

DEFINE_EVENT(udc_log_gadget, usb_gadget_set_remote_wakeup,
	TP_PROTO(struct usb_gadget *g, int ret),
	TP_ARGS(g, ret)
);

DEFINE_EVENT(udc_log_gadget, usb_gadget_set_selfpowered,
	TP_PROTO(struct usb_gadget *g, int ret),
	TP_ARGS(g, ret)
);

DEFINE_EVENT(udc_log_gadget, usb_gadget_clear_selfpowered,
	TP_PROTO(struct usb_gadget *g, int ret),
	TP_ARGS(g, ret)
);

DEFINE_EVENT(udc_log_gadget, usb_gadget_vbus_connect,
	TP_PROTO(struct usb_gadget *g, int ret),
	TP_ARGS(g, ret)
);

DEFINE_EVENT(udc_log_gadget, usb_gadget_vbus_draw,
	TP_PROTO(struct usb_gadget *g, int ret),
	TP_ARGS(g, ret)
);

DEFINE_EVENT(udc_log_gadget, usb_gadget_vbus_disconnect,
	TP_PROTO(struct usb_gadget *g, int ret),
	TP_ARGS(g, ret)
);

DEFINE_EVENT(udc_log_gadget, usb_gadget_connect,
	TP_PROTO(struct usb_gadget *g, int ret),
	TP_ARGS(g, ret)
);

DEFINE_EVENT(udc_log_gadget, usb_gadget_disconnect,
	TP_PROTO(struct usb_gadget *g, int ret),
	TP_ARGS(g, ret)
);

DEFINE_EVENT(udc_log_gadget, usb_gadget_deactivate,
	TP_PROTO(struct usb_gadget *g, int ret),
	TP_ARGS(g, ret)
);

DEFINE_EVENT(udc_log_gadget, usb_gadget_activate,
	TP_PROTO(struct usb_gadget *g, int ret),
	TP_ARGS(g, ret)
);

DECLARE_EVENT_CLASS(udc_log_ep,
	TP_PROTO(struct usb_ep *ep, int ret),
	TP_ARGS(ep, ret),
	TP_STRUCT__entry(
		__field(u32, edw3)
		__field(u32, edw1)
		__field(u32, edw2)
		__field(int, ret)
	),
	TP_fast_assign(
		__entry->edw3 = ep->dw3;
		__entry->edw1 = ep->dw1;
		__entry->edw2 = ep->dw2;
		__entry->ret = ret;
	),
	TP_printk("%s: mps %d/%d streams %d mult %d burst %d addr %02x %s --> %d",
		usb_gadget_ep_name(__get_buf(USB_EP_MAX_NAME_LEN), __entry->edw3),
		u32_get_bits(__entry->edw1, USB_EP_MAXPACKET),
		u32_get_bits(__entry->edw1, USB_EP_MAXPACKET_LIMIT),
		u32_get_bits(__entry->edw2, USB_EP_MAX_STREAMS),
		u32_get_bits(__entry->edw2, USB_EP_MULT),
		u32_get_bits(__entry->edw2, USB_EP_MAXBURST),
		u32_get_bits(__entry->edw3, USB_EP_ADDRESS),
		__print_flags(__entry->edw3, ":", USB_EP_FLAGS), ret)
);

DEFINE_EVENT(udc_log_ep, usb_ep_set_maxpacket_limit,
	TP_PROTO(struct usb_ep *ep, int ret),
	TP_ARGS(ep, ret)
);

DEFINE_EVENT(udc_log_ep, usb_ep_enable,
	TP_PROTO(struct usb_ep *ep, int ret),
	TP_ARGS(ep, ret)
);

DEFINE_EVENT(udc_log_ep, usb_ep_disable,
	TP_PROTO(struct usb_ep *ep, int ret),
	TP_ARGS(ep, ret)
);

DEFINE_EVENT(udc_log_ep, usb_ep_set_halt,
	TP_PROTO(struct usb_ep *ep, int ret),
	TP_ARGS(ep, ret)
);

DEFINE_EVENT(udc_log_ep, usb_ep_clear_halt,
	TP_PROTO(struct usb_ep *ep, int ret),
	TP_ARGS(ep, ret)
);

DEFINE_EVENT(udc_log_ep, usb_ep_set_wedge,
	TP_PROTO(struct usb_ep *ep, int ret),
	TP_ARGS(ep, ret)
);

DEFINE_EVENT(udc_log_ep, usb_ep_fifo_status,
	TP_PROTO(struct usb_ep *ep, int ret),
	TP_ARGS(ep, ret)
);

DEFINE_EVENT(udc_log_ep, usb_ep_fifo_flush,
	TP_PROTO(struct usb_ep *ep, int ret),
	TP_ARGS(ep, ret)
);

DECLARE_EVENT_CLASS(udc_log_req,
	TP_PROTO(struct usb_ep *ep, struct usb_request *req, int ret),
	TP_ARGS(ep, req, ret),
	TP_STRUCT__entry(
		__field(u32, edw3)
		__field(unsigned, length)
		__field(unsigned, actual)
		__field(unsigned, num_sgs)
		__field(unsigned, num_mapped_sgs)
		__field(u32, rdw1)
		__field(int, status)
		__field(int, ret)
		__field(struct usb_request *, req)
	),
	TP_fast_assign(
		__entry->edw3 = ep->dw3;
		__entry->length = req->length;
		__entry->actual = req->actual;
		__entry->num_sgs = req->num_sgs;
		__entry->num_mapped_sgs = req->num_mapped_sgs;
		__entry->rdw1 = req->dw1;
		__entry->status = req->status;
		__entry->ret = ret;
		__entry->req = req;
	),
	TP_printk("%s: req %p length %d/%d sgs %d/%d stream %d %s status %d --> %d",
		usb_gadget_ep_name(__get_buf(USB_EP_MAX_NAME_LEN), __entry->edw3),
		__entry->req, __entry->actual, __entry->length,
		__entry->num_mapped_sgs, __entry->num_sgs,
		u32_get_bits(__entry->rdw1, USB_REQ_STREAM_ID),
		__print_flags(__entry->rdw1, ":", USB_REQ_FLAGS),
		__entry->status, __entry->ret)
);

DEFINE_EVENT(udc_log_req, usb_ep_alloc_request,
	TP_PROTO(struct usb_ep *ep, struct usb_request *req, int ret),
	TP_ARGS(ep, req, ret)
);

DEFINE_EVENT(udc_log_req, usb_ep_free_request,
	TP_PROTO(struct usb_ep *ep, struct usb_request *req, int ret),
	TP_ARGS(ep, req, ret)
);

DEFINE_EVENT(udc_log_req, usb_ep_queue,
	TP_PROTO(struct usb_ep *ep, struct usb_request *req, int ret),
	TP_ARGS(ep, req, ret)
);

DEFINE_EVENT(udc_log_req, usb_ep_dequeue,
	TP_PROTO(struct usb_ep *ep, struct usb_request *req, int ret),
	TP_ARGS(ep, req, ret)
);

DEFINE_EVENT(udc_log_req, usb_gadget_giveback_request,
	TP_PROTO(struct usb_ep *ep, struct usb_request *req, int ret),
	TP_ARGS(ep, req, ret)
);

#endif /* __UDC_TRACE_H */

/* this part has to be here */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace

#include <trace/define_trace.h>
