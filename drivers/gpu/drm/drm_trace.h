/* SPDX-License-Identifier: GPL-2.0 */
#if !defined(_DRM_TRACE_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _DRM_TRACE_H_

#include <linux/stringify.h>
#include <linux/types.h>
#include <linux/tracepoint.h>

struct drm_file;

#undef TRACE_SYSTEM
#define TRACE_SYSTEM drm
#define TRACE_INCLUDE_FILE drm_trace

TRACE_EVENT(drm_vblank_event,
	    TP_PROTO(int crtc, unsigned int seq, ktime_t time, bool high_prec),
	    TP_ARGS(crtc, seq, time, high_prec),
	    TP_STRUCT__entry(
		    __field(int, crtc)
		    __field(unsigned int, seq)
		    __field(ktime_t, time)
		    __field(bool, high_prec)
		    ),
	    TP_fast_assign(
		    __entry->crtc = crtc;
		    __entry->seq = seq;
		    __entry->time = time;
		    __entry->high_prec = high_prec;
			),
	    TP_printk("crtc=%d, seq=%u, time=%lld, high-prec=%s",
			__entry->crtc, __entry->seq, __entry->time,
			__entry->high_prec ? "true" : "false")
);

TRACE_EVENT(drm_vblank_event_queued,
	    TP_PROTO(struct drm_file *file, int crtc, unsigned int seq),
	    TP_ARGS(file, crtc, seq),
	    TP_STRUCT__entry(
		    __field(struct drm_file *, file)
		    __field(int, crtc)
		    __field(unsigned int, seq)
		    ),
	    TP_fast_assign(
		    __entry->file = file;
		    __entry->crtc = crtc;
		    __entry->seq = seq;
		    ),
	    TP_printk("file=%p, crtc=%d, seq=%u", __entry->file, __entry->crtc, \
		      __entry->seq)
);

TRACE_EVENT(drm_vblank_event_delivered,
	    TP_PROTO(struct drm_file *file, int crtc, unsigned int seq),
	    TP_ARGS(file, crtc, seq),
	    TP_STRUCT__entry(
		    __field(struct drm_file *, file)
		    __field(int, crtc)
		    __field(unsigned int, seq)
		    ),
	    TP_fast_assign(
		    __entry->file = file;
		    __entry->crtc = crtc;
		    __entry->seq = seq;
		    ),
	    TP_printk("file=%p, crtc=%d, seq=%u", __entry->file, __entry->crtc, \
		      __entry->seq)
);

DECLARE_EVENT_CLASS(drm_vblank_get_put_template,
		    TP_PROTO(int crtc, int refcount),
		    TP_ARGS(crtc, refcount),
		    TP_STRUCT__entry(
			__field(int, crtc)
			__field(int, refcount)
		    ),
		    TP_fast_assign(
			__entry->crtc = crtc;
			__entry->refcount = refcount;
		    ),
		    TP_printk(
			"crtc=%d, refcount=%u",
			__entry->crtc, __entry->refcount
		    )
);

DEFINE_EVENT(drm_vblank_get_put_template, drm_vblank_get,
	     TP_PROTO(int crtc, int refcount),
	     TP_ARGS(crtc, refcount));

/* put's refcount not sync'd using vbl_lock, use for debugging purposes only */
DEFINE_EVENT(drm_vblank_get_put_template, drm_vblank_put,
	     TP_PROTO(int crtc, int refcount),
	     TP_ARGS(crtc, refcount));

DECLARE_EVENT_CLASS(drm_vblank_on_off_template,
		    TP_PROTO(int crtc, int refcount, bool enabled, bool inmodeset),
		    TP_ARGS(crtc, refcount, enabled, inmodeset),
		    TP_STRUCT__entry(
			__field(int, crtc)
			__field(int, refcount)
			__field(bool, enabled)
			__field(bool, inmodeset)
		    ),
		    TP_fast_assign(
			__entry->crtc = crtc;
			__entry->refcount = refcount;
			__entry->enabled = enabled;
			__entry->inmodeset = inmodeset;
		    ),
		    TP_printk(
			"crtc=%d, refcount=%u, enabled=%s, inmodeset=%s",
			__entry->crtc, __entry->refcount,
			__entry->enabled ? "true" : "false",
			__entry->inmodeset ? "true" : "false"
		    )
);

DEFINE_EVENT(drm_vblank_on_off_template, drm_vblank_on,
	     TP_PROTO(int crtc, int refcount, bool enabled, bool inmodeset),
	     TP_ARGS(crtc, refcount, enabled, inmodeset));

DEFINE_EVENT(drm_vblank_on_off_template, drm_vblank_off,
	     TP_PROTO(int crtc, int refcount, bool enabled, bool inmodeset),
	     TP_ARGS(crtc, refcount, enabled, inmodeset));

DECLARE_EVENT_CLASS(drm_deferred_vblank_template,
		    TP_PROTO(int crtc),
		    TP_ARGS(crtc),
		    TP_STRUCT__entry(
			__field(int, crtc)
		    ),
		    TP_fast_assign(
			__entry->crtc = crtc;
		    ),
		    TP_printk(
			"crtc=%d",
			__entry->crtc
		    )
);

DEFINE_EVENT(drm_deferred_vblank_template, drm_deferred_vblank_enable_queued,
	     TP_PROTO(int crtc),
	     TP_ARGS(crtc));

DEFINE_EVENT(drm_deferred_vblank_template, drm_deferred_vblank_enable,
	     TP_PROTO(int crtc),
	     TP_ARGS(crtc));

TRACE_EVENT(drm_deferred_vblank_disable_queued,
	    TP_PROTO(int crtc, int delay_ms),
	    TP_ARGS(crtc, delay_ms),
	    TP_STRUCT__entry(
		__field(int, crtc)
		__field(int, delay_ms)
	    ),
	    TP_fast_assign(
		__entry->crtc = crtc;
		__entry->delay_ms = delay_ms;
	    ),
	    TP_printk(
		"crtc=%d, delay_ms=%d",
		__entry->crtc,
		__entry->delay_ms
	    )
);

DEFINE_EVENT(drm_deferred_vblank_template, drm_deferred_vblank_disable,
	     TP_PROTO(int crtc),
	     TP_ARGS(crtc));

DEFINE_EVENT(drm_deferred_vblank_template,
	     drm_deferred_vblank_wait_enable_start,
	     TP_PROTO(int crtc),
	     TP_ARGS(crtc));

DEFINE_EVENT(drm_deferred_vblank_template,
	     drm_deferred_vblank_wait_enable_end,
	     TP_PROTO(int crtc),
	     TP_ARGS(crtc));

#endif /* _DRM_TRACE_H_ */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ../../drivers/gpu/drm
#include <trace/define_trace.h>
