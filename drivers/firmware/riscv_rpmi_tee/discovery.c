// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/cleanup.h>
#include <linux/mailbox/riscv-rpmi-message.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "rpmi_tee_private.h"

/* Role flags in PROBE_DOMAIN and PROBE_ENDPOINT responses. */
#define RPMI_TEE_DOMAIN_TEE		BIT(31)
#define RPMI_TEE_ENDPOINT_TEE		BIT(31)
#define RPMI_TEE_ENDPOINT_PHYSICAL	BIT(30)

/**
 * struct rpmi_tee_probe_system_resp - PROBE_SYSTEM response prefix
 * @status: RPMI completion status.
 * @caller_domain: Calling endpoint's domain ID.
 * @caller_endpoint: Calling physical endpoint ID.
 * @domain_count: Number of entries in @domains.
 * @domains: Reachable domain identifiers.
 */
struct rpmi_tee_probe_system_resp {
	__le32 status;
	__le32 caller_domain;
	__le32 caller_endpoint;
	__le32 domain_count;
	__le32 domains[];
} __packed;

/**
 * struct rpmi_tee_probe_domain_req - PROBE_DOMAIN request
 * @domain_id: Domain identifier from PROBE_SYSTEM.
 */
struct rpmi_tee_probe_domain_req {
	__le32 domain_id;
} __packed;

/**
 * struct rpmi_tee_probe_domain_resp - PROBE_DOMAIN response prefix
 * @status: RPMI completion status.
 * @flags: Domain role flags.
 * @endpoint_count: Number of entries in @endpoints.
 * @endpoints: Physical and proxied endpoint IDs.
 */
struct rpmi_tee_probe_domain_resp {
	__le32 status;
	__le32 flags;
	__le32 endpoint_count;
	__le32 endpoints[];
} __packed;

/**
 * struct rpmi_tee_probe_endpoint_req - PROBE_ENDPOINT request
 * @endpoint_id: Endpoint identifier from PROBE_DOMAIN.
 */
struct rpmi_tee_probe_endpoint_req {
	__le32 endpoint_id;
} __packed;

/**
 * struct rpmi_tee_probe_endpoint_resp - PROBE_ENDPOINT response prefix
 * @status: RPMI completion status.
 * @flags: Endpoint role and lifetime flags.
 * @name: Debug name, not a matching identity.
 * @proxied_count: Number of proxied endpoint IDs in @data.
 * @parcel_count: Number of parcel IDs in @data.
 * @service_count: Number of 16-byte service UUIDs in @data.
 * @metadata_len: Metadata length in bytes.
 * @data: Proxied endpoint IDs, parcel IDs, service UUIDs, then metadata.
 */
struct rpmi_tee_probe_endpoint_resp {
	__le32 status;
	__le32 flags;
	u8 name[32];
	__le32 proxied_count;
	__le32 parcel_count;
	__le32 service_count;
	__le32 metadata_len;
	u8 data[];
} __packed;

struct rpmi_tee_system_info {
	u32 caller_domain;
	u32 caller_endpoint;
	u32 domain_count;
	const u8 *domains;
};

/* Return the domain ID at a validated index in the system response. */
static inline u32
rpmi_tee_domain_id_at(const struct rpmi_tee_system_info *info, u32 index)
{
	return get_unaligned_le32(info->domains +
				  (size_t)index * sizeof(__le32));
}

struct rpmi_tee_domain_info {
	u32 flags;
	u32 endpoint_count;
	const u8 *endpoints;
};

/* Return the endpoint ID at a validated index in the domain response. */
static inline u32
rpmi_tee_endpoint_id_at(const struct rpmi_tee_domain_info *info, u32 index)
{
	return get_unaligned_le32(info->endpoints +
				  (size_t)index * sizeof(__le32));
}

struct rpmi_tee_endpoint_info {
	u32 flags;
	u32 service_count;
	const u8 *services;
};

/* Copy the service UUID at a validated index in the endpoint response. */
static inline void
rpmi_tee_service_uuid_at(const struct rpmi_tee_endpoint_info *info,
			 u32 index, uuid_t *uuid)
{
	import_uuid(uuid, info->services + (size_t)index * UUID_SIZE);
}

/* rpmi_tee_parse_*_response parse and validate the response. */

