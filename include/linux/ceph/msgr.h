/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CEPH_MSGR_H
#define CEPH_MSGR_H

/*
 * Data types for message passing layer used by Ceph.
 */

#define CEPH_MON_PORT    6789  /* default monitor port */

/*
 * tcp connection banner.  include a protocol version. and adjust
 * whenever the wire protocol changes.  try to keep this string length
 * constant.
 */
#define CEPH_BANNER "ceph v027"
#define CEPH_BANNER_LEN 9
#define CEPH_BANNER_MAX_LEN 30


/*
 * messenger V2 connection banner prefix.
 * The full banner string should have the form: "ceph v2\n<le16>"
 * the 2 bytes are the length of the remaining banner.
 */
#define CEPH_BANNER_V2 "ceph v2\n"
#define CEPH_BANNER_V2_LEN 8
#define CEPH_BANNER_V2_PREFIX_LEN (CEPH_BANNER_V2_LEN + sizeof(__le16))

/*
 * messenger V2 features
 */
#define CEPH_MSGR2_INCARNATION_1 (0ull)

#define DEFINE_MSGR2_FEATURE(bit, incarnation, name)               \
	static const uint64_t __maybe_unused CEPH_MSGR2_FEATURE_##name = (1ULL << bit); \
	static const uint64_t __maybe_unused CEPH_MSGR2_FEATUREMASK_##name =            \
			(1ULL << bit | CEPH_MSGR2_INCARNATION_##incarnation);

#define HAVE_MSGR2_FEATURE(x, name) \
	(((x) & (CEPH_MSGR2_FEATUREMASK_##name)) == (CEPH_MSGR2_FEATUREMASK_##name))

DEFINE_MSGR2_FEATURE( 0, 1, REVISION_1)   // msgr2.1

#define CEPH_MSGR2_SUPPORTED_FEATURES (CEPH_MSGR2_FEATURE_REVISION_1)

#define CEPH_MSGR2_REQUIRED_FEATURES  (CEPH_MSGR2_FEATURE_REVISION_1)


/*
 * Rollover-safe type and comparator for 32-bit sequence numbers.
 * Comparator returns -1, 0, or 1.
 */
typedef __u32 ceph_seq_t;

static inline __s32 ceph_seq_cmp(__u32 a, __u32 b)
{
       return (__s32)a - (__s32)b;
}


/*
 * Entity name metadata: Logical identifier for a Ceph process participating
 * in the cluster network. Combines entity type (monitor, OSD, MDS, client)
 * with a unique number to create globally unique process identifiers.
 *
 * entity_name -- logical name for a process participating in the
 * network, e.g. 'mds0' or 'osd3'.
 */
struct ceph_entity_name {
	/* Entity type (monitor, OSD, MDS, client, etc.) */
	__u8 type;      /* CEPH_ENTITY_TYPE_* */
	/* Unique number within the entity type */
	__le64 num;
} __attribute__ ((packed));

#define CEPH_ENTITY_TYPE_MON    0x01
#define CEPH_ENTITY_TYPE_MDS    0x02
#define CEPH_ENTITY_TYPE_OSD    0x04
#define CEPH_ENTITY_TYPE_CLIENT 0x08
#define CEPH_ENTITY_TYPE_AUTH   0x20

#define CEPH_ENTITY_TYPE_ANY    0xFF

extern const char *ceph_entity_type_name(int type);

/*
 * Entity address metadata: Network address information for a Ceph entity.
 * Contains address type, process nonce for disambiguation, and the actual
 * network socket address for establishing connections.
 */
struct ceph_entity_addr {
	/* Address type identifier */
	__le32 type;  /* CEPH_ENTITY_ADDR_TYPE_* */
	/* Unique process identifier (typically PID) */
	__le32 nonce;
	/* Socket address (IPv4/IPv6) */
	struct sockaddr_storage in_addr;
} __attribute__ ((packed));

static inline bool ceph_addr_equal_no_type(const struct ceph_entity_addr *lhs,
					   const struct ceph_entity_addr *rhs)
{
	return !memcmp(&lhs->in_addr, &rhs->in_addr, sizeof(lhs->in_addr)) &&
	       lhs->nonce == rhs->nonce;
}

/*
 * Entity instance metadata: Complete identification of a Ceph entity
 * combining logical name with network address. Uniquely identifies
 * a specific process instance in the cluster.
 */
struct ceph_entity_inst {
	/* Logical entity name (type + number) */
	struct ceph_entity_name name;
	/* Network address for this entity */
	struct ceph_entity_addr addr;
} __attribute__ ((packed));


/* used by message exchange protocol */
#define CEPH_MSGR_TAG_READY         1  /* server->client: ready for messages */
#define CEPH_MSGR_TAG_RESETSESSION  2  /* server->client: reset, try again */
#define CEPH_MSGR_TAG_WAIT          3  /* server->client: wait for racing
					  incoming connection */
#define CEPH_MSGR_TAG_RETRY_SESSION 4  /* server->client + cseq: try again
					  with higher cseq */
#define CEPH_MSGR_TAG_RETRY_GLOBAL  5  /* server->client + gseq: try again
					  with higher gseq */
#define CEPH_MSGR_TAG_CLOSE         6  /* closing pipe */
#define CEPH_MSGR_TAG_MSG           7  /* message */
#define CEPH_MSGR_TAG_ACK           8  /* message ack */
#define CEPH_MSGR_TAG_KEEPALIVE     9  /* just a keepalive byte! */
#define CEPH_MSGR_TAG_BADPROTOVER   10 /* bad protocol version */
#define CEPH_MSGR_TAG_BADAUTHORIZER 11 /* bad authorizer */
#define CEPH_MSGR_TAG_FEATURES      12 /* insufficient features */
#define CEPH_MSGR_TAG_SEQ           13 /* 64-bit int follows with seen seq number */
#define CEPH_MSGR_TAG_KEEPALIVE2    14 /* keepalive2 byte + ceph_timespec */
#define CEPH_MSGR_TAG_KEEPALIVE2_ACK 15 /* keepalive2 reply */
#define CEPH_MSGR_TAG_CHALLENGE_AUTHORIZER 16  /* cephx v2 doing server challenge */

/*
 * Connection negotiation request metadata: Initial message sent to establish
 * a connection with another Ceph entity. Contains feature negotiation,
 * authentication details, and connection sequencing information.
 */
struct ceph_msg_connect {
	/* Feature flags supported by this client */
	__le64 features;
	/* Entity type of the connecting host */
	__le32 host_type;    /* CEPH_ENTITY_TYPE_* */
	/* Global connection sequence (across all sessions) */
	__le32 global_seq;
	/* Connection sequence within current session */
	__le32 connect_seq;  /* count connections initiated in this session */
	/* Wire protocol version */
	__le32 protocol_version;
	/* Authentication protocol identifier */
	__le32 authorizer_protocol;
	/* Length of authentication data */
	__le32 authorizer_len;
	/* Connection flags (lossy, etc.) */
	__u8  flags;         /* CEPH_MSG_CONNECT_* */
} __attribute__ ((packed));

/*
 * Connection negotiation reply metadata: Response to connection request
 * indicating whether connection was accepted, feature set negotiated,
 * and any authentication challenges or requirements.
 */
struct ceph_msg_connect_reply {
	/* Reply tag (ready, retry, error, etc.) */
	__u8 tag;
	/* Feature flags enabled for this session */
	__le64 features;
	/* Server's global sequence number */
	__le32 global_seq;
	/* Server's connection sequence number */
	__le32 connect_seq;
	/* Negotiated protocol version */
	__le32 protocol_version;
	/* Length of authorization challenge data */
	__le32 authorizer_len;
	/* Connection reply flags */
	__u8 flags;
} __attribute__ ((packed));

#define CEPH_MSG_CONNECT_LOSSY  1  /* messages i send may be safely dropped */


/*
 * Legacy message header metadata: Original wire format for message headers
 * in older Ceph versions. Contains complete routing information, payload
 * lengths, and both source and original source for message forwarding.
 */
struct ceph_msg_header_old {
	/* Message sequence number for this session */
	__le64 seq;
	/* Transaction identifier for request/reply correlation */
	__le64 tid;
	/* Message type identifier */
	__le16 type;
	/* Message priority (higher value = higher priority) */
	__le16 priority;
	/* Version of message encoding/format */
	__le16 version;

	/* Payload section lengths */
	/* Length of front payload section */
	__le32 front_len; /* bytes in main payload */
	/* Length of middle payload section */
	__le32 middle_len;/* bytes in middle payload */
	/* Length of data payload section */
	__le32 data_len;  /* bytes of data payload */
	/* Data offset (sender: full offset, receiver: page-masked) */
	__le16 data_off;  /* sender: include full offset;
			     receiver: mask against ~PAGE_MASK */

	/* Message routing information */
	/* Current source and original source entities */
	struct ceph_entity_inst src, orig_src;
	/* Reserved field */
	__le32 reserved;
	/* Header CRC32c checksum */
	__le32 crc;
} __attribute__ ((packed));

/*
 * Standard message header metadata: Current wire format for message headers.
 * Streamlined compared to legacy format, containing essential routing and
 * payload information with compatibility version support.
 */
struct ceph_msg_header {
	/* Message sequence number for this session */
	__le64 seq;
	/* Transaction identifier for request/reply correlation */
	__le64 tid;
	/* Message type identifier */
	__le16 type;
	/* Message priority (higher value = higher priority) */
	__le16 priority;
	/* Version of message encoding/format */
	__le16 version;

	/* Payload section lengths */
	/* Length of front payload section */
	__le32 front_len; /* bytes in main payload */
	/* Length of middle payload section */
	__le32 middle_len;/* bytes in middle payload */
	/* Length of data payload section */
	__le32 data_len;  /* bytes of data payload */
	/* Data offset (sender: full offset, receiver: page-masked) */
	__le16 data_off;  /* sender: include full offset;
			     receiver: mask against ~PAGE_MASK */

	/* Message source entity name */
	struct ceph_entity_name src;
	/* Compatibility version for backward compatibility */
	__le16 compat_version;
	/* Reserved field */
	__le16 reserved;
	/* Header CRC32c checksum */
	__le32 crc;
} __attribute__ ((packed));

/*
 * Messenger v2 header metadata: Enhanced message header for messenger v2
 * protocol with improved padding support, acknowledgment sequencing, and
 * extended compatibility information.
 */
struct ceph_msg_header2 {
	/* Message sequence number for this session */
	__le64 seq;
	/* Transaction identifier for request/reply correlation */
	__le64 tid;
	/* Message type identifier */
	__le16 type;
	/* Message priority (higher value = higher priority) */
	__le16 priority;
	/* Version of message encoding/format */
	__le16 version;

	/* Data padding and alignment */
	/* Length of pre-padding before data payload */
	__le32 data_pre_padding_len;
	/* Data offset (sender: full offset, receiver: page-masked) */
	__le16 data_off;  /* sender: include full offset;
			     receiver: mask against ~PAGE_MASK */

	/* Acknowledgment sequence number */
	__le64 ack_seq;
	/* Message flags */
	__u8 flags;
	/* Oldest code version that can decode this message.  unknown if zero. */
	__le16 compat_version;
	/* Reserved field */
	__le16 reserved;
} __attribute__ ((packed));

#define CEPH_MSG_PRIO_LOW     64
#define CEPH_MSG_PRIO_DEFAULT 127
#define CEPH_MSG_PRIO_HIGH    196
#define CEPH_MSG_PRIO_HIGHEST 255

/*
 * Legacy message footer metadata: Integrity validation for older message format.
 * Contains CRC checksums for each payload section to detect transmission errors
 * and corruption. Used with legacy message headers for backward compatibility.
 */
struct ceph_msg_footer_old {
	/* CRC32c checksums for payload integrity validation */
	__le32 front_crc, middle_crc, data_crc;
	/* Message completion and validation flags */
	__u8 flags;
} __attribute__ ((packed));

/*
 * Standard message footer metadata: Enhanced integrity validation with digital
 * signatures. Includes CRC checksums for each payload section plus optional
 * cryptographic signature for message authenticity and non-repudiation.
 */
struct ceph_msg_footer {
	/* CRC32c checksums for payload integrity validation */
	__le32 front_crc, middle_crc, data_crc;
	/* 64-bit digital signature for message authenticity (PLR) */
	__le64  sig;
	/* Message completion and validation flags */
	__u8 flags;
} __attribute__ ((packed));

#define CEPH_MSG_FOOTER_COMPLETE  (1<<0)   /* msg wasn't aborted */
#define CEPH_MSG_FOOTER_NOCRC     (1<<1)   /* no data crc */
#define CEPH_MSG_FOOTER_SIGNED	  (1<<2)   /* msg was signed */


#endif
