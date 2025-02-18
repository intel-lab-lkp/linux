/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _XE_DRM_CLIENT_H_
#define _XE_DRM_CLIENT_H_

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/spinlock.h>

#define MAX_BLAME_LEN	50

struct drm_file;
struct drm_printer;
struct pagefault;
struct xe_bo;
struct xe_exec_queue;

struct blame {
	/** @exec_queue_id: ID number of banned exec queue */
	u32 exec_queue_id;
	/** @pf: pagefault on engine of banned exec queue, if any at time */
	struct pagefault *pf;
	/** @list: link into @xe_drm_client.blame_list */
	struct list_head list;
};

struct xe_drm_client {
	struct kref kref;
	unsigned int id;
#ifdef CONFIG_PROC_FS
	/**
	 * @bos_lock: lock protecting @bos_list
	 */
	spinlock_t bos_lock;
	/**
	 * @bos_list: list of bos created by this client
	 *
	 * Protected by @bos_lock.
	 */
	struct list_head bos_list;
	/**
	 * @blame_lock: lock protecting @blame_list
	 */
	spinlock_t blame_lock;
	/**
	 * @blame_list: list of banned exec queues associated with this drm
	 *		client, as well as any pagefaults at time of ban.
	 *
	 * Protected by @blame_lock;
	 */
	struct list_head blame_list;
	/**
	 * @blame_len: length of @blame_list
	 */
	unsigned int blame_len;
	/** @reset_count: number of times this drm client has seen an engine reset */
	atomic_t reset_count;
#endif
};

	static inline struct xe_drm_client *
xe_drm_client_get(struct xe_drm_client *client)
{
	kref_get(&client->kref);
	return client;
}

void __xe_drm_client_free(struct kref *kref);

static inline void xe_drm_client_put(struct xe_drm_client *client)
{
	kref_put(&client->kref, __xe_drm_client_free);
}

struct xe_drm_client *xe_drm_client_alloc(void);
static inline struct xe_drm_client *
xe_drm_client_get(struct xe_drm_client *client);
static inline void xe_drm_client_put(struct xe_drm_client *client);
#ifdef CONFIG_PROC_FS
void xe_drm_client_fdinfo(struct drm_printer *p, struct drm_file *file);
void xe_drm_client_add_bo(struct xe_drm_client *client,
			  struct xe_bo *bo);
void xe_drm_client_remove_bo(struct xe_bo *bo);
void xe_drm_client_add_blame(struct xe_drm_client *client,
			     struct xe_exec_queue *q);
void xe_drm_client_remove_blame(struct xe_drm_client *client,
				struct xe_exec_queue *q);
#else
static inline void xe_drm_client_add_bo(struct xe_drm_client *client,
					struct xe_bo *bo)
{
}

static inline void xe_drm_client_remove_bo(struct xe_bo *bo)
{
}

static inline void xe_drm_client_add_blame(struct xe_drm_client *client,
					   struct xe_exec_queue *q)
{
}

static inline void xe_drm_client_remove_blame(struct xe_drm_client *client,
					      struct xe_exec_queue *q)
{
}
#endif
#endif
