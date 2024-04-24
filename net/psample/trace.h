/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM psample

#if !defined(_TRACE_PSAMPLE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_PSAMPLE__H

#include <linux/skbuff.h>
#include <linux/tracepoint.h>
#include <net/psample.h>

TRACE_EVENT(psample_sample_packet,

	TP_PROTO(struct psample_group *group, struct sk_buff *skb,
		 u32 sample_rate, const struct psample_metadata *md),

	TP_ARGS(group, skb, sample_rate, md),

	TP_STRUCT__entry(
		__field(u32, group_num)
		__field(u32, refcount)
		__field(u32, seq)
		__field(void *, skbaddr)
		__field(unsigned int, len)
		__field(unsigned int, data_len)
		__field(u32, sample_rate)
		__field(int, in_ifindex)
		__field(int, out_ifindex)
		__field(const void *, user_cookie)
		__field(u32, user_cookie_len)
	),

	TP_fast_assign(
		__entry->group_num = group->group_num;
		__entry->refcount = group->refcount;
		__entry->seq = group->seq;
		__entry->skbaddr = skb;
		__entry->len = skb->len;
		__entry->data_len = skb->data_len;
		__entry->sample_rate = sample_rate;
		__entry->in_ifindex = md->in_ifindex;
		__entry->out_ifindex = md->out_ifindex;
		__entry->user_cookie = &md->user_cookie[0];
		__entry->user_cookie_len = md->user_cookie_len;
	),

	TP_printk("group_num=%u refcount=%u seq=%u skbaddr=%p len=%u data_len=%u sample_rate=%u in_ifindex=%d out_ifindex=%d user_cookie=%p user_cookie_len=%u",
		  __entry->group_num, __entry->refcount, __entry->seq,
		  __entry->skbaddr, __entry->len, __entry->data_len,
		  __entry->sample_rate, __entry->in_ifindex,
		  __entry->out_ifindex, __entry->user_cookie,
		  __entry->user_cookie_len)
);

#endif /* _TRACE_PSAMPLE_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ../../net/psample
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace
#include <trace/define_trace.h>
