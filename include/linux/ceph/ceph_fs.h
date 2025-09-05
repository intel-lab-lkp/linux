/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ceph_fs.h - Ceph constants and data types to share between kernel and
 * user space.
 *
 * Most types in this file are defined as little-endian, and are
 * primarily intended to describe data structures that pass over the
 * wire or that are stored on disk.
 *
 * LGPL2
 */

#ifndef CEPH_FS_H
#define CEPH_FS_H

#include <linux/ceph/msgr.h>
#include <linux/ceph/rados.h>

/*
 * subprotocol versions.  when specific messages types or high-level
 * protocols change, bump the affected components.  we keep rev
 * internal cluster protocols separately from the public,
 * client-facing protocol.
 */
#define CEPH_OSDC_PROTOCOL   24 /* server/client */
#define CEPH_MDSC_PROTOCOL   32 /* server/client */
#define CEPH_MONC_PROTOCOL   15 /* server/client */


#define CEPH_INO_ROOT   1
#define CEPH_INO_CEPH   2            /* hidden .ceph dir */
#define CEPH_INO_GLOBAL_SNAPREALM  3 /* global dummy snaprealm */

/* arbitrary limit on max # of monitors (cluster of 3 is typical) */
#define CEPH_MAX_MON   31

/*
 * Legacy file layout metadata: Wire format for older file layout structures.
 * Describes how a file's data is striped across RADOS objects and distributed
 * across placement groups. Maintained for backward compatibility.
 */
struct ceph_file_layout_legacy {
	/* File-to-object mapping parameters */
	/* Stripe unit size in bytes (must be page-aligned) */
	__le32 fl_stripe_unit;
	/* Number of objects to stripe across */
	__le32 fl_stripe_count;
	/* Maximum object size before creating new objects */
	__le32 fl_object_size;
	/* Content-addressable storage hash (unused) */
	__le32 fl_cas_hash;

	/* Placement group to disk layout */
	/* Per-object parity stripe unit (unused) */
	__le32 fl_object_stripe_unit;

	/* Object to placement group layout */
	/* Unused field (was preferred primary PG) */
	__le32 fl_unused;
	/* Pool ID for namespace, CRUSH rules, replication level */
	__le32 fl_pg_pool;
} __attribute__ ((packed));

struct ceph_string;
/*
 * File layout metadata: Describes how a file's data is distributed across
 * RADOS objects within a storage pool. Controls striping, object sizing,
 * and namespace placement for optimal performance and data distribution.
 */
struct ceph_file_layout {
	/* File-to-object striping parameters */
	/* Stripe unit size in bytes */
	u32 stripe_unit;
	/* Number of objects to stripe data across */
	u32 stripe_count;
	/* Maximum size of individual RADOS objects */
	u32 object_size;
	/* Target RADOS pool ID */
	s64 pool_id;
	/* Optional pool namespace (RCU-protected string) */
	struct ceph_string __rcu *pool_ns;
};

extern int ceph_file_layout_is_valid(const struct ceph_file_layout *layout);
extern void ceph_file_layout_from_legacy(struct ceph_file_layout *fl,
				struct ceph_file_layout_legacy *legacy);
extern void ceph_file_layout_to_legacy(struct ceph_file_layout *fl,
				struct ceph_file_layout_legacy *legacy);

#define CEPH_MIN_STRIPE_UNIT 65536

/*
 * Directory layout metadata: Describes how directory entries are distributed
 * and hashed for efficient lookup and enumeration. Currently minimal with
 * most fields reserved for future expansion.
 */
struct ceph_dir_layout {
	/* Directory hash function ID (see ceph_hash.h) */
	__u8   dl_dir_hash;
	/* Reserved fields for future use */
	__u8   dl_unused1;
	__u16  dl_unused2;
	__u32  dl_unused3;
} __attribute__ ((packed));

/* crypto algorithms */
#define CEPH_CRYPTO_NONE 0x0
#define CEPH_CRYPTO_AES  0x1

#define CEPH_AES_IV "cephsageyudagreg"

/* security/authentication protocols */
#define CEPH_AUTH_UNKNOWN	0x0
#define CEPH_AUTH_NONE	 	0x1
#define CEPH_AUTH_CEPHX	 	0x2

#define CEPH_AUTH_MODE_NONE		0
#define CEPH_AUTH_MODE_AUTHORIZER	1
#define CEPH_AUTH_MODE_MON		10

/* msgr2 protocol modes */
#define CEPH_CON_MODE_UNKNOWN	0x0
#define CEPH_CON_MODE_CRC	0x1
#define CEPH_CON_MODE_SECURE	0x2

#define CEPH_AUTH_UID_DEFAULT ((__u64) -1)

const char *ceph_auth_proto_name(int proto);
const char *ceph_con_mode_name(int mode);

/*********************************************
 * message layer
 */

/*
 * message types
 */

/* misc */
#define CEPH_MSG_SHUTDOWN               1
#define CEPH_MSG_PING                   2

/* client <-> monitor */
#define CEPH_MSG_MON_MAP                4
#define CEPH_MSG_MON_GET_MAP            5
#define CEPH_MSG_STATFS                 13
#define CEPH_MSG_STATFS_REPLY           14
#define CEPH_MSG_MON_SUBSCRIBE          15
#define CEPH_MSG_MON_SUBSCRIBE_ACK      16
#define CEPH_MSG_AUTH			17
#define CEPH_MSG_AUTH_REPLY		18
#define CEPH_MSG_MON_GET_VERSION        19
#define CEPH_MSG_MON_GET_VERSION_REPLY  20

/* client <-> mds */
#define CEPH_MSG_MDS_MAP                21
#define CEPH_MSG_FS_MAP_USER            103

#define CEPH_MSG_CLIENT_SESSION         22
#define CEPH_MSG_CLIENT_RECONNECT       23

#define CEPH_MSG_CLIENT_REQUEST         24
#define CEPH_MSG_CLIENT_REQUEST_FORWARD 25
#define CEPH_MSG_CLIENT_REPLY           26
#define CEPH_MSG_CLIENT_METRICS         29
#define CEPH_MSG_CLIENT_CAPS            0x310
#define CEPH_MSG_CLIENT_LEASE           0x311
#define CEPH_MSG_CLIENT_SNAP            0x312
#define CEPH_MSG_CLIENT_CAPRELEASE      0x313
#define CEPH_MSG_CLIENT_QUOTA           0x314

/* pool ops */
#define CEPH_MSG_POOLOP_REPLY           48
#define CEPH_MSG_POOLOP                 49

/* mon commands */
#define CEPH_MSG_MON_COMMAND            50
#define CEPH_MSG_MON_COMMAND_ACK        51

