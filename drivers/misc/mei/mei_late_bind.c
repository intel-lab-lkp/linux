// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Intel Corporation
 */
#include <drm/intel/i915_component.h>
#include <drm/intel/late_bind_mei_interface.h>
#include <linux/component.h>
#include <linux/pci.h>
#include <linux/mei_cl_bus.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/uuid.h>

#include "mkhi.h"

#define GFX_SRV_MKHI_LATE_BINDING_CMD 0x12
#define GFX_SRV_MKHI_LATE_BINDING_RSP (GFX_SRV_MKHI_LATE_BINDING_CMD | 0x80)

#define LATE_BIND_SEND_TIMEOUT_MSEC 3000
#define LATE_BIND_RECV_TIMEOUT_MSEC 3000

/**
 * struct csc_heci_late_bind_req - late binding request
 * @header: @ref mkhi_msg_hdr
 * @type: type of the late binding payload
 * @flags: flags to be passed to the firmware
 * @reserved: reserved for future use by firmware, must be set to 0
 * @payload_size: size of the payload data in bytes
 * @payload: data to be sent to the firmware
 */
struct csc_heci_late_bind_req {
	struct mkhi_msg_hdr header;
	__le32 type;
	__le32 flags;
	__le32 reserved[2];
	__le32 payload_size;
	u8  payload[] __counted_by(payload_size);
} __packed;

/**
 * struct csc_heci_late_bind_rsp - late binding response
 * @header: @ref mkhi_msg_hdr
 * @type: type of the late binding payload
 * @reserved: reserved for future use by firmware, must be set to 0
 * @status: status of the late binding command execution by firmware
 */
struct csc_heci_late_bind_rsp {
	struct mkhi_msg_hdr header;
	__le32 type;
	__le32 reserved[2];
	__le32 status;
} __packed;

static int mei_late_bind_check_response(const struct device *dev, const struct mkhi_msg_hdr *hdr)
{
	if (hdr->group_id != MKHI_GROUP_ID_GFX) {
		dev_err(dev, "Mismatch group id: 0x%x instead of 0x%x\n",
			hdr->group_id, MKHI_GROUP_ID_GFX);
		return -EINVAL;
	}

	if (hdr->command != GFX_SRV_MKHI_LATE_BINDING_RSP) {
		dev_err(dev, "Mismatch command: 0x%x instead of 0x%x\n",
			hdr->command, GFX_SRV_MKHI_LATE_BINDING_RSP);
		return -EINVAL;
	}

	if (hdr->result) {
		dev_err(dev, "Error in result: 0x%x\n", hdr->result);
		return -EINVAL;
	}

	return 0;
}

static int mei_late_bind_push_config(struct device *dev, enum late_bind_type type, u32 flags,
				     const void *payload, size_t payload_size)
{
	struct mei_cl_device *cldev;
	struct csc_heci_late_bind_req *req = NULL;
	struct csc_heci_late_bind_rsp rsp;
	size_t req_size;
	ssize_t bytes;
	int ret;

	cldev = to_mei_cl_device(dev);

	ret = mei_cldev_enable(cldev);
	if (ret) {
		dev_dbg(dev, "mei_cldev_enable failed. %d\n", ret);
		return ret;
	}

	req_size = struct_size(req, payload, payload_size);
	if (req_size > mei_cldev_mtu(cldev)) {
		dev_err(dev, "Payload is too big %zu\n", payload_size);
		ret = -EMSGSIZE;
		goto end;
	}

	req = kmalloc(req_size, GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto end;
	}

	req->header.group_id = MKHI_GROUP_ID_GFX;
	req->header.command = GFX_SRV_MKHI_LATE_BINDING_CMD;
	req->type = cpu_to_le32(type);
	req->flags = cpu_to_le32(flags);
	req->reserved[0] = 0;
	req->reserved[1] = 0;
	req->payload_size = cpu_to_le32(payload_size);
	memcpy(req->payload, payload, payload_size);

	bytes = mei_cldev_send_timeout(cldev,
				       (void *)req, req_size, LATE_BIND_SEND_TIMEOUT_MSEC);
	if (bytes < 0) {
		dev_err(dev, "mei_cldev_send failed. %zd\n", bytes);
		ret = bytes;
		goto end;
	}

	bytes = mei_cldev_recv_timeout(cldev,
				       (void *)&rsp, sizeof(rsp), LATE_BIND_RECV_TIMEOUT_MSEC);
	if (bytes < 0) {
		dev_err(dev, "mei_cldev_recv failed. %zd\n", bytes);
		ret = bytes;
		goto end;
	}
	if (bytes < sizeof(rsp.header)) {
		dev_err(dev, "bad response header from the firmware: size %zd < %zu\n",
			bytes, sizeof(rsp.header));
		ret = -EPROTO;
		goto end;
	}
	if (mei_late_bind_check_response(dev, &rsp.header)) {
		dev_err(dev, "bad result response from the firmware: 0x%x\n",
			*(uint32_t *)&rsp.header);
		ret = -EPROTO;
		goto end;
	}
	if (bytes < sizeof(rsp)) {
		dev_err(dev, "bad response from the firmware: size %zd < %zu\n",
			bytes, sizeof(rsp));
		ret = -EPROTO;
		goto end;
	}

	dev_dbg(dev, "status = %u\n", le32_to_cpu(rsp.status));
	ret = (int)le32_to_cpu(rsp.status);
end:
	mei_cldev_disable(cldev);
	kfree(req);
	return ret;
}

