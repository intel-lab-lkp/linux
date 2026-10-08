// SPDX-License-Identifier: GPL-2.0-only
/*
 * RISC-V RPMI TEE transport
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/mailbox_client.h>
#include <linux/mailbox/riscv-rpmi-message.h>
#include <linux/cleanup.h>
#include <linux/bitfield.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/rpmi_tee.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "rpmi_tee_private.h"

#define RPMI_TEE_FEATURE_MEMORY_LEND		1
#define RPMI_TEE_FEATURE_MEMORY_SHARE		2
#define RPMI_TEE_FEATURE_SIGNAL_BUS		3
#define RPMI_TEE_FEATURE_MULTISEGMENT_OPS	4

#define RPMI_TEE_MEMORY_FEATURE_UNSUPPORTED		0
#define RPMI_TEE_MEMORY_FEATURE_TEE_ONLY		1
#define RPMI_TEE_MEMORY_FEATURE_FULLY_SUPPORTED	2

/* TEE_CALL memory-access flags and block format. */
#define RPMI_TEE_ACCESS_READ			BIT(29)
#define RPMI_TEE_ACCESS_WRITE			BIT(30)
#define RPMI_TEE_ACCESS_EXEC			BIT(31)

#define RPMI_TEE_MEM_PAGE_SHIFT			12
#define RPMI_TEE_MEM_PAGE_SIZE			BIT(RPMI_TEE_MEM_PAGE_SHIFT)
#define RPMI_TEE_BLOCK_MAX_PAGES		4096

/* TEE_MEMORY_PARCEL_CREATE flags. */
#define RPMI_TEE_PARCEL_MULTI_SEGMENT	BIT(31)

/* TEE_MEMORY_SEGMENT_SEND flags. */
#define RPMI_TEE_SEGMENT_LAST		BIT(31)

/* TEE_SIGNAL_BUS_SETUP feature value and TEE_SIGNAL_RETRIEVE flags. */
#define RPMI_TEE_SIGNAL_MODE_MASK	GENMASK(1, 0)
#define RPMI_TEE_SIGNAL_WIDTH_MASK	GENMASK(11, 2)
#define RPMI_TEE_SIGNAL_INDEX_MASK	GENMASK(31, 12)

#define RPMI_TEE_SIGNAL_MODE_SYSTEM_MSI	1
#define RPMI_TEE_SIGNAL_MORE_AVAILABLE	BIT(31)

/**
 * struct rpmi_tee_probe_features_req - TEE_PROBE_FEATURES request
 * @feature_id: TEE feature identifier to query.
 */
struct rpmi_tee_probe_features_req {
	__le32 feature_id;
} __packed;

/**
 * struct rpmi_tee_probe_features_resp - TEE_PROBE_FEATURES response
 * @status: RPMI completion status.
 * @value: Feature-specific value.
 */
struct rpmi_tee_probe_features_resp {
	__le32 status;
	__le32 value;
} __packed;

/**
 * struct rpmi_tee_call_req - TEE_CALL request prefix
 * @sender_id: Calling REE endpoint identifier.
 * @target_id: Destination TEE endpoint identifier.
 * @service: UUID of the target service.
 * @service_data_len: Length of @service_data in bytes.
 * @service_data: Service-defined request data.
 */
struct rpmi_tee_call_req {
	__le32 sender_id;
	__le32 target_id;
	u8 service[UUID_SIZE];
	__le32 service_data_len;
	u8 service_data[];
} __packed;

/**
 * struct rpmi_tee_call_resp - TEE_CALL response prefix
 * @status: RPMI completion status.
 * @service_data_len: Length of @service_data in bytes.
 * @service_data: Service-defined response data.
 */
struct rpmi_tee_call_resp {
	__le32 status;
	__le32 service_data_len;
	u8 service_data[];
} __packed;

/**
 * struct rpmi_tee_parcel_create_req - MEMORY_PARCEL_CREATE request prefix
 * @creator_id: Endpoint identifier creating the parcel.
 * @creator_access: Creator's residual access permissions.
 * @receiver_count: Number of receiver endpoint and access pairs in @data.
 * @flags: Parcel creation flags.
 * @nonce: Caller-provided parcel nonce.
 * @block_count: Number of memory blocks included in this request.
 * @label: Caller-provided parcel label.
 * @data: Receiver endpoint IDs, receiver access values, then memory blocks.
 */
struct rpmi_tee_parcel_create_req {
	__le32 creator_id;
	__le32 creator_access;
	__le32 receiver_count;
	__le32 flags;
	__le32 nonce;
	__le32 block_count;
	u8 label[16];
	u8 data[];
} __packed;

#define RPMI_TEE_PARCEL_CREATE_SIZE \
	(sizeof(struct rpmi_tee_parcel_create_req))
/* Size of one RECEIVER_ID[] and ACCESS[] entry pair. */
#define RPMI_TEE_PARCEL_CREATE_RECEIVER_INFO_SIZE	(2 * sizeof(__le32))
/* Size of one BLOCK_HIGH[] and BLOCK_LOW[] entry pair. */
#define RPMI_TEE_PARCEL_CREATE_BLOCK_SIZE		(2 * sizeof(__le32))

/* Store one receiver in adjacent RECEIVER_ID[] and ACCESS[] arrays at @data. */
static inline void rpmi_tee_put_receiver(u8 *data, u32 count, u32 index,
					 u32 id, u32 access)
{
	put_unaligned_le32(id, data + index * sizeof(__le32));
	put_unaligned_le32(access, data + (count + index) * sizeof(__le32));
}

/* Store one block in adjacent BLOCK_HIGH[] and BLOCK_LOW[] arrays at @data. */
static inline void rpmi_tee_put_block(u8 *data, u32 count, u32 index,
				      u32 high, u32 low)
{
	put_unaligned_le32(high, data + index * sizeof(__le32));
	put_unaligned_le32(low, data + (count + index) * sizeof(__le32));
}

/**
 * struct rpmi_tee_parcel_create_resp - MEMORY_PARCEL_CREATE response
 * @status: RPMI completion status.
 * @parcel_id: Identifier assigned to the new parcel.
 */
struct rpmi_tee_parcel_create_resp {
	__le32 status;
	__le32 parcel_id;
} __packed;

/**
 * struct rpmi_tee_segment_send_req - MEMORY_SEGMENT_SEND request prefix
 * @parcel_id: Identifier of the partially created parcel.
 * @flags: Segment flags.
 * @block_count: Number of memory blocks in @data.
 * @data: Memory block address and size pairs.
 */
struct rpmi_tee_segment_send_req {
	__le32 parcel_id;
	__le32 flags;
	__le32 block_count;
	u8 data[];
} __packed;

#define RPMI_TEE_SEGMENT_SEND_SIZE \
	(sizeof(struct rpmi_tee_segment_send_req))
/* Size of one BLOCK_HIGH[] and BLOCK_LOW[] entry pair. */
#define RPMI_TEE_SEGMENT_SEND_BLOCK_SIZE	(2 * sizeof(__le32))

