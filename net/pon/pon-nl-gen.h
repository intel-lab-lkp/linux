/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/pon.yaml */
/* YNL-GEN kernel header */
/* To regenerate run: tools/net/ynl/ynl-regen.sh */

#ifndef _LINUX_PON_GEN_H
#define _LINUX_PON_GEN_H

#include <net/netlink.h>
#include <net/genetlink.h>

#include <uapi/linux/pon.h>

int pon_device_get_locked(const struct genl_split_ops *ops,
			  struct sk_buff *skb, struct genl_info *info);
void
pon_device_unlock(const struct genl_split_ops *ops, struct sk_buff *skb,
		  struct genl_info *info);

int pon_nl_dev_get_doit(struct sk_buff *skb, struct genl_info *info);
int pon_nl_dev_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb);
int pon_nl_dev_set_doit(struct sk_buff *skb, struct genl_info *info);
int pon_nl_tcont_get_doit(struct sk_buff *skb, struct genl_info *info);
int pon_nl_tcont_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb);
int pon_nl_tcont_set_doit(struct sk_buff *skb, struct genl_info *info);
int pon_nl_tcont_del_doit(struct sk_buff *skb, struct genl_info *info);
int pon_nl_gem_get_doit(struct sk_buff *skb, struct genl_info *info);
int pon_nl_gem_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb);
int pon_nl_gem_new_doit(struct sk_buff *skb, struct genl_info *info);
int pon_nl_gem_del_doit(struct sk_buff *skb, struct genl_info *info);
int pon_nl_gem_map_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb);
int pon_nl_gem_map_new_doit(struct sk_buff *skb, struct genl_info *info);
int pon_nl_gem_map_del_doit(struct sk_buff *skb, struct genl_info *info);
int pon_nl_omci_register_doit(struct sk_buff *skb, struct genl_info *info);
int pon_nl_omci_tx_doit(struct sk_buff *skb, struct genl_info *info);

enum {
	PON_NLGRP_MGMT,
	PON_NLGRP_STATE,
};

extern struct genl_family pon_nl_family;

#endif /* _LINUX_PON_GEN_H */