/* osd */
#define CEPH_MSG_OSD_MAP                41
#define CEPH_MSG_OSD_OP                 42
#define CEPH_MSG_OSD_OPREPLY            43
#define CEPH_MSG_WATCH_NOTIFY           44
#define CEPH_MSG_OSD_BACKOFF            61


/* watch-notify operations */
enum {
	CEPH_WATCH_EVENT_NOTIFY		  = 1, /* notifying watcher */
	CEPH_WATCH_EVENT_NOTIFY_COMPLETE  = 2, /* notifier notified when done */
	CEPH_WATCH_EVENT_DISCONNECT       = 3, /* we were disconnected */
};


/*
 * Monitor request header metadata: Common header for all client requests
 * to Ceph monitors. Includes version tracking and session identification
 * for proper request sequencing and duplicate detection.
 */
struct ceph_mon_request_header {
	/* Highest map version client currently has */
	__le64 have_version;
	/* Monitor rank for this session */
	__le16 session_mon;
	/* Transaction ID for this monitor session */
	__le64 session_mon_tid;
} __attribute__ ((packed));

/*
 * Ceph monitor statfs request structure
 *
 * Sent to the monitor to request filesystem statistics information.
 * Can request stats for the entire cluster or for a specific data pool.
 * The monitor responds with usage, capacity, and object count information.
 */
struct ceph_mon_statfs {
	struct ceph_mon_request_header monhdr; /* standard monitor request header */
	struct ceph_fsid fsid;                 /* filesystem identifier */
	__u8 contains_data_pool;               /* whether requesting pool-specific stats */
	__le64 data_pool;                      /* specific pool ID (if contains_data_pool) */
} __attribute__ ((packed));

/*
 * Filesystem statistics metadata: Reports storage usage and capacity
 * information for a Ceph filesystem or pool. Used by statfs() system call.
 */
struct ceph_statfs {
	/* Total capacity in kilobytes */
	__le64 kb;
	/* Used space in kilobytes */
	__le64 kb_used;
	/* Available space in kilobytes */
	__le64 kb_avail;
	/* Total number of objects stored */
	__le64 num_objects;
} __attribute__ ((packed));

/*
 * Ceph monitor statfs reply structure
 *
 * Response from the monitor containing filesystem statistics information.
 * Sent in response to a ceph_mon_statfs request, providing current usage,
 * capacity, and object count data for the requested filesystem or pool.
 */
struct ceph_mon_statfs_reply {
	struct ceph_fsid fsid;         /* filesystem identifier */
	__le64 version;                /* statistics version/timestamp */
	struct ceph_statfs st;         /* actual filesystem statistics */
} __attribute__ ((packed));

/*
 * Ceph monitor command structure
 *
 * Used to send administrative commands to the Ceph monitor. The command
 * is specified as a text string that follows this header structure.
 * Monitor responds with command results or error information.
 */
struct ceph_mon_command {
	struct ceph_mon_request_header monhdr; /* standard monitor request header */
	struct ceph_fsid fsid;                 /* filesystem identifier */
	__le32 num_strs;                       /* number of command strings (always 1) */
	__le32 str_len;                        /* length of command string */
	char str[];                            /* command string (variable length) */
} __attribute__ ((packed));

/*
 * Ceph OSD map request structure
 *
 * Sent to the monitor to request OSD map updates. The client specifies
 * a starting epoch to receive incremental map updates from that point.
 * Essential for maintaining current cluster topology and OSD status.
 */
struct ceph_osd_getmap {
	struct ceph_mon_request_header monhdr; /* standard monitor request header */
	struct ceph_fsid fsid;                 /* filesystem identifier */
	__le32 start;                          /* starting epoch for map updates */
} __attribute__ ((packed));

/*
 * Ceph MDS map request structure
 *
 * Sent to the monitor to request MDS map updates. Contains information
 * about active metadata servers, their states, and filesystem layout.
 * Critical for clients to know which MDS to contact for operations.
 */
struct ceph_mds_getmap {
	struct ceph_mon_request_header monhdr; /* standard monitor request header */
	struct ceph_fsid fsid;                 /* filesystem identifier */
} __attribute__ ((packed));

/*
 * Ceph client mount request structure
 *
 * Minimal structure sent to the monitor during client mount operations.
 * Used to signal client presence and initiate the mount handshake with
 * the monitor. Contains only the basic monitor request header.
 */
struct ceph_client_mount {
	struct ceph_mon_request_header monhdr; /* standard monitor request header */
} __attribute__ ((packed));

#define CEPH_SUBSCRIBE_ONETIME    1  /* i want only 1 update after have */

/*
 * Ceph monitor subscription item
 *
 * Specifies subscription parameters for receiving map updates from the
 * monitor. Used within subscription requests to indicate starting epoch
 * and subscription behavior (one-time vs continuous updates).
 */
struct ceph_mon_subscribe_item {
	__le64 start;                          /* starting epoch/version for updates */
	__u8 flags;                            /* subscription flags (CEPH_SUBSCRIBE_*) */
} __attribute__ ((packed));

/*
 * Ceph monitor subscription acknowledgment
 *
 * Response from monitor confirming subscription requests. Indicates how long
 * the subscription will remain active and confirms the filesystem ID.
 * Used for managing subscription renewal timing.
 */
struct ceph_mon_subscribe_ack {
	__le32 duration;                       /* subscription duration in seconds */
	struct ceph_fsid fsid;                 /* filesystem identifier */
} __attribute__ ((packed));

#define CEPH_FS_CLUSTER_ID_NONE  -1

/*
 * mdsmap flags
 */
#define CEPH_MDSMAP_DOWN    (1<<0)  /* cluster deliberately down */

/*
 * mds states
 *   > 0 -> in
 *  <= 0 -> out
 */
#define CEPH_MDS_STATE_DNE          0  /* down, does not exist. */
#define CEPH_MDS_STATE_STOPPED     -1  /* down, once existed, but no subtrees.
					  empty log. */
#define CEPH_MDS_STATE_BOOT        -4  /* up, boot announcement. */
#define CEPH_MDS_STATE_STANDBY     -5  /* up, idle.  waiting for assignment. */
#define CEPH_MDS_STATE_CREATING    -6  /* up, creating MDS instance. */
#define CEPH_MDS_STATE_STARTING    -7  /* up, starting previously stopped mds */
#define CEPH_MDS_STATE_STANDBY_REPLAY -8 /* up, tailing active node's journal */
#define CEPH_MDS_STATE_REPLAYONCE   -9 /* up, replaying an active node's journal */

