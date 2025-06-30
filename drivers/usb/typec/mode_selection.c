// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Google LLC.
 */

#include <linux/usb/typec_altmode.h>
#include <linux/vmalloc.h>
#include "mode_selection.h"
#include "class.h"

static const char * const mode_names[] = {
	[TYPEC_DP_ALTMODE] = "DP",
	[TYPEC_TBT_ALTMODE] = "TBT",
	[TYPEC_USB4_MODE] = "USB4",
};
static const char * const default_priorities = "USB4 TBT DP";

/* -------------------------------------------------------------------------- */
/* port 'mode_priorities' attribute */
static int typec_mode_parse_priority_string(const char *str, int *list)
{
	const bool user_settings = list[0] == TYPEC_MODE_MAX;
	char *buf, *ptr;
	char *token;
	int ret = 0;

	buf = vmalloc(strlen(str) + 1);
	if (!buf)
		return -ENOMEM;
	for (int i = 0; i <= strlen(str); i++)
		buf[i] = (str[i] == '\n') ? '\0' : str[i];
	ptr = buf;

	while ((token = strsep(&ptr, " ")) && !ret) {
		if (strlen(token)) {
			int mode = 0;

			while ((mode < TYPEC_MODE_MAX) &&
				strcmp(token, mode_names[mode]))
				mode++;
			if (mode == TYPEC_MODE_MAX) {
				ret = -EINVAL;
				continue;
			}

			for (int i = 0; i < TYPEC_MODE_MAX; i++) {
				if (list[i] == TYPEC_MODE_MAX) {
					list[i] = mode;
					break;
				}
				if (list[i] == mode) {
					if (user_settings)
						ret = -EINVAL;
					break;
				}
			}
		}
	}
	vfree(buf);

	return ret;
}

int typec_mode_priorities_set(struct typec_port *port,
		const char *user_priorities)
{
	int list[TYPEC_MODE_MAX];
	int ret;

	for (int i = 0; i < TYPEC_MODE_MAX; i++)
		list[i] = TYPEC_MODE_MAX;

	ret = typec_mode_parse_priority_string(user_priorities, list);
	if (!ret)
		ret = typec_mode_parse_priority_string(default_priorities, list);

	if (!ret)
		for (int i = 0; i < TYPEC_MODE_MAX; i++)
			port->mode_priority_list[i] = list[i];

	return ret;
}

static int port_altmode_supported(struct device *dev, void *data)
{
	if (!strcmp(dev->type->name, ALTERNATE_MODE_DEVICE_TYPE_NAME)) {
		struct typec_altmode *alt = to_typec_altmode(dev);

		if (*(int *)data == typec_svid_to_altmode(alt->svid))
			return 1;
	}
	return 0;
}

static bool port_mode_supported(struct typec_port *port, int mode)
{
	if (mode >= TYPEC_MODE_MAX)
		return false;
	if (mode == TYPEC_USB4_MODE)
		return !!(port->cap->usb_capability & USB_CAPABILITY_USB4);
	return device_for_each_child(&port->dev, &mode, port_altmode_supported);
}

int typec_mode_priorities_get(struct typec_port *port, char *buf)
{
	ssize_t count = 0;

	for (int i = 0; i < TYPEC_MODE_MAX; i++) {
		int mode = port->mode_priority_list[i];

		if (port_mode_supported(port, mode))
			count += sysfs_emit_at(buf, count, "%s ", mode_names[mode]);
	}

	return count + sysfs_emit_at(buf, count, "\n");
}