static int rpmi_tee_parse_system_response(const void *data, size_t len,
					  struct rpmi_tee_system_info *info)
{
	const struct rpmi_tee_probe_system_resp *resp = data;
	u32 count;

	if (len < sizeof(*resp))
		return -EPROTO;

	count = get_unaligned_le32(&resp->domain_count);
	if (len != sizeof(*resp) + (u64)count * sizeof(__le32))
		return -EPROTO;

	info->caller_domain = get_unaligned_le32(&resp->caller_domain);
	info->caller_endpoint = get_unaligned_le32(&resp->caller_endpoint);
	info->domain_count = count;
	/* Borrowed pointer, use rpmi_tee_domain_id_at() to access. */
	info->domains = (const u8 *)data + sizeof(*resp);

	return 0;
}

static int rpmi_tee_parse_domain_response(const void *data, size_t len,
					  struct rpmi_tee_domain_info *info)
{
	const struct rpmi_tee_probe_domain_resp *resp = data;
	u32 count;

	if (len < sizeof(*resp))
		return -EPROTO;

	count = get_unaligned_le32(&resp->endpoint_count);
	if (len != sizeof(*resp) + (u64)count * sizeof(__le32))
		return -EPROTO;

	info->flags = get_unaligned_le32(&resp->flags);
	info->endpoint_count = count;
	/* Borrowed pointer, use rpmi_tee_endpoint_id_at() to access. */
	info->endpoints = (const u8 *)data + sizeof(*resp);

	return 0;
}

static int rpmi_tee_parse_endpoint_response(const void *data, size_t len,
					    struct rpmi_tee_endpoint_info *info)
{
	const struct rpmi_tee_probe_endpoint_resp *resp = data;
	size_t remaining, service_offset;
	u32 proxied, parcels, services;

	if (len < sizeof(*resp))
		return -EPROTO;

	remaining = len - sizeof(*resp);
	proxied = get_unaligned_le32(&resp->proxied_count);
	parcels = get_unaligned_le32(&resp->parcel_count);
	services = get_unaligned_le32(&resp->service_count);

	/* Check both ID arrays before adding their counts. */
	if (proxied > remaining / sizeof(__le32) ||
	    parcels > remaining / sizeof(__le32) - proxied)
		return -EPROTO;

	service_offset = ((size_t)proxied + parcels) * sizeof(__le32);
	remaining -= service_offset;
	if (services > remaining / UUID_SIZE)
		return -EPROTO;

	/* Ignore metadata following the service UUID array. */
	info->flags = get_unaligned_le32(&resp->flags);
	info->service_count = services;
	/* Borrowed pointer, use rpmi_tee_service_uuid_at() to get UUID. */
	info->services = resp->data + service_offset;

	return 0;
}

