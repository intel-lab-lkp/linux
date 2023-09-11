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
	TP_printk("speed %d/%d state %d %dmA [%s%s%s%s%s%s%s%s%s%s%s%s%s%s] --> %d",
		__entry->speed, __entry->max_speed, __entry->state, __entry->mA,
		USB_GADGET_SG_SUPPORTED(__entry->gdw1) ? "sg:" : "",
		USB_GADGET_IS_OTG(__entry->gdw1) ? "OTG:" : "",
		USB_GADGET_IS_A_PERIPHERAL(__entry->gdw1) ? "a_peripheral:" : "",
		USB_GADGET_B_HNP_ENABLE(__entry->gdw1) ? "b_hnp:" : "",
		USB_GADGET_A_HNP_SUPPORT(__entry->gdw1) ? "a_hnp:" : "",
		USB_GADGET_HNP_POLLING_SUPPORT(__entry->gdw1) ? "hnp_poll:" : "",
		USB_GADGET_HOST_REQUEST_FLAG(__entry->gdw1) ? "hostreq:" : "",
		USB_GADGET_QUIRK_EP_OUT_ALIGNED_SIZE(__entry->gdw1) ? "out_aligned:" : "",
		USB_GADGET_QUIRK_ALTSET_NOT_SUPP(__entry->gdw1) ? "no_altset:" : "",
		USB_GADGET_QUIRK_STALL_NOT_SUPP(__entry->gdw1) ? "no_stall:" : "",
		USB_GADGET_QUIRK_ZLP_NOT_SUPP(__entry->gdw1) ? "no_zlp" : "",
		USB_GADGET_IS_SELFPOWERED(__entry->gdw1) ? "self-powered:" : "bus-powered:",
		USB_GADGET_DEACTIVATED(__entry->gdw1) ? "deactivated:" : "activated:",
		USB_GADGET_CONNECTED(__entry->gdw1) ? "connected" : "disconnected",
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
		__field(u8, address)
		__field(bool, claimed)
		__field(bool, enabled)
		__field(int, ret)
	),
	TP_fast_assign(
		__entry->edw3 = ep->dw3;
		__entry->edw1 = ep->dw1;
		__entry->edw2 = ep->dw2;
		__entry->address = ep->address,
		__entry->claimed = ep->claimed;
		__entry->enabled = ep->enabled;
		__entry->ret = ret;
	),
	TP_printk("%s: mps %d/%d streams %d mult %d burst %d addr %02x %s%s --> %d",
		USB_EP_NAME(__entry->edw3), USB_EP_MAXPACKET(__entry->edw1),
		USB_EP_MAXPACKET_LIMIT(__entry->edw1),
		USB_EP_MAX_STREAMS(__entry->edw2), USB_EP_MULT(__entry->edw2),
		USB_EP_MAXBURST(__entry->edw2),
		__entry->address, __entry->claimed ? "claimed:" : "released:",
		__entry->enabled ? "enabled" : "disabled", ret)
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
	TP_printk("%s: req %p length %d/%d sgs %d/%d stream %d %s%s%s status %d --> %d",
		USB_EP_NAME(__entry->edw3), __entry->req, __entry->actual, __entry->length,
		__entry->num_mapped_sgs, __entry->num_sgs, USB_REQ_STREAM_ID(__entry->rdw1),
		USB_REQ_ZERO(__entry->rdw1) ? "Z" : "z",
		USB_REQ_SHORT_NOT_OK(__entry->rdw1) ? "S" : "s",
		USB_REQ_NO_INTERRUPT(__entry->rdw1) ? "i" : "I",
		__entry->status, __entry->ret
	)
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
