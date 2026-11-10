// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/drbd2.yaml */
/* YNL-GEN kernel source */
/* To regenerate run: tools/net/ynl/ynl-regen.sh */

#include <net/netlink.h>
#include <net/genetlink.h>

#include "drbd2_nl_gen.h"

#include <uapi/linux/drbd2.h>

/* Common nested types */
const struct nla_policy drbd2_address_nl_policy[DRBD2_A_ADDRESS_IPV6 + 1] = {
	[DRBD2_A_ADDRESS_FAMILY] = { .type = NLA_U16, },
	[DRBD2_A_ADDRESS_PORT] = { .type = NLA_BE16, },
	[DRBD2_A_ADDRESS_IPV4] = { .type = NLA_BE32, },
	[DRBD2_A_ADDRESS_IPV6] = NLA_POLICY_EXACT_LEN(16),
};

const struct nla_policy drbd2_cbt_start_parms_nl_policy[DRBD2_A_CBT_START_PARMS_BOOKMARK_ID + 1] = {
	[DRBD2_A_CBT_START_PARMS_BOOKMARK_ID] = { .type = NLA_U64, },
};

const struct nla_policy drbd2_connect_parms_nl_policy[DRBD2_A_CONNECT_PARMS_DISCARD_MY_DATA + 1] = {
	[DRBD2_A_CONNECT_PARMS_TENTATIVE] = { .type = NLA_FLAG, },
	[DRBD2_A_CONNECT_PARMS_DISCARD_MY_DATA] = { .type = NLA_FLAG, },
};

const struct nla_policy drbd2_context_nl_policy[DRBD2_A_CONTEXT_PEER_ADDRESS + 1] = {
	[DRBD2_A_CONTEXT_RESOURCE_NAME] = { .type = NLA_NUL_STRING, .len = DRBD2_RESOURCE_NAME_MAX, },
	[DRBD2_A_CONTEXT_VOLUME] = { .type = NLA_U32, },
	[DRBD2_A_CONTEXT_MINOR] = { .type = NLA_U32, },
	[DRBD2_A_CONTEXT_PEER_NODE_ID] = { .type = NLA_U32, },
	[DRBD2_A_CONTEXT_CONNECTION_NAME] = { .type = NLA_NUL_STRING, .len = DRBD2_CONNECTION_NAME_MAX, },
	[DRBD2_A_CONTEXT_MY_ADDRESS] = NLA_POLICY_NESTED(drbd2_address_nl_policy),
	[DRBD2_A_CONTEXT_PEER_ADDRESS] = NLA_POLICY_NESTED(drbd2_address_nl_policy),
};

const struct nla_policy drbd2_detach_parms_nl_policy[DRBD2_A_DETACH_PARMS_INTENTIONAL_DISKLESS_DETACH + 1] = {
	[DRBD2_A_DETACH_PARMS_FORCE] = { .type = NLA_FLAG, },
	[DRBD2_A_DETACH_PARMS_INTENTIONAL_DISKLESS_DETACH] = { .type = NLA_FLAG, },
};

const struct nla_policy drbd2_device_conf_nl_policy[DRBD2_A_DEVICE_CONF_DISCARD_GRANULARITY + 1] = {
	[DRBD2_A_DEVICE_CONF_MAX_BIO_SIZE] = { .type = NLA_U32, },
	[DRBD2_A_DEVICE_CONF_INTENTIONAL_DISKLESS] = { .type = NLA_U8, },
	[DRBD2_A_DEVICE_CONF_BLOCK_SIZE] = { .type = NLA_U32, },
	[DRBD2_A_DEVICE_CONF_DISCARD_GRANULARITY] = { .type = NLA_U32, },
};

const struct nla_policy drbd2_disconnect_parms_nl_policy[DRBD2_A_DISCONNECT_PARMS_FORCE + 1] = {
	[DRBD2_A_DISCONNECT_PARMS_FORCE] = { .type = NLA_FLAG, },
};