static const struct late_bind_component_ops mei_late_bind_ops = {
	.push_config = mei_late_bind_push_config,
};

static int mei_component_master_bind(struct device *dev)
{
	return component_bind_all(dev, (void *)&mei_late_bind_ops);
}

static void mei_component_master_unbind(struct device *dev)
{
	component_unbind_all(dev, (void *)&mei_late_bind_ops);
}

static const struct component_master_ops mei_component_master_ops = {
	.bind = mei_component_master_bind,
	.unbind = mei_component_master_unbind,
};

/**
 * mei_late_bind_component_match - compare function for matching mei late bind.
 *
 *    This function checks if requester is Intel PCI_CLASS_DISPLAY_VGA or
 *    PCI_CLASS_DISPLAY_OTHER device, and checks if the requester is the
 *    grand parent of mei_if i.e. late_bind mei device
 *
 * @dev: master device
 * @subcomponent: subcomponent to match (INTEL_COMPONENT_LATE_BIND)
 * @data: compare data (late_bind mei device on mei bus)
 *
 * Return:
 * * 1 - if components match
 * * 0 - otherwise
 */
static int mei_late_bind_component_match(struct device *dev, int subcomponent,
					 void *data)
{
	struct device *base = data;
	struct pci_dev *pdev;

	if (!dev)
		return 0;

	if (!dev_is_pci(dev))
		return 0;

	pdev = to_pci_dev(dev);

	if (pdev->vendor != PCI_VENDOR_ID_INTEL)
		return 0;

	if (pdev->class != (PCI_CLASS_DISPLAY_VGA << 8) &&
	    pdev->class != (PCI_CLASS_DISPLAY_OTHER << 8))
		return 0;

	if (subcomponent != INTEL_COMPONENT_LATE_BIND)
		return 0;

	base = base->parent;
	if (!base) /* mei device */
		return 0;

	base = base->parent; /* pci device */

	return !!base && dev == base;
}

static int mei_late_bind_probe(struct mei_cl_device *cldev,
			       const struct mei_cl_device_id *id)
{
	struct component_match *master_match = NULL;
	int ret;

	component_match_add_typed(&cldev->dev, &master_match,
				  mei_late_bind_component_match, &cldev->dev);
	if (IS_ERR_OR_NULL(master_match))
		return -ENOMEM;

	ret = component_master_add_with_match(&cldev->dev,
					      &mei_component_master_ops,
					      master_match);
	if (ret < 0)
		dev_err(&cldev->dev, "Master comp add failed %d\n", ret);

	return ret;
}

static void mei_late_bind_remove(struct mei_cl_device *cldev)
{
	component_master_del(&cldev->dev, &mei_component_master_ops);
}

#define MEI_GUID_MKHI UUID_LE(0xe2c2afa2, 0x3817, 0x4d19, \
			      0x9d, 0x95, 0x6, 0xb1, 0x6b, 0x58, 0x8a, 0x5d)

static struct mei_cl_device_id mei_late_bind_tbl[] = {
	{ .uuid = MEI_GUID_MKHI, .version = MEI_CL_VERSION_ANY },
	{ }
};
MODULE_DEVICE_TABLE(mei, mei_late_bind_tbl);

static struct mei_cl_driver mei_late_bind_driver = {
	.id_table = mei_late_bind_tbl,
	.name = KBUILD_MODNAME,
	.probe = mei_late_bind_probe,
	.remove	= mei_late_bind_remove,
};

module_mei_cl_driver(mei_late_bind_driver);

MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MEI Late Binding");
