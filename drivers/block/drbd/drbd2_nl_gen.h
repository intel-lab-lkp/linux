/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/drbd2.yaml */
/* YNL-GEN kernel header */
/* To regenerate run: tools/net/ynl/ynl-regen.sh */

#ifndef _LINUX_DRBD2_GEN_H
#define _LINUX_DRBD2_GEN_H

#include <net/netlink.h>
#include <net/genetlink.h>

#include <uapi/linux/drbd2.h>

/* Common nested types */
extern const struct nla_policy drbd2_address_nl_policy[DRBD2_A_ADDRESS_IPV6 + 1];
extern const struct nla_policy drbd2_cbt_start_parms_nl_policy[DRBD2_A_CBT_START_PARMS_BOOKMARK_ID + 1];
extern const struct nla_policy drbd2_connect_parms_nl_policy[DRBD2_A_CONNECT_PARMS_DISCARD_MY_DATA + 1];
extern const struct nla_policy drbd2_context_nl_policy[DRBD2_A_CONTEXT_PEER_ADDRESS + 1];
extern const struct nla_policy drbd2_detach_parms_nl_policy[DRBD2_A_DETACH_PARMS_INTENTIONAL_DISKLESS_DETACH + 1];
extern const struct nla_policy drbd2_device_conf_nl_policy[DRBD2_A_DEVICE_CONF_DISCARD_GRANULARITY + 1];
extern const struct nla_policy drbd2_disconnect_parms_nl_policy[DRBD2_A_DISCONNECT_PARMS_FORCE + 1];
extern const struct nla_policy drbd2_disk_conf_nl_policy[DRBD2_A_DISK_CONF_BITMAP + 1];
extern const struct nla_policy drbd2_invalidate_parms_nl_policy[DRBD2_A_INVALIDATE_PARMS_RESET_BITMAP + 1];
extern const struct nla_policy drbd2_invalidate_peer_parms_nl_policy[DRBD2_A_INVALIDATE_PEER_PARMS_RESET_BITMAP + 1];
extern const struct nla_policy drbd2_net_conf_nl_policy[DRBD2_A_NET_CONF_RDMA_CTRL_SNDBUF_SIZE + 1];
extern const struct nla_policy drbd2_new_current_uuid_parms_nl_policy[DRBD2_A_NEW_CURRENT_UUID_PARMS_FORCE_RESYNC + 1];
extern const struct nla_policy drbd2_peer_device_conf_nl_policy[DRBD2_A_PEER_DEVICE_CONF_PEER_TIEBREAKER + 1];
extern const struct nla_policy drbd2_rename_parms_nl_policy[DRBD2_A_RENAME_PARMS_NEW_NAME + 1];
extern const struct nla_policy drbd2_resize_parms_nl_policy[DRBD2_A_RESIZE_PARMS_AL_STRIPE_SIZE + 1];
extern const struct nla_policy drbd2_resource_opts_nl_policy[DRBD2_A_RESOURCE_OPTS_EXPLICIT_DRBD8_COMPAT + 1];
extern const struct nla_policy drbd2_set_role_parms_nl_policy[DRBD2_A_SET_ROLE_PARMS_FORCE + 1];
extern const struct nla_policy drbd2_start_ov_parms_nl_policy[DRBD2_A_START_OV_PARMS_STOP_SECTOR + 1];
extern const struct nla_policy drbd2_suspend_io_parms_nl_policy[DRBD2_A_SUSPEND_IO_PARMS_FLUSH_TO_PEERS + 1];

int drbd2_pre_doit(const struct genl_split_ops *ops, struct sk_buff *skb,
		   struct genl_info *info);
void
drbd2_post_doit(const struct genl_split_ops *ops, struct sk_buff *skb,
		struct genl_info *info);
int drbd2_nl_device_get_done(struct netlink_callback *cb);
int drbd2_nl_connection_get_done(struct netlink_callback *cb);
int drbd2_nl_peer_device_get_done(struct netlink_callback *cb);
int drbd2_nl_path_get_done(struct netlink_callback *cb);
int drbd2_nl_state_get_done(struct netlink_callback *cb);

int drbd2_nl_resource_new_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_resource_del_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_resource_set_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_resource_rename_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_resource_down_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_resource_primary_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_resource_secondary_doit(struct sk_buff *skb,
				     struct genl_info *info);
int drbd2_nl_resource_suspend_io_doit(struct sk_buff *skb,
				      struct genl_info *info);
int drbd2_nl_resource_resume_io_doit(struct sk_buff *skb,
				     struct genl_info *info);
int drbd2_nl_device_new_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_device_del_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_device_attach_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_device_detach_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_disk_set_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_device_resize_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_device_outdate_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_device_invalidate_doit(struct sk_buff *skb,
				    struct genl_info *info);
int drbd2_nl_device_new_current_uuid_doit(struct sk_buff *skb,
					  struct genl_info *info);
int drbd2_nl_connection_new_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_connection_del_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_connection_connect_doit(struct sk_buff *skb,
				     struct genl_info *info);
int drbd2_nl_connection_disconnect_doit(struct sk_buff *skb,
					struct genl_info *info);
int drbd2_nl_connection_set_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_connection_forget_doit(struct sk_buff *skb,
				    struct genl_info *info);
int drbd2_nl_path_new_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_path_del_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_peer_device_set_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_peer_device_invalidate_doit(struct sk_buff *skb,
					 struct genl_info *info);
int drbd2_nl_peer_device_pause_sync_doit(struct sk_buff *skb,
					 struct genl_info *info);
int drbd2_nl_peer_device_resume_sync_doit(struct sk_buff *skb,
					  struct genl_info *info);
int drbd2_nl_peer_device_start_ov_doit(struct sk_buff *skb,
				       struct genl_info *info);
int drbd2_nl_timeout_type_get_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_resource_get_dumpit(struct sk_buff *skb,
				 struct netlink_callback *cb);
int drbd2_nl_device_get_dumpit(struct sk_buff *skb,
			       struct netlink_callback *cb);
int drbd2_nl_connection_get_dumpit(struct sk_buff *skb,
				   struct netlink_callback *cb);
int drbd2_nl_peer_device_get_dumpit(struct sk_buff *skb,
				    struct netlink_callback *cb);
int drbd2_nl_path_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb);
int drbd2_nl_state_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb);
int drbd2_nl_device_cbt_flush_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_device_cbt_start_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_device_cbt_stop_doit(struct sk_buff *skb, struct genl_info *info);
int drbd2_nl_device_cbt_status_doit(struct sk_buff *skb,
				    struct genl_info *info);

enum {
	DRBD2_NLGRP_EVENTS,
};

extern struct genl_family drbd2_nl_family;

#endif /* _LINUX_DRBD2_GEN_H */