/**
 * struct rpmi_tee_parcel_reclaim_req - MEMORY_PARCEL_RECLAIM request
 * @parcel_id: Identifier of the parcel to reclaim.
 */
struct rpmi_tee_parcel_reclaim_req {
	__le32 parcel_id;
} __packed;

/**
 * struct rpmi_tee_parcel_reclaim_resp - MEMORY_PARCEL_RECLAIM response
 * @status: RPMI completion status.
 * @flags: Reclaim result flags.
 */
struct rpmi_tee_parcel_reclaim_resp {
	__le32 status;
	__le32 flags;
} __packed;

/**
 * struct rpmi_tee_signal_bus_setup_req - SIGNAL_BUS_SETUP request
 * @target_id: TEE endpoint identifier that owns the signal bus.
 * @bus_width: Total number of signal IDs in the bus.
 * @sender_signals: Number of signal IDs reserved for the endpoint sender.
 */
struct rpmi_tee_signal_bus_setup_req {
	__le32 target_id;
	__le32 bus_width;
	__le32 sender_signals;
} __packed;

/**
 * struct rpmi_tee_signal_bus_teardown_req - SIGNAL_BUS_TEARDOWN request
 * @target_id: TEE endpoint identifier that owns the signal bus.
 */
struct rpmi_tee_signal_bus_teardown_req {
	__le32 target_id;
} __packed;

/**
 * struct rpmi_tee_signal_raise_req - SIGNAL_RAISE request prefix
 * @target_id: TEE endpoint identifier that owns the signal bus.
 * @signal_count: Number of signal IDs in @signals.
 * @signals: Signal IDs to raise.
 */
struct rpmi_tee_signal_raise_req {
	__le32 target_id;
	__le32 signal_count;
	__le32 signals[];
} __packed;

/**
 * struct rpmi_tee_signal_raise_one_req - Single-signal SIGNAL_RAISE request
 * @target_id: TEE endpoint identifier that owns the signal bus.
 * @signal_count: Must be one.
 * @signal: Signal ID to raise.
 */
struct rpmi_tee_signal_raise_one_req {
	__le32 target_id;
	__le32 signal_count;
	__le32 signal;
} __packed;

/**
 * struct rpmi_tee_signal_retrieve_resp - SIGNAL_RETRIEVE response prefix
 * @status: RPMI completion status.
 * @flags: Response flags.
 * @target_id: TEE endpoint identifier that owns the signal bus.
 * @signal_count: Number of signal IDs in @signals.
 * @signals: Retrieved signal IDs.
 */
struct rpmi_tee_signal_retrieve_resp {
	__le32 status;
	__le32 flags;
	__le32 target_id;
	__le32 signal_count;
	__le32 signals[];
} __packed;

enum rpmi_tee_signal_state {
	RPMI_TEE_SIGNAL_ACTIVE,
	RPMI_TEE_SIGNAL_RELEASING,
};

struct rpmi_tee_signal_reservation {
	struct rpmi_tee_device *rdev;
	rpmi_tee_notifier_cb cb;
	void *cb_data;
	enum rpmi_tee_signal_state state;
};

struct rpmi_tee_signal_bus {
	struct list_head node;	/* Link in the notification signal-bus list. */
	struct mutex lock;	/* Serializes reservation state and lifetime. */
	struct xarray reservations;
	u32 endpoint_id;
	u32 width;
	u32 tee_to_ree_count;
};

struct rpmi_tee_child {
	struct list_head node;
	struct rpmi_tee_device *rdev;
};

struct rpmi_tee_block_iter {
	struct scatterlist *sg;
	phys_addr_t address;
	size_t length;
};

struct rpmi_tee_parcel_xfer {
	struct rpmi_tee_block_iter iter;
	u32 parcel_id;
	u32 block_count;
	u32 next_block;
};

/* rpmi_tee_send_with_status() - Send an RPMI TEE service request. */
int rpmi_tee_send_with_status(struct rpmi_tee_transport *priv, u32 service_id,
			      const void *req, size_t req_len, void *resp,
			      size_t *resp_len, s32 *status)
{
	size_t max_resp_len = *resp_len;
	struct rpmi_mbox_message msg;
	int ret;

	if (req_len > priv->mbox.max_msg_data_size ||
	    max_resp_len > priv->mbox.max_msg_data_size)
		return -EMSGSIZE;

	rpmi_mbox_init_send_with_response(&msg, service_id, (void *)req,
					  req_len, resp, max_resp_len);
	ret = rpmi_mbox_send_message_sync(priv->mbox.chan, &msg);
	if (ret)
		return ret;
	/* At least STATUS word should be present. */
	if (msg.data.out_response_len < sizeof(__le32))
		return -EPROTO;

	*resp_len = msg.data.out_response_len;
	*status = (s32)get_unaligned_le32(resp);

	return 0;
}

/**
 * rpmi_tee_send() - Send an RPMI TEE service request
 * @priv: RPMI TEE transport
 * @service_id: RPMI TEE service identifier
 * @req: Request data
 * @req_len: Request data length
 * @resp: Response data buffer, or %NULL for a status-only response
 * @resp_len: On entry, response buffer capacity; on success, response length
 *
 * Pass both @resp and @resp_len as %NULL when the service has no response
 * payload beyond the mandatory RPMI status word.
 *
 * Return: 0 on success, or a negative error code.
 */
static int rpmi_tee_send(struct rpmi_tee_transport *priv, u32 service_id,
			 const void *req, size_t req_len, void *resp,
			 size_t *resp_len)
{
	__le32 status_resp;
	size_t status_resp_len = sizeof(status_resp);
	s32 status;
	int ret;

	if (!resp && !resp_len) {
		resp = &status_resp;
		resp_len = &status_resp_len;
	} else if (!resp || !resp_len) {
		return -EINVAL;
	}

	ret = rpmi_tee_send_with_status(priv, service_id, req, req_len, resp,
					resp_len, &status);
	if (ret)
		return ret;

	if (status == RPMI_ERR_NO_DATA)
		return -ENODATA;

	return rpmi_to_linux_error(status);
}

/**
 * rpmi_tee_get_attr() - Get an RPMI mailbox attribute
 * @priv: RPMI TEE transport
 * @id: Attribute identifier
 * @value: Returned attribute value
 *
 * Return: 0 on success, or a negative error code on failure.
 */
static int rpmi_tee_get_attr(struct rpmi_tee_transport *priv,
			     enum rpmi_mbox_attribute_id id, u32 *value)
{
	struct rpmi_mbox_message msg;
	int ret;

	rpmi_mbox_init_get_attribute(&msg, id);
	ret = rpmi_mbox_send_message_sync(priv->mbox.chan, &msg);
	if (ret)
		return ret;

	*value = msg.attr.value;

	return 0;
}

