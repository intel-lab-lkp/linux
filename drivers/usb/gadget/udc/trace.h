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

DECLARE_EVENT_CLASS_PRINT_INIT(udc_log_gadget,
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
	TP_printk("speed %d/%d state %d %dmA [%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s] --> %d",
		__entry->speed, __entry->max_speed, __entry->state, __entry->mA,
		tg.sg_supported ? "sg:" : "",
		tg.is_otg ? "OTG:" : "",
		tg.is_a_peripheral ? "a_peripheral:" : "",
		tg.b_hnp_enable ? "b_hnp:" : "",
		tg.a_hnp_support ? "a_hnp:" : "",
		tg.a_alt_hnp_support ? "a_alt_hnp:" : "",
		tg.hnp_polling_support ? "hnp_poll:" : "",
		tg.host_request_flag ? "hostreq:" : "",
		tg.quirk_ep_out_aligned_size ? "out_aligned:" : "",
		tg.quirk_altset_not_supp ? "no_altset:" : "",
		tg.quirk_stall_not_supp ? "no_stall:" : "",
		tg.quirk_zlp_not_supp ? "no_zlp" : "",
		tg.quirk_avoids_skb_reserve ? "no_skb_reserve" : "",
		tg.is_selfpowered ? "self-powered:" : "bus-powered:",
		tg.deactivated ? "deactivated:" : "activated:",
		tg.connected ? "connected" : "disconnected",
		tg.lpm_capable ? "lpm-capable" : "",
		tg.wakeup_capable ? "wakeup-capable" : "",
		tg.wakeup_armed ? "wakeup-armed" : "",
		__entry->ret),
	TP_printk_init(
		struct usb_gadget tg;
		tg.dw1 = __entry->gdw1;
	)
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

DECLARE_EVENT_CLASS_PRINT_INIT(udc_log_ep,
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
	TP_printk("%s: mps %d/%d streams %d mult %d burst %d addr %02x %s%s --> %d",
		__s, te.maxpacket, te.maxpacket_limit,
		te.max_streams, te.mult, te.maxburst,
		te.address, te.claimed ? "claimed:" : "released:",
		te.enabled ? "enabled" : "disabled", ret),
	TP_printk_init(
		struct usb_ep te;
		char __s[9];
		te.dw1 = __entry->edw1;
		te.dw2 = __entry->edw2;
		te.dw3 = __entry->edw3;
		snprintf(__s, 9, "ep%d%s", te.address, \
			(te.caps.dir_in && te.caps.dir_out) ? "" : \
			te.caps.dir_in ? "in" : "out");
	)
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

DECLARE_EVENT_CLASS_PRINT_INIT(udc_log_req,
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
		__s,__entry->req,  __entry->actual, __entry->length,
		__entry->num_mapped_sgs, __entry->num_sgs, tr.stream_id,
		tr.zero ? "Z" : "z",
		tr.short_not_ok ? "S" : "s",
		tr.no_interrupt ? "i" : "I",
		__entry->status, __entry->ret),
	TP_printk_init(
		struct usb_ep te;
		struct usb_request tr;
		char __s[9];
		te.dw3 = __entry->edw3;
		tr.dw1 = __entry->rdw1;
		snprintf(__s, 9, "ep%d%s", te.address, \
			(te.caps.dir_in && te.caps.dir_out) ? "" : \
			te.caps.dir_in ? "in" : "out");
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
