/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright(c) 2025 STMicroelectronics
 */

#ifndef REMOTEPROC_TEE_H
#define REMOTEPROC_TEE_H

#include <linux/tee_drv.h>
#include <linux/firmware.h>
#include <linux/remoteproc.h>

#if IS_ENABLED(CONFIG_REMOTEPROC_TEE)

int rproc_tee_register(struct device *dev, struct rproc **rproc,
		       unsigned int rproc_id, bool auto_boot);
int rproc_tee_unregister(struct device *dev, struct rproc *rproc);
int rproc_tee_parse_fw(struct rproc *rproc, const struct firmware *fw);
int rproc_tee_load_fw(struct rproc *rproc, const struct firmware *fw);
void rproc_tee_release_fw(struct rproc *rproc);
struct resource_table *rproc_tee_find_loaded_rsc_table(struct rproc *rproc,
						       const struct firmware *fw);
int rproc_tee_start(struct rproc *rproc);
int rproc_tee_stop(struct rproc *rproc);
int rproc_tee_pa_to_da(struct rproc *rproc, phys_addr_t pa, size_t size, u64 *da);
#else

static inline int rproc_tee_register(struct device *dev, struct rproc **rproc,
				     unsigned int rproc_id, bool auto_boot)
{
	return -ENODEV;
}

static inline int rproc_tee_parse_fw(struct rproc *rproc, const struct firmware *fw)
{
	/* This shouldn't be possible */
	WARN_ON(1);

	return 0;
}

static inline int rproc_tee_unregister(struct device *dev, struct rproc *rproc)
{
	/* This shouldn't be possible */
	WARN_ON(1);

	return 0;
}

static inline int rproc_tee_load_fw(struct rproc *rproc, const struct firmware *fw)
{
	/* This shouldn't be possible */
	WARN_ON(1);

	return 0;
}

static inline int rproc_tee_start(struct rproc *rproc)
{
	/* This shouldn't be possible */
	WARN_ON(1);

	return 0;
}

static inline int rproc_tee_stop(struct rproc *rproc)
{
	/* This shouldn't be possible */
	WARN_ON(1);

	return 0;
}

static inline void rproc_tee_release_fw(struct rproc *rproc)
{
	/* This shouldn't be possible */
	WARN_ON(1);
}

static inline struct resource_table *
rproc_tee_find_loaded_rsc_table(struct rproc *rproc, const struct firmware *fw)
{
	/* This shouldn't be possible */
	WARN_ON(1);

	return NULL;
}
#endif /* CONFIG_REMOTEPROC_TEE */
#endif /* REMOTEPROC_TEE_H */
