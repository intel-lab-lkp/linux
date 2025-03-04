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

#ifdef pr_fmt
#undef pr_fmt
#endif

#define pr_fmt(fmt) "landlock-supervise: " fmt

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

/**
 * landlock_ask_supervised_layers - check if all denied layers
 * are supervised, and if yes, ask all of them for permission.
 *
 * Return whether access should be allowed.  If denied_layers
 * contains any non-supervised layer, will return false without
 * making any supervisor event.
 *
 * Caller owns any paths passed in, we might get refs.
 */
bool landlock_ask_supervised_layers(
	const struct landlock_ruleset *const domain,
	const layer_mask_t denied_layers,
	const landlock_supervise_event_type_t request_type,
	const access_mask_t access_request, const struct path *const path1,
	const struct path *const path2, const bool path1_new,
	const bool path2_new, const __u16 port)
{
	size_t layer_level;
	unsigned long denied_layers_ = denied_layers;

	if (WARN_ON_ONCE(!denied_layers)) {
		return true;
	}

	for_each_set_bit(layer_level, &denied_layers_, domain->num_layers) {
		if (!domain->layer_stack[layer_level].supervisor) {
			return false;
		}
	}

	/*
	 * All denied layers are supervisor layers, so we just ask
	 * them in turn. There's good argument for either order (top
	 * -> bottom, or the other way), so we just do the easiest
	 * thing here.
	 */

	for_each_set_bit(layer_level, &denied_layers_, domain->num_layers) {
		struct landlock_supervisor *const supervisor =
			domain->layer_stack[layer_level].supervisor;

		/*
		 * supervisor will stay valid here because we're blocking
		 * this thread which references the layer, which in terms
		 * references the supervisor.
		 */

		/* TODO: memchg supervisor owner then allocate with account */
		struct landlock_supervise_event_kernel *event __free(
			landlock_put_supervise_event) =
			kzalloc(sizeof(*event), GFP_KERNEL_ACCOUNT);

		int rc;

		if (!event) {
			pr_alert(
				"failed to allocate memory for supervisor event\n");
			return false;
		}

		refcount_set(&event->usage, 1);
		event->state = LANDLOCK_SUPERVISE_EVENT_NEW;

		event->type = request_type;
		event->access_request = access_request;
		event->accessor = get_pid(task_pid(current));
		switch (request_type) {
		case LANDLOCK_SUPERVISE_EVENT_TYPE_FS_ACCESS:
			if (path1) {
				path_get(path1);
				event->target_1 = *path1;
				event->target_1_is_new = path1_new;
			}
			if (path2) {
				path_get(path2);
				event->target_2 = *path2;
				event->target_2_is_new = path2_new;
			}
			break;
		case LANDLOCK_SUPERVISE_EVENT_TYPE_NET_ACCESS:
			event->port = port;
			break;
		}

		if (WARN_ON(!supervisor)) {
			/*
			 * We checked all denied layers are supervised
			 * earlier...
			 */
			return false;
		}

		spin_lock(&supervisor->lock);
		event->event_id = supervisor->next_event_id++;
		landlock_get_supervise_event(event);
		list_add_tail(&event->node, &supervisor->event_queue);
		spin_unlock(&supervisor->lock);
		wake_up(&supervisor->poll_event_wq);

		rc = wait_var_event_killable(
			event, LANDLOCK_SUPERVISE_EVENT_HANDLED(event));
		if (rc) {
			/* Task died, doesn't matter what we say */
			return false;
		}
		if (event->state != LANDLOCK_SUPERVISE_EVENT_ALLOWED) {
			return false;
		}

		/* event has __free */
	}

	return true;
}
