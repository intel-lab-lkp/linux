// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2020 Intel Corporation. */
#include <uapi/linux/ndctl.h>
#include <linux/unaligned.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <cxlmem.h>
#include <cxl.h>
#include "core.h"

/**
 * DOC: cxl pmem
 *
 * The core CXL PMEM infrastructure supports persistent memory
 * provisioning and serves as a bridge to the LIBNVDIMM subsystem. A CXL
 * 'bridge' device is added at the root of a CXL device topology if
 * platform firmware advertises at least one persistent memory capable
 * CXL window. That root-level bridge corresponds to a LIBNVDIMM 'bus'
 * device. Then for each cxl_memdev in the CXL device topology a bridge
 * device is added to host a LIBNVDIMM dimm object. When these bridges
 * are registered native LIBNVDIMM uapis are translated to CXL
 * operations, for example, namespace label access commands.
 */

static DEFINE_IDA(cxl_nvdimm_bridge_ida);

static void cxl_nvdimm_bridge_release(struct device *dev)
{
	struct cxl_nvdimm_bridge *cxl_nvb = to_cxl_nvdimm_bridge(dev);

	ida_free(&cxl_nvdimm_bridge_ida, cxl_nvb->id);
	kfree(cxl_nvb);
}

static const struct attribute_group *cxl_nvdimm_bridge_attribute_groups[] = {
	&cxl_base_attribute_group,
	NULL,
};

const struct device_type cxl_nvdimm_bridge_type = {
	.name = "cxl_nvdimm_bridge",
	.release = cxl_nvdimm_bridge_release,
	.groups = cxl_nvdimm_bridge_attribute_groups,
};

struct cxl_nvdimm_bridge *to_cxl_nvdimm_bridge(struct device *dev)
{
	if (dev_WARN_ONCE(dev, dev->type != &cxl_nvdimm_bridge_type,
			  "not a cxl_nvdimm_bridge device\n"))
		return NULL;
	return container_of(dev, struct cxl_nvdimm_bridge, dev);
}
EXPORT_SYMBOL_NS_GPL(to_cxl_nvdimm_bridge, "CXL");

/**
 * cxl_find_nvdimm_bridge() - find a bridge device relative to a port
 * @port: any descendant port of an nvdimm-bridge associated
 *        root-cxl-port
 */
struct cxl_nvdimm_bridge *cxl_find_nvdimm_bridge(struct cxl_port *port)
{
	struct cxl_root *cxl_root __free(put_cxl_root) = find_cxl_root(port);
	struct device *dev;

	if (!cxl_root)
		return NULL;

	dev = device_find_child(&cxl_root->port.dev,
				&cxl_nvdimm_bridge_type,
				device_match_type);

	if (!dev)
		return NULL;

	return to_cxl_nvdimm_bridge(dev);
}
EXPORT_SYMBOL_NS_GPL(cxl_find_nvdimm_bridge, "CXL");

static struct lock_class_key cxl_nvdimm_bridge_key;

static struct cxl_nvdimm_bridge *cxl_nvdimm_bridge_alloc(struct cxl_port *port)
{
	struct cxl_nvdimm_bridge *cxl_nvb;
	struct device *dev;
	int rc;

	cxl_nvb = kzalloc(sizeof(*cxl_nvb), GFP_KERNEL);
	if (!cxl_nvb)
		return ERR_PTR(-ENOMEM);

	rc = ida_alloc(&cxl_nvdimm_bridge_ida, GFP_KERNEL);
	if (rc < 0)
		goto err;
	cxl_nvb->id = rc;

	dev = &cxl_nvb->dev;
	cxl_nvb->port = port;
	device_initialize(dev);
	lockdep_set_class(&dev->mutex, &cxl_nvdimm_bridge_key);
	device_set_pm_not_required(dev);
	dev->parent = &port->dev;
	dev->bus = &cxl_bus_type;
	dev->type = &cxl_nvdimm_bridge_type;

	return cxl_nvb;

err:
	kfree(cxl_nvb);
	return ERR_PTR(rc);
}

static void unregister_nvb(void *_cxl_nvb)
{
	struct cxl_nvdimm_bridge *cxl_nvb = _cxl_nvb;

	device_unregister(&cxl_nvb->dev);
}

static int detach_nvdimm(struct device *dev, void *data)
{
	struct cxl_nvdimm *cxl_nvd;
	bool release = false;

	if (!is_cxl_nvdimm(dev))
		return 0;

	scoped_guard(device, dev) {
		if (dev->driver) {
			cxl_nvd = to_cxl_nvdimm(dev);
			if (cxl_nvd->cxlmd && cxl_nvd->cxlmd->cxl_nvb == data)
				release = true;
		}
	}
	if (release)
		device_release_driver(dev);
	return 0;
}

