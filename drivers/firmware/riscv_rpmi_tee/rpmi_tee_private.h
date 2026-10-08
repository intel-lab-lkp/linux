/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _RPMI_TEE_PRIVATE_H
#define _RPMI_TEE_PRIVATE_H

#include <linux/list.h>
#include <linux/mailbox_client.h>
#include <linux/types.h>
#include <linux/uuid.h>

/* TEE service group and the services used by this module. */
#define RPMI_SRVGRP_TEE		0x10

#define RPMI_TEE_SRV_PROBE_SYSTEM	0x03
#define RPMI_TEE_SRV_PROBE_DOMAIN	0x04
#define RPMI_TEE_SRV_PROBE_ENDPOINT	0x05

struct rpmi_tee_mbox {
	struct mbox_client client;
	struct mbox_chan *chan;
	u32 max_msg_data_size;
};

struct rpmi_tee_transport {
	struct device *dev;
	struct rpmi_tee_mbox mbox;
	u32 self_id;
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