const struct nla_policy drbd2_disk_conf_nl_policy[DRBD2_A_DISK_CONF_BITMAP + 1] = {
	[DRBD2_A_DISK_CONF_BACKING_DEV] = { .type = NLA_NUL_STRING, .len = DRBD2_DEVICE_PATH_MAX, },
	[DRBD2_A_DISK_CONF_META_DEV] = { .type = NLA_NUL_STRING, .len = DRBD2_DEVICE_PATH_MAX, },
	[DRBD2_A_DISK_CONF_META_DEV_IDX] = { .type = NLA_S32, },
	[DRBD2_A_DISK_CONF_SIZE] = { .type = NLA_U64, },
	[DRBD2_A_DISK_CONF_ON_IO_ERROR] = NLA_POLICY_MAX(NLA_U32, 2),
	[DRBD2_A_DISK_CONF_RESYNC_AFTER] = { .type = NLA_S32, },
	[DRBD2_A_DISK_CONF_AL_EXTENTS] = { .type = NLA_U32, },
	[DRBD2_A_DISK_CONF_DISK_BARRIER] = { .type = NLA_U8, },
	[DRBD2_A_DISK_CONF_DISK_FLUSHES] = { .type = NLA_U8, },
	[DRBD2_A_DISK_CONF_DISK_DRAIN] = { .type = NLA_U8, },
	[DRBD2_A_DISK_CONF_MD_FLUSHES] = { .type = NLA_U8, },
	[DRBD2_A_DISK_CONF_DISK_TIMEOUT] = { .type = NLA_U32, },
	[DRBD2_A_DISK_CONF_READ_BALANCING] = NLA_POLICY_MAX(NLA_U32, 10),
	[DRBD2_A_DISK_CONF_UNPLUG_WATERMARK] = { .type = NLA_U32, },
	[DRBD2_A_DISK_CONF_RS_DISCARD_GRANULARITY] = { .type = NLA_U32, },
	[DRBD2_A_DISK_CONF_AL_UPDATES] = { .type = NLA_U8, },
	[DRBD2_A_DISK_CONF_DISCARD_ZEROES_IF_ALIGNED] = { .type = NLA_U8, },
	[DRBD2_A_DISK_CONF_DISABLE_WRITE_SAME] = { .type = NLA_U8, },
	[DRBD2_A_DISK_CONF_BITMAP] = { .type = NLA_U8, },
};

const struct nla_policy drbd2_invalidate_parms_nl_policy[DRBD2_A_INVALIDATE_PARMS_RESET_BITMAP + 1] = {
	[DRBD2_A_INVALIDATE_PARMS_SYNC_FROM_PEER_NODE_ID] = { .type = NLA_U32, },
	[DRBD2_A_INVALIDATE_PARMS_RESET_BITMAP] = { .type = NLA_U8, },
};

const struct nla_policy drbd2_invalidate_peer_parms_nl_policy[DRBD2_A_INVALIDATE_PEER_PARMS_RESET_BITMAP + 1] = {
	[DRBD2_A_INVALIDATE_PEER_PARMS_RESET_BITMAP] = { .type = NLA_U8, },
};

