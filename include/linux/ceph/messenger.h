/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __FS_CEPH_MESSENGER_H
#define __FS_CEPH_MESSENGER_H

#include <linux/bvec.h>
#include <linux/crypto.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/radix-tree.h>
#include <linux/uio.h>
#include <linux/workqueue.h>
#include <net/net_namespace.h>

#include <linux/ceph/types.h>
#include <linux/ceph/buffer.h>

struct ceph_msg;
struct ceph_connection;
struct ceph_msg_data_cursor;

/*
 * Connection operations metadata: Callback interface for handling network
 * connection events and message processing. Allows different Ceph subsystems
 * (monitors, OSDs, MDS) to customize connection behavior.
 */
struct ceph_connection_operations {
	/* Connection reference management */
	struct ceph_connection *(*get)(struct ceph_connection *);
	void (*put)(struct ceph_connection *);

	/* Handle incoming message dispatch to appropriate handler */
	void (*dispatch) (struct ceph_connection *con, struct ceph_msg *m);

	/* Authentication and authorization callbacks */
	/* Get authorizer for outgoing connection */
	struct ceph_auth_handshake *(*get_authorizer) (
				struct ceph_connection *con,
			       int *proto, int force_new);
	/* Handle authentication challenge */
	int (*add_authorizer_challenge)(struct ceph_connection *con,
					void *challenge_buf,
					int challenge_buf_len);
	/* Verify authorizer reply */
	int (*verify_authorizer_reply) (struct ceph_connection *con);
	/* Invalidate current authorizer */
	int (*invalidate_authorizer)(struct ceph_connection *con);

	/* Network error handling */
	/* Socket error occurred (disconnect, etc.) */
	void (*fault) (struct ceph_connection *con);

	/* Peer reset connection, potential message loss */
	/* a remote host as terminated a message exchange session, and messages
	 * we sent (or they tried to send us) may be lost. */
	void (*peer_reset) (struct ceph_connection *con);

	/* Message allocation and processing */
	/* Allocate message for incoming data */
	struct ceph_msg * (*alloc_msg) (struct ceph_connection *con,
					struct ceph_msg_header *hdr,
					int *skip);

	/* Re-encode message for retransmission */
	void (*reencode_message) (struct ceph_msg *msg);

	/* Message signing and verification */
	int (*sign_message) (struct ceph_msg *msg);
	int (*check_message_signature) (struct ceph_msg *msg);

	/* Messenger v2 authentication exchange */
	/* Get initial authentication request */
	int (*get_auth_request)(struct ceph_connection *con,
				void *buf, int *buf_len,
				void **authorizer, int *authorizer_len);
	/* Handle multi-round authentication */
	int (*handle_auth_reply_more)(struct ceph_connection *con,
				      void *reply, int reply_len,
				      void *buf, int *buf_len,
				      void **authorizer, int *authorizer_len);
	/* Authentication completed successfully */
	int (*handle_auth_done)(struct ceph_connection *con,
				u64 global_id, void *reply, int reply_len,
				u8 *session_key, int *session_key_len,
				u8 *con_secret, int *con_secret_len);
	/* Authentication failed with unsupported method */
	int (*handle_auth_bad_method)(struct ceph_connection *con,
				      int used_proto, int result,
				      const int *allowed_protos, int proto_cnt,
				      const int *allowed_modes, int mode_cnt);

	/**
	 * sparse_read: read sparse data
	 * @con: connection we're reading from
	 * @cursor: data cursor for reading extents
	 * @buf: optional buffer to read into
	 *
	 * This should be called more than once, each time setting up to
	 * receive an extent into the current cursor position, and zeroing
	 * the holes between them.
	 *
	 * Returns amount of data to be read (in bytes), 0 if reading is
	 * complete, or -errno if there was an error.
	 *
	 * If @buf is set on a >0 return, then the data should be read into
	 * the provided buffer. Otherwise, it should be read into the cursor.
	 *
	 * The sparse read operation is expected to initialize the cursor
	 * with a length covering up to the end of the last extent.
	 */
	int (*sparse_read)(struct ceph_connection *con,
			   struct ceph_msg_data_cursor *cursor,
			   char **buf);

};

