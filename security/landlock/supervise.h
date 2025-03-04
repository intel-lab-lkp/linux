/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Landlock LSM - Implementation specific to landlock-supervise
 *
 * Copyright © 2025 Tingmao Wang <m@maowtm.org>
 */

#ifndef _SECURITY_LANDLOCK_SUPERVISE_H
#define _SECURITY_LANDLOCK_SUPERVISE_H

#include <linux/refcount.h>
#include <linux/wait.h>
#include <linux/path.h>
#include <linux/pid.h>
#include <uapi/linux/landlock.h>

#include "access.h"
#include "ruleset.h"

/**
 * Each supervisor is associated with one active layer in a
 * domain (or associated with a not-yet-active layer in a struct
 * landlock_ruleset).  User-space interact with the event queue
 * through a landlock_supervise_fd.
 */
struct landlock_supervisor {
	refcount_t usage;
	spinlock_t lock;
	/* protected by @lock, contains landlock_supervise_event_kernel */
	struct list_head event_queue;
	/* protected by @lock, contains landlock_supervise_event_kernel */
	struct list_head notified_events;
	struct wait_queue_head poll_event_wq;
	/* protected by @lock */
	u32 next_event_id;
};

enum landlock_supervise_event_state {
	LANDLOCK_SUPERVISE_EVENT_NEW,
	LANDLOCK_SUPERVISE_EVENT_NOTIFIED,
	LANDLOCK_SUPERVISE_EVENT_ALLOWED,
	LANDLOCK_SUPERVISE_EVENT_DENIED,
};

struct landlock_supervise_event_kernel {
	struct list_head node;
	refcount_t usage;
	enum landlock_supervise_event_state state;

	/* Cookie as presented to user-space */
	u32 event_id;

	landlock_supervise_event_type_t type;
	access_mask_t access_request;
	struct pid *accessor;
	union {
		struct {
			/**
			 * @target_1: The first (and may be the only, for
			 * most requests) target path. To expose as much
			 * useful information to the supervisor as possible,
			 * for file creation and deletion, this points to the
			 * actual path being created (or deleted), rather
			 * than the parent directory. Note that for the
			 * create case, this means that the dentry will be
			 * negative (unless we end up in some horrible race).
			 * In the create case, target_1_is_new is set, so
			 * that we know to pass the parent as the fd to the
			 * user-space supervisor, and fill destname with the
			 * name of the file.
			 *
			 * For refer (link and rename), this points to the
			 * source (or simply the first argument in case of
			 * exchange) being linked. It will necessarily have
			 * to be an existing file (even though the dentry may
			 * turn negative).
			 */
			struct path target_1;
			/**
			 * @target_2: The destination path for link and
			 * rename (or simply the second argument in case of
			 * exchange). target_2_is_new will be set unless this
			 * is an exchange.
			 */
			struct path target_2;

			u8 target_1_is_new : 1;
			u8 target_2_is_new : 1;
		};
		struct {
			__u16 port;
		};
	};
};

#define LANDLOCK_SUPERVISE_EVENT_HANDLED(event)                \
	((event)->state == LANDLOCK_SUPERVISE_EVENT_ALLOWED || \
	 (event)->state == LANDLOCK_SUPERVISE_EVENT_DENIED)

struct landlock_supervisor *landlock_create_supervisor(void);
void landlock_get_supervisor(struct landlock_supervisor *const supervisor);
void landlock_put_supervisor(struct landlock_supervisor *const supervisor);

static inline void landlock_get_supervise_event(
	struct landlock_supervise_event_kernel *const event)
{
	refcount_inc(&event->usage);
}

static inline void landlock_put_supervise_event(
	struct landlock_supervise_event_kernel *const event)
{
	if (refcount_dec_and_test(&event->usage)) {
		switch (event->type) {
		case LANDLOCK_SUPERVISE_EVENT_TYPE_FS_ACCESS:
			if (event->target_1.dentry)
				path_put(&event->target_1);
			if (event->target_2.dentry)
				path_put(&event->target_2);
			break;
		case LANDLOCK_SUPERVISE_EVENT_TYPE_NET_ACCESS:
			break;
		}
		put_pid(event->accessor);
		kfree(event);
	}
}

DEFINE_FREE(landlock_put_supervise_event,
	    struct landlock_supervise_event_kernel *,
	    if (_T) landlock_put_supervise_event(_T))

static inline bool
landlock_has_supervisors(const struct landlock_ruleset *const domain)
{
	size_t layer_level;
	for (layer_level = 0; layer_level < domain->num_layers; layer_level++) {
		if (domain->layer_stack[layer_level].supervisor)
			return true;
	}
	return false;
}

static inline layer_mask_t landlock_layer_masks_to_denied_layers(
	const access_mask_t access_request, const layer_mask_t layer_masks[],
	const size_t masks_array_size, const int num_layers)
{
	unsigned long access_req = access_request;
	layer_mask_t denied_layers = 0;
	size_t layer_level;
	unsigned long access_bit;

	for (layer_level = 0; layer_level < num_layers; layer_level++) {
		for_each_set_bit(access_bit, &access_req, masks_array_size) {
			if (layer_masks[access_bit] & BIT_ULL(layer_level))
				denied_layers |= BIT_ULL(layer_level);
		}
	}

	return denied_layers;
}

bool landlock_ask_supervised_layers(
	const struct landlock_ruleset *const domain,
	const layer_mask_t denied_layers,
	const landlock_supervise_event_type_t request_type,
	const access_mask_t access_request, const struct path *const path1,
	const struct path *const path2, const bool path1_new,
	const bool path2_new, const __u16 port);

#endif /* _SECURITY_LANDLOCK_SUPERVISE_H */
