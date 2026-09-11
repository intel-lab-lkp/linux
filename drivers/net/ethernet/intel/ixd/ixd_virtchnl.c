// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2025 Intel Corporation */

#include "ixd.h"
#include "ixd_ctlq.h"
#include "ixd_lan_regs.h"
#include "ixd_virtchnl.h"

/**
 * ixd_vc_recv_event_msg - Handle virtchnl event message
 * @adapter: The adapter handling the message
 * @ctlq_msg: Message received
 */
void ixd_vc_recv_event_msg(struct ixd_adapter *adapter,
			   struct libie_ctlq_msg *ctlq_msg)
{
	int payload_size = ctlq_msg->data_len;
	struct virtchnl2_event *v2e;

	if (payload_size < sizeof(*v2e)) {
		dev_warn_ratelimited(ixd_to_dev(adapter),
				     "Failed to receive valid payload for event msg (op 0x%X len %u)\n",
				     ctlq_msg->chnl_opcode,
				     payload_size);
		return;
	}

	v2e = (struct virtchnl2_event *)ctlq_msg->recv_mem.iov_base;

	dev_dbg(ixd_to_dev(adapter), "Got event 0x%X from the CP\n",
		le32_to_cpu(v2e->event));
}

/**
 * ixd_vc_can_handle_msg - Decide if an event has to be handled by virtchnl code
 * @ctlq_msg: Message received
 *
 * Return: %true if virtchnl code can handle the event, %false otherwise
 */
bool ixd_vc_can_handle_msg(struct libie_ctlq_msg *ctlq_msg)
{
	return ctlq_msg->chnl_opcode == VIRTCHNL2_OP_EVENT;
}

/**
 * ixd_handle_caps - Handle VIRTCHNL2_OP_GET_CAPS response
 * @adapter: The adapter for which the capabilities are being updated
 * @recv_buff: Buffer containing the response
 * @recv_size: Response buffer size
 * @ctx: unused
 *
 * Return: %0 if the response format is correct and was handled as expected,
 * negative error otherwise.
 */
static int ixd_handle_caps(struct ixd_adapter *adapter, void *recv_buff,
			   size_t recv_size, void *ctx)
{
	if (recv_size < sizeof(adapter->caps))
		return -EBADMSG;

	adapter->caps = *(typeof(adapter->caps) *)recv_buff;

	return 0;
}

static void ixd_fill_caps(struct ixd_adapter *adapter, void *send_buff,
			  void *ctx)
{
	struct virtchnl2_get_capabilities *caps = send_buff;

	caps->other_caps = cpu_to_le64(VIRTCHNL2_CAP_LAN_MEMORY_REGIONS);
}

/**
 * ixd_req_vc_caps - Request and save device capability
 * @adapter: The adapter to get the capabilities for
 *
 * Return: success or error if sending the get capability message fails
 */
static int ixd_req_vc_caps(struct ixd_adapter *adapter)
{
	const struct ixd_ctlq_req req = {
		.opcode = VIRTCHNL2_OP_GET_CAPS,
		.send_size = sizeof(struct virtchnl2_get_capabilities),
		.ctx = NULL,
		.send_buff_init = ixd_fill_caps,
		.recv_process = ixd_handle_caps,
	};

	return ixd_ctlq_do_req(adapter, &req);
}

static bool ixd_is_cap_ena(struct ixd_adapter *adapter, u64 cap)
{
	return (le64_to_cpu(adapter->caps.other_caps) & cap) == cap;
}

/**
 * ixd_get_vc_ver - Get version info from adapter
 *
 * Return: filled in virtchannel2 version info, ready for sending
 */
static struct virtchnl2_version_info ixd_get_vc_ver(void)
{
	return (struct virtchnl2_version_info) {
		.major = cpu_to_le32(VIRTCHNL2_VERSION_MAJOR_2),
		.minor = cpu_to_le32(VIRTCHNL2_VERSION_MINOR_0),
	};
}

static void ixd_fill_vc_ver(struct ixd_adapter *adapter, void *send_buff,
			    void *ctx)
{
	*(struct virtchnl2_version_info *)send_buff = ixd_get_vc_ver();
}

/**
 * ixd_handle_vc_ver - Handle VIRTCHNL2_OP_VERSION response
 * @adapter: The adapter for which the version is being updated
 * @recv_buff: Buffer containing the response
 * @recv_size: Response buffer size
 * @ctx: Unused
 *
 * Return: %0 if the response format is correct and was handled as expected,
 * negative error otherwise.
 */