/* use format string %s%lld */
#define ENTITY_NAME(n) ceph_entity_type_name((n).type), le64_to_cpu((n).num)

/*
 * Ceph messenger metadata: Core messaging infrastructure that manages network
 * connections and message routing. Handles connection multiplexing, sequencing,
 * and provides the foundation for all Ceph network communication.
 */
struct ceph_messenger {
	/* Messenger identity (entity name + network address) */
	struct ceph_entity_inst inst;
	/* Encoded network address for this messenger */
	struct ceph_entity_addr my_enc_addr;

	/* Shutdown coordination */
	atomic_t stopping;
	/* Network namespace for socket operations */
	possible_net_t net;

	/*
	 * Global connection sequence number for race disambiguation:
	 * the global_seq counts connections i (attempt to) initiate
	 * in order to disambiguate certain connect race conditions.
	 */
	u32 global_seq;
	/* Protects global_seq updates */
	spinlock_t global_seq_lock;
};

/*
 * Message data container types: Defines different ways message payload data
 * can be stored and transmitted. Supports various kernel memory management
 * patterns for efficient zero-copy I/O operations.
 */
enum ceph_msg_data_type {
	CEPH_MSG_DATA_NONE,	/* message contains no data payload */
	CEPH_MSG_DATA_PAGES,	/* data source/destination is a page array */
	CEPH_MSG_DATA_PAGELIST,	/* data source/destination is a pagelist */
#ifdef CONFIG_BLOCK
	CEPH_MSG_DATA_BIO,	/* data source/destination is a bio list */
#endif /* CONFIG_BLOCK */
	CEPH_MSG_DATA_BVECS,	/* data source/destination is a bio_vec array */
	CEPH_MSG_DATA_ITER,	/* data source/destination is an iov_iter */
};

#ifdef CONFIG_BLOCK

/*
 * Bio iterator metadata: Tracks position within a chain of bio structures
 * for efficient traversal during network I/O operations. Maintains current
 * bio and position within that bio's vector array.
 */
struct ceph_bio_iter {
	/* Current bio in the chain */
	struct bio *bio;
	/* Current position within the bio */
	struct bvec_iter iter;
};

#define __ceph_bio_iter_advance_step(it, n, STEP) do {			      \
	unsigned int __n = (n), __cur_n;				      \
									      \
	while (__n) {							      \
		BUG_ON(!(it)->iter.bi_size);				      \
		__cur_n = min((it)->iter.bi_size, __n);			      \
		(void)(STEP);						      \
		bio_advance_iter((it)->bio, &(it)->iter, __cur_n);	      \
		if (!(it)->iter.bi_size && (it)->bio->bi_next) {	      \
			dout("__ceph_bio_iter_advance_step next bio\n");      \
			(it)->bio = (it)->bio->bi_next;			      \
			(it)->iter = (it)->bio->bi_iter;		      \
		}							      \
		__n -= __cur_n;						      \
	}								      \
} while (0)

/*
 * Advance @it by @n bytes.
 */
#define ceph_bio_iter_advance(it, n)					      \
	__ceph_bio_iter_advance_step(it, n, 0)

/*
 * Advance @it by @n bytes, executing BVEC_STEP for each bio_vec.
 */
#define ceph_bio_iter_advance_step(it, n, BVEC_STEP)			      \
	__ceph_bio_iter_advance_step(it, n, ({				      \
		struct bio_vec bv;					      \
		struct bvec_iter __cur_iter;				      \
									      \
		__cur_iter = (it)->iter;				      \
		__cur_iter.bi_size = __cur_n;				      \
		__bio_for_each_segment(bv, (it)->bio, __cur_iter, __cur_iter) \
			(void)(BVEC_STEP);				      \
	}))

#endif /* CONFIG_BLOCK */

/*
 * Bio vector iterator metadata: Tracks position within an array of bio_vec
 * structures for efficient memory region traversal during network operations.
 * Provides unified interface for vectored I/O.
 */
