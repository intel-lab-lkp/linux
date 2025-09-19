// SPDX-License-Identifier: GPL-2.0
/*
 * Ceph fs string constants
 */
#include <linux/module.h>
#include <linux/ceph/types.h>

#include "kunit_tests.h"

const char *ceph_mds_state_name_strings[] = {
/* 0 */		"down:dne",
/* 1 */		"down:stopped",
/* 2 */		"up:boot",
/* 3 */		"up:standby",
/* 4 */		"up:standby-replay",
/* 5 */		"up:oneshot-replay",
/* 6 */		"up:creating",
/* 7 */		"up:starting",
/* 8 */		"up:replay",
/* 9 */		"up:resolve",
/* 10 */	"up:reconnect",
/* 11 */	"up:rejoin",
/* 12 */	"up:clientreplay",
/* 13 */	"up:active",
/* 14 */	"up:stopping",
/* 15 */	"???"
};
EXPORT_SYMBOL_IF_KUNIT(ceph_mds_state_name_strings);

VISIBLE_IF_KUNIT
int ceph_mds_state_2_str_idx(int mds_state)
{
	switch (mds_state) {
		/* down and out */
	case CEPH_MDS_STATE_DNE:
		return CEPH_MDS_STATE_DNE_STR_IDX;		/* 0 */
	case CEPH_MDS_STATE_STOPPED:
		return CEPH_MDS_STATE_STOPPED_STR_IDX;		/* 1 */
		/* up and out */
	case CEPH_MDS_STATE_BOOT:
		return CEPH_MDS_STATE_BOOT_STR_IDX;		/* 2 */
	case CEPH_MDS_STATE_STANDBY:
		return CEPH_MDS_STATE_STANDBY_STR_IDX;		/* 3 */
	case CEPH_MDS_STATE_STANDBY_REPLAY:
		return CEPH_MDS_STATE_CREATING_STR_IDX;		/* 4 */
	case CEPH_MDS_STATE_REPLAYONCE:
		return CEPH_MDS_STATE_STARTING_STR_IDX;		/* 5 */
	case CEPH_MDS_STATE_CREATING:
		return CEPH_MDS_STATE_STANDBY_REPLAY_STR_IDX;	/* 6 */
	case CEPH_MDS_STATE_STARTING:
		return CEPH_MDS_STATE_REPLAYONCE_STR_IDX;	/* 7 */
		/* up and in */
	case CEPH_MDS_STATE_REPLAY:
		return CEPH_MDS_STATE_REPLAY_STR_IDX;		/* 8 */
	case CEPH_MDS_STATE_RESOLVE:
		return CEPH_MDS_STATE_RESOLVE_STR_IDX;		/* 9 */
	case CEPH_MDS_STATE_RECONNECT:
		return CEPH_MDS_STATE_RECONNECT_STR_IDX;	/* 10 */
	case CEPH_MDS_STATE_REJOIN:
		return CEPH_MDS_STATE_REJOIN_STR_IDX;		/* 11 */
	case CEPH_MDS_STATE_CLIENTREPLAY:
		return CEPH_MDS_STATE_CLIENTREPLAY_STR_IDX;	/* 12 */
	case CEPH_MDS_STATE_ACTIVE:
		return CEPH_MDS_STATE_ACTIVE_STR_IDX;		/* 13 */
	case CEPH_MDS_STATE_STOPPING:
		return CEPH_MDS_STATE_STOPPING_STR_IDX;		/* 14 */
	default:
		/* do nothing */
		break;
	}

	return CEPH_MDS_STATE_UNKNOWN_NAME_STR_IDX;
}
EXPORT_SYMBOL_IF_KUNIT(ceph_mds_state_2_str_idx);

const char *ceph_mds_state_name(int s)
{
	return ceph_mds_state_name_strings[ceph_mds_state_2_str_idx(s)];
}
EXPORT_SYMBOL_IF_KUNIT(ceph_mds_state_name);

const char *ceph_session_op_name_strings[] = {
/* 0 */		"request_open",
/* 1 */		"open",
/* 2 */		"request_close",
/* 3 */		"close",
/* 4 */		"request_renewcaps",
/* 5 */		"renewcaps",
/* 6 */		"stale",
/* 7 */		"recall_state",
/* 8 */		"flushmsg",
/* 9 */		"flushmsg_ack",
/* 10 */	"force_ro",
/* 11 */	"reject",
/* 12 */	"flush_mdlog",
/* 13 */	"???"
};
EXPORT_SYMBOL_IF_KUNIT(ceph_session_op_name_strings);

VISIBLE_IF_KUNIT
int ceph_session_op_2_str_idx(int op)
{
	if (op < CEPH_SESSION_REQUEST_OPEN ||
	    op >= CEPH_SESSION_UNKNOWN_NAME)
		return CEPH_SESSION_UNKNOWN_NAME;

	return op;
}
EXPORT_SYMBOL_IF_KUNIT(ceph_session_op_2_str_idx);

