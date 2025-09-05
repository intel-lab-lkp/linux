/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FS_CEPH_OSD_CLIENT_H
#define _FS_CEPH_OSD_CLIENT_H

#include <linux/bitrev.h>
#include <linux/completion.h>
#include <linux/kref.h>
#include <linux/mempool.h>
#include <linux/rbtree.h>
#include <linux/refcount.h>
#include <linux/ktime.h>

#include <linux/ceph/types.h>
#include <linux/ceph/osdmap.h>
#include <linux/ceph/messenger.h>
#include <linux/ceph/msgpool.h>
#include <linux/ceph/auth.h>
#include <linux/ceph/pagelist.h>

struct ceph_msg;
struct ceph_snap_context;
struct ceph_osd_request;
struct ceph_osd_client;

/*
 * Completion callback for async operations: Called when OSD request completes
 * to notify the submitter of success or failure. Used for writepages and other
 * asynchronous I/O operations that need completion notification.
 */
typedef void (*ceph_osdc_callback_t)(struct ceph_osd_request *);

#define CEPH_HOMELESS_OSD	-1

/*
 * Sparse read extent metadata: Describes a single data extent in a SPARSE_READ reply.
 * Sparse reads allow efficient retrieval of files with holes by only transferring
 * data regions, skipping over sparse (zero) areas.
 *
 * Note that these come from the OSD as little-endian values. On BE arches,
 * we convert them in-place after receipt.
 */
struct ceph_sparse_extent {
	/* Offset of this extent within the object */
	u64	off;
	/* Length of data in this extent */
	u64	len;
} __packed;

/*
 * Sparse read state machine values: Tracks the parsing progress through
 * a SPARSE_READ reply message, which contains header, extent array, and data.
 */
enum ceph_sparse_read_state {
	/* Reading sparse read reply header */
	CEPH_SPARSE_READ_HDR	= 0,
	/* Reading extent array (offset/length pairs) */
	CEPH_SPARSE_READ_EXTENTS,
	/* Reading data length field */
	CEPH_SPARSE_READ_DATA_LEN,
	/* Pre-processing before reading actual data */
	CEPH_SPARSE_READ_DATA_PRE,
	/* Reading the actual file data */
	CEPH_SPARSE_READ_DATA,
};

/*
 * Sparse read parser metadata: Tracks the state of parsing a SPARSE_READ reply.
 * A SPARSE_READ reply is a 32-bit count of extents, followed by an array of
 * 64-bit offset/length pairs, and then all of the actual file data
 * concatenated after it (sans holes).
 *
 * Unfortunately, we don't know how long the extent array is until we've
 * started reading the data section of the reply. The caller should send down
 * a destination buffer for the array, but we'll alloc one if it's too small
 * or if the caller doesn't.
 */
struct ceph_sparse_read {
	/* Current state in the parsing state machine */
	enum ceph_sparse_read_state	sr_state;
	/* Original request offset for validation */
	u64				sr_req_off;
	/* Original request length for validation */
	u64				sr_req_len;
	/* Current position in the receive buffer */
	u64				sr_pos;
	/* Current extent being processed */
	int				sr_index;
	/* Total length of actual data (excluding holes) */
	u32				sr_datalen;
	/* Number of extents in the reply */
	u32				sr_count;
	/* Allocated length of extent array */
	int				sr_ext_len;
	/* Dynamic array of extent descriptors */
	struct ceph_sparse_extent	*sr_extent;
};

/*
 * OSD connection metadata: Represents a single Object Storage Daemon (OSD)
 * that we're actively communicating with. Manages the network connection,
 * pending requests, authentication, and sparse read state for this OSD.
 *
 * Note that the o_requests tree can be searched while holding the "lock" mutex
 * or the "o_requests_lock" spinlock. Insertion or removal requires both!
 */