struct ceph_bvec_iter {
	/* Array of bio vectors */
	struct bio_vec *bvecs;
	/* Current position within the array */
	struct bvec_iter iter;
};

#define __ceph_bvec_iter_advance_step(it, n, STEP) do {			      \
	BUG_ON((n) > (it)->iter.bi_size);				      \
	(void)(STEP);							      \
	bvec_iter_advance((it)->bvecs, &(it)->iter, (n));		      \
} while (0)

/*
 * Advance @it by @n bytes.
 */
#define ceph_bvec_iter_advance(it, n)					      \
	__ceph_bvec_iter_advance_step(it, n, 0)

/*
 * Advance @it by @n bytes, executing BVEC_STEP for each bio_vec.
 */
#define ceph_bvec_iter_advance_step(it, n, BVEC_STEP)			      \
	__ceph_bvec_iter_advance_step(it, n, ({				      \
		struct bio_vec bv;					      \
		struct bvec_iter __cur_iter;				      \
									      \
		__cur_iter = (it)->iter;				      \
		__cur_iter.bi_size = (n);				      \
		for_each_bvec(bv, (it)->bvecs, __cur_iter, __cur_iter)	      \
			(void)(BVEC_STEP);				      \
	}))

#define ceph_bvec_iter_shorten(it, n) do {				      \
	BUG_ON((n) > (it)->iter.bi_size);				      \
	(it)->iter.bi_size = (n);					      \
} while (0)

/*
 * Message data container metadata: Flexible container for message payload data
 * using different storage methods. Supports various kernel memory management
 * patterns for efficient zero-copy network operations.
 */
struct ceph_msg_data {
	/* Type of data container */
	enum ceph_msg_data_type		type;
	/* Type-specific data container */
	union {
#ifdef CONFIG_BLOCK
		/* Block I/O bio container */
		struct {
			/* Bio chain position and iterator */
			struct ceph_bio_iter	bio_pos;
			/* Total data length in bio chain */
			u32			bio_length;
		};
#endif /* CONFIG_BLOCK */
		/* Bio vector array container */
		struct ceph_bvec_iter	bvec_pos;
		/* Page array container */
		struct {
			/* Array of page pointers */
			struct page	**pages;
			/* Total data length in bytes */
			size_t		length;
			/* Alignment offset in first page */
			unsigned int	alignment;
			/* Whether we own and must free the pages */
			bool		own_pages;
		};
		/* Pagelist container */
		struct ceph_pagelist	*pagelist;
		/* Generic iterator container */
		struct iov_iter		iter;
	};
};

/*
 * Message data cursor metadata: Tracks progress through message data during
 * network transmission or reception. Maintains position across multiple data
 * items and handles different container types efficiently.
 */
struct ceph_msg_data_cursor {
	/* Total bytes remaining across all data items */
	size_t			total_resid;

	/* Current data item being processed */
	struct ceph_msg_data	*data;
	/* Bytes remaining in current data item */
	size_t			resid;
	/* Residual sparse read length */
	int			sr_resid;
	/* Whether CRC calculation is needed */
	bool			need_crc;
	/* Type-specific position tracking */
	union {
#ifdef CONFIG_BLOCK
		/* Bio iterator for block I/O */
		struct ceph_bio_iter	bio_iter;
#endif /* CONFIG_BLOCK */
		/* Bio vector iterator */
		struct bvec_iter	bvec_iter;
		/* Page array position tracking */
		struct {
			/* Byte offset within current page */
			unsigned int	page_offset;
			/* Current page index in array */
			unsigned short	page_index;
			/* Total pages in array */
			unsigned short	page_count;
		};
		/* Pagelist position tracking */
		struct {
			/* Current page from pagelist */
			struct page	*page;
			/* Byte offset from start of pagelist */
			size_t		offset;
		};
		/* Iterator position tracking */
		struct {
			/* Current iov_iter state */
			struct iov_iter		iov_iter;
			/* Length of last operation */
			unsigned int		lastlen;
		};
	};
};

