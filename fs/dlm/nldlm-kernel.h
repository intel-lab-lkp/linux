/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/nldlm.yaml */
/* YNL-GEN kernel header */

#ifndef _LINUX_NLDLM_GEN_H
#define _LINUX_NLDLM_GEN_H

#include <net/netlink.h>
#include <net/genetlink.h>

#include <uapi/linux/nldlm.h>

/* Common nested types */
extern const struct nla_policy nldlm_addr_nl_policy[NLDLM_A_ADDR_ADDR6 + 1];

int nldlm_nl_get_node_doit(struct sk_buff *skb, struct genl_info *info);
int nldlm_nl_get_node_dumpit(struct sk_buff *skb, struct netlink_callback *cb);
int nldlm_nl_add_node_doit(struct sk_buff *skb, struct genl_info *info);
int nldlm_nl_del_node_doit(struct sk_buff *skb, struct genl_info *info);
int nldlm_nl_get_ls_doit(struct sk_buff *skb, struct genl_info *info);
int nldlm_nl_get_ls_dumpit(struct sk_buff *skb, struct netlink_callback *cb);
int nldlm_nl_get_ls_member_doit(struct sk_buff *skb, struct genl_info *info);
int nldlm_nl_get_ls_member_dumpit(struct sk_buff *skb,
				  struct netlink_callback *cb);
int nldlm_nl_ls_add_member_doit(struct sk_buff *skb, struct genl_info *info);
int nldlm_nl_ls_del_member_doit(struct sk_buff *skb, struct genl_info *info);
int nldlm_nl_ls_ctrl_doit(struct sk_buff *skb, struct genl_info *info);
int nldlm_nl_ls_event_done_doit(struct sk_buff *skb, struct genl_info *info);
int nldlm_nl_get_cfg_doit(struct sk_buff *skb, struct genl_info *info);
int nldlm_nl_set_our_nodeid_doit(struct sk_buff *skb, struct genl_info *info);
int nldlm_nl_set_cluster_name_doit(struct sk_buff *skb, struct genl_info *info);
int nldlm_nl_set_protocol_doit(struct sk_buff *skb, struct genl_info *info);
int nldlm_nl_set_port_doit(struct sk_buff *skb, struct genl_info *info);
int nldlm_nl_set_recover_timeout_doit(struct sk_buff *skb,
				      struct genl_info *info);
int nldlm_nl_set_inactive_timeout_doit(struct sk_buff *skb,
				       struct genl_info *info);
int nldlm_nl_set_log_level_doit(struct sk_buff *skb, struct genl_info *info);
int nldlm_nl_set_default_mark_doit(struct sk_buff *skb, struct genl_info *info);
int nldlm_nl_set_recover_callbacks_doit(struct sk_buff *skb,
					struct genl_info *info);

enum {
	NLDLM_NLGRP_LS_EVENT,
};

extern struct genl_family nldlm_nl_family;

#endif /* _LINUX_NLDLM_GEN_H */