struct ceph_osd {
	/* Reference counting for safe cleanup */
	refcount_t o_ref;
	/* Index of current sparse read operation */
	int o_sparse_op_idx;
	/* Back-reference to OSD client */
	struct ceph_osd_client *o_osdc;
	/* OSD identifier number in the cluster */
	int o_osd;
	/* OSD incarnation number for detecting restarts */
	int o_incarnation;
	/* Red-black tree node in osdc->osds tree */
	struct rb_node o_node;
	/* Network connection to this OSD */
	struct ceph_connection o_con;
	/* Protects request trees (fast path) */
	spinlock_t o_requests_lock;
	/* Tree of regular requests to this OSD */
	struct rb_root o_requests;
	/* Tree of linger requests (watches/notifies) */
	struct rb_root o_linger_requests;
	/* Backoff mappings by placement group */
	struct rb_root o_backoff_mappings;
	/* Backoff mappings by backoff ID */
	struct rb_root o_backoffs_by_id;
	/* LRU list node for idle OSD cleanup */
	struct list_head o_osd_lru;
	/* Authentication handshake state */
	struct ceph_auth_handshake o_auth;
	/* Time when this OSD should be considered for LRU eviction */
	unsigned long lru_ttl;
	/* Keepalive processing list */
	struct list_head o_keepalive_item;
	/* Serializes OSD operations (slow path) */
	struct mutex lock;
	/* Sparse read parsing state for this OSD */
	struct ceph_sparse_read	o_sparse_read;
};

#define CEPH_OSD_SLAB_OPS	2
#define CEPH_OSD_MAX_OPS	16

/*
 * OSD data container types: Defines the different ways data can be provided
 * to OSD operations, allowing flexible memory management for different I/O patterns.
 */
enum ceph_osd_data_type {
	/* No data attached */
	CEPH_OSD_DATA_TYPE_NONE = 0,
	/* Array of struct page pointers */
	CEPH_OSD_DATA_TYPE_PAGES,
	/* Ceph pagelist structure */
	CEPH_OSD_DATA_TYPE_PAGELIST,
#ifdef CONFIG_BLOCK
	/* Block I/O bio structure */
	CEPH_OSD_DATA_TYPE_BIO,
#endif /* CONFIG_BLOCK */
	/* Array of bio_vec structures */
	CEPH_OSD_DATA_TYPE_BVECS,
	/* Iterator over memory regions */
	CEPH_OSD_DATA_TYPE_ITER,
};

/*
 * OSD data container metadata: Flexible container for different types of data
 * that can be sent to or received from OSDs. Supports various memory layouts
 * for efficient I/O without unnecessary copying.
 */
struct ceph_osd_data {
	/* Type of data container being used */
	enum ceph_osd_data_type	type;
	union {
		/* Page array data container */
		struct {
			/* Array of page pointers */
			struct page	**pages;
			/* Total data length */
			u64		length;
			/* Alignment requirement for first page */
			u32		alignment;
			/* Pages allocated from mempool */
			bool		pages_from_pool;
			/* We own the pages and must free them */
			bool		own_pages;
		};
		/* Ceph pagelist container */
		struct ceph_pagelist	*pagelist;
#ifdef CONFIG_BLOCK
		/* Block I/O bio container */
		struct {
			/* Bio iterator position */
			struct ceph_bio_iter	bio_pos;
			/* Length of bio data */
			u32			bio_length;
		};
#endif /* CONFIG_BLOCK */
		/* Bio vector array container */
		struct {
			/* Bio vector iterator position */
			struct ceph_bvec_iter	bvec_pos;
			/* Number of bio vectors */
			u32			num_bvecs;
		};
		/* Generic iterator over memory */
		struct iov_iter		iter;
	};
};

/*
 * OSD request operation metadata: Describes a single operation within an OSD request.
 * Each request can contain multiple operations that are executed atomically.
 * Supports various operation types like read/write, class methods, xattrs, etc.
 */
struct ceph_osd_req_op {
	/* Operation type (CEPH_OSD_OP_*) */
	u16 op;
	/* Operation flags (CEPH_OSD_OP_FLAG_*) */
	u32 flags;
	/* Length of input data */
	u32 indata_len;
	/* Length of expected output data */
	u32 outdata_len;
	/* Operation result/error code */
	s32 rval;