/*
 * Ceph network message metadata: Complete network message structure containing
 * header, payload data, and transmission state. Supports various data container
 * types for efficient zero-copy operations and handles message lifecycle.
 *
 * a single message.  it contains a header (src, dest, message type, etc.),
 * footer (crc values, mainly), a "front" message body, and possibly a
 * data payload (stored in some number of pages).
 */
struct ceph_msg {
	/* Message header with routing and type information */
	struct ceph_msg_header hdr;
	/* Message footer with CRC and other metadata */
	union {
		/* Current footer format */
		struct ceph_msg_footer footer;
		/* Legacy footer format for compatibility */
		struct ceph_msg_footer_old old_footer;
	};
	/* Front payload (small, inline message data) */
	struct kvec front;
	/* Middle payload (optional, variable-length) */
	struct ceph_buffer *middle;

	/* Data payload information */
	/* Total length of all data items */
	size_t				data_length;
	/* Array of data containers */
	struct ceph_msg_data		*data;
	/* Current number of data items */
	int				num_data_items;
	/* Maximum data items allocated */
	int				max_data_items;
	/* Current position for data processing */
	struct ceph_msg_data_cursor	cursor;

	/* Connection and list management */
	/* Connection this message belongs to */
	struct ceph_connection *con;
	/* Linkage for connection message queues */
	struct list_head list_head;

	/* Message lifecycle and state */
	/* Reference counting for safe cleanup */
	struct kref kref;
	/* More messages follow this one */
	bool more_to_follow;
	/* Message needs output sequence number */
	bool needs_out_seq;
	/* Total bytes expected for sparse read */
	u64 sparse_read_total;
	/* Allocated length of front buffer */
	int front_alloc_len;

	/* Memory pool this message came from */
	struct ceph_msgpool *pool;
};

/*
 * connection states
 */
#define CEPH_CON_S_CLOSED		1
#define CEPH_CON_S_PREOPEN		2
#define CEPH_CON_S_V1_BANNER		3
#define CEPH_CON_S_V1_CONNECT_MSG	4
#define CEPH_CON_S_V2_BANNER_PREFIX	5
#define CEPH_CON_S_V2_BANNER_PAYLOAD	6
#define CEPH_CON_S_V2_HELLO		7
#define CEPH_CON_S_V2_AUTH		8
#define CEPH_CON_S_V2_AUTH_SIGNATURE	9
#define CEPH_CON_S_V2_SESSION_CONNECT	10
#define CEPH_CON_S_V2_SESSION_RECONNECT	11
#define CEPH_CON_S_OPEN			12
#define CEPH_CON_S_STANDBY		13

/*
 * ceph_connection flag bits
 */
#define CEPH_CON_F_LOSSYTX		0  /* we can close channel or drop
					      messages on errors */
#define CEPH_CON_F_KEEPALIVE_PENDING	1  /* we need to send a keepalive */
#define CEPH_CON_F_WRITE_PENDING	2  /* we have data ready to send */
#define CEPH_CON_F_SOCK_CLOSED		3  /* socket state changed to closed */
#define CEPH_CON_F_BACKOFF		4  /* need to retry queuing delayed
					      work */

/* ceph connection fault delay defaults, for exponential backoff */
#define BASE_DELAY_INTERVAL	(HZ / 4)
#define MAX_DELAY_INTERVAL	(15 * HZ)

/*
 * Messenger v1 connection info metadata: Protocol-specific state for messenger
 * version 1 connections. Handles legacy wire protocol, authentication handshake,
 * and connection negotiation with older Ceph versions.
 */
struct ceph_connection_v1_info {
	/* Output vector management for sending */
	/* Array of vectors for header/footer data */
	struct kvec out_kvec[8], *out_kvec_cur;
	/* Number of kvecs remaining to send */
	int out_kvec_left;
	/* Bytes to skip in current operation */
	int out_skip;
	/* Total bytes left in kvec array */
	int out_kvec_bytes;
	/* More data follows the kvecs */
	bool out_more;
	/* Current message send complete */
	bool out_msg_done;

	/* Authentication state */
	/* Current authentication handshake */
	struct ceph_auth_handshake *auth;
	/* Need to retry authentication */
	int auth_retry;       /* true if we need a newer authorizer */

