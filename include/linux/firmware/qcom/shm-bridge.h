/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Linaro Limited
 */

#ifndef _LINUX_QCOM_SHM_BRIDGE
#define _LINUX_QCOM_SHM_BRIDGE

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/gfp.h>
#include <linux/types.h>

struct qcom_shm_bridge_pool;

struct qcom_shm_bridge_pool *qcom_shm_bridge_pool_new(size_t size);
struct qcom_shm_bridge_pool *
qcom_shm_bridge_pool_ref(struct qcom_shm_bridge_pool *pool);
void qcom_shm_bridge_pool_unref(struct qcom_shm_bridge_pool *pool);
struct qcom_shm_bridge_pool *
devm_qcom_shm_bridge_pool_new(struct device *dev, size_t size);

void *qcom_shm_bridge_alloc(struct qcom_shm_bridge_pool *pool,
			    size_t size, gfp_t gfp);
void qcom_shm_bridge_free(void *vaddr);
void *devm_qcom_shm_bridge_alloc(struct device *dev,
				 struct qcom_shm_bridge_pool *pool,
				 size_t size, gfp_t gfp);

phys_addr_t qcom_shm_bridge_to_phys_addr(void *vaddr);

#endif /* _LINUX_QCOM_SHM_BRIDGE */
