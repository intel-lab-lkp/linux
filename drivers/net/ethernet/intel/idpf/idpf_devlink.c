// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 Intel Corporation */

#include <linux/pci.h>
#include <linux/unaligned.h>

#include "idpf.h"
#include "idpf_devlink.h"

#define IDPF_DEVLINK_INFO_LEN		128

/**
 * idpf_info_get_dsn - format the PCI DSN as the device serial number
 * @adapter: the idpf adapter structure
 * @buf: buffer to store the formatted serial number
 * @buf_size: size of @buf
 *
 * Return: true if the device reports a DSN, false otherwise.
 */
static bool idpf_info_get_dsn(struct idpf_adapter *adapter, char *buf,
			      size_t buf_size)
{
	u64 dsn = pci_get_dsn(adapter->pdev);
	u8 dsn_be[sizeof(dsn)];

	if (!dsn)
		return false;

	/* Copy the DSN into an array in Big Endian format */
	put_unaligned_be64(dsn, dsn_be);
	snprintf(buf, buf_size, "%8phD", dsn_be);

	return true;
}

/**
 * idpf_devlink_info_get - .info_get devlink handler
 * @devlink: devlink instance structure
 * @req: the devlink info request
 * @extack: extended netlink ack structure
 *
 * Callback for the devlink .info_get operation. Reports information about the
 * device. The virtchnl version is only reported once it has been negotiated,
 * as the instance is registered before the handshake runs.
 *
 * Return: zero on success or a negative error code on failure.
 */
static int idpf_devlink_info_get(struct devlink *devlink,
				 struct devlink_info_req *req,
				 struct netlink_ext_ack *extack)
{
	struct idpf_adapter *adapter = devlink_priv(devlink);
	char buf[IDPF_DEVLINK_INFO_LEN];
	u32 maj, min;
	int err;

	if (idpf_info_get_dsn(adapter, buf, sizeof(buf))) {
		err = devlink_info_serial_number_put(req, buf);
		if (err)
			return err;
	}

	maj = READ_ONCE(adapter->virt_ver_maj);
	min = READ_ONCE(adapter->virt_ver_min);
	if (!maj && !min)
		return 0;

	snprintf(buf, sizeof(buf), "%u.%u", maj, min);

	return devlink_info_version_running_put(req,
					DEVLINK_INFO_VERSION_GENERIC_FW_MGMT_API,
					buf);
}

static const struct devlink_ops idpf_devlink_ops = {
	.info_get = idpf_devlink_info_get,
};

/**
 * idpf_adapter_alloc - allocate devlink and return adapter
 * @dev: IDPF device to allocate for
 *
 * Allocate a devlink instance for this device and return the private area as
 * the adapter structure.
 *
 * Return: adapter structure on success, NULL on failure
 */
struct idpf_adapter *idpf_adapter_alloc(struct device *dev)
{
	struct devlink *devlink;

	devlink = devlink_alloc(&idpf_devlink_ops, sizeof(struct idpf_adapter),
				dev);
	if (!devlink)
		return NULL;

	return devlink_priv(devlink);
}