	/* Connection negotiation temporary storage */
	/* Received protocol banner */
	u8 in_banner[CEPH_BANNER_MAX_LEN];
	/* Peer's actual network address */
	struct ceph_entity_addr actual_peer_addr;
	/* Address peer should use for us */
	struct ceph_entity_addr peer_addr_for_me;
	/* Our outgoing connection message */
	struct ceph_msg_connect out_connect;
	/* Peer's connection reply */
	struct ceph_msg_connect_reply in_reply;

	/* Input stream position tracking */
	int in_base_pos;     /* bytes read */

	/* Sparse read support */
	/* Current receive vector for sparse data */
	struct kvec in_sr_kvec; /* current location to receive into */
	/* Length of current sparse extent */
	u64 in_sr_len;		/* amount of data in this extent */

	/* Incoming message temporary storage */
	/* Protocol control byte */
	u8 in_tag;
	/* Incoming message header */
	struct ceph_msg_header in_hdr;
	/* Temporary acknowledgment storage */
	__le64 in_temp_ack;

	/* Outgoing message temporary storage */
	/* Outgoing message header */
	struct ceph_msg_header out_hdr;
	/* Temporary acknowledgment for sending */
	__le64 out_temp_ack;
	/* Keepalive timestamp */
	struct ceph_timespec out_temp_keepalive2;

	/* Connection sequence tracking */
	/* Our connection attempt sequence */
	u32 connect_seq;      /* identify the most recent connection
				 attempt for this session */
	/* Peer's global sequence for this connection */
	u32 peer_global_seq;
};

#define CEPH_CRC_LEN			4
#define CEPH_GCM_KEY_LEN		16
#define CEPH_GCM_IV_LEN			sizeof(struct ceph_gcm_nonce)
#define CEPH_GCM_BLOCK_LEN		16
#define CEPH_GCM_TAG_LEN		16

#define CEPH_PREAMBLE_LEN		32
#define CEPH_PREAMBLE_INLINE_LEN	48
#define CEPH_PREAMBLE_PLAIN_LEN		CEPH_PREAMBLE_LEN
#define CEPH_PREAMBLE_SECURE_LEN	(CEPH_PREAMBLE_LEN +		\
					 CEPH_PREAMBLE_INLINE_LEN +	\
					 CEPH_GCM_TAG_LEN)
#define CEPH_EPILOGUE_PLAIN_LEN		(1 + 3 * CEPH_CRC_LEN)
#define CEPH_EPILOGUE_SECURE_LEN	(CEPH_GCM_BLOCK_LEN + CEPH_GCM_TAG_LEN)

#define CEPH_FRAME_MAX_SEGMENT_COUNT	4

/*
 * Frame descriptor metadata: Describes the structure of a messenger v2 frame
 * including segment count, lengths, and alignment requirements. Used for
 * efficient frame parsing and construction.
 */
struct ceph_frame_desc {
	/* Frame type tag identifier */
	int fd_tag;
	/* Number of segments in this frame */
	int fd_seg_cnt;
	/* Logical length of each segment */
	int fd_lens[CEPH_FRAME_MAX_SEGMENT_COUNT];
	/* Alignment requirements for each segment */
	int fd_aligns[CEPH_FRAME_MAX_SEGMENT_COUNT];
};

/*
 * GCM nonce metadata: Nonce structure for AES-GCM encryption in messenger v2.
 * Combines a fixed portion with an incrementing counter for cryptographic
 * uniqueness across encrypted frames.
 */
struct ceph_gcm_nonce {
	/* Fixed portion of the nonce */
	__le32 fixed;
	/* Incrementing counter portion */
	__le64 counter __packed;
};

/*
 * Ceph connection version 2 protocol state
 *
 * Contains all state information specific to the Ceph messenger v2 protocol.
 * Version 2 adds significant security enhancements including on-wire encryption,
 * authentication signatures, and secure session establishment over v1.
 */