#define CEPH_MDS_STATE_REPLAY       8  /* up, replaying journal. */
#define CEPH_MDS_STATE_RESOLVE      9  /* up, disambiguating distributed
					  operations (import, rename, etc.) */
#define CEPH_MDS_STATE_RECONNECT    10 /* up, reconnect to clients */
#define CEPH_MDS_STATE_REJOIN       11 /* up, rejoining distributed cache */
#define CEPH_MDS_STATE_CLIENTREPLAY 12 /* up, replaying client operations */
#define CEPH_MDS_STATE_ACTIVE       13 /* up, active */
#define CEPH_MDS_STATE_STOPPING     14 /* up, but exporting metadata */

extern const char *ceph_mds_state_name(int s);


/*
 * metadata lock types.
 *  - these are bitmasks.. we can compose them
 *  - they also define the lock ordering by the MDS
 *  - a few of these are internal to the mds
 */
#define CEPH_LOCK_DVERSION    1
#define CEPH_LOCK_DN          2
#define CEPH_LOCK_ISNAP       16
#define CEPH_LOCK_IVERSION    32    /* mds internal */
#define CEPH_LOCK_IFILE       64
#define CEPH_LOCK_IAUTH       128
#define CEPH_LOCK_ILINK       256
#define CEPH_LOCK_IDFT        512   /* dir frag tree */
#define CEPH_LOCK_INEST       1024  /* mds internal */
#define CEPH_LOCK_IXATTR      2048
#define CEPH_LOCK_IFLOCK      4096  /* advisory file locks */
#define CEPH_LOCK_INO         8192  /* immutable inode bits; not a lock */
#define CEPH_LOCK_IPOLICY     16384 /* policy lock on dirs. MDS internal */

/* client_session ops */
enum {
	CEPH_SESSION_REQUEST_OPEN,
	CEPH_SESSION_OPEN,
	CEPH_SESSION_REQUEST_CLOSE,
	CEPH_SESSION_CLOSE,
	CEPH_SESSION_REQUEST_RENEWCAPS,
	CEPH_SESSION_RENEWCAPS,
	CEPH_SESSION_STALE,
	CEPH_SESSION_RECALL_STATE,
	CEPH_SESSION_FLUSHMSG,
	CEPH_SESSION_FLUSHMSG_ACK,
	CEPH_SESSION_FORCE_RO,
	CEPH_SESSION_REJECT,
	CEPH_SESSION_REQUEST_FLUSH_MDLOG,
};

#define CEPH_SESSION_BLOCKLISTED	(1 << 0)  /* session blocklisted */

extern const char *ceph_session_op_name(int op);

/*
 * MDS session header metadata: Header for metadata server session messages.
 * Manages the client-MDS session lifecycle including capability and lease limits.
 */
struct ceph_mds_session_head {
	/* Session operation type */
	__le32 op;
	/* Session sequence number */
	__le64 seq;
	/* Message timestamp */
	struct ceph_timespec stamp;
	/* Maximum capabilities client can hold */
	__le32 max_caps;
	/* Maximum directory entry leases */
	__le32 max_leases;
} __attribute__ ((packed));

/* client_request */
/*
 * metadata ops.
 *  & 0x001000 -> write op
 *  & 0x010000 -> follow symlink (e.g. stat(), not lstat()).
 &  & 0x100000 -> use weird ino/path trace
 */
#define CEPH_MDS_OP_WRITE        0x001000
enum {
	CEPH_MDS_OP_LOOKUP     = 0x00100,
	CEPH_MDS_OP_GETATTR    = 0x00101,
	CEPH_MDS_OP_LOOKUPHASH = 0x00102,
	CEPH_MDS_OP_LOOKUPPARENT = 0x00103,
	CEPH_MDS_OP_LOOKUPINO  = 0x00104,
	CEPH_MDS_OP_LOOKUPNAME = 0x00105,
	CEPH_MDS_OP_GETVXATTR  = 0x00106,

	CEPH_MDS_OP_SETXATTR   = 0x01105,
	CEPH_MDS_OP_RMXATTR    = 0x01106,
	CEPH_MDS_OP_SETLAYOUT  = 0x01107,
	CEPH_MDS_OP_SETATTR    = 0x01108,
	CEPH_MDS_OP_SETFILELOCK= 0x01109,
	CEPH_MDS_OP_GETFILELOCK= 0x00110,
	CEPH_MDS_OP_SETDIRLAYOUT=0x0110a,

	CEPH_MDS_OP_MKNOD      = 0x01201,
	CEPH_MDS_OP_LINK       = 0x01202,
	CEPH_MDS_OP_UNLINK     = 0x01203,
	CEPH_MDS_OP_RENAME     = 0x01204,
	CEPH_MDS_OP_MKDIR      = 0x01220,
	CEPH_MDS_OP_RMDIR      = 0x01221,
	CEPH_MDS_OP_SYMLINK    = 0x01222,

	CEPH_MDS_OP_CREATE     = 0x01301,
	CEPH_MDS_OP_OPEN       = 0x00302,
	CEPH_MDS_OP_READDIR    = 0x00305,

	CEPH_MDS_OP_LOOKUPSNAP = 0x00400,
	CEPH_MDS_OP_MKSNAP     = 0x01400,
	CEPH_MDS_OP_RMSNAP     = 0x01401,
	CEPH_MDS_OP_LSSNAP     = 0x00402,
	CEPH_MDS_OP_RENAMESNAP = 0x01403,
};

#define IS_CEPH_MDS_OP_NEWINODE(op) (op == CEPH_MDS_OP_CREATE     || \
				     op == CEPH_MDS_OP_MKNOD      || \
				     op == CEPH_MDS_OP_MKDIR      || \
				     op == CEPH_MDS_OP_SYMLINK)

extern const char *ceph_mds_op_name(int op);

#define CEPH_SETATTR_MODE              (1 << 0)
#define CEPH_SETATTR_UID               (1 << 1)
#define CEPH_SETATTR_GID               (1 << 2)
#define CEPH_SETATTR_MTIME             (1 << 3)
#define CEPH_SETATTR_ATIME             (1 << 4)
#define CEPH_SETATTR_SIZE              (1 << 5)
#define CEPH_SETATTR_CTIME             (1 << 6)
#define CEPH_SETATTR_MTIME_NOW         (1 << 7)
#define CEPH_SETATTR_ATIME_NOW         (1 << 8)
#define CEPH_SETATTR_BTIME             (1 << 9)
#define CEPH_SETATTR_KILL_SGUID        (1 << 10)
#define CEPH_SETATTR_FSCRYPT_AUTH      (1 << 11)
#define CEPH_SETATTR_FSCRYPT_FILE      (1 << 12)

