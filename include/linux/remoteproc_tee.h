/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright(c) 2024 STMicroelectronics
 */

#ifndef REMOTEPROC_TEE_H
#define REMOTEPROC_TEE_H

#include <linux/tee_drv.h>
#include <linux/firmware.h>
#include <linux/remoteproc.h>

struct rproc;

/**
 * struct tee_rproc - TEE remoteproc structure
 * @node:		Reference in list
 * @rproc:		Remoteproc reference
 * @parent:		Parent device
 * @rproc_id:		Identifier of the target firmware
 * @session_id:		TEE session identifier
 */
struct tee_rproc {
	struct list_head node;
	struct rproc *rproc;
	struct device *parent;
	u32 rproc_id;
	u32 session_id;
};

#if IS_REACHABLE(CONFIG_REMOTEPROC_TEE)

struct tee_rproc *tee_rproc_register(struct device *dev, struct rproc *rproc,
				     unsigned int rproc_id);
int tee_rproc_unregister(struct tee_rproc *trproc);
int tee_rproc_parse_fw(struct rproc *rproc, const struct firmware *fw);
int tee_rproc_load_fw(struct rproc *rproc, const struct firmware *fw);
void tee_rproc_release_fw(struct rproc *rproc);
struct resource_table *tee_rproc_find_loaded_rsc_table(struct rproc *rproc,
						       const struct firmware *fw);
int tee_rproc_start(struct rproc *rproc);
int tee_rproc_stop(struct rproc *rproc);

#else

static inline struct tee_rproc *tee_rproc_register(struct device *dev, struct rproc *rproc,
						   unsigned int rproc_id)
{
	return ERR_PTR(-ENODEV);
}

static inline int tee_rproc_parse_fw(struct rproc *rproc, const struct firmware *fw)
{
	/* This shouldn't be possible */
	WARN_ON(1);

	return 0;
}

static inline int tee_rproc_unregister(struct tee_rproc *trproc)
{
	/* This shouldn't be possible */
	WARN_ON(1);

	return 0;
}

static inline int tee_rproc_load_fw(struct rproc *rproc,  const struct firmware *fw)
{
	/* This shouldn't be possible */
	WARN_ON(1);

	return 0;
}

static inline int tee_rproc_start(struct rproc *rproc)
{
	/* This shouldn't be possible */
	WARN_ON(1);

	return 0;
}

static inline int tee_rproc_stop(struct rproc *rproc)
{
	/* This shouldn't be possible */
	WARN_ON(1);

	return 0;
}

static inline void tee_rproc_release_fw(struct rproc *rproc)
{
	/* This shouldn't be possible */
	WARN_ON(1);
}

static inline struct resource_table *
tee_rproc_find_loaded_rsc_table(struct rproc *rproc, const struct firmware *fw)
{
	/* This shouldn't be possible */
	WARN_ON(1);

	return NULL;
}
#endif /* CONFIG_REMOTEPROC_TEE */
#endif /* REMOTEPROC_TEE_H */