static int ixd_handle_vc_ver(struct ixd_adapter *adapter, void *recv_buff,
			     size_t recv_size, void *ctx)
{
	struct virtchnl2_version_info need_ver = ixd_get_vc_ver();
	struct virtchnl2_version_info *recv_ver;

	if (recv_size < sizeof(need_ver))
		return -EBADMSG;

	recv_ver = recv_buff;
	if (le32_to_cpu(need_ver.major) != le32_to_cpu(recv_ver->major) ||
	    le32_to_cpu(need_ver.minor) != le32_to_cpu(recv_ver->minor))
		dev_warn(ixd_to_dev(adapter),
			 "Virtchnl version does not match (expected %u.%u, received %u.%u)\n",
			 le32_to_cpu(need_ver.major),
			 le32_to_cpu(need_ver.minor),
			 le32_to_cpu(recv_ver->major),
			 le32_to_cpu(recv_ver->minor));

	if (le32_to_cpu(need_ver.major) != le32_to_cpu(recv_ver->major)) {
		dev_err(ixd_to_dev(adapter),
			"Device initialization failed due to virtchnl major version mismatch\n");
		return -EOPNOTSUPP;
	}

	adapter->vc_ver.major = le32_to_cpu(recv_ver->major);
	adapter->vc_ver.minor = le32_to_cpu(recv_ver->minor);

	return 0;
}

/**
 * ixd_req_vc_version - Request and save Virtchannel2 version
 * @adapter: The adapter to get the version for
 *
 * Return: success or error if sending fails or the response was not as expected
 */
static int ixd_req_vc_version(struct ixd_adapter *adapter)
{
	const struct ixd_ctlq_req req = {
		.opcode = VIRTCHNL2_OP_VERSION,
		.send_size = sizeof(struct virtchnl2_version_info),
		.ctx = NULL,
		.send_buff_init = ixd_fill_vc_ver,
		.recv_process = ixd_handle_vc_ver,
	};

	return ixd_ctlq_do_req(adapter, &req);
}

static void ixd_fill_lan_mmio_regions(struct ixd_adapter *adapter,
				      void *send_buff, void *ctx)
{
	struct virtchnl2_get_lan_memory_regions *lan = send_buff;

	/* Needed to be parsed correctly, this mem_region doesn't matter. */
	lan->num_memory_regions = cpu_to_le16(1);
}

static int ixd_handle_lan_mmio_regions(struct ixd_adapter *adapter,
				       void *recv_buff, size_t recv_size,
				       void *ctx)
{
	struct libie_mmio_info *mmio_info = &adapter->cp_ctx.mmio_info;
	struct virtchnl2_get_lan_memory_regions *recv_mmio = recv_buff;
	int num_regions;

	if (recv_size < sizeof(*recv_mmio))
		return -EBADMSG;

	num_regions = le16_to_cpu(recv_mmio->num_memory_regions);
	if (!num_regions)
		return -EBADMSG;

	if (recv_size < struct_size(recv_mmio, mem_reg, num_regions))
		return -EBADMSG;

	for (int i = 0; i < num_regions; i++) {
		struct virtchnl2_mem_region *reg = &recv_mmio->mem_reg[i];
		resource_size_t offset, size;

		offset = le64_to_cpu(reg->start_offset);
		size = le64_to_cpu(reg->size);

		/* Empty regions are used as a padding, skip them. */
		if (!size)
			continue;

		if (!libie_pci_map_mmio_region(mmio_info, offset, size)) {
			/* Unmap already mapped */
			libie_pci_unmap_fltr_regs(mmio_info,
						  ixd_iomap_is_not_start_region);
			return -EIO;
		}
	}

	return 0;
}

static int ixd_req_lan_mmio_regions(struct ixd_adapter *adapter)
{
	const struct ixd_ctlq_req req = {
		.opcode = VIRTCHNL2_OP_GET_LAN_MEMORY_REGIONS,
		.send_size = sizeof(struct virtchnl2_get_lan_memory_regions) +
			     sizeof(struct virtchnl2_mem_region),
		.ctx = NULL,
		.send_buff_init = ixd_fill_lan_mmio_regions,
		.recv_process = ixd_handle_lan_mmio_regions,
	};

	return ixd_ctlq_do_req(adapter, &req);
}

/**
 * ixd_vc_dev_init - virtchnl device core initialization
 * @adapter: device information
 *
 * Return: %0 on success or error if any step of the initialization fails
 */
int ixd_vc_dev_init(struct ixd_adapter *adapter)
{
	int err;

	err = ixd_req_vc_version(adapter);
	if (err) {
		dev_warn(ixd_to_dev(adapter),
			 "Getting virtchnl version failed, error=%pe\n",
			 ERR_PTR(err));
		return err;
	}

	err = ixd_req_vc_caps(adapter);
	if (err) {
		dev_warn(ixd_to_dev(adapter),
			 "Getting virtchnl capabilities failed, error=%pe\n",
			 ERR_PTR(err));
		return err;
	}

	if (ixd_is_cap_ena(adapter, VIRTCHNL2_CAP_LAN_MEMORY_REGIONS))
		err = ixd_req_lan_mmio_regions(adapter);
	else
		/* Fallback to mapping the remaining regions of the whole BAR */
		err = ixd_iomap_running_regions(adapter);

	if (err)
		dev_warn(ixd_to_dev(adapter),
			 "Getting LAN mmio regions failed, error=%pe\n",
			 ERR_PTR(err));

	return err;
}
