/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM rmr_clt

#if !defined(_TRACE_RMR_CLT_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_RMR_CLT_H

#include <linux/tracepoint.h>

struct rmr_clt_pool_sess;

TRACE_DEFINE_ENUM(RMR_CLT_POOL_SESS_CREATED);
TRACE_DEFINE_ENUM(RMR_CLT_POOL_SESS_NORMAL);
TRACE_DEFINE_ENUM(RMR_CLT_POOL_SESS_FAILED);
TRACE_DEFINE_ENUM(RMR_CLT_POOL_SESS_RECONNECTING);
TRACE_DEFINE_ENUM(RMR_CLT_POOL_SESS_REMOVING);

#define show_pool_sess_state(x) \
	__print_symbolic(x, \
		{ RMR_CLT_POOL_SESS_CREATED,		"CREATED" }, \
		{ RMR_CLT_POOL_SESS_NORMAL,		"NORMAL" }, \
		{ RMR_CLT_POOL_SESS_FAILED,		"FAILED" }, \
		{ RMR_CLT_POOL_SESS_RECONNECTING,	"RECONNECTING" }, \
		{ RMR_CLT_POOL_SESS_REMOVING,		"REMOVING" })

TRACE_EVENT(pool_sess_change_state,
	TP_PROTO(struct rmr_clt_pool_sess *pool_sess,
		 int newstate,
		 int oldstate,
		 int changed),

	TP_ARGS(pool_sess, newstate, oldstate, changed),

	TP_STRUCT__entry(
		__string(sessname, pool_sess->sessname)
		__field(int, newstate)
		__field(int, oldstate)
		__field(int, changed)
	),

	TP_fast_assign(
		__assign_str(sessname);
		__entry->newstate = newstate;
		__entry->oldstate = oldstate;
		__entry->changed = changed;
	),

	TP_printk("RMR-CLT: sessname=%s newstate='%s' oldstate='%s' state-changed='%d'",
		   __get_str(sessname),
		   show_pool_sess_state(__entry->newstate),
		   show_pool_sess_state(__entry->oldstate),
		   __entry->changed
	)
);

DECLARE_EVENT_CLASS(rtrs_clt_request_class,
	TP_PROTO(int dir, struct rmr_clt_sess_iu *sess_iu),

	TP_ARGS(dir, sess_iu),

	TP_STRUCT__entry(
		__field(int, dir)
		__array(char, sessname, NAME_MAX)
		__field(void *, rtrs)
		__field(void *, clt_sess)
	),

	TP_fast_assign(
		struct rmr_clt_pool_sess *pool_sess = sess_iu->pool_sess;
		struct rmr_clt_sess *clt_sess = pool_sess->clt_sess;

		__entry->dir = dir;
		memcpy(__entry->sessname, pool_sess->sessname, NAME_MAX);
		__entry->rtrs = clt_sess->rtrs;
		__entry->clt_sess = clt_sess;
	),

	TP_printk("rtrs clt request: sessname=%s dir=%s rtrs=%p clt_sess=%p",
		   __entry->sessname,
		   __print_symbolic(__entry->dir,
			{ READ, "READ" },
			{ WRITE, "WRITE" }),
		   __entry->rtrs,
		   __entry->clt_sess
	)
);

#define DEFINE_RTRS_CLT_EVENT(name) \
DEFINE_EVENT(rtrs_clt_request_class, name, \
	TP_PROTO(int dir, struct rmr_clt_sess_iu *sess_iu), \
	TP_ARGS(dir, sess_iu))

DEFINE_RTRS_CLT_EVENT(send_usr_msg);
DEFINE_RTRS_CLT_EVENT(retry_failed_read);
DEFINE_RTRS_CLT_EVENT(rmr_clt_request);
DEFINE_RTRS_CLT_EVENT(rmr_clt_cmd_with_rsp);
DEFINE_RTRS_CLT_EVENT(send_map_update);

#endif /* _TRACE_RMR_CLT_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE rmr-clt-trace
#include <trace/define_trace.h>

