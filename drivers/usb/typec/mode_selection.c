// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Google LLC.
 */

#include <linux/usb/typec_altmode.h>
#include <linux/vmalloc.h>
#include <linux/usb/pd_vdo.h>
#include <linux/kfifo.h>
#include "mode_selection.h"
#include "class.h"

static unsigned int mode_selection_timeout = 4000;
module_param(mode_selection_timeout, uint, 0644);
MODULE_PARM_DESC(mode_selection_timeout, "The timeout mode entry, ms");

static unsigned int mode_selection_delay = 1000;
module_param(mode_selection_delay, uint, 0644);
MODULE_PARM_DESC(mode_selection_delay,
	"The delay between attempts to enter or exit a mode, ms");

static unsigned int mode_selection_entry_attempts = 4;
module_param(mode_selection_entry_attempts, uint, 0644);
MODULE_PARM_DESC(mode_selection_entry_attempts,
	"Max attempts to enter mode on BUSY result");

static const char * const mode_names[] = {
	[TYPEC_DP_ALTMODE] = "DP",
	[TYPEC_TBT_ALTMODE] = "TBT",
	[TYPEC_USB4_MODE] = "USB4",
};
static const char * const default_priorities = "USB4 TBT DP";

struct mode_selection_state {
	int mode;
	bool enable;
	bool cable_capability;
	bool enter;
	int attempt_count;
	int result;
};

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

/* -------------------------------------------------------------------------- */
/* partner 'mod_selection' attribute */

/**
 * mode_selection_next() - Process mode selection results and schedule next
 * action
 *
 * This function evaluates the outcome of the previous mode entry or exit
 * attempt. Based on this result, it determines the next mode to process and
 * schedules `mode_selection_work()` if further actions are required.
 *
 * If the previous mode entry was successful, the mode selection sequence is
 * considered complete for the current cycle.
 *
 * If the previous mode entry failed, this function schedules
 * `mode_selection_work()` to attempt exiting the mode that was partially
 * activated but not fully entered.
 *
 * If the previous operation was an exit (after a failed entry attempt),
 * `mode_selection_next()` then advances the internal list of candidate
 * modes to determine the next mode to enter.
 */
static void mode_selection_next(
	struct typec_partner *partner, struct mode_selection_state *ms)
{
	if (!ms->enter) {
		kfifo_skip(&partner->mode_sequence);
	} else if (!ms->result) {
		dev_info(&partner->dev, "%s mode entered\n", mode_names[ms->mode]);

		partner->active_mode = ms;
		kfifo_reset(&partner->mode_sequence);
	} else {
		dev_err(&partner->dev, "%s mode entry failed: %pe\n",
			mode_names[ms->mode], ERR_PTR(ms->result));

		if (ms->result != -EBUSY ||
			ms->attempt_count >= mode_selection_entry_attempts)
			ms->enter = false;
	}

	if (!kfifo_is_empty(&partner->mode_sequence))
		schedule_delayed_work(&partner->mode_selection_work,
			msecs_to_jiffies(mode_selection_delay));
}

static void mode_selection_complete(struct typec_partner *partner,
				const int mode, const int result)
{
	struct mode_selection_state *ms;

	mutex_lock(&partner->mode_sequence_lock);
	if (kfifo_peek(&partner->mode_sequence, &ms)) {
		if (ms->mode == mode) {
			ms->result = result;
			cancel_delayed_work(&partner->mode_selection_work);
			mode_selection_next(partner, ms);
		}
	}
	mutex_unlock(&partner->mode_sequence_lock);
}

void typec_mode_selection_altmode_complete(struct typec_altmode *alt,
				const int result)
{
	mode_selection_complete(to_typec_partner(alt->dev.parent),
		typec_svid_to_altmode(alt->svid), result);
}
EXPORT_SYMBOL_GPL(typec_mode_selection_altmode_complete);

void typec_mode_selection_usb4_complete(struct typec_partner *partner,
				const int result)
{
	mode_selection_complete(partner, TYPEC_USB4_MODE, result);
}
EXPORT_SYMBOL_GPL(typec_mode_selection_usb4_complete);

static void mode_selection_activate_usb4_mode(struct typec_partner *partner,
	struct mode_selection_state *ms)
{
	struct typec_port *port = to_typec_port(partner->dev.parent);
	int result = -EOPNOTSUPP;

	if (port->ops && port->ops->enter_usb_mode) {
		if (ms->enter && port->usb_mode != USB_MODE_USB4)
			result = -EPERM;
		else
			result = port->ops->enter_usb_mode(port,
				ms->enter ? USB_MODE_USB4 : USB_MODE_USB3);
	}

	if (ms->enter)
		ms->result = result;
}

