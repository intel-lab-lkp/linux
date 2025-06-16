// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Google LLC.
 */

#include <linux/usb/typec_altmode.h>
#include <linux/vmalloc.h>
#include "class.h"

#define MODE_PRIORITY_DISABLED -1

static const char * const altmode_names[] = {
	[TYPEC_ALTMODE_DP] = "DP",
	[TYPEC_ALTMODE_TBT] = "TBT",
	[TYPEC_ALTMODE_USB4] = "USB4",
};
static const char * const default_priorities = "USB4=0 TBT=1 DP=2";

/* -------------------------------------------------------------------------- */
/* port 'altmode_priorities' attribute */

int typec_mode_priorities_set(struct typec_port *port,
		const char *user_priorities)
{
	int priorities[TYPEC_ALTMODE_MAX];
	const char *str_priority = user_priorities ? : default_priorities;
	char *buf, *buf_free;
	int ret = -EINVAL;
	char *str_name;
	int i;

	buf = vmalloc(strlen(str_priority) + 1);
	if (!buf)
		return -ENOMEM;
	strscpy(buf, str_priority, strlen(str_priority) + 1);
	buf_free = buf;

	for (i = 0; i < TYPEC_ALTMODE_MAX; i++)
		priorities[i] = MODE_PRIORITY_DISABLED;

	while ((str_name = strsep(&buf, " "))) {
		char *str_value = strchr(str_name, '=');
		int value;
		int mode;

		ret = -EINVAL;
		if (!str_value)
			goto parse_exit;
		*str_value++ = '\0';

		if (kstrtoint(str_value, 10, &value) ||
			value < MODE_PRIORITY_DISABLED)
			goto parse_exit;

		if (value > MODE_PRIORITY_DISABLED) {
			for (i = 0; i < TYPEC_ALTMODE_MAX; i++)
				if (value == priorities[i])
					goto parse_exit;
		}

		for (mode = 0; mode < TYPEC_ALTMODE_MAX &&
			strcmp(str_name, altmode_names[mode]);)
			mode++;
		if (mode == TYPEC_ALTMODE_MAX ||
			priorities[mode] != MODE_PRIORITY_DISABLED)
			goto parse_exit;

		priorities[mode] = value;
		ret = 0;
	}

	for (i = 0; i < TYPEC_ALTMODE_MAX; i++)
		port->altmode_priorities[i] = priorities[i];

parse_exit:
	vfree(buf_free);

	return ret;
}

int typec_mode_priorities_get(struct typec_port *port, char *buf)
{
	ssize_t count = 0;
	int i;

	for (i = 0; i < TYPEC_ALTMODE_MAX; i++) {
		if (i != TYPEC_ALTMODE_USB4 ||
				port->cap->usb_capability & USB_CAPABILITY_USB4)
			count += sysfs_emit_at(buf, count, "%s=%d ",
				altmode_names[i], port->altmode_priorities[i]);
	}
	return count + sysfs_emit_at(buf, count, "\n");
}
