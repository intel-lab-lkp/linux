/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __NET_PSP_H
#define __NET_PSP_H

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <net/net_trackers.h>
#include <uapi/linux/psp.h>

struct netlink_ext_ack;

#define PSP_DEFAULT_UDP_PORT	1000

struct psphdr {
	u8	nexthdr;
	u8	hdrlen;
	u8	crypt_offset;
	u8	verfl;
	__be32	spi;
	__be64	iv;
	__be64	vc[]; /* optional */
};

#define PSP_ENCAP_HLEN (sizeof(struct udphdr) + sizeof(struct psphdr))

#define PSP_SPI_KEY_ID		GENMASK(30, 0)
#define PSP_SPI_KEY_PHASE	BIT(31)

#define PSPHDR_CRYPT_OFFSET	GENMASK(5, 0)

#define PSPHDR_VERFL_SAMPLE	BIT(7)
#define PSPHDR_VERFL_DROP	BIT(6)
#define PSPHDR_VERFL_VERSION	GENMASK(5, 2)
#define PSPHDR_VERFL_VIRT	BIT(1)
#define PSPHDR_VERFL_ONE	BIT(0)

#define PSP_HDRLEN_NOOPT	((sizeof(struct psphdr) - 8) / 8)
#define PSP_HDRLEN_VC		(PSP_HDRLEN_NOOPT + 1)

/* Virtualization cookie (VC) based Rx queue steering.
 *
 * The VC is a 64b cookie which the PSP spec leaves to the implementation
 * in transport mode. We use it to let the two ends of a connection tell
 * each other which Rx queue they'd like traffic delivered to:
 *
 *  63           48 47           32 31           16 15            0
 * +---------------+---------------+---------------+---------------+
 * |    reserved   |    req qid    |    reserved   |    dst qid    |
 * +---------------+---------------+---------------+---------------+
 *
 * @req is the Rx queue the sender is asking the peer to send to.
 * @dst is the Rx queue this packet is to be delivered to, and holds the
 * @req the sender most recently saw from the peer. Each side's request
 * is what becomes the other side's destination.
 *
 * Each ID gets a 32b word to itself, of which only the low half is used
 * today. Queue counts fit in 16b for now, but growing an ID to 32b later
 * is then a matter of widening its mask, with no reshuffling of the
 * cookie and no change to what an old peer puts on the wire.
 *
 * The two directions are enabled separately, see enum psp_vc_steer.
 * %PSP_VC_STEER_RX fills in @req and needs the device to install low
 * priority steering rules matching on @dst, which take precedence over
 * the RSS table result - @dst sits in the low bits so that those rules
 * only need to mask off the bottom 16b of the cookie. %PSP_VC_STEER_TX
 * fills in @dst and needs nothing from the device, it only helps the
 * peer.
 *
 * The wire is a two party structure, so it is named from the sender's
 * side, and so are the queue IDs a received packet reports in
 * psp_skb_ext. The association records the same two IDs from our own
 * side instead: see psp_assoc.vc_loc and psp_assoc.vc_rem.
 *
 * %PSP_VC_QID_NONE means "no queue" and is what both fields hold before
 * anything has been learned. Reserved bits are 0 on Tx, ignored on Rx.
 */
#define PSP_VC_REQ_QID		GENMASK_ULL(47, 32)
#define PSP_VC_DST_QID		GENMASK_ULL(15, 0)

#define PSP_VC_QID_NONE		0xffff

/**
 * struct psp_dev_config - PSP device configuration
 * @versions: PSP versions enabled on the device
 * @vc_steer: directions taking part in VC steering, mask of enum psp_vc_steer
 */
struct psp_dev_config {
	u32 versions;
	u32 vc_steer;
};

/* Max number of devices that can be associated with a single PSP device.
 * Each entry consumes ~24 bytes in the netlink dev-get response, and the
 * response must fit in GENLMSG_DEFAULT_SIZE (~3.7KB).
 */
#define PSP_ASSOC_DEV_MAX	128

/**
 * struct psp_assoc_dev - wrapper for associated net_device
 * @dev_list: list node for psp_dev::assoc_dev_list
 * @assoc_dev: the associated net_device
 * @dev_tracker: tracker for the net_device reference
 */
struct psp_assoc_dev {
	struct list_head dev_list;
	struct net_device *assoc_dev;
	netdevice_tracker dev_tracker;
};

/**
 * struct psp_dev - PSP device struct
 * @main_netdev: original netdevice of this PSP device
 * @assoc_dev_list: list of psp_assoc_dev entries associated with this PSP device
 * @assoc_dev_cnt: number of entries in @assoc_dev_list
 * @ops:	driver callbacks
 * @caps:	device capabilities
 * @drv_priv:	driver priv pointer
 * @lock:	instance lock, protects all fields
 * @refcnt:	reference count for the instance
 * @id:		instance id
 * @generation:	current generation of the device key
 * @config:	current device configuration
 * @active_assocs:	list of registered associations
 * @prev_assocs:	associations which use old (but still usable)
 *			device key
 * @stale_assocs:	associations which use a rotated out key
 *
 * @stats:	statistics maintained by the core
 * @stats.rotations:	See stats attr key-rotations
 * @stats.stales:	See stats attr stale-events
 *
 * @rcu:	RCU head for freeing the structure
 */
struct psp_dev {
	struct net_device *main_netdev;
	struct list_head assoc_dev_list;
	int assoc_dev_cnt;

	struct psp_dev_ops *ops;
	struct psp_dev_caps *caps;
	void *drv_priv;

	struct mutex lock;
	refcount_t refcnt;

	u32 id;

	u8 generation;

	struct psp_dev_config config;