/*
 * Ceph setxattr request flags.
 */
#define CEPH_XATTR_CREATE  (1 << 0)
#define CEPH_XATTR_REPLACE (1 << 1)
#define CEPH_XATTR_REMOVE  (1 << 31)

/*
 * readdir request flags;
 */
#define CEPH_READDIR_REPLY_BITFLAGS	(1<<0)

/*
 * readdir reply flags.
 */
#define CEPH_READDIR_FRAG_END		(1<<0)
#define CEPH_READDIR_FRAG_COMPLETE	(1<<8)
#define CEPH_READDIR_HASH_ORDER		(1<<9)
#define CEPH_READDIR_OFFSET_HASH	(1<<10)

/*
 * open request flags
 */
#define CEPH_O_RDONLY		00000000
#define CEPH_O_WRONLY		00000001
#define CEPH_O_RDWR		00000002
#define CEPH_O_CREAT		00000100
#define CEPH_O_EXCL		00000200
#define CEPH_O_TRUNC		00001000
#define CEPH_O_DIRECTORY	00200000
#define CEPH_O_NOFOLLOW		00400000

/*
 * Ceph MDS request arguments union
 *
 * Contains operation-specific arguments for different MDS operations.
 * Each operation type has its own structure within the union, providing
 * the specific parameters needed for that operation while sharing the
 * same memory space efficiently.
 */
union ceph_mds_request_args {
	/* Get inode attributes operation */
	struct {
		__le32 mask;                 /* attribute mask (CEPH_CAP_*) */
	} __attribute__ ((packed)) getattr;

	/* Set inode attributes operation */
	struct {
		__le32 mode;                 /* file permissions */
		__le32 uid;                  /* user ID */
		__le32 gid;                  /* group ID */
		struct ceph_timespec mtime;  /* modification time */
		struct ceph_timespec atime;  /* access time */
		__le64 size, old_size;       /* new and old file sizes */
		__le32 mask;                 /* which attributes to set (CEPH_SETATTR_*) */
	} __attribute__ ((packed)) setattr;

	/* Read directory entries operation */
	struct {
		__le32 frag;                 /* directory fragment to read */
		__le32 max_entries;          /* maximum number of entries to return */
		__le32 max_bytes;            /* maximum response size in bytes */
		__le16 flags;                /* readdir operation flags */
		__le32 offset_hash;          /* hash offset for pagination */
	} __attribute__ ((packed)) readdir;

	/* Create device node (mknod) operation */
	struct {
		__le32 mode;                 /* file type and permissions */
		__le32 rdev;                 /* device number (major/minor) */
	} __attribute__ ((packed)) mknod;

	/* Create directory (mkdir) operation */
	struct {
		__le32 mode;                 /* directory permissions */
	} __attribute__ ((packed)) mkdir;

	/* Open/create file operation */
	struct {
		__le32 flags;                /* open flags (O_RDWR, O_CREAT, etc.) */
		__le32 mode;                 /* file permissions (for creation) */
		__le32 stripe_unit;          /* RADOS striping unit size */
		__le32 stripe_count;         /* number of objects to stripe across */
		__le32 object_size;          /* RADOS object size */
		__le32 pool;                 /* RADOS pool ID */
		__le32 mask;                 /* capability mask for new file */
		__le64 old_size;             /* previous file size (for truncation) */
	} __attribute__ ((packed)) open;

	/* Set extended attributes operation */
	struct {
		__le32 flags;                /* xattr operation flags */
		__le32 osdmap_epoch;         /* OSD map epoch for consistency */
	} __attribute__ ((packed)) setxattr;

	/* Set file/directory layout operation */
	struct {
		struct ceph_file_layout_legacy layout; /* striping layout */
	} __attribute__ ((packed)) setlayout;

	/* File locking operation */
	struct {
		__u8 rule;                   /* lock rule (CEPH_LOCK_FCNTL/FLOCK) */
		__u8 type;                   /* lock type (SHARED/EXCL/UNLOCK) */
		__le64 owner;                /* lock owner identifier */
		__le64 pid;                  /* process ID holding the lock */
		__le64 start;                /* byte offset where lock begins */
		__le64 length;               /* number of bytes to lock */
		__u8 wait;                   /* whether to wait for lock */
	} __attribute__ ((packed)) filelock_change;

	/* Lookup by inode number operation */
	struct {
		__le32 mask;                 /* attribute mask for returned data */
		__le64 snapid;               /* snapshot ID */
		__le64 parent;               /* parent inode number */
		__le32 hash;                 /* inode hash for verification */
	} __attribute__ ((packed)) lookupino;
} __attribute__ ((packed));

/*
 * Ceph MDS request arguments union (extended version)
 *
 * This union extends the original ceph_mds_request_args with support
 * for newer protocol features. It maintains backward compatibility
 * while adding extended functionality like birth time support.
 */
union ceph_mds_request_args_ext {
	union ceph_mds_request_args old; /* legacy argument formats */
	/* Extended setattr arguments with birth time support */
	struct {
		__le32 mode;                 /* file permissions */
		__le32 uid;                  /* user ID */
		__le32 gid;                  /* group ID */
		struct ceph_timespec mtime;  /* modification time */
		struct ceph_timespec atime;  /* access time */
		__le64 size, old_size;       /* current and previous file sizes */
		__le32 mask;                 /* attribute mask (CEPH_SETATTR_*) */
		struct ceph_timespec btime;  /* birth/creation time (extended) */
	} __attribute__ ((packed)) setattr_ext;
};

#define CEPH_MDS_FLAG_REPLAY		1 /* this is a replayed op */
#define CEPH_MDS_FLAG_WANT_DENTRY	2 /* want dentry in reply */
#define CEPH_MDS_FLAG_ASYNC		4 /* request is asynchronous */

/*
 * Ceph MDS request message header (legacy version)
 *
 * This is the original MDS request header format used before protocol
 * version 4. It lacks the version field and extended features present
 * in the modern header. Used for backward compatibility with older
 * MDS servers that don't support newer protocol features.
 */
struct ceph_mds_request_head_legacy {
	__le64 oldest_client_tid;      /* oldest transaction ID from client */
	__le32 mdsmap_epoch;           /* MDS map epoch client is using */
	__le32 flags;                  /* request flags (CEPH_MDS_FLAG_*) */
	__u8 num_retry, num_fwd;       /* retry and forward attempt counters */
	__le16 num_releases;           /* number of cap/lease release records */
	__le32 op;                     /* MDS operation code to perform */
	__le32 caller_uid, caller_gid; /* credentials of the caller */
	__le64 ino;                    /* inode number for replay operations */
	union ceph_mds_request_args args; /* operation-specific arguments */
} __attribute__ ((packed));