static int cxl_pmem_set_config_data(struct cxl_memdev_state *mds,
				    struct nd_cmd_set_config_hdr *cmd,
				    unsigned int buf_len)
{
	struct cxl_mailbox *cxl_mbox = &mds->cxlds.cxl_mbox;
	struct cxl_mbox_set_lsa *set_lsa;
	struct cxl_mbox_cmd mbox_cmd;
	int rc;

	if (sizeof(*cmd) > buf_len)
		return -EINVAL;

	/* 4-byte status follows the input data in the payload */
	if (size_add(struct_size(cmd, in_buf, cmd->in_length), 4) > buf_len)
		return -EINVAL;

	set_lsa =
		kvzalloc(struct_size(set_lsa, data, cmd->in_length), GFP_KERNEL);
	if (!set_lsa)
		return -ENOMEM;

	*set_lsa = (struct cxl_mbox_set_lsa) {
		.offset = cpu_to_le32(cmd->in_offset),
	};
	memcpy(set_lsa->data, cmd->in_buf, cmd->in_length);
	mbox_cmd = (struct cxl_mbox_cmd) {
		.opcode = CXL_MBOX_OP_SET_LSA,
		.payload_in = set_lsa,
		.size_in = struct_size(set_lsa, data, cmd->in_length),
	};

	rc = cxl_internal_send_cmd(cxl_mbox, &mbox_cmd);

	/*
	 * Set "firmware" status (4-packed bytes at the end of the input
	 * payload.
	 */
	put_unaligned(0, (u32 *) &cmd->in_buf[cmd->in_length]);
	kvfree(set_lsa);

	return rc;
}

static int cxl_pmem_get_config_size(struct cxl_memdev_state *mds,
				    struct nd_cmd_get_config_size *cmd,
				    unsigned int buf_len)
{
	struct cxl_mailbox *cxl_mbox = &mds->cxlds.cxl_mbox;

	if (sizeof(*cmd) > buf_len)
		return -EINVAL;

	*cmd = (struct nd_cmd_get_config_size){
		.config_size = mds->lsa_size,
		.max_xfer =
			cxl_mbox->payload_size - sizeof(struct cxl_mbox_set_lsa),
	};

	return 0;
}

static int cxl_pmem_get_config_data(struct cxl_memdev_state *mds,
				    struct nd_cmd_get_config_data_hdr *cmd,
				    unsigned int buf_len)
{
	struct cxl_mailbox *cxl_mbox = &mds->cxlds.cxl_mbox;
	struct cxl_mbox_get_lsa get_lsa;
	struct cxl_mbox_cmd mbox_cmd;
	int rc;

	if (sizeof(*cmd) > buf_len)
		return -EINVAL;
	if (struct_size(cmd, out_buf, cmd->in_length) > buf_len)
		return -EINVAL;

	get_lsa = (struct cxl_mbox_get_lsa) {
		.offset = cpu_to_le32(cmd->in_offset),
		.length = cpu_to_le32(cmd->in_length),
	};
	mbox_cmd = (struct cxl_mbox_cmd) {
		.opcode = CXL_MBOX_OP_GET_LSA,
		.payload_in = &get_lsa,
		.size_in = sizeof(get_lsa),
		.size_out = cmd->in_length,
		.payload_out = cmd->out_buf,
	};

	rc = cxl_internal_send_cmd(cxl_mbox, &mbox_cmd);
	cmd->status = 0;

	return rc;
}

static int cxl_pmem_nvdimm_ctl(struct nvdimm *nvdimm, unsigned int cmd,
			       void *buf, unsigned int buf_len)
{
	struct cxl_nvdimm *cxl_nvd = nvdimm_provider_data(nvdimm);
	unsigned long cmd_mask = nvdimm_cmd_mask(nvdimm);
	struct cxl_memdev *cxlmd = cxl_nvd->cxlmd;
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlmd->cxlds);

	if (!test_bit(cmd, &cmd_mask))
		return -ENOTTY;

	switch (cmd) {
	case ND_CMD_GET_CONFIG_SIZE:
		return cxl_pmem_get_config_size(mds, buf, buf_len);
	case ND_CMD_GET_CONFIG_DATA:
		return cxl_pmem_get_config_data(mds, buf, buf_len);
	case ND_CMD_SET_CONFIG_DATA:
		return cxl_pmem_set_config_data(mds, buf, buf_len);
	default:
		return -ENOTTY;
	}
}