const struct nla_policy drbd2_net_conf_nl_policy[DRBD2_A_NET_CONF_RDMA_CTRL_SNDBUF_SIZE + 1] = {
	[DRBD2_A_NET_CONF_SHARED_SECRET] = { .type = NLA_NUL_STRING, .len = DRBD2_SHARED_SECRET_MAX, },
	[DRBD2_A_NET_CONF_CRAM_HMAC_ALG] = { .type = NLA_NUL_STRING, .len = DRBD2_ALG_NAME_MAX, },
	[DRBD2_A_NET_CONF_INTEGRITY_ALG] = { .type = NLA_NUL_STRING, .len = DRBD2_ALG_NAME_MAX, },
	[DRBD2_A_NET_CONF_VERIFY_ALG] = { .type = NLA_NUL_STRING, .len = DRBD2_ALG_NAME_MAX, },
	[DRBD2_A_NET_CONF_CSUMS_ALG] = { .type = NLA_NUL_STRING, .len = DRBD2_ALG_NAME_MAX, },
	[DRBD2_A_NET_CONF_PROTOCOL] = NLA_POLICY_RANGE(NLA_U32, 1, 3),
	[DRBD2_A_NET_CONF_CONNECT_INT] = { .type = NLA_U32, },
	[DRBD2_A_NET_CONF_TIMEOUT] = { .type = NLA_U32, },
	[DRBD2_A_NET_CONF_PING_INT] = { .type = NLA_U32, },
	[DRBD2_A_NET_CONF_PING_TIMEO] = { .type = NLA_U32, },
	[DRBD2_A_NET_CONF_SNDBUF_SIZE] = { .type = NLA_U32, },
	[DRBD2_A_NET_CONF_RCVBUF_SIZE] = { .type = NLA_U32, },
	[DRBD2_A_NET_CONF_KO_COUNT] = { .type = NLA_U32, },
	[DRBD2_A_NET_CONF_MAX_EPOCH_SIZE] = { .type = NLA_U32, },
	[DRBD2_A_NET_CONF_AFTER_SB_0PRI] = NLA_POLICY_MAX(NLA_U32, 12),
	[DRBD2_A_NET_CONF_AFTER_SB_1PRI] = NLA_POLICY_MAX(NLA_U32, 12),
	[DRBD2_A_NET_CONF_AFTER_SB_2PRI] = NLA_POLICY_MAX(NLA_U32, 12),
	[DRBD2_A_NET_CONF_RR_CONFLICT] = NLA_POLICY_MAX(NLA_U32, 12),
	[DRBD2_A_NET_CONF_ON_CONGESTION] = NLA_POLICY_MAX(NLA_U32, 2),
	[DRBD2_A_NET_CONF_CONG_FILL] = { .type = NLA_U32, },
	[DRBD2_A_NET_CONF_CONG_EXTENTS] = { .type = NLA_U32, },
	[DRBD2_A_NET_CONF_TWO_PRIMARIES] = { .type = NLA_U8, },
	[DRBD2_A_NET_CONF_TCP_CORK] = { .type = NLA_U8, },
	[DRBD2_A_NET_CONF_ALWAYS_ASBP] = { .type = NLA_U8, },
	[DRBD2_A_NET_CONF_USE_RLE] = { .type = NLA_U8, },
	[DRBD2_A_NET_CONF_FENCING] = NLA_POLICY_MAX(NLA_U32, 2),
	[DRBD2_A_NET_CONF_CONNECTION_NAME] = { .type = NLA_NUL_STRING, .len = DRBD2_CONNECTION_NAME_MAX, },
	[DRBD2_A_NET_CONF_CSUMS_AFTER_CRASH_ONLY] = { .type = NLA_U8, },
	[DRBD2_A_NET_CONF_SOCK_CHECK_TIMEO] = { .type = NLA_U32, },
	[DRBD2_A_NET_CONF_TRANSPORT_NAME] = { .type = NLA_NUL_STRING, .len = DRBD2_TRANSPORT_NAME_MAX, },
	[DRBD2_A_NET_CONF_MAX_BUFFERS] = { .type = NLA_U32, },
	[DRBD2_A_NET_CONF_ALLOW_REMOTE_READ] = { .type = NLA_U8, },
	[DRBD2_A_NET_CONF_TLS] = { .type = NLA_U8, },
	[DRBD2_A_NET_CONF_TLS_PRIVKEY] = { .type = NLA_S32, },
	[DRBD2_A_NET_CONF_TLS_CERTIFICATE] = { .type = NLA_S32, },
	[DRBD2_A_NET_CONF_TLS_KEYRING] = { .type = NLA_S32, },
	[DRBD2_A_NET_CONF_LOAD_BALANCE_PATHS] = { .type = NLA_U8, },
	[DRBD2_A_NET_CONF_RDMA_CTRL_RCVBUF_SIZE] = { .type = NLA_U32, },
	[DRBD2_A_NET_CONF_RDMA_CTRL_SNDBUF_SIZE] = { .type = NLA_U32, },
};

const struct nla_policy drbd2_new_current_uuid_parms_nl_policy[DRBD2_A_NEW_CURRENT_UUID_PARMS_FORCE_RESYNC + 1] = {
	[DRBD2_A_NEW_CURRENT_UUID_PARMS_CLEAR_BM] = { .type = NLA_FLAG, },
	[DRBD2_A_NEW_CURRENT_UUID_PARMS_FORCE_RESYNC] = { .type = NLA_FLAG, },
};

