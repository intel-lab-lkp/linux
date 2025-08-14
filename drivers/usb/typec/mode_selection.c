// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Google LLC.
 */

#include <linux/usb/typec_altmode.h>
#include <linux/slab.h>
#include <linux/list.h>
#include "mode_selection.h"
#include "class.h"

static const char * const mode_names[TYPEC_ALTMODE_MAX] = {
	[TYPEC_ALTMODE_DP] = "DisplayPort",
	[TYPEC_ALTMODE_TBT] = "Thunderbolt3",
	[TYPEC_ALTMODE_USB4] = "USB4",
};

static const int default_priorities[TYPEC_ALTMODE_MAX] = {
	[TYPEC_ALTMODE_DP] = 2,
	[TYPEC_ALTMODE_TBT] = 1,
	[TYPEC_ALTMODE_USB4] = 0,
};

static inline enum typec_mode_type typec_svid_to_altmode(const u16 svid)
{
	switch (svid) {
	case USB_TYPEC_DP_SID:
		return TYPEC_ALTMODE_DP;
	case USB_TYPEC_TBT_SID:
		return TYPEC_ALTMODE_TBT;
	case USB_TYPEC_USB4_SID:
		return TYPEC_ALTMODE_USB4;
	}
	return TYPEC_ALTMODE_MAX;
}

/**
 * struct mode_selection_state - State tracking for a specific Type-C mode
 * @mode: The type of mode this instance represents
 * @priority: The mode priority. Lower values indicate a more preferred mode.
 * @list: List head to link this mode state into a prioritized list.
 */
struct mode_selection_state {
	enum typec_mode_type mode;
	int priority;
	struct list_head list;
};

/* -------------------------------------------------------------------------- */
/* port 'mode_priorities' attribute */

int typec_mode_set_priority(struct typec_altmode *adev, const int priority)
{
	struct typec_port *port = to_typec_port(adev->dev.parent);
	const enum typec_mode_type mode = typec_svid_to_altmode(adev->svid);
	struct mode_selection_state *ms_target = NULL;
	struct mode_selection_state *ms, *tmp;

	if (mode >= TYPEC_ALTMODE_MAX || !mode_names[mode])
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

int typec_mode_get_priority(struct typec_altmode *adev, int *priority)
{
	struct typec_port *port = to_typec_port(adev->dev.parent);
	const enum typec_mode_type mode = typec_svid_to_altmode(adev->svid);
	struct mode_selection_state *ms;

	list_for_each_entry(ms, &port->mode_list, list)
		if (ms->mode == mode) {
			*priority = ms->priority;
			return 0;
		}

	return -EOPNOTSUPP;
}

void typec_mode_selection_destroy(struct typec_port *port)
{
	struct mode_selection_state *ms, *tmp;

	list_for_each_entry_safe(ms, tmp, &port->mode_list, list) {
		list_del(&ms->list);
		kfree(ms);
	}
}