static int cxl_pmem_ctl(struct nvdimm_bus_descriptor *nd_desc,
			struct nvdimm *nvdimm, unsigned int cmd, void *buf,
			unsigned int buf_len, int *cmd_rc)
{
	/*
	 * No firmware response to translate, let the transport error
	 * code take precedence.
	 */
	*cmd_rc = 0;

	if (!nvdimm)
		return -ENOTTY;
	return cxl_pmem_nvdimm_ctl(nvdimm, cmd, buf, buf_len);
}

static void unregister_nvdimm_bus(void *_cxl_nvb)
{
	struct cxl_nvdimm_bridge *cxl_nvb = _cxl_nvb;
	struct nvdimm_bus *nvdimm_bus = cxl_nvb->nvdimm_bus;

	bus_for_each_dev(&cxl_bus_type, NULL, cxl_nvb, detach_nvdimm);

	cxl_nvb->nvdimm_bus = NULL;
	nvdimm_bus_unregister(nvdimm_bus);
}

DEFINE_FREE(put_cxl_nvb, struct cxl_nvdimm_bridge *, if (!IS_ERR_OR_NULL(_T)) put_device(&_T->dev))
static struct cxl_nvdimm_bridge *
__devm_cxl_add_nvdimm_bridge(struct device *host, struct cxl_port *port)
{
	struct device *dev;
	int rc;

	if (!IS_ENABLED(CONFIG_CXL_PMEM))
		return ERR_PTR(-ENXIO);

	struct cxl_nvdimm_bridge *cxl_nvb __free(put_cxl_nvb) =
		cxl_nvdimm_bridge_alloc(port);
	if (IS_ERR(cxl_nvb))
		return cxl_nvb;

	dev = &cxl_nvb->dev;
	rc = dev_set_name(dev, "nvdimm-bridge%d", cxl_nvb->id);
	if (rc)
		return ERR_PTR(rc);

	rc = device_add(dev);
	if (rc)
		return ERR_PTR(rc);

	rc = devm_add_action_or_reset(host, unregister_nvb, cxl_nvb);
	if (rc) {
		retain_and_null_ptr(cxl_nvb);
		return ERR_PTR(rc);
	}

	return no_free_ptr(cxl_nvb);
}

static void release_nvb(void *_cxl_nvb)
{
	struct cxl_nvdimm_bridge *cxl_nvb = _cxl_nvb;

	devm_release_action(&cxl_nvb->dev, unregister_nvb, cxl_nvb);
}

DEFINE_FREE(release_cxl_nvb, struct cxl_nvdimm_bridge *, if (!IS_ERR_OR_NULL(_T)) release_nvb(_T))
/**
 * devm_cxl_add_nvdimm_bridge() - add the root of a LIBNVDIMM topology
 * @host: platform firmware root device
 * @port: CXL port at the root of a CXL topology
 *
 * Return: bridge device that can host cxl_nvdimm objects
 */
struct cxl_nvdimm_bridge *devm_cxl_add_nvdimm_bridge(struct device *host,
						     struct cxl_port *port)
{
	struct device *dev;
	int rc;

	struct cxl_nvdimm_bridge *cxl_nvb __free(release_cxl_nvb) =
		__devm_cxl_add_nvdimm_bridge(host, port);
	if (IS_ERR(cxl_nvb))
		return cxl_nvb;

	dev = &cxl_nvb->dev;
	cxl_nvb->nd_desc = (struct nvdimm_bus_descriptor) {
		.provider_name = dev_name(dev->parent->parent),
		.module = THIS_MODULE,
		.ndctl = cxl_pmem_ctl,
	};

	cxl_nvb->nvdimm_bus =
		nvdimm_bus_register(&cxl_nvb->dev, &cxl_nvb->nd_desc);
	if (!cxl_nvb->nvdimm_bus)
		return ERR_PTR(-ENOMEM);

	rc = devm_add_action_or_reset(dev, unregister_nvdimm_bus, cxl_nvb);
	if (rc)
		return ERR_PTR(rc);

	return no_free_ptr(cxl_nvb);
}

EXPORT_SYMBOL_NS_GPL(devm_cxl_add_nvdimm_bridge, "CXL");

static void cxl_nvdimm_release(struct device *dev)
{
	struct cxl_nvdimm *cxl_nvd = to_cxl_nvdimm(dev);

	kfree(cxl_nvd);
}

static const struct attribute_group *cxl_nvdimm_attribute_groups[] = {
	&cxl_base_attribute_group,
	NULL,
};

