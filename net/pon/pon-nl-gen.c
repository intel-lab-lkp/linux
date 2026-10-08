// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/pon.yaml */
/* YNL-GEN kernel source */
/* To regenerate run: tools/net/ynl/ynl-regen.sh */

#include <net/netlink.h>
#include <net/genetlink.h>

#include "pon-nl-gen.h"

#include <uapi/linux/pon.h>

/* Integer value ranges */
static const struct netlink_range_validation pon_a_tcont_index_range = {
	.max	= 65534ULL,
};

static const struct netlink_range_validation pon_a_gem_id_range = {
	.min	= 1021ULL,
	.max	= 65534ULL,
};

static const struct netlink_range_validation pon_a_gem_tcont_index_range = {
	.max	= 65534ULL,
};

static const struct netlink_range_validation pon_a_gem_map_gem_id_range = {
	.min	= 1021ULL,
	.max	= 65534ULL,
};

/* PON_CMD_DEV_GET - do */
static const struct nla_policy pon_dev_get_nl_policy[PON_A_DEV_ID + 1] = {
	[PON_A_DEV_ID] = NLA_POLICY_MIN(NLA_U32, 1),
};

/* PON_CMD_DEV_SET - do */
static const struct nla_policy pon_dev_set_nl_policy[PON_A_DEV_ENABLE + 1] = {
	[PON_A_DEV_ID] = NLA_POLICY_MIN(NLA_U32, 1),
	[PON_A_DEV_MODE] = NLA_POLICY_MAX(NLA_U32, 2),
	[PON_A_DEV_SERIAL] = NLA_POLICY_EXACT_LEN(8),
	[PON_A_DEV_REGISTRATION_ID] = NLA_POLICY_MAX_LEN(36),
	[PON_A_DEV_ENABLE] = NLA_POLICY_MAX(NLA_U8, 1),
};

/* PON_CMD_TCONT_GET - do */
static const struct nla_policy pon_tcont_get_do_nl_policy[PON_A_TCONT_INDEX + 1] = {
	[PON_A_TCONT_DEV_ID] = NLA_POLICY_MIN(NLA_U32, 1),
	[PON_A_TCONT_INDEX] = NLA_POLICY_FULL_RANGE(NLA_U32, &pon_a_tcont_index_range),
};

/* PON_CMD_TCONT_GET - dump */
static const struct nla_policy pon_tcont_get_dump_nl_policy[PON_A_TCONT_DEV_ID + 1] = {
	[PON_A_TCONT_DEV_ID] = NLA_POLICY_MIN(NLA_U32, 1),
};

/* PON_CMD_TCONT_SET - do */
static const struct nla_policy pon_tcont_set_nl_policy[PON_A_TCONT_ALLOC_ID + 1] = {
	[PON_A_TCONT_DEV_ID] = NLA_POLICY_MIN(NLA_U32, 1),
	[PON_A_TCONT_INDEX] = NLA_POLICY_FULL_RANGE(NLA_U32, &pon_a_tcont_index_range),
	[PON_A_TCONT_ALLOC_ID] = NLA_POLICY_MAX(NLA_U32, 16383),
};

/* PON_CMD_TCONT_DEL - do */
static const struct nla_policy pon_tcont_del_nl_policy[PON_A_TCONT_INDEX + 1] = {
	[PON_A_TCONT_DEV_ID] = NLA_POLICY_MIN(NLA_U32, 1),
	[PON_A_TCONT_INDEX] = NLA_POLICY_FULL_RANGE(NLA_U32, &pon_a_tcont_index_range),
};

/* PON_CMD_GEM_GET - do */
static const struct nla_policy pon_gem_get_do_nl_policy[PON_A_GEM_ID + 1] = {
	[PON_A_GEM_DEV_ID] = NLA_POLICY_MIN(NLA_U32, 1),
	[PON_A_GEM_ID] = NLA_POLICY_FULL_RANGE(NLA_U32, &pon_a_gem_id_range),
};

/* PON_CMD_GEM_GET - dump */
static const struct nla_policy pon_gem_get_dump_nl_policy[PON_A_GEM_DEV_ID + 1] = {
	[PON_A_GEM_DEV_ID] = NLA_POLICY_MIN(NLA_U32, 1),
};

/* PON_CMD_GEM_NEW - do */
static const struct nla_policy pon_gem_new_nl_policy[PON_A_GEM_KEY_RING + 1] = {
	[PON_A_GEM_DEV_ID] = NLA_POLICY_MIN(NLA_U32, 1),
	[PON_A_GEM_ID] = NLA_POLICY_FULL_RANGE(NLA_U32, &pon_a_gem_id_range),
	[PON_A_GEM_DIR] = NLA_POLICY_RANGE(NLA_U32, 1, 3),
	[PON_A_GEM_TCONT_INDEX] = NLA_POLICY_FULL_RANGE(NLA_U32, &pon_a_gem_tcont_index_range),
	[PON_A_GEM_KEY_RING] = NLA_POLICY_MAX(NLA_U32, 3),
};