const struct nla_policy drbd2_peer_device_conf_nl_policy[DRBD2_A_PEER_DEVICE_CONF_PEER_TIEBREAKER + 1] = {
	[DRBD2_A_PEER_DEVICE_CONF_RESYNC_RATE] = { .type = NLA_U32, },
	[DRBD2_A_PEER_DEVICE_CONF_C_PLAN_AHEAD] = { .type = NLA_U32, },
	[DRBD2_A_PEER_DEVICE_CONF_C_DELAY_TARGET] = { .type = NLA_U32, },
	[DRBD2_A_PEER_DEVICE_CONF_C_FILL_TARGET] = { .type = NLA_U32, },
	[DRBD2_A_PEER_DEVICE_CONF_C_MAX_RATE] = { .type = NLA_U32, },
	[DRBD2_A_PEER_DEVICE_CONF_C_MIN_RATE] = { .type = NLA_U32, },
	[DRBD2_A_PEER_DEVICE_CONF_BITMAP] = { .type = NLA_U8, },
	[DRBD2_A_PEER_DEVICE_CONF_RESYNC_WITHOUT_REPLICATION] = { .type = NLA_U8, },
	[DRBD2_A_PEER_DEVICE_CONF_PEER_TIEBREAKER] = { .type = NLA_U8, },
};

const struct nla_policy drbd2_rename_parms_nl_policy[DRBD2_A_RENAME_PARMS_NEW_NAME + 1] = {
	[DRBD2_A_RENAME_PARMS_NEW_NAME] = { .type = NLA_NUL_STRING, .len = DRBD2_RESOURCE_NAME_MAX, },
};

const struct nla_policy drbd2_resize_parms_nl_policy[DRBD2_A_RESIZE_PARMS_AL_STRIPE_SIZE + 1] = {
	[DRBD2_A_RESIZE_PARMS_SIZE] = { .type = NLA_U64, },
	[DRBD2_A_RESIZE_PARMS_ASSUME_PEER_HAS_SPACE] = { .type = NLA_FLAG, },
	[DRBD2_A_RESIZE_PARMS_ASSUME_CLEAN] = { .type = NLA_FLAG, },
	[DRBD2_A_RESIZE_PARMS_AL_STRIPES] = { .type = NLA_U32, },
	[DRBD2_A_RESIZE_PARMS_AL_STRIPE_SIZE] = { .type = NLA_U32, },
};

const struct nla_policy drbd2_resource_opts_nl_policy[DRBD2_A_RESOURCE_OPTS_EXPLICIT_DRBD8_COMPAT + 1] = {
	[DRBD2_A_RESOURCE_OPTS_CPU_MASK] = { .type = NLA_NUL_STRING, .len = DRBD2_CPU_MASK_SIZE, },
	[DRBD2_A_RESOURCE_OPTS_ON_NO_DATA_ACCESSIBLE] = NLA_POLICY_MAX(NLA_U32, 1),
	[DRBD2_A_RESOURCE_OPTS_AUTO_PROMOTE] = { .type = NLA_U8, },
	[DRBD2_A_RESOURCE_OPTS_NODE_ID] = { .type = NLA_U32, },
	[DRBD2_A_RESOURCE_OPTS_PEER_ACK_WINDOW] = { .type = NLA_U32, },
	[DRBD2_A_RESOURCE_OPTS_TWOPC_TIMEOUT] = { .type = NLA_U32, },
	[DRBD2_A_RESOURCE_OPTS_TWOPC_RETRY_TIMEOUT] = { .type = NLA_U32, },
	[DRBD2_A_RESOURCE_OPTS_PEER_ACK_DELAY] = { .type = NLA_U32, },
	[DRBD2_A_RESOURCE_OPTS_AUTO_PROMOTE_TIMEOUT] = { .type = NLA_U32, },
	[DRBD2_A_RESOURCE_OPTS_MAX_IO_DEPTH] = { .type = NLA_U32, },
	[DRBD2_A_RESOURCE_OPTS_QUORUM] = { .type = NLA_S32, },
	[DRBD2_A_RESOURCE_OPTS_ON_NO_QUORUM] = NLA_POLICY_MAX(NLA_U32, 1),
	[DRBD2_A_RESOURCE_OPTS_QUORUM_MIN_REDUNDANCY] = { .type = NLA_S32, },
	[DRBD2_A_RESOURCE_OPTS_ON_SUSPENDED_PRIMARY_OUTDATED] = NLA_POLICY_MAX(NLA_U32, 1),
	[DRBD2_A_RESOURCE_OPTS_DRBD8_COMPAT_MODE] = { .type = NLA_U8, },
	[DRBD2_A_RESOURCE_OPTS_EXPLICIT_DRBD8_COMPAT] = { .type = NLA_U8, },
};

const struct nla_policy drbd2_set_role_parms_nl_policy[DRBD2_A_SET_ROLE_PARMS_FORCE + 1] = {
	[DRBD2_A_SET_ROLE_PARMS_FORCE] = { .type = NLA_FLAG, },
};