const struct device_type cxl_nvdimm_type = {
	.name = "cxl_nvdimm",
	.release = cxl_nvdimm_release,
	.groups = cxl_nvdimm_attribute_groups,
};

bool is_cxl_nvdimm(struct device *dev)
{
	return dev->type == &cxl_nvdimm_type;
}
EXPORT_SYMBOL_NS_GPL(is_cxl_nvdimm, "CXL");

struct cxl_nvdimm *to_cxl_nvdimm(struct device *dev)
{
	if (dev_WARN_ONCE(dev, !is_cxl_nvdimm(dev),
			  "not a cxl_nvdimm device\n"))
		return NULL;
	return container_of(dev, struct cxl_nvdimm, dev);
}
EXPORT_SYMBOL_NS_GPL(to_cxl_nvdimm, "CXL");

static struct lock_class_key cxl_nvdimm_key;

static struct cxl_nvdimm *cxl_nvdimm_alloc(struct cxl_nvdimm_bridge *cxl_nvb,
					   struct cxl_memdev *cxlmd)
{
	struct cxl_nvdimm *cxl_nvd;
	struct device *dev;

	cxl_nvd = kzalloc(sizeof(*cxl_nvd), GFP_KERNEL);
	if (!cxl_nvd)
		return ERR_PTR(-ENOMEM);

	dev = &cxl_nvd->dev;
	cxl_nvd->cxlmd = cxlmd;
	cxlmd->cxl_nvd = cxl_nvd;
	device_initialize(dev);
	lockdep_set_class(&dev->mutex, &cxl_nvdimm_key);
	device_set_pm_not_required(dev);
	dev->parent = &cxlmd->dev;
	dev->bus = &cxl_bus_type;
	dev->type = &cxl_nvdimm_type;
	/*
	 * A "%llx" string is 17-bytes vs dimm_id that is max
	 * NVDIMM_KEY_DESC_LEN
	 */
	BUILD_BUG_ON(sizeof(cxl_nvd->dev_id) < 17 ||
		     sizeof(cxl_nvd->dev_id) > NVDIMM_KEY_DESC_LEN);
	sprintf(cxl_nvd->dev_id, "%llx", cxlmd->cxlds->serial);

	return cxl_nvd;
}

static void cxlmd_release_nvdimm(void *_cxlmd)
{
	struct cxl_memdev *cxlmd = _cxlmd;
	struct cxl_nvdimm *cxl_nvd = cxlmd->cxl_nvd;
	struct cxl_nvdimm_bridge *cxl_nvb = cxlmd->cxl_nvb;

	cxl_nvd->cxlmd = NULL;
	cxlmd->cxl_nvd = NULL;
	cxlmd->cxl_nvb = NULL;
	device_unregister(&cxl_nvd->dev);
	put_device(&cxl_nvb->dev);
}

/**
 * devm_cxl_add_nvdimm() - add a bridge between a cxl_memdev and an nvdimm
 * @parent_port: parent port for the (to be added) @cxlmd endpoint port
 * @cxlmd: cxl_memdev instance that will perform LIBNVDIMM operations
 *
 * Return: 0 on success negative error code on failure.
 */
int devm_cxl_add_nvdimm(struct cxl_port *parent_port,
			struct cxl_memdev *cxlmd)
{
	struct cxl_nvdimm_bridge *cxl_nvb;
	struct cxl_nvdimm *cxl_nvd;
	struct device *dev;
	int rc;

	cxl_nvb = cxl_find_nvdimm_bridge(parent_port);
	if (!cxl_nvb)
		return -ENODEV;

	cxl_nvd = cxl_nvdimm_alloc(cxl_nvb, cxlmd);
	if (IS_ERR(cxl_nvd)) {
		rc = PTR_ERR(cxl_nvd);
		goto err_alloc;
	}
	cxlmd->cxl_nvb = cxl_nvb;

	dev = &cxl_nvd->dev;
	rc = dev_set_name(dev, "pmem%d", cxlmd->id);
	if (rc)
		goto err;

	rc = device_add(dev);
	if (rc)
		goto err;

	dev_dbg(&cxlmd->dev, "register %s\n", dev_name(dev));

	/* @cxlmd carries a reference on @cxl_nvb until cxlmd_release_nvdimm */
	return devm_add_action_or_reset(&cxlmd->dev, cxlmd_release_nvdimm, cxlmd);

err:
	put_device(dev);
err_alloc:
	cxlmd->cxl_nvb = NULL;
	cxlmd->cxl_nvd = NULL;
	put_device(&cxl_nvb->dev);

	return rc;
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_add_nvdimm, "CXL");