	/* Operation-specific parameters */
	union {
		/* Raw data input for simple operations */
		struct ceph_osd_data raw_data_in;
		/* Extent-based operations (read/write) */
		struct {
			/* Offset within object + length of extent */
			u64 offset, length;
			/* Truncation parameters */
			u64 truncate_size;
			u32 truncate_seq;
			/* Sparse extent information */
			int sparse_ext_cnt;
			struct ceph_sparse_extent *sparse_ext;
			/* Data payload */
			struct ceph_osd_data osd_data;
		} extent;
		/* Extended attribute operations */
		struct {
			/* Attribute name and value lengths */
			u32 name_len;
			u32 value_len;
			/* Comparison operation type */
			__u8 cmp_op;       /* CEPH_OSD_CMPXATTR_OP_* */
			/* Comparison mode */
			__u8 cmp_mode;     /* CEPH_OSD_CMPXATTR_MODE_* */
			/* Attribute data */
			struct ceph_osd_data osd_data;
		} xattr;
		/* Object class method invocation */
		struct {
			/* Class and method names */
			const char *class_name;
			const char *method_name;
			/* Method call data */
			struct ceph_osd_data request_info;
			struct ceph_osd_data request_data;
			struct ceph_osd_data response_data;
			/* Name lengths */
			__u8 class_len;
			__u8 method_len;
			/* Input data length */
			u32 indata_len;
		} cls;
		/* Watch operations for object monitoring */
		struct {
			/* Watch cookie for identification */
			u64 cookie;
			/* Watch operation type */
			__u8 op;           /* CEPH_OSD_WATCH_OP_ */
			/* Generation number */
			u32 gen;
		} watch;
		/* Notify acknowledgment */
		struct {
			/* Acknowledgment data */
			struct ceph_osd_data request_data;
		} notify_ack;
		/* Object notification */
		struct {
			/* Notification cookie */
			u64 cookie;
			/* Notification payload */
			struct ceph_osd_data request_data;
			struct ceph_osd_data response_data;
		} notify;
		/* List current watchers */
		struct {
			/* Watcher list response */
			struct ceph_osd_data response_data;
		} list_watchers;
		/* Allocation hint for object sizing */
		struct {
			/* Expected object and write sizes */
			u64 expected_object_size;
			u64 expected_write_size;
			/* Allocation flags */
			u32 flags;  /* CEPH_OSD_OP_ALLOC_HINT_FLAG_* */
		} alloc_hint;
		/* Copy from another object */
		struct {
			/* Source snapshot ID */
			u64 snapid;
			/* Source object version */
			u64 src_version;
			/* Copy operation flags */
			u8 flags;
			/* Source fadvise flags */
			u32 src_fadvise_flags;
			/* Destination data */
			struct ceph_osd_data osd_data;
		} copy_from;
		/* Assert object version */
		struct {
			/* Expected version */
			u64 ver;
		} assert_ver;
	};
};

/*
 * OSD request target metadata: Contains object location and placement group
 * mapping information for routing requests to the correct OSD. Includes both
 * the original target and the resolved placement information.
 */
struct ceph_osd_request_target {
	/* Original object identifier */
	struct ceph_object_id base_oid;
	/* Original object locator (pool, namespace) */
	struct ceph_object_locator base_oloc;
	/* Resolved target object identifier */
	struct ceph_object_id target_oid;
	/* Resolved target object locator */
	struct ceph_object_locator target_oloc;

	/* Last raw placement group we mapped to */
	struct ceph_pg pgid;
	/* Last actual sharded placement group */
	struct ceph_spg spgid;
	/* Number of placement groups in pool */
	u32 pg_num;
	/* Bitmask for PG number calculation */
	u32 pg_num_mask;
	/* Acting OSD set for this PG */
	struct ceph_osds acting;
	/* Up OSD set for this PG */
	struct ceph_osds up;
	/* Replication size */
	int size;
	/* Minimum replicas required */
	int min_size;
	/* Use bitwise sorting for object names */
	bool sort_bitwise;
	/* Recovery can delete objects */
	bool recovery_deletes;

