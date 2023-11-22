/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/ulp_ddp.yaml */
/* YNL-GEN kernel header */

#ifndef _LINUX_ULP_DDP_GEN_H
#define _LINUX_ULP_DDP_GEN_H

#include <net/netlink.h>
#include <net/genetlink.h>

#include <uapi/linux/ulp_ddp.h>

int ulp_ddp_get_netdev(const struct genl_split_ops *ops, struct sk_buff *skb,
		       struct genl_info *info);
void
ulp_ddp_put_netdev(const struct genl_split_ops *ops, struct sk_buff *skb,
		   struct genl_info *info);

int ulp_ddp_nl_caps_get_doit(struct sk_buff *skb, struct genl_info *info);
int ulp_ddp_nl_stats_get_doit(struct sk_buff *skb, struct genl_info *info);
int ulp_ddp_nl_caps_set_doit(struct sk_buff *skb, struct genl_info *info);

enum {
	ULP_DDP_NLGRP_MGMT,
};

extern struct genl_family ulp_ddp_nl_family;

#endif /* _LINUX_ULP_DDP_GEN_H */