/* PON_CMD_GEM_DEL - do */
static const struct nla_policy pon_gem_del_nl_policy[PON_A_GEM_ID + 1] = {
	[PON_A_GEM_DEV_ID] = NLA_POLICY_MIN(NLA_U32, 1),
	[PON_A_GEM_ID] = NLA_POLICY_FULL_RANGE(NLA_U32, &pon_a_gem_id_range),
};

/* PON_CMD_GEM_MAP_GET - dump */
static const struct nla_policy pon_gem_map_get_nl_policy[PON_A_GEM_MAP_DEV_ID + 1] = {
	[PON_A_GEM_MAP_DEV_ID] = NLA_POLICY_MIN(NLA_U32, 1),
};

/* PON_CMD_GEM_MAP_NEW - do */
static const struct nla_policy pon_gem_map_new_nl_policy[PON_A_GEM_MAP_DSCP + 1] = {
	[PON_A_GEM_MAP_DEV_ID] = NLA_POLICY_MIN(NLA_U32, 1),
	[PON_A_GEM_MAP_GEM_ID] = NLA_POLICY_FULL_RANGE(NLA_U32, &pon_a_gem_map_gem_id_range),
	[PON_A_GEM_MAP_TAG] = NLA_POLICY_MAX(NLA_U32, 1),
	[PON_A_GEM_MAP_VID] = NLA_POLICY_MAX(NLA_U32, 4094),
	[PON_A_GEM_MAP_PBIT] = NLA_POLICY_MAX(NLA_U32, 7),
	[PON_A_GEM_MAP_DSCP] = NLA_POLICY_MAX(NLA_U32, 63),
};

/* PON_CMD_GEM_MAP_DEL - do */
static const struct nla_policy pon_gem_map_del_nl_policy[PON_A_GEM_MAP_DSCP + 1] = {
	[PON_A_GEM_MAP_DEV_ID] = NLA_POLICY_MIN(NLA_U32, 1),
	[PON_A_GEM_MAP_GEM_ID] = NLA_POLICY_FULL_RANGE(NLA_U32, &pon_a_gem_map_gem_id_range),
	[PON_A_GEM_MAP_TAG] = NLA_POLICY_MAX(NLA_U32, 1),
	[PON_A_GEM_MAP_VID] = NLA_POLICY_MAX(NLA_U32, 4094),
	[PON_A_GEM_MAP_PBIT] = NLA_POLICY_MAX(NLA_U32, 7),
	[PON_A_GEM_MAP_DSCP] = NLA_POLICY_MAX(NLA_U32, 63),
};

/* PON_CMD_OMCI_REGISTER - do */
static const struct nla_policy pon_omci_register_nl_policy[PON_A_OMCI_DEV_ID + 1] = {
	[PON_A_OMCI_DEV_ID] = NLA_POLICY_MIN(NLA_U32, 1),
};

/* PON_CMD_OMCI_TX - do */
static const struct nla_policy pon_omci_tx_nl_policy[PON_A_OMCI_PDU + 1] = {
	[PON_A_OMCI_DEV_ID] = NLA_POLICY_MIN(NLA_U32, 1),
	[PON_A_OMCI_PDU] = NLA_POLICY_MAX_LEN(1976),
};