struct ceph_connection_v2_info {
	/* Incoming message processing state */
	struct iov_iter in_iter;                /* iterator for incoming data */
	struct kvec in_kvecs[5];                /* scatter-gather vectors for recvmsg */
	struct bio_vec in_bvec;                 /* bio vector for cursor-based receives */
	int in_kvec_cnt;                        /* number of active input kvecs */
	int in_state;                           /* current input state (IN_S_*) */

	/* Outgoing message processing state */
	struct iov_iter out_iter;               /* iterator for outgoing data */
	struct kvec out_kvecs[8];               /* scatter-gather vectors for sendmsg */
	struct bio_vec out_bvec;                /* bio vector for cursor/zero sends */
	int out_kvec_cnt;                       /* number of active output kvecs */
	int out_state;                          /* current output state (OUT_S_*) */

	int out_zero;                           /* number of zero bytes to send */
	bool out_iter_sendpage;                 /* use sendpage optimization when possible */

	/* Message data handling */
	struct ceph_frame_desc in_desc;         /* incoming frame descriptor */
	struct ceph_msg_data_cursor in_cursor;  /* cursor for incoming message data */
	struct ceph_msg_data_cursor out_cursor; /* cursor for outgoing message data */

	/* Cryptographic subsystem for v2 security features */
	struct crypto_shash *hmac_tfm;          /* HMAC transform for post-auth signatures */
	struct crypto_aead *gcm_tfm;            /* AES-GCM transform for wire encryption */
	struct aead_request *gcm_req;           /* GCM request structure */
	struct crypto_wait gcm_wait;            /* wait queue for crypto operations */
	struct ceph_gcm_nonce in_gcm_nonce;     /* GCM nonce for incoming data */
	struct ceph_gcm_nonce out_gcm_nonce;    /* GCM nonce for outgoing data */

	/* Encrypted data page management */
	struct page **in_enc_pages;             /* pages for incoming encrypted data */
	int in_enc_page_cnt;                    /* number of incoming encrypted pages */
	int in_enc_resid;                       /* remaining bytes in incoming encryption */
	int in_enc_i;                           /* current incoming encryption page index */
	struct page **out_enc_pages;            /* pages for outgoing encrypted data */
	int out_enc_page_cnt;                   /* number of outgoing encrypted pages */
	int out_enc_resid;                      /* remaining bytes in outgoing encryption */
	int out_enc_i;                          /* current outgoing encryption page index */

	int con_mode;                           /* connection mode (CEPH_CON_MODE_*) */

	/* Connection buffer management */
	void *conn_bufs[16];                    /* connection-specific buffers */
	int conn_buf_cnt;                       /* number of active connection buffers */
	int data_len_remain;                    /* remaining data length to process */

	/* Signature generation vectors */
	struct kvec in_sign_kvecs[8];           /* vectors for incoming signature calculation */
	struct kvec out_sign_kvecs[8];          /* vectors for outgoing signature calculation */
	int in_sign_kvec_cnt;                   /* number of incoming signature vectors */
	int out_sign_kvec_cnt;                  /* number of outgoing signature vectors */

	/* Session and sequence management */
	u64 client_cookie;                      /* client session cookie */
	u64 server_cookie;                      /* server session cookie */
	u64 global_seq;                         /* global sequence number */
	u64 connect_seq;                        /* connection sequence number */
	u64 peer_global_seq;                    /* peer's global sequence number */

	/* Protocol buffers */
	u8 in_buf[CEPH_PREAMBLE_SECURE_LEN];    /* buffer for incoming preambles */
	u8 out_buf[CEPH_PREAMBLE_SECURE_LEN];   /* buffer for outgoing preambles */

	/* Frame epilogue for integrity checking */
	struct {
		u8 late_status;                 /* late frame status (FRAME_LATE_STATUS_*) */
		union {
			struct {
				u32 front_crc;  /* CRC of front portion */
				u32 middle_crc; /* CRC of middle portion */
				u32 data_crc;   /* CRC of data portion */
			} __packed;
			u8 pad[CEPH_GCM_BLOCK_LEN - 1]; /* padding for GCM alignment */
		};
	} out_epil;
};

