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

#include "access.h"
#include "ruleset.h"

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

	/* more fields to come */
};

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
	if (refcount_dec_and_test(&event->usage))
		kfree(event);
}

#endif /* _SECURITY_LANDLOCK_SUPERVISE_H */