	struct list_head active_assocs;
	struct list_head prev_assocs;
	struct list_head stale_assocs;

	struct {
		unsigned long rotations;
		unsigned long stales;
	} stats;

	struct rcu_head rcu;
};

#define PSP_GEN_VALID_MASK	0x7f

/**
 * struct psp_dev_caps - PSP device capabilities
 */
struct psp_dev_caps {
	/**
	 * @versions: mask of supported PSP versions
	 * Set this field to 0 to indicate PSP is not supported at all.
	 */
	u32 versions;

	/**
	 * @assoc_drv_spc: size of driver-specific state in Tx assoc
	 * Determines the size of struct psp_assoc::drv_data
	 */
	u32 assoc_drv_spc;

	/**
	 * @vc_steer: device can steer received traffic on the VC
	 * Only gates PSP_VC_STEER_RX, granting a peer's request needs
	 * nothing from the device.
	 */
	bool vc_steer;
};

#define PSP_MAX_KEY	32

#define PSP_HDR_SIZE	16	/* Fixed part of the PSP header */
#define PSP_VC_SIZE	8	/* Optional virtualization cookie */
#define PSP_TRL_SIZE	16	/* AES-GCM/GMAC trailer size */

/* Keep free of padding, the whole struct gets memcmp()ed by GRO */
struct psp_skb_ext {
	__be32 spi;
	u16 dev_id;
	u8 generation;
	u8 version;
	u16 vc_req;	/* Queue the sender asked for, or PSP_VC_QID_NONE */
	u16 vc_dst;	/* Queue the sender addressed, or PSP_VC_QID_NONE */
};

static_assert(sizeof(struct psp_skb_ext) == 12,
	      "struct psp_skb_ext must not contain padding");

struct psp_key_parsed {
	__be32 spi;
	u8 key[PSP_MAX_KEY];
};

/**
 * enum psp_assoc_flags - flags of struct psp_assoc
 * @PSP_ASSOC_VC_TX: grant the queue the peer asks for in the cookie
 * @PSP_ASSOC_VC_RX: ask the peer for a queue in the cookie
 */
enum psp_assoc_flags {
	PSP_ASSOC_VC_TX			= BIT(0),
	PSP_ASSOC_VC_RX			= BIT(1),
};

#define PSP_ASSOC_VC_ANY	(PSP_ASSOC_VC_TX | PSP_ASSOC_VC_RX)

struct psp_assoc {
	struct psp_dev *psd;

	u16 dev_id;
	u8 generation;
	u8 version;
	u8 peer_tx;
	/* enum psp_assoc_flags. Written under psd->lock, additionally read
	 * on the Tx fast path without it. A snapshot of the device config
	 * taken when the association was created, so that the header size,
	 * and with it the MSS, cannot change under an established
	 * connection.
	 */
	u8 flags;

	/* Queue IDs for the VC, ours and the peer's. @vc_loc is refreshed
	 * from the Tx queue selection and goes out as the cookie's request,
	 * @vc_rem is learned from the peer's requests and goes back out as
	 * the destination. Both are PSP_VC_QID_NONE until something is
	 * learned. Written without the socket lock, always use
	 * READ_ONCE()/WRITE_ONCE().
	 */
	u16 vc_loc;
	u16 vc_rem;

	u32 upgrade_seq;

	struct psp_key_parsed tx;
	struct psp_key_parsed rx;

	refcount_t refcnt;
	struct rcu_head rcu;
	struct work_struct work;
	struct list_head assocs_list;

	u8 drv_data[] __aligned(8);
};

struct psp_dev_stats {
	union {
		struct {
			u64 rx_packets;
			u64 rx_bytes;
			u64 rx_auth_fail;
			u64 rx_error;
			u64 rx_bad;
			u64 tx_packets;
			u64 tx_bytes;
			u64 tx_error;
		};
		DECLARE_FLEX_ARRAY(u64, required);
	};
};

/**
 * struct psp_dev_ops - netdev driver facing PSP callbacks
 */
struct psp_dev_ops {
	/**
	 * @set_config: set configuration of a PSP device
	 * Driver can inspect @psd->config for the previous configuration.
	 * Core will update @psd->config with @config on success.
	 */
	int (*set_config)(struct psp_dev *psd, struct psp_dev_config *conf,
			  struct netlink_ext_ack *extack);

	/**
	 * @key_rotate: rotate the device key
	 */
	int (*key_rotate)(struct psp_dev *psd, struct netlink_ext_ack *extack);

	/**
	 * @rx_spi_alloc: allocate an Rx SPI+key pair
	 * Allocate an Rx SPI and resulting derived key.
	 * This key should remain valid until key rotation.
	 */
	int (*rx_spi_alloc)(struct psp_dev *psd, u32 version,
			    struct psp_key_parsed *assoc,
			    struct netlink_ext_ack *extack);

	/**
	 * @tx_key_add: add a Tx key to the device
	 * Install an association in the device. Core will allocate space
	 * for the driver to use at drv_data.
	 */
	int (*tx_key_add)(struct psp_dev *psd, struct psp_assoc *pas,
			  struct netlink_ext_ack *extack);
	/**
	 * @tx_key_del: remove a Tx key from the device
	 * Remove an association from the device.
	 */
	void (*tx_key_del)(struct psp_dev *psd, struct psp_assoc *pas);

	/**
	 * @get_stats: get statistics from the device
	 * Stats required by the spec must be maintained and filled in.
	 * Stats must be filled in member-by-member, never memset the struct.
	 */
	void (*get_stats)(struct psp_dev *psd, struct psp_dev_stats *stats);
};

#endif /* __NET_PSP_H */