const char *ceph_session_op_name(int op)
{
	return ceph_session_op_name_strings[ceph_session_op_2_str_idx(op)];
}
EXPORT_SYMBOL_IF_KUNIT(ceph_session_op_name);

const char *ceph_mds_op_name_strings[] = {
/* 0 */		"lookup",
/* 1 */		"getattr",
/* 2 */		"lookuphash",
/* 3 */		"lookupparent",
/* 4 */		"lookupino",
/* 5 */		"lookupname",
/* 6 */		"getvxattr",
/* 7 */		"setxattr",
/* 8 */		"rmxattr",
/* 9 */		"setlayou",
/* 10 */	"setattr",
/* 11 */	"setfilelock",
/* 12 */	"getfilelock",
/* 13 */	"setdirlayout",
/* 14 */	"mknod",
/* 15 */	"link",
/* 16 */	"unlink",
/* 17 */	"rename",
/* 18 */	"mkdir",
/* 19 */	"rmdir",
/* 20 */	"symlink",
/* 21 */	"create",
/* 22 */	"open",
/* 23 */	"readdir",
/* 24 */	"lookupsnap",
/* 25 */	"mksnap",
/* 26 */	"rmsnap",
/* 27 */	"lssnap",
/* 28 */	"renamesnap",
/* 29 */	"???"
};
EXPORT_SYMBOL_IF_KUNIT(ceph_mds_op_name_strings);

VISIBLE_IF_KUNIT
int ceph_mds_op_2_str_idx(int op)
{
	switch (op) {
	case CEPH_MDS_OP_LOOKUP:
		return CEPH_MDS_OP_LOOKUP_STR_IDX;		/* 0 */
	case CEPH_MDS_OP_LOOKUPHASH:
		return CEPH_MDS_OP_LOOKUPHASH_STR_IDX;		/* 2 */
	case CEPH_MDS_OP_LOOKUPPARENT:
		return CEPH_MDS_OP_LOOKUPPARENT_STR_IDX;	/* 3 */
	case CEPH_MDS_OP_LOOKUPINO:
		return CEPH_MDS_OP_LOOKUPINO_STR_IDX;		/* 4 */
	case CEPH_MDS_OP_LOOKUPNAME:
		return CEPH_MDS_OP_LOOKUPNAME_STR_IDX;		/* 5 */
	case CEPH_MDS_OP_GETATTR:
		return CEPH_MDS_OP_GETATTR_STR_IDX;		/* 1 */
	case CEPH_MDS_OP_GETVXATTR:
		return CEPH_MDS_OP_GETVXATTR_STR_IDX;		/* 6 */
	case CEPH_MDS_OP_SETXATTR:
		return CEPH_MDS_OP_SETXATTR_STR_IDX;		/* 7 */
	case CEPH_MDS_OP_SETATTR:
		return CEPH_MDS_OP_SETATTR_STR_IDX;		/* 10 */
	case CEPH_MDS_OP_RMXATTR:
		return CEPH_MDS_OP_RMXATTR_STR_IDX;		/* 8 */
	case CEPH_MDS_OP_SETLAYOUT:
		return CEPH_MDS_OP_SETLAYOUT_STR_IDX;		/* 9 */
	case CEPH_MDS_OP_SETDIRLAYOUT:
		return CEPH_MDS_OP_SETDIRLAYOUT_STR_IDX;	/* 13 */
	case CEPH_MDS_OP_READDIR:
		return CEPH_MDS_OP_READDIR_STR_IDX;		/* 23 */
	case CEPH_MDS_OP_MKNOD:
		return CEPH_MDS_OP_MKNOD_STR_IDX;		/* 14 */
	case CEPH_MDS_OP_LINK:
		return CEPH_MDS_OP_LINK_STR_IDX;		/* 15 */
	case CEPH_MDS_OP_UNLINK:
		return CEPH_MDS_OP_UNLINK_STR_IDX;		/* 16 */
	case CEPH_MDS_OP_RENAME:
		return CEPH_MDS_OP_RENAME_STR_IDX;		/* 17 */
	case CEPH_MDS_OP_MKDIR:
		return CEPH_MDS_OP_MKDIR_STR_IDX;		/* 18 */
	case CEPH_MDS_OP_RMDIR:
		return CEPH_MDS_OP_RMDIR_STR_IDX;		/* 19 */
	case CEPH_MDS_OP_SYMLINK:
		return CEPH_MDS_OP_SYMLINK_STR_IDX;		/* 20 */
	case CEPH_MDS_OP_CREATE:
		return CEPH_MDS_OP_CREATE_STR_IDX;		/* 21 */
	case CEPH_MDS_OP_OPEN:
		return CEPH_MDS_OP_OPEN_STR_IDX;		/* 22 */
	case CEPH_MDS_OP_LOOKUPSNAP:
		return CEPH_MDS_OP_LOOKUPSNAP_STR_IDX;		/* 24 */
	case CEPH_MDS_OP_LSSNAP:
		return CEPH_MDS_OP_LSSNAP_STR_IDX;		/* 27 */
	case CEPH_MDS_OP_MKSNAP:
		return CEPH_MDS_OP_MKSNAP_STR_IDX;		/* 25 */
	case CEPH_MDS_OP_RMSNAP:
		return CEPH_MDS_OP_RMSNAP_STR_IDX;		/* 26 */
	case CEPH_MDS_OP_RENAMESNAP:
		return CEPH_MDS_OP_RENAMESNAP_STR_IDX;		/* 28 */
	case CEPH_MDS_OP_SETFILELOCK:
		return CEPH_MDS_OP_SETFILELOCK_STR_IDX;		/* 11 */
	case CEPH_MDS_OP_GETFILELOCK:
		return CEPH_MDS_OP_GETFILELOCK_STR_IDX;		/* 12 */
	default:
		/* do nothing */
		break;
	}

	return CEPH_MDS_OP_UNKNOWN_NAME_STR_IDX;
}
EXPORT_SYMBOL_IF_KUNIT(ceph_mds_op_2_str_idx);