static int mode_selection_activate_altmode(struct device *dev, void *data)
{
	struct typec_altmode *alt = to_typec_altmode(dev);
	struct mode_selection_state *ms = (struct mode_selection_state *)data;
	int result = -ENODEV;

	if (!strcmp(dev->type->name, ALTERNATE_MODE_DEVICE_TYPE_NAME)) {
		if (ms->mode == typec_svid_to_altmode(alt->svid)) {
			if (alt->ops && alt->ops->activate)
				result = alt->ops->activate(alt, ms->enter ? 1 : 0);
			else
				result = -EOPNOTSUPP;
		}
	}

	if (ms->enter)
		ms->result = result;

	return result == -ENODEV ? 0 : 1;
}

static void mode_selection_activate_mode(struct typec_partner *partner,
	struct mode_selection_state *ms)
{
	dev_info(&partner->dev, "%s %s mode\n",
		ms->enter ? "Enter" : "Exit", mode_names[ms->mode]);

	if (ms->enter)
		ms->attempt_count++;

	if (ms->mode == TYPEC_USB4_MODE)
		mode_selection_activate_usb4_mode(partner, ms);
	else
		device_for_each_child(&partner->dev, ms,
			mode_selection_activate_altmode);

	if (ms->enter && ms->result)
		dev_err(&partner->dev, "%s mode activation failed: %pe\n",
			mode_names[ms->mode], ERR_PTR(ms->result));
}

/**
 * mode_selection_work() - Activate entry into the upcoming mode
 *
 * This function works in conjunction with `mode_selection_next()`.
 * It attempts to activate the next mode in the selection sequence.
 *
 * If the mode activation (`mode_selection_activate_mode()`) fails,
 * `mode_selection_next()` will be called to initiate a new selection cycle.
 *
 * Otherwise, the result is temporarily set to -ETIME, and
 * `mode_selection_activate_mode()` is scheduled for a subsequent entry after a
 * timeout period. The alternate mode driver is expected to call back with the
 * actual mode entry result. Upon this callback, `mode_selection_next()` will
 * determine the subsequent mode and re-schedule mode_selection_work().
 */
static void mode_selection_work(struct work_struct *work)
{
	struct typec_partner *partner = container_of(work, struct typec_partner,
						  mode_selection_work.work);
	struct mode_selection_state *ms;

	mutex_lock(&partner->mode_sequence_lock);
	if (kfifo_peek(&partner->mode_sequence, &ms)) {
		if (ms->enter && ms->result == -ETIME) {
			mode_selection_next(partner, ms);
		} else {
			mode_selection_activate_mode(partner, ms);

			if (ms->enter) {
				if (!ms->result) {
					ms->result = -ETIME;
					schedule_delayed_work(&partner->mode_selection_work,
						msecs_to_jiffies(mode_selection_timeout));
				} else {
					ms->enter = ms->result == -EBUSY;
					mode_selection_next(partner, ms);
				}
			} else
				mode_selection_next(partner, ms);
		}
	}
	mutex_unlock(&partner->mode_sequence_lock);
}

static void mode_selection_init(struct typec_partner *partner)
{
	for (int i = 0; i < TYPEC_MODE_MAX; i++) {
		partner->mode_states[i].mode = i;
		partner->mode_states[i].enter = true;
		partner->mode_states[i].result = 0;
		partner->mode_states[i].attempt_count = 0;
	}

	kfifo_reset(&partner->mode_sequence);
	partner->active_mode = NULL;
}

int typec_mode_selection_create(struct typec_partner *partner)
{
	partner->mode_states = vmalloc(
		sizeof(struct mode_selection_state) * TYPEC_MODE_MAX);
	if (!partner->mode_states)
		return -ENOMEM;

	INIT_KFIFO(partner->mode_sequence);
	mutex_init(&partner->mode_sequence_lock);
	mode_selection_init(partner);
	INIT_DELAYED_WORK(&partner->mode_selection_work, mode_selection_work);

	return 0;
}

void typec_mode_selection_add_mode(struct typec_partner *partner,
		const int mode)
{
	if (partner->mode_states)
		partner->mode_states[mode].enable =
			port_mode_supported(to_typec_port(partner->dev.parent), mode);
}

void typec_mode_selection_add_cable(struct typec_partner *partner,
		struct typec_cable *cable)
{
	const u32 id_header = cable->identity->id_header;
	const u32 vdo1 = cable->identity->vdo[0];
	const u32 type = PD_IDH_PTYPE(id_header);
	const u32 speed = VDO_TYPEC_CABLE_SPEED(vdo1);
	bool capability[] = {
		[TYPEC_DP_ALTMODE] = true,
		[TYPEC_TBT_ALTMODE] = false,
		[TYPEC_USB4_MODE] = false,
	};

	if (!partner->mode_states)
		return;

	if (type == IDH_PTYPE_PCABLE) {
		capability[TYPEC_DP_ALTMODE] = (speed > CABLE_USB2_ONLY);
		capability[TYPEC_TBT_ALTMODE] = (speed > CABLE_USB2_ONLY);
		capability[TYPEC_USB4_MODE] = (speed > CABLE_USB2_ONLY);
	} else if (type == IDH_PTYPE_ACABLE) {
		const u32 vdo2 = cable->identity->vdo[1];
		const u32 version = VDO_TYPEC_CABLE_VERSION(vdo1);
		const bool usb4_support = VDO_TYPEC_CABLE_USB4_SUPP(vdo2);
		const bool modal_support = PD_IDH_MODAL_SUPP(id_header);

		capability[TYPEC_DP_ALTMODE] = modal_support;
		capability[TYPEC_TBT_ALTMODE] = true;
		if (version == CABLE_VDO_VER1_3)
			capability[TYPEC_USB4_MODE] = usb4_support;
		else
			capability[TYPEC_USB4_MODE] = modal_support;
	}