#define CEPH_MDS_REQUEST_HEAD_VERSION  3

/*
 * Ceph MDS request message header
 *
 * Contains the header information for all MDS request messages, including
 * operation type, client context, retry information, and operation-specific
 * arguments. This structure has evolved over time with different versions.
 */
struct ceph_mds_request_head {
	__le16 version;                /* header structure version */
	__le64 oldest_client_tid;      /* oldest transaction ID from client */
	__le32 mdsmap_epoch;           /* MDS map epoch client is using */
	__le32 flags;                  /* request flags (CEPH_MDS_FLAG_*) */
	__u8 num_retry, num_fwd;       /* legacy retry and forward counters */
	__le16 num_releases;           /* number of cap/lease release records */
	__le32 op;                     /* MDS operation code to perform */
	__le32 caller_uid, caller_gid; /* credentials of the caller */
	__le64 ino;                    /* inode number for replay operations */
	union ceph_mds_request_args_ext args; /* operation-specific arguments */

	__le32 ext_num_retry;          /* extended retry attempt counter */
	__le32 ext_num_fwd;            /* extended forward attempt counter */

	__le32 struct_len;             /* size of this header structure */
	__le32 owner_uid, owner_gid;   /* ownership for inode creation operations */
} __attribute__ ((packed));

/*
 * Ceph MDS capability/lease release record
 *
 * Included in MDS requests to inform the MDS about capabilities or
 * directory leases that the client is releasing. This allows the
 * client to proactively return unused capabilities to reduce overhead.
 */
struct ceph_mds_request_release {
	__le64 ino, cap_id;            /* inode number and capability identifier */
	__le32 caps, wanted;           /* capabilities being released/still wanted */
	__le32 seq, issue_seq, mseq;   /* sequence numbers for capability tracking */
	__le32 dname_seq;              /* directory name lease sequence number */
	__le32 dname_len;              /* length of dentry name string (follows) */
} __attribute__ ((packed));

/* client reply */
/*
 * Ceph MDS reply message header
 *
 * Contains the header information for all MDS reply messages, including
 * operation status, result codes, and flags indicating what additional
 * data structures follow in the message payload.
 */
struct ceph_mds_reply_head {
	__le32 op;                     /* MDS operation that was performed */
	__le32 result;                 /* operation result code (errno) */
	__le32 mdsmap_epoch;           /* MDS map epoch when reply was sent */
	__u8 safe;                     /* true if operation committed to disk */
	__u8 is_dentry, is_target;     /* flags: dentry and target inode data included */
} __attribute__ ((packed));

/* one for each node split */
/*
 * Ceph directory fragment tree split record
 *
 * Describes how a directory fragment is split into smaller fragments.
 * Each record specifies a fragment ID and the number of bits by which
 * it should be split to create multiple sub-fragments.
 */
struct ceph_frag_tree_split {
	__le32 frag;                   /* fragment identifier to split */
	__le32 by;                     /* number of bits to split by */
} __attribute__ ((packed));

/*
 * Ceph directory fragment tree header
 *
 * Contains the complete fragment tree structure for a directory, describing
 * how the directory namespace is divided among multiple fragments. Large
 * directories are split into fragments for load distribution across MDS nodes.
 */
struct ceph_frag_tree_head {
	__le32 nsplits;                /* number of fragment split records */
	struct ceph_frag_tree_split splits[]; /* array of split records */
} __attribute__ ((packed));

/* capability issue, for bundling with mds reply */
/*
 * Ceph MDS reply capability structure
 *
 * Contains capability information included in MDS replies, specifying
 * what capabilities are being granted to the client for an inode.
 */
struct ceph_mds_reply_cap {
	__le32 caps, wanted;           /* capabilities issued and wanted */
	__le64 cap_id;                 /* unique capability identifier */
	__le32 seq, mseq;              /* sequence and migration sequence numbers */
	__le64 realm;                  /* snapshot realm this cap belongs to */
	__u8 flags;                    /* capability flags (CEPH_CAP_FLAG_*) */
} __attribute__ ((packed));

#define CEPH_CAP_FLAG_AUTH	(1 << 0)  /* cap is issued by auth mds */
#define CEPH_CAP_FLAG_RELEASE	(1 << 1)  /* release the cap */

/*
 * Ceph MDS reply inode structure
 *
 * Contains complete inode metadata bundled with MDS replies. This allows
 * the MDS to send updated inode information along with operation results
 * to keep clients synchronized with the current inode state.
 */
struct ceph_mds_reply_inode {
	__le64 ino;                    /* inode number */
	__le64 snapid;                 /* snapshot ID */
	__le32 rdev;                   /* device number for special files */
	__le64 version;                /* inode version number */
	__le64 xattr_version;          /* extended attributes version */
	struct ceph_mds_reply_cap cap; /* capabilities issued for this inode */
	struct ceph_file_layout_legacy layout; /* file striping layout */
	struct ceph_timespec ctime, mtime, atime; /* timestamps */
	__le32 time_warp_seq;          /* time warp sequence number */
	__le64 size, max_size, truncate_size; /* file size information */
	__le32 truncate_seq;           /* truncate operation sequence */
	__le32 mode, uid, gid;         /* file permissions and ownership */
	__le32 nlink;                  /* number of hard links */
	__le64 files, subdirs, rbytes, rfiles, rsubdirs; /* directory statistics */
	struct ceph_timespec rctime;   /* recursive change time */
	struct ceph_frag_tree_head fragtree; /* fragment tree (must be at end) */
} __attribute__ ((packed));
/* followed by frag array, symlink string, dir layout, xattr blob */

/*
 * Ceph MDS reply lease structure
 *
 * Contains directory name lease information included in MDS replies.
 * Directory leases allow clients to cache directory entries and negative
 * lookups to improve performance by reducing round trips to the MDS.
 */
struct ceph_mds_reply_lease {
	__le16 mask;            /* lease type mask (CEPH_LEASE_*) */
	__le32 duration_ms;     /* lease duration in milliseconds */
	__le32 seq;             /* lease sequence number */
} __attribute__ ((packed));

#define CEPH_LEASE_VALID        (1 | 2) /* old and new bit values */
#define CEPH_LEASE_PRIMARY_LINK 4       /* primary linkage */

/*
 * Ceph MDS reply directory fragment structure
 *
 * Contains information about directory fragment distribution across MDS nodes.
 * Large directories are split into fragments that can be distributed across
 * multiple MDS nodes for load balancing and scalability.
 */