const char *ceph_mds_op_name(int op)
{
	return ceph_mds_op_name_strings[ceph_mds_op_2_str_idx(op)];
}
EXPORT_SYMBOL_IF_KUNIT(ceph_mds_op_name);

const char *ceph_cap_op_name_strings[] = {
/* 0 */		"grant",
/* 1 */		"revoke",
/* 2 */		"trunc",
/* 3 */		"export",
/* 4 */		"import",
/* 5 */		"update",
/* 6 */		"drop",
/* 7 */		"flush",
/* 8 */		"flush_ack",
/* 9 */		"flushsnap",
/* 10 */	"flushsnap_ack",
/* 11 */	"release",
/* 12 */	"renew",
/* 13 */	"???"
};
EXPORT_SYMBOL_IF_KUNIT(ceph_cap_op_name_strings);

VISIBLE_IF_KUNIT
int ceph_cap_op_2_str_idx(int op)
{
	if (op < CEPH_CAP_OP_GRANT ||
	    op >= CEPH_CAP_OP_UNKNOWN_NAME)
		return CEPH_SESSION_UNKNOWN_NAME;

	return op;
}
EXPORT_SYMBOL_IF_KUNIT(ceph_cap_op_2_str_idx);

const char *ceph_cap_op_name(int op)
{
	return ceph_cap_op_name_strings[ceph_cap_op_2_str_idx(op)];
}
EXPORT_SYMBOL_IF_KUNIT(ceph_cap_op_name);

const char *ceph_lease_op_name_strings[] = {
/* 0 */		"revoke",
/* 1 */		"release",
/* 2 */		"renew",
/* 3 */		"revoke_ack",
/* 4 */		"???"
};
EXPORT_SYMBOL_IF_KUNIT(ceph_lease_op_name_strings);

VISIBLE_IF_KUNIT
int ceph_lease_op_2_str_idx(int op)
{
	switch (op) {
	case CEPH_MDS_LEASE_REVOKE:
		return CEPH_MDS_LEASE_REVOKE_STR_IDX;
	case CEPH_MDS_LEASE_RELEASE:
		return CEPH_MDS_LEASE_RELEASE_STR_IDX;
	case CEPH_MDS_LEASE_RENEW:
		return CEPH_MDS_LEASE_RENEW_STR_IDX;
	case CEPH_MDS_LEASE_REVOKE_ACK:
		return CEPH_MDS_LEASE_REVOKE_ACK_STR_IDX;
	default:
		/* do nothing */
		break;
	}

	return CEPH_MDS_LEASE_UNKNOWN_NAME_STR_IDX;
}
EXPORT_SYMBOL_IF_KUNIT(ceph_lease_op_2_str_idx);

const char *ceph_lease_op_name(int op)
{
	return ceph_lease_op_name_strings[ceph_lease_op_2_str_idx(op)];
}
EXPORT_SYMBOL_IF_KUNIT(ceph_lease_op_name);

const char *ceph_snap_op_name_strings[] = {
/* 0 */		"update",
/* 1 */		"create",
/* 2 */		"destroy",
/* 3 */		"split",
/* 4 */		"???"
};
EXPORT_SYMBOL_IF_KUNIT(ceph_snap_op_name_strings);

VISIBLE_IF_KUNIT
int ceph_snap_op_2_str_idx(int op)
{
	if (op < CEPH_SNAP_OP_UPDATE ||
	    op >= CEPH_SNAP_OP_UNKNOWN_NAME)
		return CEPH_SNAP_OP_UNKNOWN_NAME;

	return op;
}
EXPORT_SYMBOL_IF_KUNIT(ceph_snap_op_2_str_idx);

const char *ceph_snap_op_name(int op)
{
	return ceph_snap_op_name_strings[ceph_snap_op_2_str_idx(op)];
}
EXPORT_SYMBOL_IF_KUNIT(ceph_snap_op_name);
