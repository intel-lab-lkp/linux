/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LINUX_GN_H__
#define __LINUX_GN_H__

#include <net/sock.h>
#include <uapi/linux/gn.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <asm/byteorder.h>
#include <linux/etherdevice.h>

#define GN_MAX_HEADER_SZ 88
#define GN_MAXSZ (ETH_DATA_LEN - GN_MAX_HEADER_SZ)
#define GN_MAXHOPS 255 /* 8 bits of hop counter */

#define GN_VERSION 1
#define GN_BROADCAST_ADDR 0xffffffffffffULL
/* itsGnDefaultHopLimit: Default hop limit (for BH RHL/CH MHL) */
#define DEFAULT_HOP_LIMIT 10

/* UNIX timestamp in ms for the ETSI ITS Epoch (2004-01-01 00:00:00 TAI).
 * 1072915200000 ms (2004-01-01 UTC) + 32000 ms (32 leap seconds).
 */
#define GN_EPOCH_UNIX_MS 1072915232000LLU

/* time until location table entry becomes invalid */
#define GN_LOC_TE_LIFETIME 20000
/* time between two beacons */
#define GN_BEACON_RETRANSMIT_TIME 3000
/* time between retransmits of an unanswered location service request */
#define GN_LS_RETRANSMIT_TIME 1000

/* sizes in KiB */
#define GN_UC_BUF_SIZE 256
#define GN_BC_BUF_SIZE 1024
#define GN_CBF_BUF_SIZE 256

#define GN_UC_BUF 0
#define GN_BC_BUF 1
#define GN_CBF_BUF 2

#define BH_NH_ANY 0
#define BH_NH_COMMON_HEADER 1
#define BH_NH_SECURED_PACKET 2

#define GN_LT_BASE_50MS  (0 << 6)
#define GN_LT_BASE_1S    (1 << 6)
#define GN_LT_BASE_10S   (2 << 6)
#define GN_LT_BASE_100S  (3 << 6)
#define GN_ENCODE_LT(base, mult) ((base) | ((mult) & 0x3f))

/* itsGnDefaultPacketLifetime: Default packet lifetime encoded as 60s (Base=1s, Mult=60) */
#define BH_LT_DEFAULT GN_ENCODE_LT(GN_LT_BASE_1S, 60)

#define CH_NH_ANY 0
#define CH_NH_BTPA 1
#define CH_NH_BTPB 2
#define CH_NH_IPV6 3

#define CH_FLAG_MOBILE (1 << 7)

/* itsGnDefaultTrafficClass: Default traffic class */
#define CH_TC_DEFAULT 0

#define CH_HT_BEACON 1
#define CH_HT_GUC 2
#define CH_HT_GAC 3
#define CH_HT_GBC 4
#define CH_HT_TSB 5
#define CH_HT_LS 6

#define CH_HST_GABC_CIRCLE 0
#define CH_HST_GABC_RECT 1
#define CH_HST_GABC_ELIP 2

#define CH_HST_TSB_SINGLE_HOP 0
#define CH_HST_TSB_MULTI_HOP 1

#define CH_HST_LS_REQUEST 0
#define CH_HST_LS_REPLY 1

#define CH_HST_UNSPECIFIED 0
#define CH_HST_BEACON 1

#define CH_PL_EMPTY 0L
#define BEACON_MHL 1

#define GN_BASE_HEADER_SIZE \
	(sizeof(struct gn_basic_header) + sizeof(struct gn_common_header))
#define GN_SET_BTP(skb, btp_h, extended_header_type)                         \
	do {                                                                 \
		skb_set_transport_header(                                    \
			skb,                                                 \
			GN_BASE_HEADER_SIZE + sizeof(extended_header_type)); \
		btp_h = (struct btp_header *)skb_transport_header(skb);      \
	} while (0)

#define GN_GUC_HLEN sizeof(struct gn_guc_header)
#define GN_GXC_HLEN sizeof(struct gn_gxc_header)
#define GN_SHB_HLEN sizeof(struct gn_shb_header)
#define GN_TSB_HLEN sizeof(struct gn_tsb_header)
#define GN_BEACON_HLEN sizeof(struct gn_beacon_header)
#define GN_LS_REQUEST_HLEN sizeof(struct gn_ls_request_header)
#define GN_LS_REPLY_HLEN sizeof(struct gn_ls_reply_header)
#define BTP_HLEN sizeof(struct btp_header)

/**
 *	struct gn_iface - GeoNetworking interface
 *	@dev - Network device associated with this interface
 *	@address - Our address
 *	@local_sn - Current sequence number
 */
struct gn_iface {
	struct net_device	*dev;
	gn_address_t	address;
	struct gn_position pos;
	atomic_t local_sn;
	struct hlist_node hnode;
	struct rcu_head rcu;
};

struct gn_sock {
	/* struct sock has to be the first member of gn_sock */
	struct sock	sk;
	struct gn_scope scope;
	gn_address_t src_addr;
	gn_address_t dst_addr;
	u16 src_port;
	u16 dst_port;
	u8 protocol;
};

static inline struct gn_sock *gn_sk(struct sock *sk)
{
	return (struct gn_sock *)sk;
}

struct btp_header {
	__be16 dst_port;
	__be16 src_port;
} __packed;

enum ITS_TYPE {
	UNKNOWN,
	PEDESTRIAN,
	CYCLIST,
	MOPED,
	MOTORCYCLE,
	PASSENGER_CAR,
	BUS,
	LIGHT_TRUCK,
	HEAVY_TRUCK,
	TAILER,
	SPECIAL_VEHICLE,
	TRAM,
	ROAD_SIDE_UNIT
};

