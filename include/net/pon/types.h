/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */

#ifndef __NET_PON_TYPES_H
#define __NET_PON_TYPES_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/refcount.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <net/pon/ploam.h>
#include <uapi/linux/pon.h>

struct net_device;
struct netlink_ext_ack;
struct pon_conduit;
struct pon_dev;
struct pon_work;
struct sk_buff;

#define PON_LOG_LINES		16
#define PON_LOG_LINE_LEN	64

typedef void (*pon_work_func_t)(struct pon_dev *pdev, struct pon_work *work);

/**
 * struct pon_work - a work item that runs in the instance's context
 * @entry: link in the instance's work list
 * @func: what to run
 *
 * The core lends a driver its serialized context. The handler runs on the
 * instance's ordered workqueue with the instance lock held, so it may sleep
 * and it cannot interleave with a netlink transaction.
 */
struct pon_work {
	struct list_head entry;
	pon_work_func_t func;
};

/* The wire defines these, so they come from the PLOAM vocabulary. */
#define PON_SERIAL_LEN		PON_PLOAM_SN_LEN
#define PON_REG_ID_LEN		PON_PLOAM_REG_ID_LEN

/**
 * struct pon_identity - what dev-set delivers, identity and the settings
 *			 that travel with it
 * @mode: requested operating mode, enum pon_mode
 * @serial: serial number, 4 ASCII vendor id then 4 vendor specific bytes
 * @reg_id: registration id used for XG(S)-PON authentication, padded with
 *	    0x00 bytes to PON_REG_ID_LEN (ITU-T G.9807.1 Table C.11.25)
 * @serial_set: @serial carries a value
 * @mode_set: @mode carries a value
 * @reg_id_len: PON_REG_ID_LEN when @reg_id carries a value, 0 when absent
 */
struct pon_identity {
	enum pon_mode mode;
	u8 serial[PON_SERIAL_LEN];
	u8 reg_id[PON_REG_ID_LEN];
	bool serial_set;
	bool mode_set;
	u8 reg_id_len;
};

/**
 * struct pon_tcont_cfg - one T-CONT binding
 * @index: T-CONT index in the device, 0 based
 * @alloc_id: alloc-id assigned by the OLT
 */
struct pon_tcont_cfg {
	u16 index;
	u16 alloc_id;
};

/**
 * struct pon_gem_cfg - one GEM port
 * @id: GEM port id assigned by the OLT
 * @dir: direction, enum pon_gem_dir
 * @tcont_index: T-CONT the upstream half rides, valid when @tcont_valid
 * @alloc_id: alloc-id of that T-CONT, resolved by the core, 0 when
 *	@tcont_valid is false
 * @key_ring: encryption key ring, enum pon_gem_key_ring (ITU-T G.988
 *	clause 9.2.3), never broadcast
 * @tcont_valid: @tcont_index carries a value. A downstream broadcast GEM
 *	carries none
 */
struct pon_gem_cfg {
	u16 id;
	enum pon_gem_dir dir;
	u16 tcont_index;
	u16 alloc_id;
	enum pon_gem_key_ring key_ring;
	bool tcont_valid;
};

/**
 * struct pon_gem_map_cfg - one upstream classifier rule
 * @gem_id: GEM port the matching frames map to
 * @vid: outer VLAN ID to match, valid when @vid_valid
 * @tagged: tag state to match, valid when @tag_valid
 * @pbit: outer VLAN priority to match, valid when @pbit_valid
 * @dscp: IP DSCP to match, valid when @dscp_valid
 * @tag_valid: @tagged carries a value
 * @vid_valid: @vid carries a value
 * @pbit_valid: @pbit carries a value
 * @dscp_valid: @dscp carries a value
 *
 * A rule matches on the members marked valid. A more specific rule
 * wins over a less specific one.
 */
struct pon_gem_map_cfg {
	u16 gem_id;
	u16 vid;
	bool tagged;
	u8 pbit;
	u8 dscp;
	bool tag_valid;
	bool vid_valid;
	bool pbit_valid;
	bool dscp_valid;
};

