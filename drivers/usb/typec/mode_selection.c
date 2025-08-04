// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Google LLC.
 */

#include <linux/usb/typec_altmode.h>
#include <linux/slab.h>
#include <linux/list.h>
#include "mode_selection.h"
#include "class.h"

static const char * const mode_names[TYPEC_MODE_MAX] = {
	[TYPEC_DP_ALTMODE] = "DisplayPort",
	[TYPEC_TBT_ALTMODE] = "Thunderbolt3",
	[TYPEC_USB4_MODE] = "USB4",
};

static const int default_priorities[TYPEC_MODE_MAX] = {
	[TYPEC_DP_ALTMODE] = 2,
	[TYPEC_TBT_ALTMODE] = 1,
	[TYPEC_USB4_MODE] = 0,
};

/**
 * struct mode_selection_state - State tracking for a specific Type-C mode
 * @mode: The type of mode this instance represents
 * @name: Name string pointer
 * @priority: The mode priority. Higher values indicate a more preferred mode.
 * @list: List head to link this mode state into a prioritized list.
 */
struct mode_selection_state {
	enum typec_mode_type mode;
	const char *name;
	int priority;
	struct list_head list;
};

/* -------------------------------------------------------------------------- */
/* port 'mode_priorities' attribute */
void typec_mode_selection_init(struct typec_port *port)
{
	INIT_LIST_HEAD(&port->mode_list);
}

void typec_mode_selection_destroy(struct typec_port *port)
{
	struct mode_selection_state *ms, *tmp;

	list_for_each_entry_safe(ms, tmp, &port->mode_list, list) {
		list_del(&ms->list);
		kfree(ms);
	}
}

int typec_mode_set_priority(struct typec_port *port,
		const enum typec_mode_type mode, const int priority)
{
	struct mode_selection_state *ms_target = NULL;
	struct mode_selection_state *ms, *tmp;

	if (mode >= TYPEC_MODE_MAX || !mode_names[mode])
		return -EOPNOTSUPP;

	list_for_each_entry_safe(ms, tmp, &port->mode_list, list) {
		if (ms->mode == mode) {
			ms_target = ms;
			list_del(&ms->list);
			break;
		}
	}

	if (!ms_target) {
		ms_target = kzalloc(sizeof(struct mode_selection_state), GFP_KERNEL);
		if (!ms_target)
			return -ENOMEM;
		ms_target->mode = mode;
		ms_target->name = mode_names[mode];
		INIT_LIST_HEAD(&ms_target->list);
	}

	if (priority >= 0)
		ms_target->priority = priority;
	else
		ms_target->priority = default_priorities[mode];

	while (ms_target) {
		struct mode_selection_state *ms_peer = NULL;

		list_for_each_entry(ms, &port->mode_list, list)
			if (ms->priority >= ms_target->priority) {
				if (ms->priority == ms_target->priority)
					ms_peer = ms;
				break;
			}

		list_add_tail(&ms_target->list, &ms->list);
		ms_target = ms_peer;
		if (ms_target) {
			ms_target->priority++;
			list_del(&ms_target->list);
		}
	}

	return 0;
}

int typec_mode_get_priority(struct typec_port *port,
		const enum typec_mode_type mode, int *priority)
{
	struct mode_selection_state *ms;

	list_for_each_entry(ms, &port->mode_list, list)
		if (ms->mode == mode) {
			*priority = ms->priority;
			return 0;
		}

	return -EOPNOTSUPP;
}

ssize_t typec_mode_get_priority_list(struct typec_port *port, char *buf)
{
	struct mode_selection_state *ms;
	ssize_t count = 0;

	list_for_each_entry(ms, &port->mode_list, list)
		count += sysfs_emit_at(buf, count, "%s ", ms->name);

	return count + sysfs_emit_at(buf, count, "\n");
}