#define GN_ADDRESS_M BIT(63)
#define GN_ADDRESS_ST GENMASK(62, 58)
#define GN_ADDRESS_RES GENMASK(57, 48)
#define GN_ADDRESS_MID GENMASK(47, 0)

typedef __be16 gn_spai_t;

#define GN_LPV_PAI BIT(15)
#define GN_LPV_S GENMASK(14, 0)

struct gn_lpv {
	gn_address_t addr;
	__be32 tst;
	__be32 lat;
	__be32 lon;
	gn_spai_t spai;
	__be16 h; //signed
} __packed;

struct gn_spv {
	gn_address_t addr;
	__be32 tst;
	__be32 lat; //short
	__be32 lon;
} __packed;

/*version: Identifies the version of the GeoNetworking protocol
 *
 *nh: Identifies the type of header immediately following the GeoNetworking
 *    Basic Header
 *reserved: Reserved. Set to 0
 *
 *lt: Lifetime field.
 *Indicates the maximum tolerable time a packet may be buffered until it
 *reaches its destination Bit 0 to Bit 5:
 *lt_sub:
 *0) 50 ms
 *1) 1s
 *2) 10 s
 *3) 100 s
 *4) 600 s
 *5) 1000s
 *
 *Bit 6 to Bit 11:
 *lt_mul: Multiplier for lt_sub, between 0 and 63
 *
 *rhl: Remaining hop limit. Set to the maximum hop limit (mhl) initially and
 *decremented by 1 at each hop.
 *The packet is dropped when rhl reaches 0
 */
struct gn_basic_header {
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u8 nh : 4;
	__u8 version : 4;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u8 version : 4;
	__u8 nh : 4;
#else
#error "please fix asm/byteorder.h"
#endif
	__u8 reserved;
	__u8 lt;
	__u8 rhl;
} __packed;

struct gn_common_header {
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u8 reserved : 4;
	__u8 nh : 4;
	__u8 hst : 4;
	__u8 ht : 4;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u8 nh : 4;
	__u8 reserved : 4;
	__u8 ht : 4;
	__u8 hst : 4;
#else
#error "please fix asm/byteorder.h"
#endif

	__u8 tc;
	__u8 flags;
	__be16 pl;
	__u8 mhl;
	__u8 reserved2;
} __packed;

/*there are 6 types of headers:
 * 1) GUC packet header (clause 9.8.2).
 * 2) TSB packet header (clause 9.8.3).
 * 3) SHB packet header (clause 9.8.4).
 * 4) GBC and GAC packet headers (clause 9.8.5).
 * 5) BEACON packet header (clause 9.8.6).
 * 6) LS Request and LS Reply packet headers (clause 9.8.7 and clause 9.8.8).
 */

struct gn_guc_header {
	__be16 sn;
	__be16 reserved;
	struct gn_lpv sopv;
	struct gn_spv depv;
} __packed;

struct gn_tsb_header {
	__be16 sn;
	__be16 reserved;
	struct gn_lpv sopv;
} __packed;

struct gn_shb_header {
	struct gn_lpv sopv;
	__be32 mdd;
} __packed;

/* GAC and GBC share the same header structure */
struct gn_gxc_header {
	__be16 sn;
	__be16 reserved;
	struct gn_lpv sopv;
	__be32 gap_lat; //signed
	__be32 gap_lon; //signed
	__be16 da;
	__be16 db;
	__be16 angle;
	__be16 reserved2;
} __packed;

#define gn_gac_header gn_gxc_header
#define gn_gbc_header gn_gxc_header

struct gn_beacon_header {
	struct gn_lpv sopv;
} __packed;

struct gn_ls_request_header {
	__be16 sn;
	__be16 reserved;
	struct gn_lpv sopv;
	gn_address_t addr;
} __packed;

//same structure as gn_guc_header, left in for abstraction
struct gn_ls_reply_header {
	__be16 sn;
	__be16 reserved;
	struct gn_lpv sopv;
	struct gn_spv depv;
} __packed;

struct gn_header {
	struct gn_basic_header gb_h;
	struct gn_common_header gc_h;
	union {
		struct gn_guc_header guc_h;
		union {
			struct gn_gxc_header gac_h;
			struct gn_gxc_header gbc_h;
		};
		struct gn_tsb_header tsb_h;
		struct gn_shb_header shb_h;
		struct gn_beacon_header beacon_h;
		struct gn_ls_request_header ls_request_h;
		struct gn_ls_reply_header ls_reply_h;
		__be16 sn;
	};
} __packed;

/* Inter module exports */

u32 gn_tai_to_gn(ktime_t tai_time);
u32 gn_timestamp_now(void);

void gn_fill_sopv(struct gn_iface *gnif, struct gn_lpv *sopv,
		  gn_address_t addr);

extern struct hlist_head gn_sockets;
extern rwlock_t gn_sockets_lock;

extern struct hlist_head gn_interfaces;
extern spinlock_t gn_interfaces_lock;

struct gn_iface *gn_if_add_device(struct net_device *dev,
				  struct sockaddr_gn *sa);
void gn_if_drop_device(struct net_device *dev);
int gn_validate_pos(struct gn_position *pos);

int gn_netlink_init(void);
void gn_netlink_exit(void);

#ifdef CONFIG_SYSCTL
extern int gn_register_sysctl(void);
extern void gn_unregister_sysctl(void);
#else
static inline int gn_register_sysctl(void)
{
	return 0;
}
static inline void gn_unregister_sysctl(void)
{
}
#endif

#endif /* __LINUX_GN_H__ */
