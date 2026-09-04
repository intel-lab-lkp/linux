/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Intel Corporation */

#ifndef _IDPF_DEVLINK_H_
#define _IDPF_DEVLINK_H_
#include <net/devlink.h>

struct idpf_adapter;

struct idpf_adapter *idpf_adapter_alloc(struct device *dev);

/**
 * idpf_devlink_free - teardown the devlink
 * @adapter: IDPF adapter structure to free
 */
static inline void idpf_devlink_free(struct idpf_adapter *adapter)
{
	struct devlink *devlink = priv_to_devlink(adapter);

	devlink_free(devlink);
}

/**
 * idpf_devlink_register - register the devlink
 * @adapter: IDPF adapter structure
 */
static inline void idpf_devlink_register(struct idpf_adapter *adapter)
{
	struct devlink *devlink = priv_to_devlink(adapter);

	devlink_register(devlink);
}

/**
 * idpf_devlink_unregister - unregister the devlink
 * @adapter: IDPF adapter structure
 */
static inline void idpf_devlink_unregister(struct idpf_adapter *adapter)
{
	struct devlink *devlink = priv_to_devlink(adapter);

	devlink_unregister(devlink);
}

#endif /* _IDPF_DEVLINK_H_ */
