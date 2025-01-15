/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Internal Shared Memory
 *
 *  Definitions for the ISM module
 *
 *  Copyright IBM Corp. 2022
 */
#ifndef _ISM_H
#define _ISM_H

#include <linux/device.h>
#include <linux/workqueue.h>
#include <linux/uuid.h>

/*
 * DMB - Direct Memory Buffer
 * ==========================
 * An ism client provides an DMB as input buffer for a local receiving
 * ism device for exactly one (remote) sending ism device. Only this
 * sending device can send data into this DMB using move_data(). Sender
 * and receiver can be the same device.
 * TODO: Alignment and length rules (CPU and DMA). Device specific?
 */
struct ism_dmb {
	/* dmb_tok - Token for this dmb
	 * Used by remote sender to address this dmb.
	 * Provided by ism fabric in register_dmb().
	 * Unique per ism fabric.
	 */
	u64 dmb_tok;
	/* rgid - GID of designated remote sending device */
	//TODO: Change to uuid_t GID. Ok for now, because loopback ignores it.
	u64 rgid;
	u32 dmb_len;
	/* sba_idx - Index of this DMB on this receiving device */
	u32 sba_idx;
	u32 vlan_valid;
	u32 vlan_id;
	void *cpu_addr;
	dma_addr_t dma_addr;
};

/* ISM event structure (currently device type specific) */
// TODO: Define and describe generic event properties
struct ism_event {
	u32 type;
	u32 code;
	u64 tok;
	u64 time;
	u64 info;
};

//TODO: use enum typedef
#define ISM_EVENT_DMB	0
#define ISM_EVENT_GID	1
#define ISM_EVENT_SWR	2

struct ism_dev;

/*
 * ISM clients
 * ===========
 * All ism clients have access to all ism devices
 * and must provide the following functions to be called by
 * ism device drivers:
 */
struct ism_client {
	/* client name for logging and debugging purposes */
	const char *name;
	/**
	 *  add() - add an ism device
	 *  @dev: device that was added
	 *
	 * Will be called during ism_register_client() for all existing
	 * ism devices and whenever a new ism device is registered.
	 * *dev is valid until ism_client->remove() is called.
	 */
	void (*add)(struct ism_dev *dev);
	/**
	 * remove() - remove an ism device
	 * @dev: device to be removed
	 *
	 * Will be called whenever an ism device is unregistered.
	 * Before this call the device is already inactive: It will
	 * no longer call client handlers.
	 * The client must not access *dev after this call.
	 */
	void (*remove)(struct ism_dev *dev);
	/**
	 * handle_event() - Handle control information sent by device
	 * @dev: device reporting the event
	 * @event: ism event structure
	 */
	void (*handle_event)(struct ism_dev *dev, struct ism_event *event);
	/**
	 * handle_irq() - Handle signalling of a DMB
	 * @dev: device owns the dmb
	 * @bit: sba_idx=idx of the ism_dmb that got signalled
	 *	TODO: Pass a priv pointer to ism_dmb instead of 'bit'(?)
	 * @dmbemask: ism signalling mask of the dmb
	 *
	 * Handle signalling of a dmb that was registered by this client
	 * for this device.
	 * The ism device can coalesce multiple signalling triggers into a
	 * single call of handle_irq(). dmbemask can be used to indicate
	 * different kinds of triggers.
	 */
	void (*handle_irq)(struct ism_dev *dev, unsigned int bit, u16 dmbemask);
	/* client index - provided by ism layer */
	u8 id;
};

int ism_register_client(struct ism_client *client);
int  ism_unregister_client(struct ism_client *client);

//TODO: Pair descriptions with functions
/*
 * ISM devices
 * ===========
 */
