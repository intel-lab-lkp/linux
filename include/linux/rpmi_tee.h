/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * RISC-V RPMI TEE transport interface
 */

#ifndef _LINUX_RPMI_TEE_H
#define _LINUX_RPMI_TEE_H

#include <linux/device.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/uuid.h>

struct rpmi_tee_ops;

struct rpmi_tee_device {
	struct device dev;
	u32 id;
	u32 endpoint_id;	/* RPMI endpoint identifier of the TEE. */
	uuid_t uuid;		/* UUID identifying the TEE service. */
	const struct rpmi_tee_ops *ops;
};

#define to_rpmi_tee_dev(d) container_of(d, struct rpmi_tee_device, dev)

struct rpmi_tee_device_id {
	uuid_t uuid;
};

struct rpmi_tee_driver {
	const char *name;
	int (*probe)(struct rpmi_tee_device *rdev);
	void (*remove)(struct rpmi_tee_device *rdev);
	/* NULL-UUID-terminated list of supported service UUIDs. */
	const struct rpmi_tee_device_id *id_table;
	struct device_driver driver;
};

#define to_rpmi_tee_drv(d) \
	container_of_const(d, struct rpmi_tee_driver, driver)

/**
 * struct rpmi_tee_msg_limits - TEE_CALL service payload limits
 * @max_req_size: Maximum request payload size in bytes.
 * @max_resp_size: Maximum response payload size in bytes.
 *
 * Limits exclude the RPMI TEE_CALL request and response prefixes, but include
 * any service-specific headers supplied by the caller.
 */
struct rpmi_tee_msg_limits {
	size_t max_req_size;
	size_t max_resp_size;
};

/**
 * struct rpmi_tee_info_ops - RPMI TEE transport information operations
 * @msg_limits_get: Return cached TEE_CALL payload limits for the transport
 *	serving @rdev in @limits. Limits remain fixed for the transport lifetime.
 *	Return 0 on success, or a negative error code on failure.
 */
struct rpmi_tee_info_ops {
	int (*msg_limits_get)(struct rpmi_tee_device *rdev,
			      struct rpmi_tee_msg_limits *limits);
};

struct rpmi_tee_msg_ops {
	int (*call)(struct rpmi_tee_device *rdev, const void *req,
		    size_t req_len, void *resp, size_t *resp_len);
};

/* RPMI TEE transport operation groups. */
struct rpmi_tee_ops {
	const struct rpmi_tee_info_ops *info_ops;
	const struct rpmi_tee_msg_ops *msg_ops;
};

extern const struct bus_type rpmi_tee_bus_type;

#if IS_REACHABLE(CONFIG_RISCV_RPMI_TEE_TRANSPORT)
struct rpmi_tee_device *
rpmi_tee_device_register(const uuid_t *uuid, u32 endpoint_id,
			 const struct rpmi_tee_ops *ops, struct device *parent);
void rpmi_tee_device_unregister(struct rpmi_tee_device *rdev);
int rpmi_tee_driver_register(struct rpmi_tee_driver *driver,
			     struct module *owner, const char *mod_name);
void rpmi_tee_driver_unregister(struct rpmi_tee_driver *driver);
#else
static inline struct rpmi_tee_device *
rpmi_tee_device_register(const uuid_t *uuid, u32 endpoint_id,
			 const struct rpmi_tee_ops *ops, struct device *parent)
{
	return NULL;
}

static inline void rpmi_tee_device_unregister(struct rpmi_tee_device *rdev)
{
}

static inline int rpmi_tee_driver_register(struct rpmi_tee_driver *driver,
					   struct module *owner,
					   const char *mod_name)
{
	return -EOPNOTSUPP;
}

static inline void rpmi_tee_driver_unregister(struct rpmi_tee_driver *driver)
{
}
#endif

#define rpmi_tee_register(driver) \
	rpmi_tee_driver_register(driver, THIS_MODULE, KBUILD_MODNAME)
#define rpmi_tee_unregister(driver) \
	rpmi_tee_driver_unregister(driver)

#endif /* _LINUX_RPMI_TEE_H */