/* Validate the RPMI mailbox transport and cache its message size. */
static int rpmi_tee_check_transport(struct rpmi_tee_transport *priv)
{
	u32 value;
	int ret;

	ret = rpmi_tee_get_attr(priv, RPMI_MBOX_ATTR_SPEC_VERSION, &value);
	if (ret)
		return ret;
	if (value < RPMI_MKVER(1, 0))
		return -EPROTONOSUPPORT;

	ret = rpmi_tee_get_attr(priv, RPMI_MBOX_ATTR_SERVICEGROUP_ID, &value);
	if (ret)
		return ret;
	if (value != RPMI_SRVGRP_TEE)
		return -ENODEV;

	ret = rpmi_tee_get_attr(priv, RPMI_MBOX_ATTR_SERVICEGROUP_VERSION,
				&value);
	if (ret)
		return ret;
	if (value < RPMI_MKVER(1, 0))
		return -EPROTONOSUPPORT;

	ret = rpmi_tee_get_attr(priv, RPMI_MBOX_ATTR_MAX_MSG_DATA_SIZE, &value);
	if (ret)
		return ret;
	/* The mandatory TEE_CALL request and response must fit the mailbox. */
	if (value < sizeof(struct rpmi_tee_call_req) ||
	    value < sizeof(struct rpmi_tee_call_resp))
		return -EMSGSIZE;

	priv->mbox.max_msg_data_size = value;
	priv->max_call_req_size = value - sizeof(struct rpmi_tee_call_req);
	priv->max_call_resp_size = value - sizeof(struct rpmi_tee_call_resp);

	return 0;
}

/* RPMI TEE SERVICE GRP API. */

/* TEE_PROBE_FEATURES. */
static int rpmi_tee_probe_features(struct rpmi_tee_transport *priv,
				   u32 feature_id, u32 *value)
{
	struct rpmi_tee_probe_features_req req = {
		.feature_id = cpu_to_le32(feature_id),
	};
	struct rpmi_tee_probe_features_resp resp;
	size_t resp_len = sizeof(resp);
	int ret;

	ret = rpmi_tee_send(priv, RPMI_TEE_SRV_PROBE_FEATURES, &req,
			    sizeof(req), &resp, &resp_len);
	if (ret)
		return ret;
	if (resp_len != sizeof(resp))
		return -EPROTO;

	*value = get_unaligned_le32(&resp.value);

	return 0;
}

/* Return the transport that owns @rdev. */
static struct rpmi_tee_transport *
rpmi_tee_device_to_transport(struct rpmi_tee_device *rdev)
{
	return dev_get_drvdata(rdev->dev.parent);
}

static int rpmi_tee_op_msg_limits_get(struct rpmi_tee_device *rdev,
				      struct rpmi_tee_msg_limits *limits)
{
	struct rpmi_tee_transport *priv = rpmi_tee_device_to_transport(rdev);

	if (!limits)
		return -EINVAL;

	limits->max_req_size = priv->max_call_req_size;
	limits->max_resp_size = priv->max_call_resp_size;

	return 0;
}

/**
 * rpmi_tee_op_call - Invoke a service offered by a TEE endpoint
 * @rdev: TEE service device.
 * @req: Service-defined request data.
 * @req_len: Length of @req in bytes.
 * @resp: Buffer for service-defined response data.
 * @resp_len: On entry, capacity of @resp; on success, response length.
 *
 * MPXY can return -ENOSPC after the TEE has processed the request when the
 * response exceeds the supplied buffer. Callers must not blindly retry a
 * non-idempotent request in that case.
 *
 * Return: 0 on success, or a negative error code.
 */
static int rpmi_tee_op_call(struct rpmi_tee_device *rdev, const void *req,
			    size_t req_len, void *resp, size_t *resp_len)
{
	struct rpmi_tee_transport *priv = rpmi_tee_device_to_transport(rdev);
	size_t call_req_len, call_resp_len;
	u32 service_data_len;
	int ret;

	if (!resp_len || (!req && req_len) || (!resp && *resp_len))
		return -EINVAL;

	/* TEE_CALL payload must fit the mailbox. */
	if (req_len > priv->max_call_req_size ||
	    *resp_len > priv->max_call_resp_size)
		return -EMSGSIZE;

	call_req_len = sizeof(struct rpmi_tee_call_req) + req_len;
	call_resp_len = sizeof(struct rpmi_tee_call_resp) + *resp_len;

	struct rpmi_tee_call_req *call_req __free(kfree) =
		kzalloc(call_req_len, GFP_KERNEL);
	if (!call_req)
		return -ENOMEM;

	struct rpmi_tee_call_resp *call_resp __free(kfree) =
		kzalloc(call_resp_len, GFP_KERNEL);
	if (!call_resp)
		return -ENOMEM;

	call_req->sender_id = cpu_to_le32(priv->self_id);
	call_req->target_id = cpu_to_le32(rdev->endpoint_id);
	export_uuid(call_req->service, &rdev->uuid);
	call_req->service_data_len = cpu_to_le32(req_len);
	if (req_len)
		memcpy(call_req->service_data, req, req_len);
	/* Make TEE CALL. */
	ret = rpmi_tee_send(priv, RPMI_TEE_SRV_CALL, call_req, call_req_len,
			    call_resp, &call_resp_len);
	if (ret)
		return ret;

	if (call_resp_len < sizeof(*call_resp))
		return -EPROTO;
	service_data_len = get_unaligned_le32(&call_resp->service_data_len);
	/* Verify the firmware-reported length fits the actual response. */
	if (service_data_len != call_resp_len - sizeof(*call_resp))
		return -EPROTO;

	if (service_data_len)
		memcpy(resp, call_resp->service_data, service_data_len);
	*resp_len = service_data_len;

	return 0;
}

/* MEMORY_PARCEL_RECLAIM. */
static int rpmi_tee_memory_reclaim(struct rpmi_tee_transport *priv,
				   u32 parcel_id)
{
	struct rpmi_tee_parcel_reclaim_req req = {
		.parcel_id = cpu_to_le32(parcel_id),
	};
	struct rpmi_tee_parcel_reclaim_resp resp;
	size_t resp_len = sizeof(resp);
	int ret;

	ret = rpmi_tee_send(priv, RPMI_TEE_SRV_MEMORY_PARCEL_RECLAIM, &req,
			    sizeof(req), &resp, &resp_len);
	if (ret)
		return ret;

	if (resp_len != sizeof(resp))
		return -EPROTO;

	return 0;
}

static int rpmi_tee_op_memory_reclaim(struct rpmi_tee_device *rdev,
				       u32 parcel_id)
{
	return rpmi_tee_memory_reclaim(rpmi_tee_device_to_transport(rdev),
				       parcel_id);
}

/**
 * rpmi_tee_count_blocks_sg - Count RPMI memory blocks in an SG list
 * @sg: First SG entry describing the memory to share or lend.
 * @count_out: Returns the number of RPMI memory blocks.
 *
 * Validates that every entry represents one or more whole 4 KiB pages. An
 * RPMI memory block represents at most @RPMI_TEE_BLOCK_MAX_PAGES pages.
 *
 * Return: 0 on success, or a negative error code.
 */
