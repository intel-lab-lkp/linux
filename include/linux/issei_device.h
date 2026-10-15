/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025-2026 Intel Corporation */
#ifndef _ISSEI_DEVICE_H_
#define _ISSEI_DEVICE_H_

#include <linux/issei.h>
#include <linux/types.h>
#include <linux/uuid.h>

/* add MODULE_IMPORT_NS("INTEL_SSEI") to use these APIs */

struct class_interface;
struct device;

#if IS_ENABLED(CONFIG_INTEL_SSEI)

void *issei_device_open(int (*match)(struct device *, const void *), const void *data);
void issei_device_release(void *ctx);

int issei_device_connect(void *ctx, const uuid_t *client_uuid, struct issei_client *conn);
int issei_device_disconnect(void *ctx);

ssize_t issei_device_write(void *ctx, const u8 *ubuf, size_t length, unsigned long timeout);
ssize_t issei_device_read(void *ctx, u8 *ubuf, size_t length, unsigned long timeout);

int issei_device_dma_map(void *ctx, size_t size, dma_addr_t *daddr, void **vaddr);
void issei_device_dma_unmap(void *ctx);

int issei_device_register_interface(struct class_interface *intf);
#define issei_device_unregister_interface(intf) \
	class_interface_unregister(intf)

#else

void *issei_device_open(int (*match)(struct device *, const void *), const void *data)
{
	return ERR_PTR(-EOPNOTSUPP);
}

void issei_device_release(void *ctx)
{
}

int issei_device_connect(void *ctx, const uuid_t *client_uuid, struct issei_client *conn)
{
	return -EOPNOTSUPP;
}

int issei_device_disconnect(void *ctx)
{
	return -EOPNOTSUPP;
}

ssize_t issei_device_write(void *ctx, const u8 *ubuf, size_t length, unsigned long timeout)
{
	return -EOPNOTSUPP;
}

ssize_t issei_device_read(void *ctx, u8 *ubuf, size_t length, unsigned long timeout);
{
	return -EOPNOTSUPP;
}

int issei_device_dma_map(void *ctx, size_t size, dma_addr_t *daddr, void **vaddr);
{
	return -EOPNOTSUPP;
}

void issei_device_dma_unmap(void *ctx);
{
}

int issei_device_register_interface(struct class_interface *intf)
{
	return -EOPNOTSUPP;
}

void issei_device_unregister_interface(struct class_interface *intf)
{
}

#endif /* CONFIG_INTEL_SSEI */

#endif /* _ISSEI_DEVICE_H_ */