/*
 * Ceph network connection metadata: Represents a single network connection
 * to another Ceph entity (monitor, OSD, MDS). Manages message queuing,
 * sequencing, authentication, and connection state to ensure reliable,
 * ordered message delivery even across network interruptions.
 *
 * A single connection with another host.
 *
 * We maintain a queue of outgoing messages, and some session state to
 * ensure that we can preserve the lossless, ordered delivery of
 * messages in the case of a TCP disconnect.
 */
struct ceph_connection {
	/* Private data for connection owner (mon_client, osd_client, etc.) */
	void *private;

	/* Callback operations for connection events */
	const struct ceph_connection_operations *ops;

	/* Parent messenger instance */
	struct ceph_messenger *msgr;

	/* Connection state management */
	/* Current protocol state */
	int state;
	/* Atomic socket state */
	atomic_t sock_state;
	/* Network socket */
	struct socket *sock;

	/* Connection flags and error state */
	unsigned long flags;
	/* Human-readable error message */
	const char *error_msg;

	/* Peer identification */
	/* Peer entity name (type + ID) */
	struct ceph_entity_name peer_name;
	/* Peer network address */
	struct ceph_entity_addr peer_addr;
	/* Feature flags supported by peer */
	u64 peer_features;

	/* Connection serialization */
	struct mutex mutex;

	/* Message queue management */
	/* Queue of messages waiting to be sent */
	struct list_head out_queue;
	/* Messages sent but not yet acknowledged */
	struct list_head out_sent;
	/* Last message sequence number queued */
	u64 out_seq;

	/* Sequence number tracking for reliable delivery */
	/* Last message received, last message acknowledged */
	u64 in_seq, in_seq_acked;

	/* Current message processing */
	/* Message currently being received */
	struct ceph_msg *in_msg;
	/* Message currently being sent */
	struct ceph_msg *out_msg;        /* sending message (== tail of
					    out_sent) */

	/* I/O processing support */
	/* Temporary page for small I/O operations */
	struct page *bounce_page;
	/* CRC values for incoming message verification */
	u32 in_front_crc, in_middle_crc, in_data_crc;

	/* Keepalive and connection health */
	/* Timestamp of last keepalive acknowledgment */
	struct timespec64 last_keepalive_ack;

	/* Work queue processing */
	/* Delayed work for send/receive operations */
	struct delayed_work work;
	/* Current exponential backoff delay */
	unsigned long       delay;          /* current delay interval */

	/* Protocol version-specific information */
	union {
		/* Messenger v1 protocol state */
		struct ceph_connection_v1_info v1;
		/* Messenger v2 protocol state */
		struct ceph_connection_v2_info v2;
	};
};

extern struct page *ceph_zero_page;

void ceph_con_flag_clear(struct ceph_connection *con, unsigned long con_flag);
void ceph_con_flag_set(struct ceph_connection *con, unsigned long con_flag);
bool ceph_con_flag_test(struct ceph_connection *con, unsigned long con_flag);
bool ceph_con_flag_test_and_clear(struct ceph_connection *con,
				  unsigned long con_flag);
bool ceph_con_flag_test_and_set(struct ceph_connection *con,
				unsigned long con_flag);

void ceph_encode_my_addr(struct ceph_messenger *msgr);

int ceph_tcp_connect(struct ceph_connection *con);
int ceph_con_close_socket(struct ceph_connection *con);
void ceph_con_reset_session(struct ceph_connection *con);

u32 ceph_get_global_seq(struct ceph_messenger *msgr, u32 gt);
void ceph_con_discard_sent(struct ceph_connection *con, u64 ack_seq);
void ceph_con_discard_requeued(struct ceph_connection *con, u64 reconnect_seq);

void ceph_msg_data_cursor_init(struct ceph_msg_data_cursor *cursor,
			       struct ceph_msg *msg, size_t length);
struct page *ceph_msg_data_next(struct ceph_msg_data_cursor *cursor,
				size_t *page_offset, size_t *length);
void ceph_msg_data_advance(struct ceph_msg_data_cursor *cursor, size_t bytes);

u32 ceph_crc32c_page(u32 crc, struct page *page, unsigned int page_offset,
		     unsigned int length);

