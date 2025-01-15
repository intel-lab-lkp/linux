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

/* The remote peer rgid can use dmb_tok to write into this buffer. */
struct ism_dmb {
	u64 dmb_tok;
	u64 rgid;
	u32 dmb_len;
	u32 sba_idx;
	u32 vlan_valid;
	u32 vlan_id;
	void *cpu_addr;
	dma_addr_t dma_addr;
};

struct ism_event {
	u32 type;
	u32 code;
	u64 tok;
	u64 time;
	u64 info;
};

#define ISM_EVENT_DMB	0
#define ISM_EVENT_GID	1
#define ISM_EVENT_SWR	2

struct ism_dev;

struct ism_client {
	const char *name;
	void (*add)(struct ism_dev *dev);
	void (*remove)(struct ism_dev *dev);
	void (*handle_event)(struct ism_dev *dev, struct ism_event *event);
	/* Parameter dmbemask contains a bit vector with updated DMBEs, if sent
	 * via ism_move_data(). Callback function must handle all active bits
	 * indicated by dmbemask.
	 */
	void (*handle_irq)(struct ism_dev *dev, unsigned int bit, u16 dmbemask);
	/* Private area - don't touch! */
	u8 id;
};

int ism_register_client(struct ism_client *client);
int  ism_unregister_client(struct ism_client *client);

/* Mandatory operations for all ism devices:
 * int (*query_remote_gid)(struct ism_dev *dev, uuid_t *rgid,
 *	                   u32 vid_valid, u32 vid);
 *	Query whether remote GID rgid is reachable via this device and this
 *	vlan id. Vlan id is only checked if vid_valid != 0.
 *
 * int (*register_dmb)(struct ism_dev *dev, struct ism_dmb *dmb,
 *			    void *client);
 *	Register an ism_dmb buffer for this device and this client.
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
 * int (*supports_v2)(void);
 *
 * u16 (*get_chid)(struct ism_dev *dev);
 *	Returns ism fabric identifier (channel id) of this device.
 *	Only devices on the same ism fabric can communicate.
 *	chid is unique per HW system, except for 0xFFFF, which denotes
 *	an ism_loopback device that can only communicate with itself.
 *	Use chid for fast negative checks, but only query_remote_gid()
 *	can give a reliable positive answer.
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
};

/* Unless we gain unexpected popularity, this limit should hold for a while */
#define MAX_CLIENTS		8
#define NO_CLIENT		0xff		/* must be >= MAX_CLIENTS */
#define ISM_NR_DMBS		1920

struct ism_dev {
	const struct ism_ops *ops;
	spinlock_t lock; /* protects the ism device */
	struct list_head list;
	struct pci_dev *pdev;

	struct ism_sba *sba;
	dma_addr_t sba_dma_addr;
	DECLARE_BITMAP(sba_bitmap, ISM_NR_DMBS);
	u8 *sba_client_arr;	/* entries are indices into 'clients' array */
	void *priv[MAX_CLIENTS];

	struct ism_eq *ieq;
	dma_addr_t ieq_dma_addr;

	struct device dev;
	uuid_t gid;
	int ieq_idx;

	struct ism_client *subs[MAX_CLIENTS];
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

#define ISM_RESERVED_VLANID	0x1FFF
#define ISM_ERROR	0xFFFF

#endif	/* _ISM_H */