	/* Request flags (CEPH_OSD_FLAG_*) */
	unsigned int flags;
	/* Whether we used a replica OSD */
	bool used_replica;
	/* Request is paused */
	bool paused;

	/* OSD map epoch used for this mapping */
	u32 epoch;
	/* Last epoch we force-resent this request */
	u32 last_force_resend;

	/* Target OSD number */
	int osd;
};

/*
 * In-flight OSD request metadata: Represents a complete request to an OSD,
 * including target information, operations, timing, and completion handling.
 * Tracks the full lifecycle from submission to completion.
 */
struct ceph_osd_request {
	/* Unique transaction ID for this client */
	u64             r_tid;
	/* Red-black tree node for OSD's request tree */
	struct rb_node  r_node;
	/* Red-black tree node for map check tree */
	struct rb_node  r_mc_node;
	/* Work item for completion processing */
	struct work_struct r_complete_work;
	/* Target OSD for this request */
	struct ceph_osd *r_osd;

	/* Request targeting and placement information */
	struct ceph_osd_request_target r_t;
#define r_base_oid	r_t.base_oid
#define r_base_oloc	r_t.base_oloc
#define r_flags		r_t.flags

	/* Network messages for request and reply */
	struct ceph_msg  *r_request, *r_reply;
	/* >0 if r_request is sending/sent */
	u32               r_sent;

	/* Number of operations in this request */
	unsigned int		r_num_ops;

	/* Overall result code for the request */
	int               r_result;

	/* Back-reference to OSD client */
	struct ceph_osd_client *r_osdc;
	/* Reference counting for safe cleanup */
	struct kref       r_kref;
	/* Request allocated from mempool */
	bool              r_mempool;
	/* Linger request - don't resend on failure */
	bool		  r_linger;
	/* Completion notification (private to osd_client.c) */
	struct completion r_completion;
	/* Callback function for async completion */
	ceph_osdc_callback_t r_callback;

	/* Context information for callbacks */
	struct inode *r_inode;
	struct list_head r_private_item;
	void *r_priv;

	/* Request parameters set by submitter */
	/* Snapshot ID for reads, CEPH_NOSNAP otherwise */
	u64 r_snapid;
	/* Snapshot context for writes */
	struct ceph_snap_context *r_snapc;
	/* Modification time for writes */
	struct timespec64 r_mtime;
	/* Data offset within object */
	u64 r_data_offset;

	/* Internal tracking fields */
	/* Data version returned in reply */
	u64 r_version;
	/* Timestamp when sent or last checked */
	unsigned long r_stamp;                /* jiffies, send or check time */
	/* Timestamp when request started */
	unsigned long r_start_stamp;          /* jiffies */
	/* Latency measurement start time */
	ktime_t r_start_latency;              /* ktime_t */
	/* Latency measurement end time */
	ktime_t r_end_latency;                /* ktime_t */
	/* Number of send attempts */
	int r_attempts;
	/* Map epoch bound for "does not exist" errors */
	u32 r_map_dne_bound;

	/* Array of operations in this request */
	struct ceph_osd_req_op r_ops[] __counted_by(r_num_ops);
};

/*
 * Request redirect metadata: Contains the new object location when an OSD
 * request is redirected to a different pool or namespace.
 */
struct ceph_request_redirect {
	/* New object locator (pool, namespace) */
	struct ceph_object_locator oloc;
};

/*
 * OSD request identifier metadata: Uniquely identifies a request across
 * the cluster by combining client identity, incarnation, and transaction ID.
 * Used for request deduplication and tracking.
 *
 * Format: caller name + incarnation# + tid to uniquely identify this request
 */
struct ceph_osd_reqid {
	/* Client entity name (type + number) */
	struct ceph_entity_name name;
	/* Transaction ID */
	__le64 tid;
	/* Client incarnation number */
	__le32 inc;
} __packed;

/*
 * Blkin tracing metadata: Distributed tracing information for performance
 * analysis and debugging. Compatible with Zipkin/Jaeger tracing systems.
 */
struct ceph_blkin_trace_info {
	/* Unique trace identifier */
	__le64 trace_id;
	/* Span identifier within the trace */
	__le64 span_id;
	/* Parent span identifier */
	__le64 parent_span_id;
} __packed;

