/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FS_CEPH_MON_CLIENT_H
#define _FS_CEPH_MON_CLIENT_H

#include <linux/completion.h>
#include <linux/kref.h>
#include <linux/rbtree.h>

#include <linux/ceph/messenger.h>

struct ceph_client;
struct ceph_mount_args;
struct ceph_auth_client;

/*
 * Monitor map metadata: Enumerates the set of all Ceph monitors in the cluster.
 * Used to track available monitors and their network addresses for failover.
 */
struct ceph_monmap {
	/* Unique filesystem identifier for this Ceph cluster */
	struct ceph_fsid fsid;
	/* Monitor map version/epoch number */
	u32 epoch;
	/* Number of monitors in the cluster */
	u32 num_mon;
	/* Array of monitor instances (address + name) */
	struct ceph_entity_inst mon_inst[] __counted_by(num_mon);
};

struct ceph_mon_client;
struct ceph_mon_generic_request;


/*
 * Monitor request callback metadata: Generic mechanism for resending monitor requests.
 * Called when switching to a new monitor or retrying failed requests.
 */
typedef void (*ceph_monc_request_func_t)(struct ceph_mon_client *monc,
					 int newmon);

/*
 * Pending monitor request metadata: Represents a monitor request that may need
 * to be retried or resent if the current monitor becomes unavailable.
 */
struct ceph_mon_request {
	/* Monitor client this request belongs to */
	struct ceph_mon_client *monc;
	/* Delayed work for request retry/resend */
	struct delayed_work delayed_work;
	/* Current retry delay in jiffies */
	unsigned long delay;
	/* Callback to execute the request */
	ceph_monc_request_func_t do_request;
};

/*
 * Generic request completion callback metadata: Called when a generic monitor
 * request completes, allowing the caller to process the response.
 */
typedef void (*ceph_monc_callback_t)(struct ceph_mon_generic_request *);

/*
 * Generic monitor request metadata: Used for statfs and mon_get_version requests
 * that need to return data to the caller. Provides synchronous and asynchronous
 * request patterns with proper cleanup and response handling.
 */
struct ceph_mon_generic_request {
	/* Monitor client this request belongs to */
	struct ceph_mon_client *monc;
	/* Reference counting for safe cleanup */
	struct kref kref;
	/* Transaction ID for request tracking */
	u64 tid;
	/* Red-black tree node for efficient lookup */
	struct rb_node node;
	/* Request completion result code */
	int result;

	/* Synchronous completion notification */
	struct completion completion;
	/* Asynchronous completion callback */
	ceph_monc_callback_t complete_cb;
	/* Caller-specific data (request ID/linger ID) */
	u64 private_data;

	/* Original request message sent to monitor */
	struct ceph_msg *request;
	/* Reply message received from monitor */
	struct ceph_msg *reply;

	/* Request-specific response data */
	union {
		/* For statfs requests: filesystem statistics */
		struct ceph_statfs *st;
		/* For version requests: newest available version */
		u64 newest;
	} u;
};

/*
 * Monitor client state metadata: Manages communication with Ceph monitor cluster.
 * Handles monitor discovery, failover, authentication, subscriptions, and request routing.
 * Provides high availability by automatically switching between available monitors.
 */
struct ceph_mon_client {
	/* Parent Ceph client instance */
	struct ceph_client *client;
	/* Current monitor map with available monitors */
	struct ceph_monmap *monmap;

	/* Serializes monitor client operations */
	struct mutex mutex;
	/* Delayed work for periodic operations and retries */
	struct delayed_work delayed_work;

	/* Authentication client for monitor authentication */
	struct ceph_auth_client *auth;
	/* Pre-allocated messages for auth and subscription protocols */
	struct ceph_msg *m_auth, *m_auth_reply, *m_subscribe, *m_subscribe_ack;
	/* Authentication request in progress flag */
	int pending_auth;

	/* Currently searching for available monitors */
	bool hunting;
	/* Index of last monitor contacted */
	int cur_mon;
	/* Time when subscriptions should be renewed */
	unsigned long sub_renew_after;
	/* Time when subscription renewal was last sent */
	unsigned long sub_renew_sent;
	/* Network connection to current monitor */
	struct ceph_connection con;

	/* Ever successfully connected to any monitor */
	bool had_a_connection;
	/* Hunt backoff multiplier [1..CEPH_MONC_HUNT_MAX_MULT] */
	int hunt_mult;

	/* Tree of pending generic requests awaiting responses */
	struct rb_root generic_request_tree;
	/* Last transaction ID assigned to generic requests */
	u64 last_tid;

	/* Map subscriptions indexed by CEPH_SUB_* constants */
	struct {
		/* Subscription request details */
		struct ceph_mon_subscribe_item item;
		/* Want to receive updates for this map type */
		bool want;
		/* Current epoch/version we have */
		u32 have;
	} subs[4];
	/* Filesystem cluster ID for "mdsmap.<id>" subscription */
	int fs_cluster_id;

#ifdef CONFIG_DEBUG_FS
	/* Debug filesystem entry for monitoring state */
	struct dentry *debugfs_file;
#endif
};

extern int ceph_monmap_contains(struct ceph_monmap *m,
				struct ceph_entity_addr *addr);

extern int ceph_monc_init(struct ceph_mon_client *monc, struct ceph_client *cl);
extern void ceph_monc_stop(struct ceph_mon_client *monc);
extern void ceph_monc_reopen_session(struct ceph_mon_client *monc);

/*
 * Map subscription type constants: Indices for different types of cluster maps
 * that can be subscribed to for receiving updates from monitors.
 */
enum {
	/* Monitor map - tracks available monitors */
	CEPH_SUB_MONMAP = 0,
	/* OSD map - tracks available storage daemons and placement groups */
	CEPH_SUB_OSDMAP,
	/* Filesystem map - tracks CephFS filesystems */
	CEPH_SUB_FSMAP,
	/* MDS map - tracks metadata server daemons */
	CEPH_SUB_MDSMAP,
};

extern const char *ceph_sub_str[];

/*
 * The model here is to indicate that we need a new map of at least
 * epoch @epoch, and also call in when we receive a map.  We will
 * periodically rerequest the map from the monitor cluster until we
 * get what we want.
 */
bool ceph_monc_want_map(struct ceph_mon_client *monc, int sub, u32 epoch,
			bool continuous);
void ceph_monc_got_map(struct ceph_mon_client *monc, int sub, u32 epoch);
void ceph_monc_renew_subs(struct ceph_mon_client *monc);

extern int ceph_monc_wait_osdmap(struct ceph_mon_client *monc, u32 epoch,
				 unsigned long timeout);

int ceph_monc_do_statfs(struct ceph_mon_client *monc, u64 data_pool,
			struct ceph_statfs *buf);

int ceph_monc_get_version(struct ceph_mon_client *monc, const char *what,
			  u64 *newest);
int ceph_monc_get_version_async(struct ceph_mon_client *monc, const char *what,
				ceph_monc_callback_t cb, u64 private_data);

int ceph_monc_blocklist_add(struct ceph_mon_client *monc,
			    struct ceph_entity_addr *client_addr);

extern int ceph_monc_open_session(struct ceph_mon_client *monc);

extern int ceph_monc_validate_auth(struct ceph_mon_client *monc);

#endif