/**
 * enum pon_event_type - what a driver reports through pon_dev_event()
 * @PON_EVENT_TYPE_TCONT_ALLOC: the OLT assigned an alloc-id with the
 *	Assign_Alloc-ID message of ITU-T G.9807.1 clause C.11.3.3.7 and the
 *	driver bound it to a transmit channel
 * @PON_EVENT_TYPE_TCONT_DEALLOC: the OLT deallocated the alloc-id with the
 *	same message and the driver released its channel. The GEM ports that
 *	ride it carry no upstream traffic until the OLT assigns it again
 */
enum pon_event_type {
	PON_EVENT_TYPE_TCONT_ALLOC,
	PON_EVENT_TYPE_TCONT_DEALLOC,
};

/**
 * struct pon_event - a discrete event a driver reports
 * @type: what happened, enum pon_event_type
 * @alloc_id: the alloc-id the OLT allocated or deallocated
 */
struct pon_event {
	enum pon_event_type type;
	u32 alloc_id;
};

/**
 * struct pon_dev_caps - what the device supports
 * @modes: bitmask of enum pon_mode the hardware can run
 * @max_tconts: number of T-CONTs
 * @max_gems: number of GEM ports userspace can create, not counting the
 *	      GEM port of the OMCC, which the driver holds itself
 */
struct pon_dev_caps {
	u32 modes;
	u16 max_tconts;
	u16 max_gems;
};

/**
 * struct pon_tx_info - what the conduit needs to send one frame
 * @gem: the GEM port id the frame goes out on
 * @channel: the transmit channel the MAC driver bound the T-CONT to. The
 *	     core carries it between the two drivers and does not interpret it
 * @queue: the queue within that channel
 * @mic_index: which OMCI integrity key signs the PDU, valid when @oam
 * @oam: the frame is an OMCI PDU rather than an Ethernet frame
 */
struct pon_tx_info {
	u16 gem;
	u8 channel;
	u8 queue;
	u8 mic_index;
	bool oam;
};

/**
 * struct pon_rx_info - what the conduit learned from the receive descriptor
 * @gem: the GEM port id the frame arrived on
 * @oam: the frame is an OMCI PDU
 * @mic_unchecked: the MAC did not verify the integrity of the PDU, valid when
 *		   @oam
 */
struct pon_rx_info {
	u16 gem;
	bool oam;
	bool mic_unchecked;
};

/**
 * struct pon_conduit_ops - what the ethernet driver does for a PON MAC
 * @xmit: queue one frame on the conduit's rings with a descriptor built from
 *	  @info. Owns the skb from the call on (as ndo_start_xmit does) and
 *	  returns NETDEV_TX_BUSY only when the frame was not taken. Called
 *	  under rcu_read_lock() with bottom halves disabled, from every PON
 *	  network device and the OMCI channel at once and without the
 *	  conduit's own transmit lock, so the driver locks its ring itself.
 */
struct pon_conduit_ops {
	netdev_tx_t (*xmit)(struct net_device *conduit, struct sk_buff *skb,
			    const struct pon_tx_info *info);
};