const struct nla_policy drbd2_start_ov_parms_nl_policy[DRBD2_A_START_OV_PARMS_STOP_SECTOR + 1] = {
	[DRBD2_A_START_OV_PARMS_START_SECTOR] = { .type = NLA_U64, },
	[DRBD2_A_START_OV_PARMS_STOP_SECTOR] = { .type = NLA_U64, },
};

const struct nla_policy drbd2_suspend_io_parms_nl_policy[DRBD2_A_SUSPEND_IO_PARMS_FLUSH_TO_PEERS + 1] = {
	[DRBD2_A_SUSPEND_IO_PARMS_BDEV_FREEZE] = { .type = NLA_U8, },
	[DRBD2_A_SUSPEND_IO_PARMS_FLUSH_TO_PEERS] = { .type = NLA_U8, },
};

/* DRBD2_CMD_RESOURCE_NEW - do */
static const struct nla_policy drbd2_resource_new_nl_policy[DRBD2_A_RESOURCE_OPTS + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_RESOURCE_OPTS] = NLA_POLICY_NESTED(drbd2_resource_opts_nl_policy),
};

/* DRBD2_CMD_RESOURCE_DEL - do */
static const struct nla_policy drbd2_resource_del_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_RESOURCE_SET - do */
static const struct nla_policy drbd2_resource_set_nl_policy[DRBD2_A_RESOURCE_OPTS + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_RESOURCE_OPTS] = NLA_POLICY_NESTED(drbd2_resource_opts_nl_policy),
	[DRBD2_A_SET_DEFAULTS] = { .type = NLA_FLAG, },
};

/* DRBD2_CMD_RESOURCE_RENAME - do */
static const struct nla_policy drbd2_resource_rename_nl_policy[DRBD2_A_RENAME_PARMS + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_RENAME_PARMS] = NLA_POLICY_NESTED(drbd2_rename_parms_nl_policy),
};

/* DRBD2_CMD_RESOURCE_DOWN - do */
static const struct nla_policy drbd2_resource_down_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_RESOURCE_PRIMARY - do */
static const struct nla_policy drbd2_resource_primary_nl_policy[DRBD2_A_SET_ROLE_PARMS + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_SET_ROLE_PARMS] = NLA_POLICY_NESTED(drbd2_set_role_parms_nl_policy),
};

/* DRBD2_CMD_RESOURCE_SECONDARY - do */
static const struct nla_policy drbd2_resource_secondary_nl_policy[DRBD2_A_SET_ROLE_PARMS + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_SET_ROLE_PARMS] = NLA_POLICY_NESTED(drbd2_set_role_parms_nl_policy),
};

/* DRBD2_CMD_RESOURCE_SUSPEND_IO - do */
static const struct nla_policy drbd2_resource_suspend_io_nl_policy[DRBD2_A_SUSPEND_IO_PARMS + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_SUSPEND_IO_PARMS] = NLA_POLICY_NESTED(drbd2_suspend_io_parms_nl_policy),
};

/* DRBD2_CMD_RESOURCE_RESUME_IO - do */
static const struct nla_policy drbd2_resource_resume_io_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_DEVICE_NEW - do */
static const struct nla_policy drbd2_device_new_nl_policy[DRBD2_A_DEVICE_CONF + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_DEVICE_CONF] = NLA_POLICY_NESTED(drbd2_device_conf_nl_policy),
};

/* DRBD2_CMD_DEVICE_DEL - do */
static const struct nla_policy drbd2_device_del_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_DEVICE_ATTACH - do */
static const struct nla_policy drbd2_device_attach_nl_policy[DRBD2_A_DISK_CONF + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_DISK_CONF] = NLA_POLICY_NESTED(drbd2_disk_conf_nl_policy),
};

/* DRBD2_CMD_DEVICE_DETACH - do */
static const struct nla_policy drbd2_device_detach_nl_policy[DRBD2_A_DETACH_PARMS + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_DETACH_PARMS] = NLA_POLICY_NESTED(drbd2_detach_parms_nl_policy),
};

/* DRBD2_CMD_DISK_SET - do */
static const struct nla_policy drbd2_disk_set_nl_policy[DRBD2_A_DISK_CONF + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_DISK_CONF] = NLA_POLICY_NESTED(drbd2_disk_conf_nl_policy),
	[DRBD2_A_SET_DEFAULTS] = { .type = NLA_FLAG, },
};

