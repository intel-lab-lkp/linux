/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM sockmap

#if !defined(_TRACE_SOCKMAP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SOCKMAP_H

#include <linux/filter.h>
#include <linux/tracepoint.h>
#include <linux/bpf.h>
#include <linux/skmsg.h>

TRACE_DEFINE_ENUM(__SK_DROP);
TRACE_DEFINE_ENUM(__SK_PASS);
TRACE_DEFINE_ENUM(__SK_REDIRECT);
TRACE_DEFINE_ENUM(__SK_NONE);

#define show_act(x) \
	__print_symbolic(x, \
		{ __SK_DROP,		"DROP" }, \
		{ __SK_PASS,		"PASS" }, \
		{ __SK_REDIRECT,	"REDIRECT" }, \
		{ __SK_NONE,		"NONE" })

#define trace_sockmap_skmsg_redirect(sk, prog, msg, act)	\
	trace_sockmap_redirect((sk), "msg", (prog), (msg)->sg.size, (act))

#define trace_sockmap_skb_redirect(sk, prog, skb, act)		\
	trace_sockmap_redirect((sk), "skb", (prog), (skb)->len, (act))

TRACE_EVENT(sockmap_redirect,
	    TP_PROTO(const struct sock *sk, const char *type,
		     const struct bpf_prog *prog, int length, int act),
	    TP_ARGS(sk, type, prog, length, act),

	TP_STRUCT__entry(
		__field(const void *, sk)
		__field(const char *, type)
		__field(__u16, family)
		__field(__u16, protocol)
		__field(int, prog_id)
		__field(int, length)
		__field(int, act)
	),

	TP_fast_assign(
		__entry->sk		= sk;
		__entry->type		= type;
		__entry->family		= sk->sk_family;
		__entry->protocol	= sk->sk_protocol;
		__entry->prog_id	= prog->aux->id;
		__entry->length		= length;
		__entry->act		= act;
	),

	TP_printk("sk=%p, type=%s, family=%d, protocol=%d, prog_id=%d, length=%d, action=%s",
		  __entry->sk, __entry->type, __entry->family, __entry->protocol,
		  __entry->prog_id, __entry->length,
		  show_act(__entry->act))
);

TRACE_EVENT(sockmap_skb_strp_parse,
	    TP_PROTO(const struct sock *sk, const struct bpf_prog *prog,
		     int size),
	    TP_ARGS(sk, prog, size),

	TP_STRUCT__entry(
		__field(const void *, sk)
		__field(__u16, family)
		__field(__u16, protocol)
		__field(int, prog_id)
		__field(int, size)
	),

	TP_fast_assign(
		__entry->sk		= sk;
		__entry->family		= sk->sk_family;
		__entry->protocol	= sk->sk_protocol;
		__entry->prog_id	= prog->aux->id;
		__entry->size		= size;
	),

	TP_printk("sk=%p, family=%d, protocol=%d, prog_id=%d, size=%d",
		  __entry->sk, __entry->family, __entry->protocol,
		  __entry->prog_id, __entry->size)
);
#endif /* _TRACE_SOCKMAP_H */

#include <trace/define_trace.h>
