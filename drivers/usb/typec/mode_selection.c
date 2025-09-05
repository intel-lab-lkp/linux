// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Google LLC.
 */

#include "mode_selection.h"
#include "class.h"
#include "bus.h"

static int increment_duplicated_priority(struct device *dev, void *data)
{
	struct typec_altmode **alt_target = (struct typec_altmode **)data;

	if (is_typec_altmode(dev)) {
		struct typec_altmode *alt = to_typec_altmode(dev);

		if (alt != *alt_target && alt->priority == (*alt_target)->priority) {
			alt->priority++;
			*alt_target = alt;
			return 1;
		}
	}

	return 0;
}

void typec_mode_set_priority(struct typec_altmode *alt,
		const unsigned int priority)
{
	struct typec_port *port = to_typec_port(alt->dev.parent);
	int res = 1;

	alt->priority = priority;

	while (res)
		res = device_for_each_child(&port->dev, &alt,
				increment_duplicated_priority);
}