/**
 * struct pon_dev - one PON MAC
 * @main_netdev: the PON data network device
 * @conduit: the ethernet device whose rings carry the frames and what its
 *	     driver does for the MAC, published as one pointer once the driver
 *	     has registered it, read under RCU, written under pon_devs_lock
 * @conduit_tracker: the reference held on the ethernet device of @conduit
 * @parent: the MAC's device, which every interface the core creates
 *	    parents onto and whose firmware node a conduit names
 * @ops: driver callbacks, NULL once the driver has unregistered. The
 *	 transmit paths read it once under RCU
 * @caps: device capabilities
 * @drv_priv: driver priv pointer
 * @lock: instance lock and the driver's upcalls. It protects every field
 *	  below it, with these exceptions. @conduit is @pon_devs_lock's. The
 *	  OMCI fields name their own rules. @ploam and @going_away are
 *	  written under the lock and read without it through READ_ONCE()
 * @refcnt: reference count for the instance
 * @id: instance id
 * @mode: active mode, enum pon_mode
 * @ploam: activation state as the driver last reported it
 * @enabled: the upstream link was last started rather than stopped
 * @omci_portid: netlink port id of the socket that owns the OMCI channel, 0
 *		 when none does, changed with cmpxchg, because a socket that
 *		 closes gives it up without the lock
 * @omci_rxq: OMCI PDUs from the OLT in the order they arrived, waiting for
 *	      the instance's context. Each one is marked when the MAC passed
 *	      it up unchecked
 * @omci_rx_work: has the driver verify the unchecked PDUs of @omci_rxq and
 *		  hands every PDU to the owner of the OMCI channel
 * @identity: the serial number as dev-set last delivered it, which dev-get
 *	      reports. The registration id goes to the driver and is not kept
 * @identity.serial: serial number, valid when @identity.serial_set
 * @identity.serial_set: dev-set delivered a serial number
 * @tconts: bound T-CONTs, struct pon_tcont
 * @gems: GEM port objects, struct pon_gem
 * @gem_maps: upstream classifier rules, struct pon_gem_map
 * @gem_netdevs: GEM port id to its network device, read under RCU on receive
 * @wq: ordered workqueue, the instance's one deferred context
 * @work: the single worker that drains @work_list
 * @work_list: work items waiting for the instance's context
 * @work_lock: guards @work_list, taken from hard interrupt
 * @log_work: prints the lines of @log_lines on system_dfl_wq, outside the
 *	      instance's context
 * @log_lines: the lines pon_dev_log() recorded and @log_work has not printed
 *	       yet, a ring that starts at @log_head
 * @log_levels: the printk level of each line of @log_lines, such as KERN_INFO
 * @log_head: the oldest line of @log_lines
 * @log_count: the lines held in @log_lines
 * @log_dropped: the oldest lines a full @log_lines dropped since @log_work
 *		 last printed the count
 * @going_away: set on the unregister path, so a work item already past its
 *		scheduling point does not reach a driver that is leaving
 * @rcu: RCU head for freeing the structure
 */
struct pon_dev {
	struct net_device *main_netdev;
	struct pon_conduit __rcu *conduit;
	netdevice_tracker conduit_tracker;
	struct device *parent;

	const struct pon_dev_ops *ops;
	const struct pon_dev_caps *caps;
	void *drv_priv;

	/* guards every field below and the driver's upcalls */
	struct mutex lock;
	refcount_t refcnt;

	u32 id;
	enum pon_mode mode;
	enum pon_ploam_state ploam;
	bool enabled;
	struct {
		u8 serial[PON_SERIAL_LEN];
		bool serial_set;
	} identity;

	struct list_head tconts;
	struct list_head gems;
	struct list_head gem_maps;
	struct xarray gem_netdevs;

	struct workqueue_struct *wq;
	struct work_struct work;
	struct list_head work_list;
	/* guards @work_list, taken from hard interrupt */
	spinlock_t work_lock;

	u32 omci_portid;
	struct sk_buff_head omci_rxq;
	struct pon_work omci_rx_work;
	struct work_struct log_work;
	char log_lines[PON_LOG_LINES][PON_LOG_LINE_LEN];
	const char *log_levels[PON_LOG_LINES];
	unsigned int log_head;
	unsigned int log_count;
	unsigned int log_dropped;
	bool going_away;

	struct rcu_head rcu;
};

/**
 * struct pon_dev_ops - netdev driver facing PON callbacks
 *
 * tcont_set, tcont_clear, gem_add, gem_del and omci_xmit are mandatory. The
 * others may be NULL and the core then answers -EOPNOTSUPP.
 *
 * Every callback runs in process context and may sleep, except where its own
 * description says otherwise. The ones that configure or read the device run
 * with the instance lock held, from a netlink handler or from the instance's
 * work, so a driver sees them one at a time and never interleaved with its
 * own pon_work handlers. The datapath callbacks run without the lock.
 */
struct pon_dev_ops {
	/**
	 * @enable: start or stop the ONU upstream link
	 * Instance lock held.
	 */
	int (*enable)(struct pon_dev *pdev, bool on,
		      struct netlink_ext_ack *extack);

	/**
	 * @set_identity: set the ONU identity and the settings that travel
	 *		  with it
	 * Only the members whose _set flag or length is nonzero changed.
	 * Instance lock held.
	 */
	int (*set_identity)(struct pon_dev *pdev,
			    const struct pon_identity *id,
			    struct netlink_ext_ack *extack);

	/**
	 * @tcont_set: bind an alloc-id to a T-CONT, or rebind it
	 * Instance lock held.
	 */
	int (*tcont_set)(struct pon_dev *pdev,
			 const struct pon_tcont_cfg *cfg,
			 struct netlink_ext_ack *extack);