bool ceph_addr_is_blank(const struct ceph_entity_addr *addr);
int ceph_addr_port(const struct ceph_entity_addr *addr);
void ceph_addr_set_port(struct ceph_entity_addr *addr, int p);

void ceph_con_process_message(struct ceph_connection *con);
int ceph_con_in_msg_alloc(struct ceph_connection *con,
			  struct ceph_msg_header *hdr, int *skip);
void ceph_con_get_out_msg(struct ceph_connection *con);

/* messenger_v1.c */
int ceph_con_v1_try_read(struct ceph_connection *con);
int ceph_con_v1_try_write(struct ceph_connection *con);
void ceph_con_v1_revoke(struct ceph_connection *con);
void ceph_con_v1_revoke_incoming(struct ceph_connection *con);
bool ceph_con_v1_opened(struct ceph_connection *con);
void ceph_con_v1_reset_session(struct ceph_connection *con);
void ceph_con_v1_reset_protocol(struct ceph_connection *con);

/* messenger_v2.c */
int ceph_con_v2_try_read(struct ceph_connection *con);
int ceph_con_v2_try_write(struct ceph_connection *con);
void ceph_con_v2_revoke(struct ceph_connection *con);
void ceph_con_v2_revoke_incoming(struct ceph_connection *con);
bool ceph_con_v2_opened(struct ceph_connection *con);
void ceph_con_v2_reset_session(struct ceph_connection *con);
void ceph_con_v2_reset_protocol(struct ceph_connection *con);


extern const char *ceph_pr_addr(const struct ceph_entity_addr *addr);

extern int ceph_parse_ips(const char *c, const char *end,
			  struct ceph_entity_addr *addr,
			  int max_count, int *count, char delim);

extern int ceph_msgr_init(void);
extern void ceph_msgr_exit(void);
extern void ceph_msgr_flush(void);

extern void ceph_messenger_init(struct ceph_messenger *msgr,
				struct ceph_entity_addr *myaddr);
extern void ceph_messenger_fini(struct ceph_messenger *msgr);
extern void ceph_messenger_reset_nonce(struct ceph_messenger *msgr);

extern void ceph_con_init(struct ceph_connection *con, void *private,
			const struct ceph_connection_operations *ops,
			struct ceph_messenger *msgr);
extern void ceph_con_open(struct ceph_connection *con,
			  __u8 entity_type, __u64 entity_num,
			  struct ceph_entity_addr *addr);
extern bool ceph_con_opened(struct ceph_connection *con);
extern void ceph_con_close(struct ceph_connection *con);
extern void ceph_con_send(struct ceph_connection *con, struct ceph_msg *msg);

extern void ceph_msg_revoke(struct ceph_msg *msg);
extern void ceph_msg_revoke_incoming(struct ceph_msg *msg);

extern void ceph_con_keepalive(struct ceph_connection *con);
extern bool ceph_con_keepalive_expired(struct ceph_connection *con,
				       unsigned long interval);

void ceph_msg_data_add_pages(struct ceph_msg *msg, struct page **pages,
			     size_t length, size_t alignment, bool own_pages);
extern void ceph_msg_data_add_pagelist(struct ceph_msg *msg,
				struct ceph_pagelist *pagelist);
#ifdef CONFIG_BLOCK
void ceph_msg_data_add_bio(struct ceph_msg *msg, struct ceph_bio_iter *bio_pos,
			   u32 length);
#endif /* CONFIG_BLOCK */
void ceph_msg_data_add_bvecs(struct ceph_msg *msg,
			     struct ceph_bvec_iter *bvec_pos);
void ceph_msg_data_add_iter(struct ceph_msg *msg,
			    struct iov_iter *iter);

struct ceph_msg *ceph_msg_new2(int type, int front_len, int max_data_items,
			       gfp_t flags, bool can_fail);
extern struct ceph_msg *ceph_msg_new(int type, int front_len, gfp_t flags,
				     bool can_fail);

extern struct ceph_msg *ceph_msg_get(struct ceph_msg *msg);
extern void ceph_msg_put(struct ceph_msg *msg);

extern void ceph_msg_dump(struct ceph_msg *msg);

#endif