struct ceph_mds_reply_dirfrag {
	__le32 frag;            /* directory fragment identifier */
	__le32 auth;            /* authoritative MDS for this fragment */
	__le32 ndist;           /* number of MDS nodes this fragment is replicated on */
	__le32 dist[];          /* array of MDS node IDs holding replicas */
} __attribute__ ((packed));

#define CEPH_LOCK_FCNTL		1
#define CEPH_LOCK_FLOCK		2
#define CEPH_LOCK_FCNTL_INTR    3
#define CEPH_LOCK_FLOCK_INTR    4


#define CEPH_LOCK_SHARED   1
#define CEPH_LOCK_EXCL     2
#define CEPH_LOCK_UNLOCK   4

/*
 * Ceph file lock structure
 *
 * Represents advisory file locks (fcntl/flock) used for coordination
 * between clients accessing the same file. The MDS mediates these locks
 * across the cluster to ensure consistency.
 */
struct ceph_filelock {
	__le64 start;   /* file byte offset where lock begins */
	__le64 length;  /* number of bytes to lock (0 = lock to EOF) */
	__le64 client;  /* client ID that holds the lock */
	__le64 owner;   /* lock owner identifier (typically file pointer) */
	__le64 pid;     /* process ID holding the lock on the client */
	__u8 type;      /* lock type: CEPH_LOCK_SHARED/EXCL/UNLOCK */
} __attribute__ ((packed));


/* file access modes */
#define CEPH_FILE_MODE_PIN        0
#define CEPH_FILE_MODE_RD         1
#define CEPH_FILE_MODE_WR         2
#define CEPH_FILE_MODE_RDWR       3  /* RD | WR */
#define CEPH_FILE_MODE_LAZY       4  /* lazy io */
#define CEPH_FILE_MODE_BITS       4
#define CEPH_FILE_MODE_MASK       ((1 << CEPH_FILE_MODE_BITS) - 1)

int ceph_flags_to_mode(int flags);

#define CEPH_INLINE_NONE	((__u64)-1)

/* capability bits */
#define CEPH_CAP_PIN         1  /* no specific capabilities beyond the pin */

/* generic cap bits */
#define CEPH_CAP_GSHARED     1  /* client can reads */
#define CEPH_CAP_GEXCL       2  /* client can read and update */
#define CEPH_CAP_GCACHE      4  /* (file) client can cache reads */
#define CEPH_CAP_GRD         8  /* (file) client can read */
#define CEPH_CAP_GWR        16  /* (file) client can write */
#define CEPH_CAP_GBUFFER    32  /* (file) client can buffer writes */
#define CEPH_CAP_GWREXTEND  64  /* (file) client can extend EOF */
#define CEPH_CAP_GLAZYIO   128  /* (file) client can perform lazy io */

#define CEPH_CAP_SIMPLE_BITS  2
#define CEPH_CAP_FILE_BITS    8

/* per-lock shift */
#define CEPH_CAP_SAUTH      2
#define CEPH_CAP_SLINK      4
#define CEPH_CAP_SXATTR     6
#define CEPH_CAP_SFILE      8
#define CEPH_CAP_SFLOCK    20

#define CEPH_CAP_BITS      22

/* composed values */
#define CEPH_CAP_AUTH_SHARED  (CEPH_CAP_GSHARED  << CEPH_CAP_SAUTH)
#define CEPH_CAP_AUTH_EXCL     (CEPH_CAP_GEXCL     << CEPH_CAP_SAUTH)
#define CEPH_CAP_LINK_SHARED  (CEPH_CAP_GSHARED  << CEPH_CAP_SLINK)
#define CEPH_CAP_LINK_EXCL     (CEPH_CAP_GEXCL     << CEPH_CAP_SLINK)
#define CEPH_CAP_XATTR_SHARED (CEPH_CAP_GSHARED  << CEPH_CAP_SXATTR)
#define CEPH_CAP_XATTR_EXCL    (CEPH_CAP_GEXCL     << CEPH_CAP_SXATTR)
#define CEPH_CAP_FILE(x)    (x << CEPH_CAP_SFILE)
#define CEPH_CAP_FILE_SHARED   (CEPH_CAP_GSHARED   << CEPH_CAP_SFILE)
#define CEPH_CAP_FILE_EXCL     (CEPH_CAP_GEXCL     << CEPH_CAP_SFILE)
#define CEPH_CAP_FILE_CACHE    (CEPH_CAP_GCACHE    << CEPH_CAP_SFILE)
#define CEPH_CAP_FILE_RD       (CEPH_CAP_GRD       << CEPH_CAP_SFILE)
#define CEPH_CAP_FILE_WR       (CEPH_CAP_GWR       << CEPH_CAP_SFILE)
#define CEPH_CAP_FILE_BUFFER   (CEPH_CAP_GBUFFER   << CEPH_CAP_SFILE)
#define CEPH_CAP_FILE_WREXTEND (CEPH_CAP_GWREXTEND << CEPH_CAP_SFILE)
#define CEPH_CAP_FILE_LAZYIO   (CEPH_CAP_GLAZYIO   << CEPH_CAP_SFILE)
#define CEPH_CAP_FLOCK_SHARED  (CEPH_CAP_GSHARED   << CEPH_CAP_SFLOCK)
#define CEPH_CAP_FLOCK_EXCL    (CEPH_CAP_GEXCL     << CEPH_CAP_SFLOCK)


/* cap masks (for getattr) */
#define CEPH_STAT_CAP_INODE    CEPH_CAP_PIN
#define CEPH_STAT_CAP_TYPE     CEPH_CAP_PIN  /* mode >> 12 */
#define CEPH_STAT_CAP_SYMLINK  CEPH_CAP_PIN
#define CEPH_STAT_CAP_UID      CEPH_CAP_AUTH_SHARED
#define CEPH_STAT_CAP_GID      CEPH_CAP_AUTH_SHARED
#define CEPH_STAT_CAP_MODE     CEPH_CAP_AUTH_SHARED
#define CEPH_STAT_CAP_NLINK    CEPH_CAP_LINK_SHARED
#define CEPH_STAT_CAP_LAYOUT   CEPH_CAP_FILE_SHARED
#define CEPH_STAT_CAP_MTIME    CEPH_CAP_FILE_SHARED
#define CEPH_STAT_CAP_SIZE     CEPH_CAP_FILE_SHARED
#define CEPH_STAT_CAP_ATIME    CEPH_CAP_FILE_SHARED  /* fixme */
#define CEPH_STAT_CAP_XATTR    CEPH_CAP_XATTR_SHARED
#define CEPH_STAT_CAP_INODE_ALL (CEPH_CAP_PIN |			\
				 CEPH_CAP_AUTH_SHARED |	\
				 CEPH_CAP_LINK_SHARED |	\
				 CEPH_CAP_FILE_SHARED |	\
				 CEPH_CAP_XATTR_SHARED)
