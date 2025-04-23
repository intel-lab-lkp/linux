/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2018-2025, Advanced Micro Devices, Inc. */

#ifndef _IONIC_API_H_
#define _IONIC_API_H_

#include <linux/auxiliary_bus.h>
#include "ionic_if.h"
#include "ionic_regs.h"

/**
 * struct ionic_aux_dev - Auxiliary device information
 * @handle:     Handle for this auxiliary device
 * @idx:        Index identifier
 * @adev:       Auxiliary device
 */
struct ionic_aux_dev {
	void *handle;
	int idx;
	struct auxiliary_device adev;
};

/**
 * struct ionic_devinfo - device information
 * @asic_type:      Device ASIC type code
 * @asic_rev:       Device ASIC revision code
 * @fw_version:     Device firmware version, as a string
 * @serial_num:     Device serial number, as a string
 */
struct ionic_devinfo {
	u8 asic_type;
	u8 asic_rev;
	char fw_version[IONIC_DEVINFO_FWVERS_BUFLEN + 1];
	char serial_num[IONIC_DEVINFO_SERIAL_BUFLEN + 1];
};

/**
 * ionic_api_get_identity - Get result of device identification
 * @handle:     Handle to lif
 * @lif_index:  This lif index
 *
 * Return: pointer to result of identification
 */
const union ionic_lif_identity *ionic_api_get_identity(void *handle,
						       int *lif_index);

/**
 * ionic_api_get_netdev_from_handle - Get a network device associated with the
 *                                    handle
 * @handle:     Handle to lif
 *
 * This returns a network device associated with the lif handle.
 * If network device is available it holds the reference to device. Caller must
 * ensure that it releases the device using dev_put() after its usage.
 *
 * Return: Network device on success or ERR_PTR(error)
 */
struct net_device *ionic_api_get_netdev_from_handle(void *handle);

/**
 * ionic_api_get_devinfo - Get device information
 * @handle:     Handle to lif
 *
 * Return: pointer to device information
 */
const struct ionic_devinfo *ionic_api_get_devinfo(void *handle);

#endif /* _IONIC_API_H_ */