	for (int i = 0; i < TYPEC_MODE_MAX; i++)
		partner->mode_states[i].cable_capability = capability[i];
}

void typec_mode_selection_destroy(struct typec_partner *partner)
{
	if (!partner->mode_states)
		return;

	mutex_lock(&partner->mode_sequence_lock);
	kfifo_reset(&partner->mode_sequence);
	mutex_unlock(&partner->mode_sequence_lock);

	cancel_delayed_work_sync(&partner->mode_selection_work);
	mutex_destroy(&partner->mode_sequence_lock);
	vfree(partner->mode_states);
	partner->mode_states = NULL;
}

/**
 * typec_mode_selection_start() - Starts the mode selection process.
 *
 * This function populates a 'mode_sequence' FIFO with pointers to
 * `struct mode_selection_state` instances. The sequence is generated based on
 * partner/cable capabilities and prioritized according to the port's settings.
 */
int typec_mode_selection_start(struct typec_partner *partner)
{
	struct typec_port *port = to_typec_port(partner->dev.parent);
	int ret = 0;

	if (!partner->mode_states)
		return -ENOMEM;

	mutex_lock(&partner->mode_sequence_lock);

	if (!kfifo_is_empty(&partner->mode_sequence))
		ret = -EINPROGRESS;
	else if (partner->active_mode)
		ret = -EALREADY;
	else {
		mode_selection_init(partner);

		for (int i = 0; i < TYPEC_MODE_MAX; i++) {
			const int mode = port->mode_priority_list[i];
			struct mode_selection_state *ms;

			if (mode < TYPEC_MODE_MAX) {
				ms = &partner->mode_states[mode];
				if (ms->enable && ms->cable_capability)
					kfifo_put(&partner->mode_sequence, ms);
			}
		}

		if (!kfifo_is_empty(&partner->mode_sequence))
			schedule_delayed_work(&partner->mode_selection_work, 0);
	}

	mutex_unlock(&partner->mode_sequence_lock);

	return ret;
}

/**
 * typec_mode_selection_reset() - Reset the mode selection process.
 *
 * This function cancels ongoing mode selection and exits the currently active
 * mode, if present.
 * It returns -EINPROGRESS when a mode exit is already scheduled, or a mode
 * entry is ongoing, indicating that the reset cannot immediately complete.
 */
int typec_mode_selection_reset(struct typec_partner *partner)
{
	struct mode_selection_state *ms;

	if (!partner->mode_states)
		return -ENOMEM;

	mutex_lock(&partner->mode_sequence_lock);
	if (kfifo_peek(&partner->mode_sequence, &ms)) {
		kfifo_reset(&partner->mode_sequence);

		if (!ms->enter || ms->result) {
			ms->attempt_count = mode_selection_entry_attempts;
			kfifo_put(&partner->mode_sequence, ms);
			mutex_unlock(&partner->mode_sequence_lock);

			return -EINPROGRESS;
		}
	}

	if (partner->active_mode) {
		partner->active_mode->enter = false;
		mode_selection_activate_mode(partner, partner->active_mode);
	}
	mode_selection_init(partner);
	mutex_unlock(&partner->mode_sequence_lock);

	return 0;
}

int typec_mode_selection_get(struct typec_partner *partner, char *buf)
{
	ssize_t count = 0;
	struct mode_selection_state *running_ms;

	if (!partner->mode_states)
		return -ENOMEM;

	mutex_lock(&partner->mode_sequence_lock);
	if (!kfifo_peek(&partner->mode_sequence, &running_ms))
		running_ms = NULL;

	for (int i = 0; i < TYPEC_MODE_MAX; i++) {
		struct mode_selection_state *ms = &partner->mode_states[i];

		if (ms->enable) {
			if (!ms->cable_capability)
				count += sysfs_emit_at(buf, count, "%s=nc ", mode_names[i]);
			else if (ms == running_ms)
				count += sysfs_emit_at(buf, count, "%s=... ", mode_names[i]);
			else if (ms->attempt_count == 0)
				count += sysfs_emit_at(buf, count, "%s ", mode_names[i]);
			else if (ms->result == 0)
				count += sysfs_emit_at(buf, count, "[%s] ", mode_names[i]);
			else
				count += sysfs_emit_at(buf, count, "%s=%pe ", mode_names[i],
					ERR_PTR(ms->result));
		}
	}
	mutex_unlock(&partner->mode_sequence_lock);

	if (count)
		count += sysfs_emit_at(buf, count, "\n");

	return count;
}