static int rpmi_tee_count_blocks_sg(struct scatterlist *sg, u32 *count_out)
{
	struct scatterlist *entry;
	u32 count = 0;

	if (!sg)
		return -EINVAL;

	for (entry = sg; entry; entry = sg_next(entry)) {
		phys_addr_t address = sg_phys(entry);
		size_t blocks;

		if (!entry->length ||
		    !IS_ALIGNED(address, RPMI_TEE_MEM_PAGE_SIZE) ||
		    !IS_ALIGNED(entry->length, RPMI_TEE_MEM_PAGE_SIZE))
			return -EINVAL;

		/* One RPMI block describes at most 4096 pages. */
		blocks = DIV_ROUND_UP(entry->length >> RPMI_TEE_MEM_PAGE_SHIFT,
				      RPMI_TEE_BLOCK_MAX_PAGES);
		if (blocks > U32_MAX - count)
			return -EOVERFLOW;

		count += blocks;
	}

	*count_out = count;

	return 0;
}

static void rpmi_tee_block_iter_init(struct rpmi_tee_block_iter *iter,
				     struct scatterlist *sg)
{
	iter->sg = sg;
	iter->address = 0;
	iter->length = 0;
}

/* Encode the next RPMI memory block from an SG iterator. */
static bool rpmi_tee_block_iter_next(struct rpmi_tee_block_iter *iter,
				     u32 *high, u32 *low)
{
	u32 pages;

	if (!iter->length) {
		if (!iter->sg)
			return false;
		/* Next SG. */
		iter->address = sg_phys(iter->sg);
		iter->length = iter->sg->length;
		iter->sg = sg_next(iter->sg);
	}

	/* RPMI memory block represents at most @RPMI_TEE_BLOCK_MAX_PAGES pages. */
	pages = min_t(size_t, iter->length >> RPMI_TEE_MEM_PAGE_SHIFT,
		      RPMI_TEE_BLOCK_MAX_PAGES);

	*high = upper_32_bits(iter->address);
	*low = lower_32_bits(iter->address) | (pages - 1);

	iter->address += (phys_addr_t)pages << RPMI_TEE_MEM_PAGE_SHIFT;
	iter->length -= (size_t)pages << RPMI_TEE_MEM_PAGE_SHIFT;

	return true;
}

static int rpmi_tee_fill_blocks(struct rpmi_tee_block_iter *iter, u8 *data,
				u32 count)
{
	u32 high, low, i;

	for (i = 0; i < count; i++) {
		if (!rpmi_tee_block_iter_next(iter, &high, &low))
			return -EINVAL;

		rpmi_tee_put_block(data, count, i, high, low);
	}

	return 0;
}

/* Convert memory access flags RPMI_TEE_MEM_ACCESS_* to RPMI_TEE_ACCESS_*. */
static u32 rpmi_tee_access(u32 mem_access)
{
	u32 tee_access = 0;

	if (mem_access & RPMI_TEE_MEM_ACCESS_READ)
		tee_access |= RPMI_TEE_ACCESS_READ;
	if (mem_access & RPMI_TEE_MEM_ACCESS_WRITE)
		tee_access |= RPMI_TEE_ACCESS_WRITE;
	if (mem_access & RPMI_TEE_MEM_ACCESS_EXEC)
		tee_access |= RPMI_TEE_ACCESS_EXEC;

	return tee_access;
}

static int rpmi_tee_reserve_segment_slot(struct rpmi_tee_transport *priv)
{
	int ret = 0;

	guard(mutex)(&priv->mem.lock);
	if (priv->mem.multisegment_active == priv->mem.multisegment_max)
		ret = -EBUSY;
	else
		priv->mem.multisegment_active++;

	return ret;
}

static void rpmi_tee_release_segment_slot(struct rpmi_tee_transport *priv)
{
	guard(mutex)(&priv->mem.lock);
	priv->mem.multisegment_active--;
}

/* Send the remaining blocks of a segmented memory parcel. */
static int rpmi_tee_parcel_send_segments(struct rpmi_tee_transport *priv,
					 struct rpmi_tee_parcel_xfer *xfer)
{
	while (xfer->next_block < xfer->block_count) {
		size_t req_len;
		u32 count;
		int ret;

		count = min_t(u32, xfer->block_count - xfer->next_block,
			      (priv->mbox.max_msg_data_size -
				RPMI_TEE_SEGMENT_SEND_SIZE) /
				RPMI_TEE_SEGMENT_SEND_BLOCK_SIZE);
		if (!count)
			return -EMSGSIZE;

		req_len = RPMI_TEE_SEGMENT_SEND_SIZE +
			RPMI_TEE_SEGMENT_SEND_BLOCK_SIZE * count;

		struct rpmi_tee_segment_send_req *req __free(kfree) =
			kzalloc(req_len, GFP_KERNEL);
		if (!req)
			return -ENOMEM;

		/* INIT request. */
		req->parcel_id = cpu_to_le32(xfer->parcel_id);
		req->flags = cpu_to_le32(xfer->next_block + count ==
					 xfer->block_count ?
					 RPMI_TEE_SEGMENT_LAST : 0);
		req->block_count = cpu_to_le32(count);
		ret = rpmi_tee_fill_blocks(&xfer->iter, req->data, count);
		if (ret)
			return ret;

		ret = rpmi_tee_send(priv, RPMI_TEE_SRV_MEMORY_SEGMENT_SEND,
				    req, req_len, NULL, NULL);
		if (ret)
			return ret;

		xfer->next_block += count;
	}

	return 0;
}

/* Create a memory parcel after the caller has validated its operation. */
static int rpmi_tee_parcel_create(struct rpmi_tee_transport *priv,
				  struct rpmi_tee_mem_args *args)
{
	struct rpmi_tee_parcel_create_resp resp;
	struct rpmi_tee_parcel_xfer xfer;
	size_t blk_off, req_len, resp_len;
	bool segmented;
	int ret;
	u32 i;

	ret = rpmi_tee_count_blocks_sg(args->sg, &xfer.block_count);
	if (ret)
		return ret;

	/* BLOCK_HIGH[] follows the request header and receiver arrays. */
	blk_off = RPMI_TEE_PARCEL_CREATE_SIZE + args->receiver_count *
		RPMI_TEE_PARCEL_CREATE_RECEIVER_INFO_SIZE;

	/* Limit the initial request to the parcel's actual block count. */
	xfer.next_block = min((priv->mbox.max_msg_data_size - blk_off) /
			      RPMI_TEE_PARCEL_CREATE_BLOCK_SIZE,
			      xfer.block_count);

	segmented = xfer.next_block < xfer.block_count;
	if (segmented) {
		if (!priv->mem.multisegment_max)
			return -EOPNOTSUPP;
		/* Reserve a slot against the firmware's advertised limit. */
		ret = rpmi_tee_reserve_segment_slot(priv);
		if (ret)
			return ret;
	}

	req_len = blk_off + RPMI_TEE_PARCEL_CREATE_BLOCK_SIZE * xfer.next_block;