/* Mandatory operations for all ism devices:
 * int (*query_remote_gid)(struct ism_dev *dev, uuid_t *rgid,
 *	                   u32 vid_valid, u32 vid);
 *	Query whether remote GID rgid is reachable via this device and this
 *	vlan id. Vlan id is only checked if vid_valid != 0.
 *	Returns 0 if remote gid is reachable.
 *
 * int (*register_dmb)(struct ism_dev *dev, struct ism_dmb *dmb,
 *			    void *client);
 *	Allocate and register an ism_dmb buffer for this device and this client.
 *	The following fields of ism_dmb must be valid:
 *	rgid, dmb_len, vlan_*; Optionally:requested sba_idx (non-zero)
 *	Upon return the following fields will be valid: dmb_tok, sba_idx
 *		cpu_addr, dma_addr (if applicable)
 *	Returns zero on success
 *
 * int (*unregister_dmb)(struct ism_dev *dev, struct ism_dmb *dmb);
 *	Unregister an ism_dmb buffer
 *
 * int (*move_data)(struct ism_dev *dev, u64 dmb_tok, unsigned int idx,
 *			 bool sf, unsigned int offset, void *data,
 *			 unsigned int size);
 *	Use dev to write data of size at offset into a remote dmb
 *	identified by dmb_tok and idx. If signal flag (sf) then signal
 *	the remote peer that data has arrived in this dmb.
 *
 * int (*unregister_dmb)(struct ism_dev *dev, struct ism_dmb *dmb);
 *	Unregister an ism_dmb buffer
 *
 * int (*supports_v2)(void);
 *
 * u16 (*get_chid)(struct ism_dev *dev);
 *	Returns ism fabric identifier (channel id) of this device.
 *	Only devices on the same ism fabric can communicate.
 *	chid is unique per HW system. Use chid for fast negative checks,
 *	but only query_remote_gid() can give a reliable positive answer:
 *	Different chid: ism is not possible
 *	Same chid: ism traffic may be possible or not
 *		   (e.g. different HW systems)
 *	EXCEPTION: A value of 0xFFFF denotes an ism_loopback device
 *		that can only communicate with itself. Use GID or
 *		query_remote_gid()to determine whether sender and
 *		receiver use the same ism_loopback device.
 *
 * struct device* (*get_dev)(struct ism_dev *dev);
 *
 * Optional operations:
 * int (*add_vlan_id)(struct ism_dev *dev, u64 vlan_id);
 * int (*del_vlan_id)(struct ism_dev *dev, u64 vlan_id);
 * int (*set_vlan_required)(struct ism_dev *dev);
 * int (*reset_vlan_required)(struct ism_dev *dev);
 *	VLAN handling is broken - don't use it
 *	Ability to assign dmbs to VLANs is missing
 *	- do we really want / need this?
 *
 * int (*signal_event)(struct ism_dev *dev, uuid_t *rgid,
 *			    u32 trigger_irq, u32 event_code, u64 info);
 *	Send a control event into the event queue of a remote gid (rgid)
 *	with (1) or without (0) triggering an interrupt at the remote gid.
 */

struct ism_ops {
	int (*query_remote_gid)(struct ism_dev *dev, uuid_t *rgid,
				u32 vid_valid, u32 vid);
	int (*register_dmb)(struct ism_dev *dev, struct ism_dmb *dmb,
			    struct ism_client *client);
	int (*unregister_dmb)(struct ism_dev *dev, struct ism_dmb *dmb);
	/**
	 * move_data() - write into a remote dmb
	 * @dev: Local sending ism device
	 * @dmb_tok: Token of the remote dmb
	 * @idx: signalling index
	 * @sf: signalling flag;
	 *      if true, idx will be turned on at target ism interrupt mask
	 *      and target device will be signalled, if required.
	 * @offset: offset within target dmb
	 * @data: pointer to data to be sent
	 * @size: length of data to be sent
	 *
	 * Use dev to write data of size at offset into a remote dmb
	 * identified by dmb_tok. Data is moved synchronously, *data can
	 * be freed when this function returns.
	 *
	 * If signalling flag (sf) is true, bit number idx bit will be
	 * turned on in the ism signalling mask, that belongs to the
	 * target dmb, and handle_irq() of the ism client that owns this
	 * dmb will be called, if required. The target device may chose to
	 * coalesce multiple signalling triggers.
	 */
	int (*move_data)(struct ism_dev *dev, u64 dmb_tok, unsigned int idx,
			 bool sf, unsigned int offset, void *data,
			 unsigned int size);
	int (*supports_v2)(void);
	u16 (*get_chid)(struct ism_dev *dev);
	struct device* (*get_dev)(struct ism_dev *dev);

