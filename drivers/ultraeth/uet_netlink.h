/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/ultraeth.yaml */
/* YNL-GEN kernel header */

#ifndef _LINUX_ULTRAETH_GEN_H
#define _LINUX_ULTRAETH_GEN_H

#include <net/netlink.h>
#include <net/genetlink.h>

#include <uapi/linux/ultraeth_nl.h>

/* Common nested types */
extern const struct nla_policy ultraeth_fep_address_nl_policy[ULTRAETH_A_FEP_ADDRESS_VERSION + 1];
extern const struct nla_policy ultraeth_fep_in_addr_nl_policy[ULTRAETH_A_FEP_IN_ADDR_FAMILY + 1];

int ultraeth_nl_context_get_dumpit(struct sk_buff *skb,
				   struct netlink_callback *cb);
int ultraeth_nl_context_new_doit(struct sk_buff *skb, struct genl_info *info);
int ultraeth_nl_context_del_doit(struct sk_buff *skb, struct genl_info *info);
int ultraeth_nl_job_get_dumpit(struct sk_buff *skb,
			       struct netlink_callback *cb);
int ultraeth_nl_job_new_doit(struct sk_buff *skb, struct genl_info *info);
int ultraeth_nl_job_del_doit(struct sk_buff *skb, struct genl_info *info);

extern struct genl_family ultraeth_nl_family;

#endif /* _LINUX_ULTRAETH_GEN_H */