/*
 * Watch notification callback: Called when a watched object receives a notification.
 * Provides the notification data and identifies the notifier.
 */
typedef void (*rados_watchcb2_t)(void *arg, u64 notify_id, u64 cookie,
				 u64 notifier_id, void *data, size_t data_len);
/*
 * Watch error callback: Called when a watch encounters an error condition
 * such as connection loss or object deletion.
 */
typedef void (*rados_watcherrcb_t)(void *arg, u64 cookie, int err);

/*
 * Long-running OSD request metadata: Represents watch and notify operations
 * that persist beyond normal request completion. Handles connection recovery
 * and maintains state for ongoing object monitoring.
 */
struct ceph_osd_linger_request {
	/* Parent OSD client */
	struct ceph_osd_client *osdc;
	/* Unique linger request identifier */
	u64 linger_id;
	/* Registration has been committed */
	bool committed;
	/* True for watch, false for notify */
	bool is_watch;

	/* Target OSD for this linger request */
	struct ceph_osd *osd;
	/* Registration request */
	struct ceph_osd_request *reg_req;
	/* Keepalive ping request */
	struct ceph_osd_request *ping_req;
	/* When last ping was sent */
	unsigned long ping_sent;
	/* Watch validity expiration time */
	unsigned long watch_valid_thru;
	/* List of pending linger work items */
	struct list_head pending_lworks;

	/* Target object and placement information */
	struct ceph_osd_request_target t;
	/* Map epoch bound for "does not exist" errors */
	u32 map_dne_bound;

	/* Modification time */
	struct timespec64 mtime;

	/* Reference counting for safe cleanup */
	struct kref kref;
	/* Serializes linger request operations */
	struct mutex lock;
	/* Red-black tree node in OSD's linger tree */
	struct rb_node node;
	/* Red-black tree node in OSDC's linger tree */
	struct rb_node osdc_node;
	/* Red-black tree node in map check tree */
	struct rb_node mc_node;
	/* List item for scanning operations */
	struct list_head scan_item;

	/* Completion synchronization */
	struct completion reg_commit_wait;
	struct completion notify_finish_wait;
	/* Error codes */
	int reg_commit_error;
	int notify_finish_error;
	int last_error;

	/* Registration generation number */
	u32 register_gen;
	/* Notification identifier */
	u64 notify_id;

	/* Callback functions */
	rados_watchcb2_t wcb;
	rados_watcherrcb_t errcb;
	/* Callback context data */
	void *data;

	/* Request data structures */
	struct ceph_pagelist *request_pl;
	struct page **notify_id_pages;

	/* Reply handling */
	struct page ***preply_pages;
	size_t *preply_len;
};

/*
 * Watch item metadata: Describes a single watcher on an object,
 * including the client identity and network address.
 */
struct ceph_watch_item {
	/* Watcher client entity name */
	struct ceph_entity_name name;
	/* Unique watch cookie */
	u64 cookie;
	/* Watcher's network address */
	struct ceph_entity_addr addr;
};

/*
 * Sharded placement group mapping metadata: Maps a sharded placement group
 * to its associated backoff requests. Used for managing flow control when
 * OSDs request clients to back off from certain operations.
 */
struct ceph_spg_mapping {
	/* Red-black tree node for efficient lookup */
	struct rb_node node;
	/* Sharded placement group identifier */
	struct ceph_spg spgid;

	/* Tree of backoff requests for this PG */
	struct rb_root backoffs;
};

/*
 * RADOS object identifier metadata: Complete identification of an object
 * in the RADOS system, including pool, namespace, name, and snapshot.
 * Used for precise object addressing and comparison operations.
 */
struct ceph_hobject_id {
	/* Object key for special objects */
	void *key;
	size_t key_len;
	/* Object identifier string */
	void *oid;
	size_t oid_len;
	/* Snapshot identifier */
	u64 snapid;
	/* Object hash value for placement */
	u32 hash;
	/* Maximum object marker */
	u8 is_max;
	/* Object namespace */
	void *nspace;
	size_t nspace_len;
	/* Pool identifier */
	s64 pool;