/* DRBD2_CMD_DEVICE_RESIZE - do */
static const struct nla_policy drbd2_device_resize_nl_policy[DRBD2_A_RESIZE_PARMS + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_RESIZE_PARMS] = NLA_POLICY_NESTED(drbd2_resize_parms_nl_policy),
};

/* DRBD2_CMD_DEVICE_OUTDATE - do */
static const struct nla_policy drbd2_device_outdate_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_DEVICE_INVALIDATE - do */
static const struct nla_policy drbd2_device_invalidate_nl_policy[DRBD2_A_INVALIDATE_PARMS + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_INVALIDATE_PARMS] = NLA_POLICY_NESTED(drbd2_invalidate_parms_nl_policy),
};

/* DRBD2_CMD_DEVICE_NEW_CURRENT_UUID - do */
static const struct nla_policy drbd2_device_new_current_uuid_nl_policy[DRBD2_A_NEW_CURRENT_UUID_PARMS + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_NEW_CURRENT_UUID_PARMS] = NLA_POLICY_NESTED(drbd2_new_current_uuid_parms_nl_policy),
};

/* DRBD2_CMD_CONNECTION_NEW - do */
static const struct nla_policy drbd2_connection_new_nl_policy[DRBD2_A_NET_CONF + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_NET_CONF] = NLA_POLICY_NESTED(drbd2_net_conf_nl_policy),
};

/* DRBD2_CMD_CONNECTION_DEL - do */
static const struct nla_policy drbd2_connection_del_nl_policy[DRBD2_A_DISCONNECT_PARMS + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_DISCONNECT_PARMS] = NLA_POLICY_NESTED(drbd2_disconnect_parms_nl_policy),
};

/* DRBD2_CMD_CONNECTION_CONNECT - do */
static const struct nla_policy drbd2_connection_connect_nl_policy[DRBD2_A_CONNECT_PARMS + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_CONNECT_PARMS] = NLA_POLICY_NESTED(drbd2_connect_parms_nl_policy),
};

/* DRBD2_CMD_CONNECTION_DISCONNECT - do */
static const struct nla_policy drbd2_connection_disconnect_nl_policy[DRBD2_A_DISCONNECT_PARMS + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_DISCONNECT_PARMS] = NLA_POLICY_NESTED(drbd2_disconnect_parms_nl_policy),
};

/* DRBD2_CMD_CONNECTION_SET - do */
static const struct nla_policy drbd2_connection_set_nl_policy[DRBD2_A_NET_CONF + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_NET_CONF] = NLA_POLICY_NESTED(drbd2_net_conf_nl_policy),
	[DRBD2_A_SET_DEFAULTS] = { .type = NLA_FLAG, },
};

/* DRBD2_CMD_CONNECTION_FORGET - do */
static const struct nla_policy drbd2_connection_forget_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_PATH_NEW - do */
static const struct nla_policy drbd2_path_new_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_PATH_DEL - do */
static const struct nla_policy drbd2_path_del_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_PEER_DEVICE_SET - do */
static const struct nla_policy drbd2_peer_device_set_nl_policy[DRBD2_A_PEER_DEVICE_CONF + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_PEER_DEVICE_CONF] = NLA_POLICY_NESTED(drbd2_peer_device_conf_nl_policy),
	[DRBD2_A_SET_DEFAULTS] = { .type = NLA_FLAG, },
};

/* DRBD2_CMD_PEER_DEVICE_INVALIDATE - do */
static const struct nla_policy drbd2_peer_device_invalidate_nl_policy[DRBD2_A_INVALIDATE_PEER_PARMS + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_INVALIDATE_PEER_PARMS] = NLA_POLICY_NESTED(drbd2_invalidate_peer_parms_nl_policy),
};

/* DRBD2_CMD_PEER_DEVICE_PAUSE_SYNC - do */
static const struct nla_policy drbd2_peer_device_pause_sync_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_PEER_DEVICE_RESUME_SYNC - do */
static const struct nla_policy drbd2_peer_device_resume_sync_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_PEER_DEVICE_START_OV - do */
static const struct nla_policy drbd2_peer_device_start_ov_nl_policy[DRBD2_A_START_OV_PARMS + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_START_OV_PARMS] = NLA_POLICY_NESTED(drbd2_start_ov_parms_nl_policy),
};

