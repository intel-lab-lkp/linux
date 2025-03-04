// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock LSM - Implementation specific to landlock-supervise
 *
 * Copyright © 2025 Tingmao Wang <m@maowtm.org>
 */

#include <linux/path.h>
#include <linux/pid.h>
#include <linux/slab.h>
#include <linux/wait_bit.h>

#include "supervise.h"

struct landlock_supervisor *landlock_create_supervisor(void)
{
	struct landlock_supervisor *supervisor;

	supervisor = kzalloc(sizeof(*supervisor), GFP_KERNEL_ACCOUNT);
	if (!supervisor)
		return ERR_PTR(-ENOMEM);
	refcount_set(&supervisor->usage, 1);
	supervisor->next_event_id = 1;
	spin_lock_init(&supervisor->lock);
	INIT_LIST_HEAD(&supervisor->event_queue);
	INIT_LIST_HEAD(&supervisor->notified_events);
	init_waitqueue_head(&supervisor->poll_event_wq);
	return supervisor;
}

void landlock_get_supervisor(struct landlock_supervisor *const supervisor)
{
	refcount_inc(&supervisor->usage);
}

static void
deny_and_put_event(struct landlock_supervise_event_kernel *const event)
{
	cmpxchg(&event->state, LANDLOCK_SUPERVISE_EVENT_NEW,
		LANDLOCK_SUPERVISE_EVENT_DENIED);
	cmpxchg(&event->state, LANDLOCK_SUPERVISE_EVENT_NOTIFIED,
		LANDLOCK_SUPERVISE_EVENT_DENIED);
	wake_up_var(event);
	landlock_put_supervise_event(event);
}

void landlock_put_supervisor(struct landlock_supervisor *const supervisor)
{
	if (refcount_dec_and_test(&supervisor->usage)) {
		struct landlock_supervise_event_kernel *freeme, *next;

		might_sleep();
		/* we are the only reference, hence no locking */

		/* deny all pending events */
		list_for_each_entry_safe(freeme, next, &supervisor->event_queue,
					 node) {
			list_del(&freeme->node);
			deny_and_put_event(freeme);
		}
		/*
		 * user reply no longer possible without any reference to
		 * supervisor, deny all notified events
		 */
		list_for_each_entry_safe(freeme, next,
					 &supervisor->notified_events, node) {
			list_del(&freeme->node);
			deny_and_put_event(freeme);
		}
		kfree(supervisor);
	}
}