	/* Cached bit-reversed hash for efficient comparisons */
	u32 hash_reverse_bits;
};

static inline void ceph_hoid_build_hash_cache(struct ceph_hobject_id *hoid)
{
	hoid->hash_reverse_bits = bitrev32(hoid->hash);
}

/*
 * OSD backoff metadata: Represents a request from an OSD for the client
 * to temporarily cease operations on a range of objects. Used for flow
 * control during recovery, rebalancing, or overload conditions.
 *
 * PG-wide backoff: [begin, end) covers a range
 * per-object backoff: begin == end covers single object
 */
struct ceph_osd_backoff {
	/* Red-black tree node indexed by PG */
	struct rb_node spg_node;
	/* Red-black tree node indexed by backoff ID */
	struct rb_node id_node;

	/* Sharded placement group this backoff applies to */
	struct ceph_spg spgid;
	/* Unique backoff identifier */
	u64 id;
	/* Beginning of object range (inclusive) */
	struct ceph_hobject_id *begin;
	/* End of object range (exclusive) */
	struct ceph_hobject_id *end;
};

#define CEPH_LINGER_ID_START	0xffff000000000000ULL

/*
 * OSD client metadata: Main interface for communicating with Ceph OSDs.
 * Manages OSD connections, request routing, map updates, and provides
 * high-level APIs for object operations, watches, and notifications.
 */
struct ceph_osd_client {
	/* Parent Ceph client instance */
	struct ceph_client     *client;

	/* Current OSD cluster map */
	struct ceph_osdmap     *osdmap;
	/* Protects OSD client state */
	struct rw_semaphore    lock;

	/* Tree of active OSD connections */
	struct rb_root         osds;
	/* LRU list of idle OSD connections */
	struct list_head       osd_lru;
	/* Protects OSD LRU operations */
	spinlock_t             osd_lru_lock;
	/* Minimum map epoch for request processing */
	u32		       epoch_barrier;
	/* Placeholder OSD for unmapped requests */
	struct ceph_osd        homeless_osd;
	/* Last transaction ID assigned */
	atomic64_t             last_tid;
	/* Last linger request ID assigned */
	u64                    last_linger_id;
	/* Tree of active linger requests */
	struct rb_root         linger_requests;
	/* Requests pending map check */
	struct rb_root         map_checks;
	/* Linger requests pending map check */
	struct rb_root         linger_map_checks;
	/* Total number of active requests */
	atomic_t               num_requests;
	/* Number of homeless (unmapped) requests */
	atomic_t               num_homeless;
	/* Error code for aborting all requests */
	int                    abort_err;
	/* Work item for request timeout processing */
	struct delayed_work    timeout_work;
	/* Work item for OSD connection timeouts */
	struct delayed_work    osds_timeout_work;
#ifdef CONFIG_DEBUG_FS
	/* Debug filesystem entry for monitoring */
	struct dentry 	       *debugfs_file;
#endif

	/* Memory pool for request allocation */
	mempool_t              *req_mempool;

	/* Message pools for efficient memory management */
	struct ceph_msgpool	msgpool_op;
	struct ceph_msgpool	msgpool_op_reply;

	/* Work queues for async processing */
	struct workqueue_struct	*notify_wq;
	struct workqueue_struct	*completion_wq;
};

static inline bool ceph_osdmap_flag(struct ceph_osd_client *osdc, int flag)
{
	return osdc->osdmap->flags & flag;
}

extern int ceph_osdc_setup(void);
extern void ceph_osdc_cleanup(void);

extern int ceph_osdc_init(struct ceph_osd_client *osdc,
			  struct ceph_client *client);
extern void ceph_osdc_stop(struct ceph_osd_client *osdc);
extern void ceph_osdc_reopen_osds(struct ceph_osd_client *osdc);

extern void ceph_osdc_handle_map(struct ceph_osd_client *osdc,
				 struct ceph_msg *msg);