/* Return an owned probe response; vanished domain/endpoint IDs yield -ENOENT. */
static int rpmi_tee_probe_info(struct rpmi_tee_transport *priv, u32 service,
			       const void *req, size_t req_len, void **data,
			       size_t *data_len)
{
	size_t resp_len = priv->mbox.max_msg_data_size;
	s32 status;
	int ret;

	void *resp __free(kfree) = kzalloc(resp_len, GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	ret = rpmi_tee_send_with_status(priv, service, req, req_len, resp,
					&resp_len, &status);
	if (ret)
		return ret;

	if (status == RPMI_ERR_INVALID_PARAM &&
	    (service == RPMI_TEE_SRV_PROBE_DOMAIN ||
	     service == RPMI_TEE_SRV_PROBE_ENDPOINT))
		return -ENOENT;

	ret = rpmi_to_linux_error(status);
	if (ret)
		return ret;

	*data_len = resp_len;
	*data = no_free_ptr(resp);

	return 0;
}

static int rpmi_tee_add_endpoint(struct rpmi_tee_discovery *system, u32 ep_id,
				 const struct rpmi_tee_endpoint_info *info)
{
	struct rpmi_tee_discovered_endpoint *ep;
	u32 i;

	/* Skip endpoints already discovered. */
	list_for_each_entry(ep, &system->eps, node)
		if (ep->ep_id == ep_id)
			return 0;

	ep = kzalloc(struct_size(ep, services, info->service_count), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;

	ep->ep_id = ep_id;
	ep->service_count = info->service_count;
	for (i = 0; i < ep->service_count; i++)
		rpmi_tee_service_uuid_at(info, i, &ep->services[i]);

	list_add_tail(&ep->node, &system->eps);

	return 0;
}

/* Discover physical TEE endpoints in one domain, skipping vanished IDs. */
static int rpmi_tee_discover_domain(struct rpmi_tee_transport *priv, u32 id,
				    struct rpmi_tee_discovery *system)
{
	struct rpmi_tee_domain_info domain;
	struct rpmi_tee_probe_domain_req req = {
		.domain_id = cpu_to_le32(id),
	};
	size_t len;
	int ret;
	u32 i;

	void *data __free(kfree) = NULL;

	ret = rpmi_tee_probe_info(priv, RPMI_TEE_SRV_PROBE_DOMAIN, &req,
				  sizeof(req), &data, &len);
	if (ret)
		return ret == -ENOENT ? 0 : ret;

	ret = rpmi_tee_parse_domain_response(data, len, &domain);
	/* Only TEE domains are processed. */
	if (ret || !(domain.flags & RPMI_TEE_DOMAIN_TEE))
		return ret;

	for (i = 0; i < domain.endpoint_count; i++) {
		u32 ep_id = rpmi_tee_endpoint_id_at(&domain, i);
		struct rpmi_tee_endpoint_info endpoint;
		struct rpmi_tee_probe_endpoint_req ep_req = {
			.endpoint_id = cpu_to_le32(ep_id),
		};

		void *ep_data __free(kfree) = NULL;

		ret = rpmi_tee_probe_info(priv, RPMI_TEE_SRV_PROBE_ENDPOINT,
					  &ep_req, sizeof(ep_req), &ep_data,
					  &len);
		if (ret == -ENOENT)
			continue;

		if (ret)
			return ret;

		ret = rpmi_tee_parse_endpoint_response(ep_data, len, &endpoint);
		if (ret)
			return ret;

		/* Only physical TEE endpoints are processed. */
		if (ep_id == priv->self_id ||
		    !(endpoint.flags & RPMI_TEE_ENDPOINT_TEE) ||
		    !(endpoint.flags & RPMI_TEE_ENDPOINT_PHYSICAL))
			continue;

		ret = rpmi_tee_add_endpoint(system, ep_id, &endpoint);
		if (ret)
			return ret;
	}

	return 0;
}

/* Free all discovered endpoints and their copied service UUIDs. */
void rpmi_tee_free_discovery(struct rpmi_tee_discovery *system)
{
	struct rpmi_tee_discovered_endpoint *ep, *next;

	list_for_each_entry_safe(ep, next, &system->eps, node) {
		list_del(&ep->node);
		kfree(ep);
	}
}

/**
 * rpmi_tee_discover_endpoints() - Discover physical TEE endpoints and services
 * @priv: RPMI TEE transport
 * @system: Returned list of endpoints and their service UUIDs
 *
 * Obtain caller identity and walk SYSTEM, DOMAIN and ENDPOINT probes. The
 * caller releases the entries with rpmi_tee_free_discovery() on success.
 *
 * Return: 0 on success, or a negative error code.
 */
int rpmi_tee_discover_endpoints(struct rpmi_tee_transport *priv,
				struct rpmi_tee_discovery *system)
{
	struct rpmi_tee_system_info info;
	size_t len;
	int ret;
	u32 i;

	void *data __free(kfree) = NULL;

	INIT_LIST_HEAD(&system->eps);
	/* PROBE_SYSTEM has no request data and returns the caller's identity. */
	ret = rpmi_tee_probe_info(priv, RPMI_TEE_SRV_PROBE_SYSTEM, NULL, 0,
				  &data, &len);
	if (ret)
		return ret;

	ret = rpmi_tee_parse_system_response(data, len, &info);
	if (ret)
		return ret;

	priv->self_id = info.caller_endpoint;
	for (i = 0; i < info.domain_count; i++) {
		u32 domain_id = rpmi_tee_domain_id_at(&info, i);

		ret = rpmi_tee_discover_domain(priv, domain_id, system);
		if (ret) {
			rpmi_tee_free_discovery(system);

			return ret;
		}
	}

	return 0;
}