/* DRBD2_CMD_TIMEOUT_TYPE_GET - do */
static const struct nla_policy drbd2_timeout_type_get_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_RESOURCE_GET - dump */
static const struct nla_policy drbd2_resource_get_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_DEVICE_GET - dump */
static const struct nla_policy drbd2_device_get_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_CONNECTION_GET - dump */
static const struct nla_policy drbd2_connection_get_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_PEER_DEVICE_GET - dump */
static const struct nla_policy drbd2_peer_device_get_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_PATH_GET - dump */
static const struct nla_policy drbd2_path_get_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_DEVICE_CBT_FLUSH - do */
static const struct nla_policy drbd2_device_cbt_flush_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_DEVICE_CBT_START - do */
static const struct nla_policy drbd2_device_cbt_start_nl_policy[DRBD2_A_CBT_START_PARMS + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
	[DRBD2_A_CBT_START_PARMS] = NLA_POLICY_NESTED(drbd2_cbt_start_parms_nl_policy),
};

/* DRBD2_CMD_DEVICE_CBT_STOP - do */
static const struct nla_policy drbd2_device_cbt_stop_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* DRBD2_CMD_DEVICE_CBT_STATUS - do */
static const struct nla_policy drbd2_device_cbt_status_nl_policy[DRBD2_A_CONTEXT + 1] = {
	[DRBD2_A_CONTEXT] = NLA_POLICY_NESTED(drbd2_context_nl_policy),
};