	struct rpmi_tee_parcel_create_req *req __free(kfree) =
		kzalloc(req_len, GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto out_release_slot;
	}

	rpmi_tee_block_iter_init(&xfer.iter, args->sg);

	/* INIT request. */
	req->creator_id = cpu_to_le32(priv->self_id);
	req->creator_access = cpu_to_le32(rpmi_tee_access(args->creator_access));
	req->receiver_count = cpu_to_le32(args->receiver_count);
	req->flags = cpu_to_le32(segmented ? RPMI_TEE_PARCEL_MULTI_SEGMENT : 0);
	req->nonce = cpu_to_le32(args->nonce);
	req->block_count = cpu_to_le32(xfer.next_block);
	memcpy(req->label, args->label, sizeof(req->label));

	for (i = 0; i < args->receiver_count; i++) {
		u32 tee_access = rpmi_tee_access(args->receivers[i].access);
		/* Store RECEIVER_ID[i] and ACCESS[i]. */
		rpmi_tee_put_receiver(req->data, args->receiver_count, i,
				      args->receivers[i].endpoint_id,
				      tee_access);
	}

	ret = rpmi_tee_fill_blocks(&xfer.iter,
				   req->data + 8 * args->receiver_count,
				   xfer.next_block);
	if (ret)
		goto out_release_slot;

	resp_len = sizeof(resp);
	ret = rpmi_tee_send(priv, RPMI_TEE_SRV_MEMORY_PARCEL_CREATE,
			    req, req_len, &resp, &resp_len);
	if (ret)
		goto out_release_slot;
	if (resp_len != sizeof(resp))
		return -EPROTO;

	xfer.parcel_id = get_unaligned_le32(&resp.parcel_id);
	/* Send remaining blocks as segments. */
	ret = rpmi_tee_parcel_send_segments(priv, &xfer);
	if (ret) {
		/* On error, retain the slot as firmware may still hold it. */
		if (rpmi_tee_memory_reclaim(priv, xfer.parcel_id)) {
			dev_warn(priv->dev, "failed to abort parcel %#x\n",
				 xfer.parcel_id);

			return ret;
		}
	} else {
		args->parcel_id = xfer.parcel_id;
	}

out_release_slot:
	if (segmented)
		rpmi_tee_release_segment_slot(priv);

	return ret;
}

/* MEMORY_PARCEL_CREATE. */
static int rpmi_tee_op_parcel_create(struct rpmi_tee_device *rdev,
				     struct rpmi_tee_mem_args *args, bool lend)
{
	struct rpmi_tee_transport *priv = rpmi_tee_device_to_transport(rdev);
	u32 i;

	if (!args || !args->receivers || !args->receiver_count)
		return -EINVAL;

	/* LEND relinquishes creator access, whereas SHARE retains it. */
	if (lend ? args->creator_access : !args->creator_access)
		return -EINVAL;

	if (args->creator_access & ~RPMI_TEE_MEM_ACCESS_MASK)
		return -EINVAL;
	for (i = 0; i < args->receiver_count; i++) {
		if (args->receivers[i].access & ~RPMI_TEE_MEM_ACCESS_MASK)
			return -EINVAL;
	}

	if (lend ? !priv->mem.lend_ok : !priv->mem.share_ok)
		return -EOPNOTSUPP;

	/* A parcel must contain at least one BLOCK_HIGH/BLOCK_LOW pair. */
	if (priv->mbox.max_msg_data_size < RPMI_TEE_PARCEL_CREATE_SIZE +
	    RPMI_TEE_PARCEL_CREATE_BLOCK_SIZE)
		return -EMSGSIZE;

	/* Check if receiver's info fit after reserving room for one block. */
	if (args->receiver_count >
	    (priv->mbox.max_msg_data_size - RPMI_TEE_PARCEL_CREATE_SIZE -
	     RPMI_TEE_PARCEL_CREATE_BLOCK_SIZE) /
	    RPMI_TEE_PARCEL_CREATE_RECEIVER_INFO_SIZE)
		return -EMSGSIZE;

	return rpmi_tee_parcel_create(priv, args);
}

static int rpmi_tee_op_memory_lend(struct rpmi_tee_device *rdev,
				   struct rpmi_tee_mem_args *args)
{
	return rpmi_tee_op_parcel_create(rdev, args, true);
}

static int rpmi_tee_op_memory_share(struct rpmi_tee_device *rdev,
				    struct rpmi_tee_mem_args *args)
{
	return rpmi_tee_op_parcel_create(rdev, args, false);
}

static struct rpmi_tee_signal_bus *
__rpmi_tee_find_signal_bus(struct rpmi_tee_transport *priv, u32 endpoint_id)
{
	struct rpmi_tee_signal_bus *bus;

	list_for_each_entry(bus, &priv->notif.buses, node) {
		if (bus->endpoint_id == endpoint_id)
			return bus;
	}

	return NULL;
}

static struct rpmi_tee_signal_bus *
rpmi_tee_find_signal_bus(struct rpmi_tee_transport *priv, u32 endpoint_id)
{
	lockdep_assert_held(&priv->notif.ops_lock);
	/* Do not access signal buses after notification shutdown starts. */
	if (priv->notif.shutting_down)
		return NULL;

	return __rpmi_tee_find_signal_bus(priv, endpoint_id);
}

/* Invoke an active signal callback without holding the bus lock. */
static int rpmi_tee_dispatch_signal(struct rpmi_tee_signal_bus *bus,
				    u32 signal)
{
	struct rpmi_tee_signal_reservation *resv;
	rpmi_tee_notifier_cb cb = NULL;
	struct rpmi_tee_device *rdev = NULL;
	void *cb_data = NULL;

	if (signal >= bus->tee_to_ree_count)
		return -EPROTO;

	scoped_guard(mutex, &bus->lock) {
		resv = xa_load(&bus->reservations, signal);
		if (resv && resv->state == RPMI_TEE_SIGNAL_ACTIVE) {
			cb = resv->cb;
			cb_data = resv->cb_data;
			rdev = resv->rdev;
		}
	}

	if (cb)
		cb(rdev, signal, cb_data);

	return 0;
}

/* Release signal IDs that have passed the empty-retrieval barrier. */
static void rpmi_tee_signal_bus_drop_releasing(struct rpmi_tee_signal_bus *bus)
{
	struct rpmi_tee_signal_reservation *resv;
	unsigned long index;

	guard(mutex)(&bus->lock);
	xa_for_each(&bus->reservations, index, resv) {
		if (resv->state != RPMI_TEE_SIGNAL_RELEASING)
			continue;

		xa_erase(&bus->reservations, index);
		kfree(resv);
	}
}

/**
 * rpmi_tee_retrieve_signals() - Drain pending TEE-to-REE signals
 * @priv: RPMI TEE transport
 *
 * Retrieve and dispatch signals until the firmware reports no pending data.
 * Return relinquished signal IDs to their buses only after that empty
 * retrieval.
 *
 * Return: 0 on success, or a negative error code.
 */
