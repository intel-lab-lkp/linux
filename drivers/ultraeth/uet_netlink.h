/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/ultraeth.yaml */
/* YNL-GEN kernel header */

#ifndef _LINUX_ULTRAETH_GEN_H
#define _LINUX_ULTRAETH_GEN_H

#include <net/netlink.h>
#include <net/genetlink.h>

#include <uapi/linux/ultraeth_nl.h>

int ultraeth_nl_context_get_dumpit(struct sk_buff *skb,
				   struct netlink_callback *cb);
int ultraeth_nl_context_new_doit(struct sk_buff *skb, struct genl_info *info);
int ultraeth_nl_context_del_doit(struct sk_buff *skb, struct genl_info *info);

extern struct genl_family ultraeth_nl_family;

#endif /* _LINUX_ULTRAETH_GEN_H */
