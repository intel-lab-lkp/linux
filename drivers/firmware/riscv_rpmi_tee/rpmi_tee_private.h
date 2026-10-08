/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _RPMI_TEE_PRIVATE_H
#define _RPMI_TEE_PRIVATE_H

#include <linux/list.h>
#include <linux/mailbox_client.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/uuid.h>

/* TEE service group and the services used by this module. */
#define RPMI_SRVGRP_TEE		0x10

#define RPMI_TEE_SRV_PROBE_FEATURES	0x02
#define RPMI_TEE_SRV_PROBE_SYSTEM	0x03
#define RPMI_TEE_SRV_PROBE_DOMAIN	0x04
#define RPMI_TEE_SRV_PROBE_ENDPOINT	0x05
#define RPMI_TEE_SRV_CALL		0x18
#define RPMI_TEE_SRV_MEMORY_PARCEL_CREATE	0x0e
#define RPMI_TEE_SRV_MEMORY_PARCEL_RECLAIM	0x11
#define RPMI_TEE_SRV_MEMORY_SEGMENT_SEND	0x12

struct rpmi_tee_notif_state {
	u32 feature;
};

struct rpmi_tee_mbox {
	struct mbox_client client;
	struct mbox_chan *chan;
	u32 max_msg_data_size;
};

struct rpmi_tee_mem_state {
	struct mutex lock;
	u32 multisegment_max;
	u32 multisegment_active;
	bool lend_ok;
	bool share_ok;
};

/**
 * struct rpmi_tee_transport - State for one RPMI TEE transport instance
 * @dev: Parent platform device.
 * @mbox: RPMI mailbox transport state.
 * @max_call_req_size: Maximum TEE_CALL request payload size in bytes.
 * @max_call_resp_size: Maximum TEE_CALL response payload size in bytes.
 * @self_id: Local REE physical endpoint identifier.
 * @devices: List of registered TEE service devices.
 * @mem: Memory parcel operation state.
 * @notif: Signal notification state.
 */
struct rpmi_tee_transport {
	struct device *dev;
	struct rpmi_tee_mbox mbox;
	size_t max_call_req_size;
	size_t max_call_resp_size;
	u32 self_id;
	struct list_head devices;
	struct rpmi_tee_mem_state mem;
	struct rpmi_tee_notif_state notif;
};

/* Report local transport errors separately from the returned RPMI status. */
int rpmi_tee_send_with_status(struct rpmi_tee_transport *priv, u32 service_id,
			      const void *req, size_t req_len, void *resp,
			      size_t *resp_len, s32 *status);

/**
 * struct rpmi_tee_discovered_endpoint - Physical TEE endpoint and its services
 * @node: Entry in &struct rpmi_tee_discovery.eps.
 * @ep_id: Physical TEE endpoint ID.
 * @service_count: Number of UUIDs in @services.
 * @services: Service UUIDs for the endpoint.
 */
struct rpmi_tee_discovered_endpoint {
	struct list_head node;
	u32 ep_id;
	u32 service_count;
	uuid_t services[] __counted_by(service_count);
};

/**
 * struct rpmi_tee_discovery - Discovered physical TEE endpoints and services
 * @eps: List of &struct rpmi_tee_discovered_endpoint entries, excluding the
 *	 caller endpoint.
 *
 * On successful discovery, the caller owns the entries and releases them with
 * rpmi_tee_free_discovery(). Endpoints without services are also included.
 */
struct rpmi_tee_discovery {
	struct list_head eps;
};

void rpmi_tee_free_discovery(struct rpmi_tee_discovery *system);
int rpmi_tee_discover_endpoints(struct rpmi_tee_transport *priv,
				struct rpmi_tee_discovery *system);

#endif /* _RPMI_TEE_PRIVATE_H */