/* Ops table for pon */
static const struct genl_split_ops pon_nl_ops[] = {
	{
		.cmd		= PON_CMD_DEV_GET,
		.pre_doit	= pon_device_get_locked,
		.doit		= pon_nl_dev_get_doit,
		.post_doit	= pon_device_unlock,
		.policy		= pon_dev_get_nl_policy,
		.maxattr	= PON_A_DEV_ID,
		.flags		= GENL_CMD_CAP_DO,
	},
	{
		.cmd	= PON_CMD_DEV_GET,
		.dumpit	= pon_nl_dev_get_dumpit,
		.flags	= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= PON_CMD_DEV_SET,
		.pre_doit	= pon_device_get_locked,
		.doit		= pon_nl_dev_set_doit,
		.post_doit	= pon_device_unlock,
		.policy		= pon_dev_set_nl_policy,
		.maxattr	= PON_A_DEV_ENABLE,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= PON_CMD_TCONT_GET,
		.pre_doit	= pon_device_get_locked,
		.doit		= pon_nl_tcont_get_doit,
		.post_doit	= pon_device_unlock,
		.policy		= pon_tcont_get_do_nl_policy,
		.maxattr	= PON_A_TCONT_INDEX,
		.flags		= GENL_CMD_CAP_DO,
	},
	{
		.cmd		= PON_CMD_TCONT_GET,
		.dumpit		= pon_nl_tcont_get_dumpit,
		.policy		= pon_tcont_get_dump_nl_policy,
		.maxattr	= PON_A_TCONT_DEV_ID,
		.flags		= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= PON_CMD_TCONT_SET,
		.pre_doit	= pon_device_get_locked,
		.doit		= pon_nl_tcont_set_doit,
		.post_doit	= pon_device_unlock,
		.policy		= pon_tcont_set_nl_policy,
		.maxattr	= PON_A_TCONT_ALLOC_ID,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= PON_CMD_TCONT_DEL,
		.pre_doit	= pon_device_get_locked,
		.doit		= pon_nl_tcont_del_doit,
		.post_doit	= pon_device_unlock,
		.policy		= pon_tcont_del_nl_policy,
		.maxattr	= PON_A_TCONT_INDEX,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= PON_CMD_GEM_GET,
		.pre_doit	= pon_device_get_locked,
		.doit		= pon_nl_gem_get_doit,
		.post_doit	= pon_device_unlock,
		.policy		= pon_gem_get_do_nl_policy,
		.maxattr	= PON_A_GEM_ID,
		.flags		= GENL_CMD_CAP_DO,
	},
	{
		.cmd		= PON_CMD_GEM_GET,
		.dumpit		= pon_nl_gem_get_dumpit,
		.policy		= pon_gem_get_dump_nl_policy,
		.maxattr	= PON_A_GEM_DEV_ID,
		.flags		= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= PON_CMD_GEM_NEW,
		.pre_doit	= pon_device_get_locked,
		.doit		= pon_nl_gem_new_doit,
		.post_doit	= pon_device_unlock,
		.policy		= pon_gem_new_nl_policy,
		.maxattr	= PON_A_GEM_KEY_RING,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= PON_CMD_GEM_DEL,
		.pre_doit	= pon_device_get_locked,
		.doit		= pon_nl_gem_del_doit,
		.post_doit	= pon_device_unlock,
		.policy		= pon_gem_del_nl_policy,
		.maxattr	= PON_A_GEM_ID,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= PON_CMD_GEM_MAP_GET,
		.dumpit		= pon_nl_gem_map_get_dumpit,
		.policy		= pon_gem_map_get_nl_policy,
		.maxattr	= PON_A_GEM_MAP_DEV_ID,
		.flags		= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= PON_CMD_GEM_MAP_NEW,
		.pre_doit	= pon_device_get_locked,
		.doit		= pon_nl_gem_map_new_doit,
		.post_doit	= pon_device_unlock,
		.policy		= pon_gem_map_new_nl_policy,
		.maxattr	= PON_A_GEM_MAP_DSCP,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= PON_CMD_GEM_MAP_DEL,
		.pre_doit	= pon_device_get_locked,
		.doit		= pon_nl_gem_map_del_doit,
		.post_doit	= pon_device_unlock,
		.policy		= pon_gem_map_del_nl_policy,
		.maxattr	= PON_A_GEM_MAP_DSCP,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= PON_CMD_OMCI_REGISTER,
		.pre_doit	= pon_device_get_locked,
		.doit		= pon_nl_omci_register_doit,
		.post_doit	= pon_device_unlock,
		.policy		= pon_omci_register_nl_policy,
		.maxattr	= PON_A_OMCI_DEV_ID,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= PON_CMD_OMCI_TX,
		.pre_doit	= pon_device_get_locked,
		.doit		= pon_nl_omci_tx_doit,
		.post_doit	= pon_device_unlock,
		.policy		= pon_omci_tx_nl_policy,
		.maxattr	= PON_A_OMCI_PDU,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
};

static const struct genl_multicast_group pon_nl_mcgrps[] = {
	[PON_NLGRP_MGMT] = { "mgmt", },
	[PON_NLGRP_STATE] = { "state", },
};

struct genl_family pon_nl_family __ro_after_init = {
	.name		= PON_FAMILY_NAME,
	.version	= PON_FAMILY_VERSION,
	.netnsok	= true,
	.parallel_ops	= true,
	.module		= THIS_MODULE,
	.split_ops	= pon_nl_ops,
	.n_split_ops	= ARRAY_SIZE(pon_nl_ops),
	.mcgrps		= pon_nl_mcgrps,
	.n_mcgrps	= ARRAY_SIZE(pon_nl_mcgrps),
};