#define CEPH_STAT_CAP_INLINE_DATA (CEPH_CAP_FILE_SHARED | \
				   CEPH_CAP_FILE_RD)
#define CEPH_STAT_RSTAT CEPH_CAP_FILE_WREXTEND

#define CEPH_CAP_ANY_SHARED (CEPH_CAP_AUTH_SHARED |			\
			      CEPH_CAP_LINK_SHARED |			\
			      CEPH_CAP_XATTR_SHARED |			\
			      CEPH_CAP_FILE_SHARED)
#define CEPH_CAP_ANY_RD   (CEPH_CAP_ANY_SHARED | CEPH_CAP_FILE_RD |	\
			   CEPH_CAP_FILE_CACHE)

#define CEPH_CAP_ANY_EXCL (CEPH_CAP_AUTH_EXCL |		\
			   CEPH_CAP_LINK_EXCL |		\
			   CEPH_CAP_XATTR_EXCL |	\
			   CEPH_CAP_FILE_EXCL)
#define CEPH_CAP_ANY_FILE_RD (CEPH_CAP_FILE_RD | CEPH_CAP_FILE_CACHE | \
			      CEPH_CAP_FILE_SHARED)
#define CEPH_CAP_ANY_FILE_WR (CEPH_CAP_FILE_WR | CEPH_CAP_FILE_BUFFER |	\
			      CEPH_CAP_FILE_EXCL)
#define CEPH_CAP_ANY_WR   (CEPH_CAP_ANY_EXCL | CEPH_CAP_ANY_FILE_WR)
#define CEPH_CAP_ANY      (CEPH_CAP_ANY_RD | CEPH_CAP_ANY_EXCL | \
			   CEPH_CAP_ANY_FILE_WR | CEPH_CAP_FILE_LAZYIO | \
			   CEPH_CAP_PIN)
#define CEPH_CAP_ALL_FILE (CEPH_CAP_PIN | CEPH_CAP_ANY_SHARED | \
			   CEPH_CAP_AUTH_EXCL | CEPH_CAP_XATTR_EXCL | \
			   CEPH_CAP_ANY_FILE_RD | CEPH_CAP_ANY_FILE_WR)

#define CEPH_CAP_LOCKS (CEPH_LOCK_IFILE | CEPH_LOCK_IAUTH | CEPH_LOCK_ILINK | \
			CEPH_LOCK_IXATTR)

/* cap masks async dir operations */
#define CEPH_CAP_DIR_CREATE	CEPH_CAP_FILE_CACHE
#define CEPH_CAP_DIR_UNLINK	CEPH_CAP_FILE_RD
#define CEPH_CAP_ANY_DIR_OPS	(CEPH_CAP_FILE_CACHE | CEPH_CAP_FILE_RD | \
				 CEPH_CAP_FILE_WREXTEND | CEPH_CAP_FILE_LAZYIO)

int ceph_caps_for_mode(int mode);

enum {
	CEPH_CAP_OP_GRANT,         /* mds->client grant */
	CEPH_CAP_OP_REVOKE,        /* mds->client revoke */
	CEPH_CAP_OP_TRUNC,         /* mds->client trunc notify */
	CEPH_CAP_OP_EXPORT,        /* mds has exported the cap */
	CEPH_CAP_OP_IMPORT,        /* mds has imported the cap */
	CEPH_CAP_OP_UPDATE,        /* client->mds update */
	CEPH_CAP_OP_DROP,          /* client->mds drop cap bits */
	CEPH_CAP_OP_FLUSH,         /* client->mds cap writeback */
	CEPH_CAP_OP_FLUSH_ACK,     /* mds->client flushed */
	CEPH_CAP_OP_FLUSHSNAP,     /* client->mds flush snapped metadata */
	CEPH_CAP_OP_FLUSHSNAP_ACK, /* mds->client flushed snapped metadata */
	CEPH_CAP_OP_RELEASE,       /* client->mds release (clean) cap */
	CEPH_CAP_OP_RENEW,         /* client->mds renewal request */
};

extern const char *ceph_cap_op_name(int op);

/* flags field in client cap messages (version >= 10) */
#define CEPH_CLIENT_CAPS_SYNC			(1<<0)
#define CEPH_CLIENT_CAPS_NO_CAPSNAP		(1<<1)
#define CEPH_CLIENT_CAPS_PENDING_CAPSNAP	(1<<2)

/*
 * Ceph MDS capability message structure
 *
 * This structure represents capability-related messages exchanged between
 * the MDS and clients. Capabilities grant permissions to perform operations
 * on inodes and include cached metadata to reduce round trips.
 */
struct ceph_mds_caps {
	__le32 op;                  /* capability operation (CEPH_CAP_OP_*) */
	__le64 ino, realm;          /* inode number and snapshot realm */
	__le64 cap_id;              /* unique capability identifier */
	__le32 seq, issue_seq;      /* sequence numbers for ordering */
	__le32 caps, wanted, dirty; /* capability bits: granted/requested/dirty */
	__le32 migrate_seq;         /* sequence number for cap migration */
	__le64 snap_follows;        /* snapshot context this cap follows */
	__le32 snap_trace_len;      /* length of snapshot trace following */

	/* File ownership and permissions */
	__le32 uid, gid, mode;      /* owner user/group ID and file mode */

	/* Link count */
	__le32 nlink;               /* number of hard links to this inode */

	/* Extended attributes */
	__le32 xattr_len;           /* length of xattr blob */
	__le64 xattr_version;       /* version of extended attributes */

	/* File data and layout (union for export/non-export operations) */
	__le64 size, max_size, truncate_size;  /* current/max/truncate file sizes */
	__le32 truncate_seq;        /* truncate operation sequence number */
	struct ceph_timespec mtime, atime, ctime;  /* file timestamps */
	struct ceph_file_layout_legacy layout;     /* file striping layout */
	__le32 time_warp_seq;       /* sequence for time warp detection */
} __attribute__ ((packed));

/*
 * Ceph MDS capability peer information structure
 *
 * This structure contains information about a capability at a peer MDS,
 * used during capability import/export operations when capabilities are
 * migrated between different MDS nodes.
 */
struct ceph_mds_cap_peer {
	__le64 cap_id;		/* capability ID at the peer MDS */
	__le32 issue_seq;	/* issue sequence number at peer MDS */
	__le32 mseq;		/* migration sequence number at peer MDS */
	__le32 mds;		/* MDS number of the peer */
	__u8   flags;		/* capability flags at peer MDS */
} __attribute__ ((packed));