	/* optional operations */
	int (*add_vlan_id)(struct ism_dev *dev, u64 vlan_id);
	int (*del_vlan_id)(struct ism_dev *dev, u64 vlan_id);
	int (*set_vlan_required)(struct ism_dev *dev);
	int (*reset_vlan_required)(struct ism_dev *dev);
	int (*signal_event)(struct ism_dev *dev, uuid_t *rgid,
			    u32 trigger_irq, u32 event_code, u64 info);
/* no copy option
 * --------------
 */
	/**
	 * support_dmb_nocopy() - does this device provide no-copy option?
	 * @dev: ism device
	 *
	 * In addition to using move_data(), a sender device can provide a
	 * kernel address + length, that represents a target dmb
	 * (like MMIO). If a sender writes into such a ghost-send-buffer
	 * (= at this kernel address) the data will automatically
	 * immediately appear in the target dmb, even without calling
	 * move_data().
	 * Note that this is NOT related to the MSG_ZEROCOPY socket flag.
	 *
	 * Either all 3 function pointers for support_dmb_nocopy(),
	 * attach_dmb() and detach_dmb() are defined, or all of them must
	 * be NULL.
	 *
	 * Return: non-zero, if no-copy is supported.
	 */
	int (*support_dmb_nocopy)(struct ism_dev *dev);
	/**
	 * attach_dmb() - attach local memory to a remote dmb
	 * @dev: Local sending ism device
	 * @dmb: all other parameters are passed in the form of a
	 *	 dmb struct
	 *	 TODO: (THIS IS CONFUSING, should be changed)
	 *  dmb_tok: (in) Token of the remote dmb, we want to attach to
	 *  cpu_addr: (out) MMIO address
	 *  dma_addr: (out) MMIO address (if applicable, invalid otherwise)
	 *  dmb_len: (out) length of local MMIO region,
	 *           equal to length of remote DMB.
	 *  sba_idx: (out) index of remote dmb (NOT HELPFUL, should be removed)
	 *
	 * Provides a memory address to the sender that can be used to
	 * directly write into the remote dmb.
	 *
	 * Return: Zero upon success, Error code otherwise
	 */
	int (*attach_dmb)(struct ism_dev *dev, struct ism_dmb *dmb);
	/**
	 * detach_dmb() - Detach the ghost buffer from a remote dmb
	 * @dev: ism device
	 * @token: dmb token of the remote dmb
	 * Return: Zero upon success, Error code otherwise
	 */
	int (*detach_dmb)(struct ism_dev *dev, u64 token);
};

/* Unless we gain unexpected popularity, this limit should hold for a while */
#define MAX_CLIENTS		8
#define NO_CLIENT		0xff		/* must be >= MAX_CLIENTS */
#define ISM_NR_DMBS		1920
/* Defined fabric id / CHID for all loopback devices: */
#define ISM_LO_RESERVED_CHID	0xFFFF
#define ISM_LO_MAX_DMBS		5000

struct ism_dev {
	const struct ism_ops *ops;
	struct list_head list;
	struct device dev;
	uuid_t gid;

	/* get this lock before accessing any of the fields below */
	spinlock_t lock;
	/* indexed by dmb idx; entries are indices into priv and subs arrays: */
	u8 *sba_client_arr;
	/* Sparse array of all ISM clients */
	struct ism_client *subs[MAX_CLIENTS];
	/* priv pointer per client; for client usage only */
	void *priv[MAX_CLIENTS];
};

int ism_dev_register(struct ism_dev *ism);
void ism_dev_unregister(struct ism_dev *ism);

static inline void *ism_get_priv(struct ism_dev *dev,
				 struct ism_client *client) {
	return dev->priv[client->id];
}
static inline void ism_set_priv(struct ism_dev *dev, struct ism_client *client,
				void *priv) {
	dev->priv[client->id] = priv;
}

static inline struct device *ism_get_dev(struct ism_dev *ism)
{
	return &ism->dev;
}

#define ISM_RESERVED_VLANID	0x1FFF
#define ISM_ERROR	0xFFFF

#endif	/* _ISM_H */