static int rpmi_tee_retrieve_signals(struct rpmi_tee_transport *priv)
{
	size_t resp_len = priv->mbox.max_msg_data_size;
	u32 flags, endpoint_id, signal_count, i;
	struct rpmi_tee_signal_bus *bus;
	s32 status;
	int ret;

	struct rpmi_tee_signal_retrieve_resp *resp __free(kfree) =
		kzalloc(resp_len, GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	for (;;) {
		scoped_guard(mutex, &priv->notif.ops_lock) {
			/* Stop retrieving so shutdown can drain the worker. */
			if (priv->notif.shutting_down)
				return 0;

			resp_len = priv->mbox.max_msg_data_size;
			ret = rpmi_tee_send_with_status(priv,
					RPMI_TEE_SRV_SIGNAL_RETRIEVE,
					NULL, 0, resp, &resp_len, &status);
			/*
			 * A signal may be raised after a clear MORE_AVAILABLE
			 * response and before relinquish. Reusing the ID could
			 * deliver it to the wrong client.
			 * Reuse relinquished IDs only after an empty retrieve.
			 */
			if (!ret && status == RPMI_ERR_NO_DATA) {
				struct rpmi_tee_signal_bus *bus;

				list_for_each_entry(bus, &priv->notif.buses, node)
					rpmi_tee_signal_bus_drop_releasing(bus);
			}
		}

		if (ret)
			return ret;
		/* The firmware has no more pending signals. */
		if (status == RPMI_ERR_NO_DATA)
			return 0;
		if (status)
			return rpmi_to_linux_error(status);
		if (resp_len < sizeof(*resp))
			return -EPROTO;

		flags = get_unaligned_le32(&resp->flags);
		endpoint_id = get_unaligned_le32(&resp->target_id);
		signal_count = get_unaligned_le32(&resp->signal_count);

		/* Validate the response flags and its variable-length signal array. */
		if ((flags & ~RPMI_TEE_SIGNAL_MORE_AVAILABLE) || !signal_count ||
		    signal_count != (resp_len - sizeof(*resp)) / sizeof(__le32))
			return -EPROTO;

		bus = __rpmi_tee_find_signal_bus(priv, endpoint_id);
		if (!bus || signal_count > bus->tee_to_ree_count)
			return -EPROTO;

		for (i = 0; i < signal_count; i++) {
			u32 signal = get_unaligned_le32(&resp->signals[i]);

			ret = rpmi_tee_dispatch_signal(bus, signal);
			if (ret)
				return ret;
		}
	}
}

static void rpmi_tee_notif_work(struct work_struct *work)
{
	struct rpmi_tee_notif_state *notif =
		container_of(work, struct rpmi_tee_notif_state, work);
	struct rpmi_tee_transport *priv =
		container_of(notif, struct rpmi_tee_transport, notif);
	int ret;

	ret = rpmi_tee_retrieve_signals(priv);
	if (ret)
		dev_warn(priv->dev, "failed to retrieve signals: %d\n", ret);
}

static irqreturn_t rpmi_tee_notif_irq_handler(int irq, void *data)
{
	struct rpmi_tee_transport *priv = data;

	queue_work(priv->notif.wq, &priv->notif.work);
	return IRQ_HANDLED;
}

/* Reserve a TEE-to-REE signal for a notification consumer. */
static int rpmi_tee_op_notify_request(struct rpmi_tee_device *rdev,
				      rpmi_tee_notifier_cb cb, void *cb_data,
				      u32 *signal)
{
	struct rpmi_tee_transport *priv = rpmi_tee_device_to_transport(rdev);
	struct rpmi_tee_signal_bus *bus;
	u32 id;

	if (!cb || !signal)
		return -EINVAL;

	guard(mutex)(&priv->notif.ops_lock);
	bus = rpmi_tee_find_signal_bus(priv, rdev->endpoint_id);
	if (!bus)
		return -EOPNOTSUPP;

	struct rpmi_tee_signal_reservation *resv __free(kfree) =
		kzalloc_obj(*resv, GFP_KERNEL);
	if (!resv)
		return -ENOMEM;

	resv->rdev = rdev;
	resv->cb = cb;
	resv->cb_data = cb_data;
	scoped_guard(mutex, &bus->lock) {
		int ret;

		ret = xa_alloc(&bus->reservations, &id, resv,
			       XA_LIMIT(0, bus->tee_to_ree_count - 1),
			       GFP_KERNEL);
		if (ret)
			return ret == -EBUSY ? -ENOSPC : ret;
	}

	*signal = id;
	/* xa_alloc owns resv. */
	retain_and_null_ptr(resv);

	return 0;
}

/* Relinquish a previously reserved TEE-to-REE signal. */
static int rpmi_tee_op_notify_relinquish(struct rpmi_tee_device *rdev,
					 u32 signal)
{
	struct rpmi_tee_transport *priv = rpmi_tee_device_to_transport(rdev);
	struct rpmi_tee_signal_bus *bus;

	guard(mutex)(&priv->notif.ops_lock);
	bus = rpmi_tee_find_signal_bus(priv, rdev->endpoint_id);
	if (!bus)
		return -EOPNOTSUPP;

	if (signal >= bus->tee_to_ree_count)
		return -EINVAL;

	scoped_guard(mutex, &bus->lock) {
		struct rpmi_tee_signal_reservation *resv;

		resv = xa_load(&bus->reservations, signal);
		if (!resv)
			return -ENOENT;
		/* Release only if @signal belongs to @rdev. */
		if (resv->rdev != rdev)
			return -EPERM;
		if (resv->state == RPMI_TEE_SIGNAL_RELEASING)
			return -EALREADY;

		resv->state = RPMI_TEE_SIGNAL_RELEASING;
	}

	queue_work(priv->notif.wq, &priv->notif.work);

	return 0;
}

/* Raise an REE-to-TEE signal. */
static int rpmi_tee_op_signal_raise(struct rpmi_tee_device *rdev, u32 signal)
{
	struct rpmi_tee_transport *priv = rpmi_tee_device_to_transport(rdev);
	struct rpmi_tee_signal_raise_one_req req = {
		.target_id = cpu_to_le32(rdev->endpoint_id),
		.signal_count = cpu_to_le32(1),
		.signal = cpu_to_le32(signal),
	};
	struct rpmi_tee_signal_bus *bus;

	guard(mutex)(&priv->notif.ops_lock);
	bus = rpmi_tee_find_signal_bus(priv, rdev->endpoint_id);
	if (!bus)
		return -EOPNOTSUPP;

	if (signal < bus->tee_to_ree_count || signal >= bus->width)
		return -EINVAL;

	return rpmi_tee_send(priv, RPMI_TEE_SRV_SIGNAL_RAISE, &req,
			     sizeof(req), NULL, NULL);
}

static const struct rpmi_tee_notifier_ops rpmi_tee_notifier_ops = {
	.notify_request = rpmi_tee_op_notify_request,
	.notify_relinquish = rpmi_tee_op_notify_relinquish,
	.signal_raise = rpmi_tee_op_signal_raise,
};

static const struct rpmi_tee_info_ops rpmi_tee_info_ops = {
	.msg_limits_get = rpmi_tee_op_msg_limits_get,
};

static const struct rpmi_tee_msg_ops rpmi_tee_msg_ops = {
	.call = rpmi_tee_op_call,
};

static const struct rpmi_tee_mem_ops rpmi_tee_mem_ops = {
	.memory_lend = rpmi_tee_op_memory_lend,
	.memory_share = rpmi_tee_op_memory_share,
	.memory_reclaim = rpmi_tee_op_memory_reclaim,
};

static const struct rpmi_tee_ops rpmi_tee_ops = {
	.info_ops = &rpmi_tee_info_ops,
	.msg_ops = &rpmi_tee_msg_ops,
	.mem_ops = &rpmi_tee_mem_ops,
	.notifier_ops = &rpmi_tee_notifier_ops,
};

static void rpmi_tee_unregister_devices(struct rpmi_tee_transport *priv)
{
	struct rpmi_tee_child *child, *tmp;

	list_for_each_entry_safe(child, tmp, &priv->devices, node) {
		list_del(&child->node);
		rpmi_tee_device_unregister(child->rdev);
		kfree(child);
	}
}

static void rpmi_tee_signal_bus_destroy_resvs(struct rpmi_tee_signal_bus *bus)
{
	struct rpmi_tee_signal_reservation *resv;
	unsigned long index;

	xa_for_each(&bus->reservations, index, resv) {
		xa_erase(&bus->reservations, index);
		kfree(resv);
	}

	xa_destroy(&bus->reservations);
}

static void rpmi_tee_teardown_signal_buses(struct rpmi_tee_transport *priv)
{
	struct rpmi_tee_signal_bus *bus, *tmp;

	list_for_each_entry_safe(bus, tmp, &priv->notif.buses, node) {
		struct rpmi_tee_signal_bus_teardown_req req = {
			.target_id = cpu_to_le32(bus->endpoint_id),
		};
		int ret;

		/* Completion includes the target TEE's teardown acknowledgment. */
		ret = rpmi_tee_send(priv, RPMI_TEE_SRV_SIGNAL_BUS_TEARDOWN, &req,
				    sizeof(req), NULL, NULL);
		if (ret)
			dev_warn(priv->dev, "failed to tear down signal bus for %#x: %d\n",
				 bus->endpoint_id, ret);

		rpmi_tee_signal_bus_destroy_resvs(bus);
		list_del(&bus->node);
		kfree(bus);
	}
}

/* Drain callbacks, remove clients, then tear down notification resources. */
static void rpmi_tee_teardown_endpoints(struct rpmi_tee_transport *priv)
{
	/* Initiate a notification shutdown. */
	scoped_guard(mutex, &priv->notif.ops_lock)
		priv->notif.shutting_down = true;

	if (priv->notif.irq_requested) {
		free_irq(priv->notif.irq, priv);
		priv->notif.irq_requested = false;
	}

	if (priv->notif.wq) {
		destroy_workqueue(priv->notif.wq);
		priv->notif.wq = NULL;
	}

	/* Clients can still send final TEE_CALLs while signal buses exist. */
	rpmi_tee_unregister_devices(priv);

	/* Public notification operations and callbacks have stopped. */
	rpmi_tee_teardown_signal_buses(priv);

	if (priv->notif.irq) {
		irq_dispose_mapping(priv->notif.irq);
		priv->notif.irq = 0;
	}
}

/* Set up an RPMI signal bus for one @endpoint_id TEE endpoint. */
static int rpmi_tee_setup_ep_signal_bus(struct rpmi_tee_transport *priv,
					u32 endpoint_id)
{
	struct rpmi_tee_signal_bus_setup_req req;
	u32 max_width, width, tee_to_ree_count;

	/* Avoid duplicate signal bus for endpoint. */
	if (__rpmi_tee_find_signal_bus(priv, endpoint_id))
		return 0;

	if (priv->mbox.max_msg_data_size <
	    sizeof(struct rpmi_tee_signal_retrieve_resp))
		return -EMSGSIZE;

	max_width = FIELD_GET(RPMI_TEE_SIGNAL_WIDTH_MASK,
			      priv->notif.feature);
	width = min(max_width,
		    2 * ((priv->mbox.max_msg_data_size -
			  sizeof(struct rpmi_tee_signal_retrieve_resp)) /
			 sizeof(__le32)) + 1);
	if (width < 2)
		return -EMSGSIZE;

	/* Use half available signals for TEE-to-REE range. */
	tee_to_ree_count = width / 2;

	struct rpmi_tee_signal_bus *bus __free(kfree) =
		kzalloc_obj(*bus, GFP_KERNEL);
	if (!bus)
		return -ENOMEM;

	mutex_init(&bus->lock);
	xa_init_flags(&bus->reservations, XA_FLAGS_ALLOC);
	bus->endpoint_id = endpoint_id;
	bus->width = width;
	bus->tee_to_ree_count = tee_to_ree_count;

	req.target_id = cpu_to_le32(endpoint_id);
	req.bus_width = cpu_to_le32(width);
	req.sender_signals = cpu_to_le32(tee_to_ree_count);
	/* Publish the local bus only after the target TEE accepts setup. */
	if (rpmi_tee_send(priv, RPMI_TEE_SRV_SIGNAL_BUS_SETUP, &req,
			  sizeof(req), NULL, NULL))
		return -EOPNOTSUPP;

	list_add_tail(&no_free_ptr(bus)->node, &priv->notif.buses);

	return 0;
}

static int rpmi_tee_map_signal_irq(struct rpmi_tee_transport *priv)
{
	struct device_node *np;
	struct of_phandle_args oirq = {};
	u32 mode, index;
	int irq;

	mode = FIELD_GET(RPMI_TEE_SIGNAL_MODE_MASK, priv->notif.feature);
	if (mode != RPMI_TEE_SIGNAL_MODE_SYSTEM_MSI)
		return 0;

	for_each_compatible_node(np, NULL, "riscv,rpmi-system-msi") {
		if (of_device_is_available(np))
			break;
	}

	if (!np)
		return 0;
	if (!irq_find_host(np)) {
		of_node_put(np);
		return -EPROBE_DEFER;
	}

	index = FIELD_GET(RPMI_TEE_SIGNAL_INDEX_MASK, priv->notif.feature);
	oirq.np = np;
	oirq.args_count = 1;
	oirq.args[0] = index;
	irq = irq_create_of_mapping(&oirq);

	of_node_put(np);

	return irq;
}

/**
 * rpmi_tee_setup_signal_bus() - Set up signal notification delivery
 * @priv: RPMI TEE transport
 * @system: Discovered TEE endpoints that may own signal buses
 *
 * Map the signal interrupt, then set up a bus for each endpoint. Endpoint
 * setup failures are nonfatal, allowing notifications for the remaining
 * endpoints. Register the interrupt handler only when at least one bus is
 * available.
 *
 * Return: 0 on completion, or -EPROBE_DEFER when the System MSI IRQ domain
 * is not ready.
 */
static int
rpmi_tee_setup_signal_bus(struct rpmi_tee_transport *priv,
			 const struct rpmi_tee_discovery *system)
{
	struct rpmi_tee_discovered_endpoint *ep;
	int ret;

	priv->notif.irq = rpmi_tee_map_signal_irq(priv);
	if (priv->notif.irq < 0)
		return priv->notif.irq;
	if (!priv->notif.irq)
		return 0;

	list_for_each_entry(ep, &system->eps, node) {
		ret = rpmi_tee_setup_ep_signal_bus(priv, ep->ep_id);
		if (ret)
			dev_warn(priv->dev, "failed to set up signal bus for %#x: %d\n",
				 ep->ep_id, ret);
	}

	if (list_empty(&priv->notif.buses))
		goto out_failed;

	ret = request_irq(priv->notif.irq, rpmi_tee_notif_irq_handler, 0,
			  dev_name(priv->dev), priv);
	if (ret) {
		dev_warn(priv->dev, "failed to request signal IRQ: %d\n", ret);
		rpmi_tee_teardown_signal_buses(priv);
		goto out_failed;
	}

	priv->notif.irq_requested = true;

	return 0;

out_failed:
	/* Failures are nonfatal; only disable notification. */
	irq_dispose_mapping(priv->notif.irq);
	priv->notif.irq = 0;

	return 0;
}

static struct rpmi_tee_device *
rpmi_tee_find_device(struct rpmi_tee_transport *priv, const uuid_t *uuid,
		     u32 endpoint_id)
{
	struct rpmi_tee_child *child;

	list_for_each_entry(child, &priv->devices, node) {
		if (child->rdev->endpoint_id == endpoint_id &&
		    uuid_equal(&child->rdev->uuid, uuid))
			return child->rdev;
	}

	return NULL;
}

static int
rpmi_tee_register_devices(struct rpmi_tee_transport *priv,
			  const struct rpmi_tee_discovered_endpoint *ep)
{
	u32 i;

	for (i = 0; i < ep->service_count; i++) {
		const uuid_t *uuid = &ep->services[i];

		/* Discard duplicate devices in the same endpoint. */
		if (rpmi_tee_find_device(priv, uuid, ep->ep_id))
			continue;

		struct rpmi_tee_child *child __free(kfree) =
			kzalloc_obj(*child, GFP_KERNEL);
		if (!child)
			return -ENOMEM;

		child->rdev =
			rpmi_tee_device_register(uuid, ep->ep_id,
						 &rpmi_tee_ops, priv->dev);
		if (IS_ERR(child->rdev))
			return PTR_ERR(child->rdev);

		list_add_tail(&no_free_ptr(child)->node, &priv->devices);
	}

	return 0;
}

static int rpmi_tee_setup_endpoints(struct rpmi_tee_transport *priv)
{
	struct rpmi_tee_discovered_endpoint *ep;
	struct rpmi_tee_discovery system;
	int ret;

	ret = rpmi_tee_discover_endpoints(priv, &system);
	if (ret)
		return ret;

	ret = rpmi_tee_setup_signal_bus(priv, &system);
	if (ret)
		goto out_failed;

	list_for_each_entry(ep, &system.eps, node) {
		ret = rpmi_tee_register_devices(priv, ep);
		if (ret) {
			rpmi_tee_teardown_endpoints(priv);
			break;
		}
	}

out_failed:
	rpmi_tee_free_discovery(&system);

	return ret;
}

static int rpmi_tee_transport_probe(struct platform_device *pdev)
{
	struct rpmi_tee_transport *priv;
	u32 value;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	platform_set_drvdata(pdev, priv);
	INIT_LIST_HEAD(&priv->devices);
	INIT_LIST_HEAD(&priv->notif.buses);
	mutex_init(&priv->mem.lock);
	mutex_init(&priv->notif.ops_lock);
	INIT_WORK(&priv->notif.work, rpmi_tee_notif_work);

	priv->mbox.client.dev = &pdev->dev;
	priv->mbox.client.tx_sync = true;
	priv->mbox.chan = mbox_request_channel(&priv->mbox.client, 0);
	if (IS_ERR(priv->mbox.chan))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->mbox.chan),
				     "failed to request mailbox channel\n");

	/* Validate the RPMI mailbox transport. */
	ret = rpmi_tee_check_transport(priv);
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			      "invalid RPMI TEE mailbox channel\n");
		goto out_failed;
	}

	ret = rpmi_tee_probe_features(priv, RPMI_TEE_FEATURE_MEMORY_LEND,
				      &value);
	if (ret)
		goto out_failed;
	priv->mem.lend_ok = value == RPMI_TEE_MEMORY_FEATURE_FULLY_SUPPORTED;

	ret = rpmi_tee_probe_features(priv, RPMI_TEE_FEATURE_MEMORY_SHARE,
				      &value);
	if (ret)
		goto out_failed;
	priv->mem.share_ok = value == RPMI_TEE_MEMORY_FEATURE_FULLY_SUPPORTED;

	ret = rpmi_tee_probe_features(priv, RPMI_TEE_FEATURE_MULTISEGMENT_OPS,
				      &priv->mem.multisegment_max);
	if (ret)
		goto out_failed;

	ret = rpmi_tee_probe_features(priv, RPMI_TEE_FEATURE_SIGNAL_BUS,
				      &priv->notif.feature);
	if (ret)
		goto out_failed;

	priv->notif.wq = alloc_workqueue("rpmi_tee_notif", WQ_UNBOUND, 0);
	if (!priv->notif.wq) {
		ret = -ENOMEM;
		goto out_failed;
	}

	ret = rpmi_tee_setup_endpoints(priv);
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			      "failed to discover RPMI TEE services\n");
		goto out_failed;
	}

	return 0;

out_failed:
	if (priv->notif.wq)
		destroy_workqueue(priv->notif.wq);
	mbox_free_channel(priv->mbox.chan);

	return ret;
}

static void rpmi_tee_transport_remove(struct platform_device *pdev)
{
	struct rpmi_tee_transport *priv = platform_get_drvdata(pdev);

	rpmi_tee_teardown_endpoints(priv);
	mbox_free_channel(priv->mbox.chan);
}

static const struct of_device_id rpmi_tee_transport_match[] = {
	{ .compatible = "riscv,rpmi-tee" },
	{ }
};
MODULE_DEVICE_TABLE(of, rpmi_tee_transport_match);

static struct platform_driver rpmi_tee_transport_driver = {
	.probe = rpmi_tee_transport_probe,
	.remove = rpmi_tee_transport_remove,
	.driver = {
		.name = "riscv-rpmi-tee",
		.of_match_table = rpmi_tee_transport_match,
	},
};

module_platform_driver(rpmi_tee_transport_driver);

MODULE_DESCRIPTION("RISC-V RPMI TEE service group transport");
MODULE_LICENSE("GPL");
