/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2011-2014 Autronica Fire and Security AS
 *
 * Author(s):
 *	2011-2014 Arvid Brodin, arvid.brodin@alten.se
 *
 * include file for HSR and PRP.
 */

#ifndef __HSR_FORWARD_H
#define __HSR_FORWARD_H

#include <linux/netdevice.h>
#include "hsr_main.h"

/* Per-frame execution source, carried explicitly down the call chain. */
enum hsr_exec_source {
	HSR_EXEC_DIRECT_LAN,	/* synchronous LAN A/B receive path */
	HSR_EXEC_INLINE,	/* inline consumer activation */
	HSR_EXEC_WORKER,	/* BH worker consumer */
};

/* Input class, stored with the immutable queue charge. */
enum hsr_job_class {
	HSR_JOB_NORMAL,
	HSR_JOB_INTERNAL_SUP,
};

/* Return codes of proto_ops->fill_frame_info(): frame completed from a
 * wire tag/RCT (0), untagged input left for the caller to complete with
 * its execution source (HSR_FRAME_PLAIN), or error (< 0).
 */
#define HSR_FRAME_PLAIN	1

void hsr_forward_skb(struct sk_buff *skb, struct hsr_port *port);
void hsr_forward_sup_skb(struct sk_buff *skb, struct hsr_port *port);
void hsr_forward_frame(struct sk_buff *skb, struct hsr_port *port,
		       enum hsr_exec_source source);
void hsr_assign_sup_seq(struct sk_buff *skb, struct hsr_priv *hsr,
			enum hsr_exec_source source);

/* Ordered forwarding queue (hsr_forward_queue.c) */
void hsr_queue_submit(struct sk_buff *skb, struct hsr_port *port,
		      enum hsr_job_class class);
void hsr_forward_init(struct hsr_priv *hsr);
void hsr_forward_stop(struct hsr_priv *hsr);
void hsr_forward_forget_port(struct hsr_priv *hsr, struct net_device *dev);

struct sk_buff *prp_create_tagged_frame(struct hsr_frame_info *frame,
					struct hsr_port *port);
struct sk_buff *hsr_create_tagged_frame(struct hsr_frame_info *frame,
					struct hsr_port *port);
struct sk_buff *hsr_get_untagged_frame(struct hsr_frame_info *frame,
				       struct hsr_port *port);
struct sk_buff *prp_get_untagged_frame(struct hsr_frame_info *frame,
				       struct hsr_port *port);
bool prp_drop_frame(struct hsr_frame_info *frame, struct hsr_port *port);
bool hsr_drop_frame(struct hsr_frame_info *frame, struct hsr_port *port);
int prp_fill_frame_info(__be16 proto, struct sk_buff *skb,
			struct hsr_frame_info *frame);
int hsr_fill_frame_info(__be16 proto, struct sk_buff *skb,
			struct hsr_frame_info *frame);
#endif /* __HSR_FORWARD_H */