/*
 * Ceph MDS capability release message header
 *
 * This structure forms the header of a capability release message sent from
 * client to MDS to inform that the client is releasing (giving up) capabilities
 * on one or more inodes. The message contains a list of cap_item structures.
 */
struct ceph_mds_cap_release {
	__le32 num;                /* number of ceph_mds_cap_item entries following */
} __attribute__ ((packed));

/*
 * Ceph MDS capability release item
 *
 * Represents a single capability being released by the client. Multiple
 * cap items can be batched together in a single cap release message.
 */
struct ceph_mds_cap_item {
	__le64 ino;                /* inode number of the file */
	__le64 cap_id;             /* unique capability identifier */
	__le32 migrate_seq, issue_seq; /* migration and issue sequence numbers */
} __attribute__ ((packed));

#define CEPH_MDS_LEASE_REVOKE           1  /*    mds  -> client */
#define CEPH_MDS_LEASE_RELEASE          2  /* client  -> mds    */
#define CEPH_MDS_LEASE_RENEW            3  /* client <-> mds    */
#define CEPH_MDS_LEASE_REVOKE_ACK       4  /* client  -> mds    */

extern const char *ceph_lease_op_name(int o);

/*
 * Ceph MDS lease message structure
 *
 * This structure represents directory name lease messages exchanged between
 * MDS and clients. Directory leases grant clients permission to cache
 * directory contents and negative dentries to improve performance.
 */
struct ceph_mds_lease {
	__u8 action;            /* lease action (CEPH_MDS_LEASE_*) */
	__le16 mask;            /* which lease type is being acted upon */
	__le64 ino;             /* inode number of parent directory */
	__le64 first, last;     /* snapshot range for the lease */
	__le32 seq;             /* lease sequence number for ordering */
	__le32 duration_ms;     /* lease duration in milliseconds (for renewals) */
} __attribute__ ((packed));
/* followed by a __le32+string for dname */

/*
 * Ceph MDS capability reconnect structure (version 2)
 *
 * Sent during MDS session reconnection to restore capability state
 * after a session has been lost. This allows the client to inform
 * the MDS about capabilities it believes it holds.
 */
struct ceph_mds_cap_reconnect {
	__le64 cap_id;          /* unique capability identifier */
	__le32 wanted;          /* capabilities the client wants */
	__le32 issued;          /* capabilities the client believes are issued */
	__le64 snaprealm;       /* snapshot realm this inode belongs to */
	__le64 pathbase;        /* base inode number for path reconstruction */
	__le32 flock_len;       /* size of file lock state blob following */
} __attribute__ ((packed));
/* followed by flock blob */

/*
 * Ceph MDS capability reconnect structure (version 1)
 *
 * Legacy version of the capability reconnect structure used for
 * backwards compatibility with older MDS versions. Contains
 * additional file metadata fields not present in version 2.
 */
struct ceph_mds_cap_reconnect_v1 {
	__le64 cap_id;          /* unique capability identifier */
	__le32 wanted;          /* capabilities the client wants */
	__le32 issued;          /* capabilities the client believes are issued */
	__le64 size;            /* file size */
	struct ceph_timespec mtime, atime; /* file modification and access times */
	__le64 snaprealm;       /* snapshot realm this inode belongs to */
	__le64 pathbase;        /* base inode number for path reconstruction */
} __attribute__ ((packed));

/*
 * Ceph MDS snapshot realm reconnect structure
 *
 * Sent during MDS session reconnection to restore snapshot realm
 * hierarchy information. This helps the MDS reconstruct the client's
 * view of snapshot realms after a session interruption.
 */
struct ceph_mds_snaprealm_reconnect {
	__le64 ino;     /* inode number of snapshot realm root directory */
	__le64 seq;     /* sequence number of this snapshot realm */
	__le64 parent;  /* inode number of parent snapshot realm */
} __attribute__ ((packed));

/*
 * snaps
 */
enum {
	CEPH_SNAP_OP_UPDATE,  /* CREATE or DESTROY */
	CEPH_SNAP_OP_CREATE,
	CEPH_SNAP_OP_DESTROY,
	CEPH_SNAP_OP_SPLIT,
};

extern const char *ceph_snap_op_name(int o);

/*
 * Ceph MDS snapshot message header
 *
 * This structure forms the header for snapshot-related messages from the MDS,
 * containing operation type and metadata about snapshot realm operations.
 */
struct ceph_mds_snap_head {
	__le32 op;                /* snapshot operation type (CEPH_SNAP_OP_*) */
	__le64 split;             /* inode number to split off into new realm */
	__le32 num_split_inos;    /* number of inodes belonging to new child realm */
	__le32 num_split_realms;  /* number of child realms under new child realm */
	__le32 trace_len;         /* size of the snapshot trace blob following */
} __attribute__ ((packed));
/* followed by split ino list, then split realms, then the trace blob */

/*
 * Ceph MDS snapshot realm information structure
 *
 * Encodes information about a snapshot realm as viewed by a client.
 * A snapshot realm represents a subtree of the filesystem that shares
 * the same snapshot history and can be snapshotted as a unit.
 */
struct ceph_mds_snap_realm {
	__le64 ino;           /* inode number of the realm root directory */
	__le64 created;       /* snapshot ID when this realm was created */
	__le64 parent;        /* inode number of parent realm (0 if root) */
	__le64 parent_since;  /* snapshot ID since realm had same parent */
	__le64 seq;           /* sequence number for realm version/updates */
	__le32 num_snaps;     /* number of snapshots in this realm */
	__le32 num_prior_parent_snaps; /* number of parent snapshots before split */
} __attribute__ ((packed));
/* followed by my snap list, then prior parent snap list */

/*
 * Ceph MDS quota information structure
 *
 * This structure represents quota-related metadata sent from the MDS
 * to update directory quota limits and current usage statistics.
 */
struct ceph_mds_quota {
	__le64 ino;			/* inode number of the directory */
	struct ceph_timespec rctime;	/* recursive change time */
	__le64 rbytes;			/* recursive bytes used in directory tree */
	__le64 rfiles;			/* recursive file count in directory tree */
	__le64 rsubdirs;		/* recursive subdirectory count */
	__u8 struct_v;			/* structure version for compatibility */
	__u8 struct_compat;		/* compatibility version */
	__le32 struct_len;		/* length of this structure */
	__le64 max_bytes;		/* quota limit: maximum bytes allowed */
	__le64 max_files;		/* quota limit: maximum files allowed */
} __attribute__ ((packed));

#endif