	/**
	 * @tcont_clear: release a T-CONT
	 * The core passes the stored binding. Instance lock held.
	 */
	int (*tcont_clear)(struct pon_dev *pdev,
			   const struct pon_tcont_cfg *cfg,
			   struct netlink_ext_ack *extack);

	/**
	 * @tcont_channel: the conduit transmit channel a T-CONT is bound to,
	 *		   optional. The core asks it for the carrier, which is
	 *		   up while a GEM port rides a T-CONT with a channel.
	 *		   Without it the carrier rises with the first GEM
	 *		   port. Return the channel, -ENOLINK while the OLT has
	 *		   not assigned the alloc-id and the T-CONT has no
	 *		   channel yet, or another negative errno. The default
	 *		   alloc-id has a channel once the ONU-ID is assigned.
	 *		   Instance lock held. rtnl is held by some callers and
	 *		   not by others, so the driver must not rely on it.
	 */
	int (*tcont_channel)(struct pon_dev *pdev,
			     const struct pon_tcont_cfg *cfg);

	/**
	 * @gem_add: create a GEM port
	 *
	 * Must also succeed for a GEM port the driver already holds and
	 * re-program it, which a T-CONT that moves to another alloc-id
	 * needs. The driver keeps a GEM port across a loss of the link and
	 * a new activation, as ITU-T G.9807.1 clause C.6.1.5.8 has the ONU
	 * keep it. A GEM port whose alloc-id has no channel yet is held and
	 * carries nothing until the OLT assigns the alloc-id. Instance lock
	 * held.
	 */
	int (*gem_add)(struct pon_dev *pdev, const struct pon_gem_cfg *cfg,
		       struct netlink_ext_ack *extack);

	/**
	 * @gem_del: destroy a GEM port
	 * Instance lock held.
	 */
	int (*gem_del)(struct pon_dev *pdev, u16 gem_id,
		       struct netlink_ext_ack *extack);

	/**
	 * @gem_xmit: send one frame on a GEM port, optional. Consumes the skb
	 *	      whatever it returns. Without it a GEM network device
	 *	      drops what it is given. Runs from ndo_start_xmit of the
	 *	      GEM network device, without the instance lock. A
	 *	      concurrent gem_del is the driver's to order.
	 */
	int (*gem_xmit)(struct pon_dev *pdev, u16 gem_id, struct sk_buff *skb);

	/**
	 * @omci_xmit: send one OMCI PDU to the OLT
	 * The skb carries the bare PDU, validated by the core. The driver
	 * consumes the skb on success and on failure. Runs from the omci-tx
	 * netlink handler, with the instance lock held and bottom halves
	 * disabled.
	 */
	int (*omci_xmit)(struct pon_dev *pdev, struct sk_buff *skb);

	/**
	 * @omci_verify: check the integrity of a received OMCI PDU that the
	 *		 MAC passed up unchecked, optional. The skb is linear
	 *		 and ends with the 4 byte MIC, which the driver checks
	 *		 with its OMCI integrity key and strips on success
	 *		 (ITU-T G.9807.1 clause C.15.7.2). The check is the
	 *		 driver's because the key never reaches the core.
	 *		 Return 0 to deliver it, a negative errno to drop it.
	 *		 Without it such a PDU is dropped. Runs in the
	 *		 instance's context with the lock held and may sleep.
	 */
	int (*omci_verify)(struct pon_dev *pdev, struct sk_buff *skb);

	/**
	 * @gem_map_set: add one upstream classifier rule. Idempotent: a rule
	 *		 the driver already holds is success, not -EEXIST. A
	 *		 rule outlives the GEM port it names.
	 *		 Instance lock held.
	 */
	int (*gem_map_set)(struct pon_dev *pdev,
			   const struct pon_gem_map_cfg *cfg,
			   struct netlink_ext_ack *extack);

	/**
	 * @gem_map_del: remove one upstream classifier rule
	 * Instance lock held.
	 */
	int (*gem_map_del)(struct pon_dev *pdev,
			   const struct pon_gem_map_cfg *cfg,
			   struct netlink_ext_ack *extack);

};

#endif /* __NET_PON_TYPES_H */