/* Ops table for drbd2 */
static const struct genl_split_ops drbd2_nl_ops[] = {
	{
		.cmd		= DRBD2_CMD_RESOURCE_NEW,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_resource_new_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_resource_new_nl_policy,
		.maxattr	= DRBD2_A_RESOURCE_OPTS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_RESOURCE_DEL,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_resource_del_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_resource_del_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_RESOURCE_SET,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_resource_set_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_resource_set_nl_policy,
		.maxattr	= DRBD2_A_RESOURCE_OPTS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_RESOURCE_RENAME,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_resource_rename_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_resource_rename_nl_policy,
		.maxattr	= DRBD2_A_RENAME_PARMS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_RESOURCE_DOWN,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_resource_down_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_resource_down_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_RESOURCE_PRIMARY,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_resource_primary_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_resource_primary_nl_policy,
		.maxattr	= DRBD2_A_SET_ROLE_PARMS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_RESOURCE_SECONDARY,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_resource_secondary_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_resource_secondary_nl_policy,
		.maxattr	= DRBD2_A_SET_ROLE_PARMS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_RESOURCE_SUSPEND_IO,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_resource_suspend_io_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_resource_suspend_io_nl_policy,
		.maxattr	= DRBD2_A_SUSPEND_IO_PARMS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_RESOURCE_RESUME_IO,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_resource_resume_io_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_resource_resume_io_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_DEVICE_NEW,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_device_new_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_device_new_nl_policy,
		.maxattr	= DRBD2_A_DEVICE_CONF,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_DEVICE_DEL,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_device_del_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_device_del_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_DEVICE_ATTACH,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_device_attach_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_device_attach_nl_policy,
		.maxattr	= DRBD2_A_DISK_CONF,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_DEVICE_DETACH,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_device_detach_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_device_detach_nl_policy,
		.maxattr	= DRBD2_A_DETACH_PARMS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_DISK_SET,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_disk_set_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_disk_set_nl_policy,
		.maxattr	= DRBD2_A_DISK_CONF,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_DEVICE_RESIZE,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_device_resize_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_device_resize_nl_policy,
		.maxattr	= DRBD2_A_RESIZE_PARMS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_DEVICE_OUTDATE,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_device_outdate_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_device_outdate_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_DEVICE_INVALIDATE,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_device_invalidate_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_device_invalidate_nl_policy,
		.maxattr	= DRBD2_A_INVALIDATE_PARMS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_DEVICE_NEW_CURRENT_UUID,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_device_new_current_uuid_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_device_new_current_uuid_nl_policy,
		.maxattr	= DRBD2_A_NEW_CURRENT_UUID_PARMS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_CONNECTION_NEW,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_connection_new_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_connection_new_nl_policy,
		.maxattr	= DRBD2_A_NET_CONF,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_CONNECTION_DEL,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_connection_del_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_connection_del_nl_policy,
		.maxattr	= DRBD2_A_DISCONNECT_PARMS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_CONNECTION_CONNECT,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_connection_connect_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_connection_connect_nl_policy,
		.maxattr	= DRBD2_A_CONNECT_PARMS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_CONNECTION_DISCONNECT,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_connection_disconnect_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_connection_disconnect_nl_policy,
		.maxattr	= DRBD2_A_DISCONNECT_PARMS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_CONNECTION_SET,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_connection_set_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_connection_set_nl_policy,
		.maxattr	= DRBD2_A_NET_CONF,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_CONNECTION_FORGET,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_connection_forget_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_connection_forget_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_PATH_NEW,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_path_new_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_path_new_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_PATH_DEL,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_path_del_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_path_del_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_PEER_DEVICE_SET,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_peer_device_set_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_peer_device_set_nl_policy,
		.maxattr	= DRBD2_A_PEER_DEVICE_CONF,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_PEER_DEVICE_INVALIDATE,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_peer_device_invalidate_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_peer_device_invalidate_nl_policy,
		.maxattr	= DRBD2_A_INVALIDATE_PEER_PARMS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_PEER_DEVICE_PAUSE_SYNC,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_peer_device_pause_sync_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_peer_device_pause_sync_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_PEER_DEVICE_RESUME_SYNC,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_peer_device_resume_sync_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_peer_device_resume_sync_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_PEER_DEVICE_START_OV,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_peer_device_start_ov_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_peer_device_start_ov_nl_policy,
		.maxattr	= DRBD2_A_START_OV_PARMS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_TIMEOUT_TYPE_GET,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_timeout_type_get_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_timeout_type_get_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_RESOURCE_GET,
		.dumpit		= drbd2_nl_resource_get_dumpit,
		.policy		= drbd2_resource_get_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= DRBD2_CMD_DEVICE_GET,
		.dumpit		= drbd2_nl_device_get_dumpit,
		.done		= drbd2_nl_device_get_done,
		.policy		= drbd2_device_get_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= DRBD2_CMD_CONNECTION_GET,
		.dumpit		= drbd2_nl_connection_get_dumpit,
		.done		= drbd2_nl_connection_get_done,
		.policy		= drbd2_connection_get_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= DRBD2_CMD_PEER_DEVICE_GET,
		.dumpit		= drbd2_nl_peer_device_get_dumpit,
		.done		= drbd2_nl_peer_device_get_done,
		.policy		= drbd2_peer_device_get_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= DRBD2_CMD_PATH_GET,
		.dumpit		= drbd2_nl_path_get_dumpit,
		.done		= drbd2_nl_path_get_done,
		.policy		= drbd2_path_get_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd	= DRBD2_CMD_STATE_GET,
		.dumpit	= drbd2_nl_state_get_dumpit,
		.done	= drbd2_nl_state_get_done,
		.flags	= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= DRBD2_CMD_DEVICE_CBT_FLUSH,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_device_cbt_flush_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_device_cbt_flush_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_DEVICE_CBT_START,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_device_cbt_start_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_device_cbt_start_nl_policy,
		.maxattr	= DRBD2_A_CBT_START_PARMS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_DEVICE_CBT_STOP,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_device_cbt_stop_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_device_cbt_stop_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRBD2_CMD_DEVICE_CBT_STATUS,
		.pre_doit	= drbd2_pre_doit,
		.doit		= drbd2_nl_device_cbt_status_doit,
		.post_doit	= drbd2_post_doit,
		.policy		= drbd2_device_cbt_status_nl_policy,
		.maxattr	= DRBD2_A_CONTEXT,
		.flags		= GENL_CMD_CAP_DO,
	},
};

static const struct genl_multicast_group drbd2_nl_mcgrps[] = {
	[DRBD2_NLGRP_EVENTS] = { "events", },
};

struct genl_family drbd2_nl_family __ro_after_init = {
	.name		= DRBD2_FAMILY_NAME,
	.version	= DRBD2_FAMILY_VERSION,
	.netnsok	= true,
	.parallel_ops	= true,
	.module		= THIS_MODULE,
	.split_ops	= drbd2_nl_ops,
	.n_split_ops	= ARRAY_SIZE(drbd2_nl_ops),
	.mcgrps		= drbd2_nl_mcgrps,
	.n_mcgrps	= ARRAY_SIZE(drbd2_nl_mcgrps),
};