void ceph_osdc_update_epoch_barrier(struct ceph_osd_client *osdc, u32 eb);
void ceph_osdc_abort_requests(struct ceph_osd_client *osdc, int err);
void ceph_osdc_clear_abort_err(struct ceph_osd_client *osdc);

#define osd_req_op_data(oreq, whch, typ, fld)				\
({									\
	struct ceph_osd_request *__oreq = (oreq);			\
	unsigned int __whch = (whch);					\
	BUG_ON(__whch >= __oreq->r_num_ops);				\
	&__oreq->r_ops[__whch].typ.fld;					\
})

struct ceph_osd_req_op *osd_req_op_init(struct ceph_osd_request *osd_req,
			    unsigned int which, u16 opcode, u32 flags);

extern void osd_req_op_raw_data_in_pages(struct ceph_osd_request *,
					unsigned int which,
					struct page **pages, u64 length,
					u32 alignment, bool pages_from_pool,
					bool own_pages);

extern void osd_req_op_extent_init(struct ceph_osd_request *osd_req,
					unsigned int which, u16 opcode,
					u64 offset, u64 length,
					u64 truncate_size, u32 truncate_seq);
extern void osd_req_op_extent_update(struct ceph_osd_request *osd_req,
					unsigned int which, u64 length);
extern void osd_req_op_extent_dup_last(struct ceph_osd_request *osd_req,
				       unsigned int which, u64 offset_inc);

extern struct ceph_osd_data *osd_req_op_extent_osd_data(
					struct ceph_osd_request *osd_req,
					unsigned int which);

extern void osd_req_op_extent_osd_data_pages(struct ceph_osd_request *,
					unsigned int which,
					struct page **pages, u64 length,
					u32 alignment, bool pages_from_pool,
					bool own_pages);
#ifdef CONFIG_BLOCK
void osd_req_op_extent_osd_data_bio(struct ceph_osd_request *osd_req,
				    unsigned int which,
				    struct ceph_bio_iter *bio_pos,
				    u32 bio_length);
#endif /* CONFIG_BLOCK */
void osd_req_op_extent_osd_data_bvecs(struct ceph_osd_request *osd_req,
				      unsigned int which,
				      struct bio_vec *bvecs, u32 num_bvecs,
				      u32 bytes);
void osd_req_op_extent_osd_data_bvec_pos(struct ceph_osd_request *osd_req,
					 unsigned int which,
					 struct ceph_bvec_iter *bvec_pos);
void osd_req_op_extent_osd_iter(struct ceph_osd_request *osd_req,
				unsigned int which, struct iov_iter *iter);

extern void osd_req_op_cls_request_data_pages(struct ceph_osd_request *,
					unsigned int which,
					struct page **pages, u64 length,
					u32 alignment, bool pages_from_pool,
					bool own_pages);
void osd_req_op_cls_request_data_bvecs(struct ceph_osd_request *osd_req,
				       unsigned int which,
				       struct bio_vec *bvecs, u32 num_bvecs,
				       u32 bytes);
extern void osd_req_op_cls_response_data_pages(struct ceph_osd_request *,
					unsigned int which,
					struct page **pages, u64 length,
					u32 alignment, bool pages_from_pool,
					bool own_pages);
int osd_req_op_cls_init(struct ceph_osd_request *osd_req, unsigned int which,
			const char *class, const char *method);
extern int osd_req_op_xattr_init(struct ceph_osd_request *osd_req, unsigned int which,
				 u16 opcode, const char *name, const void *value,
				 size_t size, u8 cmp_op, u8 cmp_mode);
extern void osd_req_op_alloc_hint_init(struct ceph_osd_request *osd_req,
				       unsigned int which,
				       u64 expected_object_size,
				       u64 expected_write_size,
				       u32 flags);
extern int osd_req_op_copy_from_init(struct ceph_osd_request *req,
				     u64 src_snapid, u64 src_version,
				     struct ceph_object_id *src_oid,
				     struct ceph_object_locator *src_oloc,
				     u32 src_fadvise_flags,
				     u32 dst_fadvise_flags,
				     u32 truncate_seq, u64 truncate_size,
				     u8 copy_from_flags);

extern struct ceph_osd_request *ceph_osdc_alloc_request(struct ceph_osd_client *osdc,
					       struct ceph_snap_context *snapc,
					       unsigned int num_ops,
					       bool use_mempool,
					       gfp_t gfp_flags);
int ceph_osdc_alloc_messages(struct ceph_osd_request *req, gfp_t gfp);

extern struct ceph_osd_request *ceph_osdc_new_request(struct ceph_osd_client *,
				      struct ceph_file_layout *layout,
				      struct ceph_vino vino,
				      u64 offset, u64 *len,
				      unsigned int which, int num_ops,
				      int opcode, int flags,
				      struct ceph_snap_context *snapc,
				      u32 truncate_seq, u64 truncate_size,
				      bool use_mempool);

int __ceph_alloc_sparse_ext_map(struct ceph_osd_req_op *op, int cnt);

/*
 * How big an extent array should we preallocate for a sparse read? This is
 * just a starting value.  If we get more than this back from the OSD, the
 * receiver will reallocate.
 */
#define CEPH_SPARSE_EXT_ARRAY_INITIAL  16

static inline int ceph_alloc_sparse_ext_map(struct ceph_osd_req_op *op, int cnt)
{
	if (!cnt)
		cnt = CEPH_SPARSE_EXT_ARRAY_INITIAL;

	return __ceph_alloc_sparse_ext_map(op, cnt);
}

extern void ceph_osdc_get_request(struct ceph_osd_request *req);
extern void ceph_osdc_put_request(struct ceph_osd_request *req);

void ceph_osdc_start_request(struct ceph_osd_client *osdc,
			     struct ceph_osd_request *req);
extern void ceph_osdc_cancel_request(struct ceph_osd_request *req);
extern int ceph_osdc_wait_request(struct ceph_osd_client *osdc,
				  struct ceph_osd_request *req);
extern void ceph_osdc_sync(struct ceph_osd_client *osdc);

extern void ceph_osdc_flush_notifies(struct ceph_osd_client *osdc);
void ceph_osdc_maybe_request_map(struct ceph_osd_client *osdc);

int ceph_osdc_call(struct ceph_osd_client *osdc,
		   struct ceph_object_id *oid,
		   struct ceph_object_locator *oloc,
		   const char *class, const char *method,
		   unsigned int flags,
		   struct page *req_page, size_t req_len,
		   struct page **resp_pages, size_t *resp_len);

/* watch/notify */
struct ceph_osd_linger_request *
ceph_osdc_watch(struct ceph_osd_client *osdc,
		struct ceph_object_id *oid,
		struct ceph_object_locator *oloc,
		rados_watchcb2_t wcb,
		rados_watcherrcb_t errcb,
		void *data);
int ceph_osdc_unwatch(struct ceph_osd_client *osdc,
		      struct ceph_osd_linger_request *lreq);

int ceph_osdc_notify_ack(struct ceph_osd_client *osdc,
			 struct ceph_object_id *oid,
			 struct ceph_object_locator *oloc,
			 u64 notify_id,
			 u64 cookie,
			 void *payload,
			 u32 payload_len);
int ceph_osdc_notify(struct ceph_osd_client *osdc,
		     struct ceph_object_id *oid,
		     struct ceph_object_locator *oloc,
		     void *payload,
		     u32 payload_len,
		     u32 timeout,
		     struct page ***preply_pages,
		     size_t *preply_len);
int ceph_osdc_list_watchers(struct ceph_osd_client *osdc,
			    struct ceph_object_id *oid,
			    struct ceph_object_locator *oloc,
			    struct ceph_watch_item **watchers,
			    u32 *num_watchers);

/* Find offset into the buffer of the end of the extent map */
static inline u64 ceph_sparse_ext_map_end(struct ceph_osd_req_op *op)
{
	struct ceph_sparse_extent *ext;

	/* No extents? No data */
	if (op->extent.sparse_ext_cnt == 0)
		return 0;

	ext = &op->extent.sparse_ext[op->extent.sparse_ext_cnt - 1];

	return ext->off + ext->len - op->extent.offset;
}

#endif
