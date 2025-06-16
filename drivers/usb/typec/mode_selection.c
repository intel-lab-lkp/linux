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

#define MODE_PRIORITY_DISABLED -1
#define MODE_SELECTION_NO_RESULT 1

static unsigned int mode_selection_timeout = 4000;
module_param(mode_selection_timeout, uint, 0644);
MODULE_PARM_DESC(mode_selection_timeout, "The timeout mode entry, ms");

static unsigned int mode_selection_delay = 1000;
module_param(mode_selection_delay, uint, 0644);
MODULE_PARM_DESC(mode_selection_delay,
	"The delay between attempts to enter or exit a mode, ms");

static const char * const altmode_names[] = {
	[TYPEC_ALTMODE_DP] = "DP",
	[TYPEC_ALTMODE_TBT] = "TBT",
	[TYPEC_ALTMODE_USB4] = "USB4",
};
static const char * const default_priorities = "USB4=0 TBT=1 DP=2";

struct mode_selection_state {
	int mode;
	bool enable;
	bool cable_capability;
	bool enter;
	int result;
};

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

/* -------------------------------------------------------------------------- */
/* partner 'mod_selection' attribute */

/**
 * mode_selection_next() - Process mode selection results and schedule next
 * action
 *
 * This function evaluates the outcome of the previous mode entry or exit
 * attempt. Based on this result, it determines the next alternate mode to
 * process and schedules `mode_selection_work()` if further actions are
 * required.
 *
 * If the previous mode entry was successful, the mode selection sequence is
 * considered complete for the current cycle.
 *
 * If the previous mode entry failed, this function schedules
 * `mode_selection_work()` to attempt exiting the currently active mode.
 *
 * If the previous operation was an exit (after a failed entry attempt),
 * `mode_selection_next()` then advances the internal list of candidate
 * modes to determine the next mode to enter.
 */
static void mode_selection_next(
	struct typec_partner *partner, struct mode_selection_state *ms)
{
	if (!ms->result && ms->enter) {
		dev_info(&partner->dev, "%s mode entered\n", altmode_names[ms->mode]);

		partner->active_mode = ms;
		kfifo_reset(&partner->mode_sequence);
	} else {
		if (ms->result && ms->enter)
			dev_err(&partner->dev, "%s mode entry failed: %pe\n",
				altmode_names[ms->mode], ERR_PTR(ms->result));

		if (ms->result != -EBUSY) {
			if (ms->enter)
				ms->enter = false;
			else
				kfifo_skip(&partner->mode_sequence);
		}

		if (!kfifo_is_empty(&partner->mode_sequence)) {
			cancel_delayed_work(&partner->mode_selection_work);
			schedule_delayed_work(&partner->mode_selection_work,
				msecs_to_jiffies(mode_selection_delay));
		}
	}
}

static void mode_selection_complete(struct typec_partner *partner,
				const int mode, const int result)
{
	struct mode_selection_state *ms;

	mutex_lock(&partner->mode_sequence_lock);
	if (kfifo_peek(&partner->mode_sequence, &ms)) {
		if (ms->mode == mode) {
			ms->result = result;
			mode_selection_next(partner, ms);
		}
	}
	mutex_unlock(&partner->mode_sequence_lock);
}

void typec_mode_selection_altmode_complete(struct typec_altmode *alt,
				const int result)
{
	mode_selection_complete(to_typec_partner(alt->dev.parent),
		TYPEC_SVID_TO_ALTMODE(alt->svid), result);
}
EXPORT_SYMBOL_GPL(typec_mode_selection_altmode_complete);

void typec_mode_selection_usb4_complete(struct typec_partner *partner,
				const int result)
{
	mode_selection_complete(partner, TYPEC_ALTMODE_USB4, result);
}
EXPORT_SYMBOL_GPL(typec_mode_selection_usb4_complete);

static int mode_selection_activate_altmode(struct device *dev, void *data)
{
	if (!strcmp(dev->type->name, ALTERNATE_MODE_DEVICE_TYPE_NAME)) {
		struct typec_altmode *alt = to_typec_altmode(dev);
		struct mode_selection_state *ms = (struct mode_selection_state *)data;

		if (ms->mode == TYPEC_SVID_TO_ALTMODE(alt->svid)) {
			int result = -EOPNOTSUPP;

			if (alt->ops && alt->ops->activate)
				result = alt->ops->activate(alt, ms->enter ? 1 : 0);
			if (ms->enter)
				ms->result = result;
			return 1;
		}
	}

	return 0;
}

static void mode_selection_activate_mode(struct typec_partner *partner,
	struct mode_selection_state *ms)
{
	dev_info(&partner->dev, "Attempt to %s %s mode\n",
		ms->enter ? "enter" : "exit", altmode_names[ms->mode]);

	if (ms->mode == TYPEC_ALTMODE_USB4) {
		struct typec_port *port = to_typec_port(partner->dev.parent);
		int result = -EOPNOTSUPP;

		if (port->ops && port->ops->enter_usb_mode)
			result = port->ops->enter_usb_mode(port,
				ms->enter ? USB_MODE_USB4 : USB_MODE_NONE);

		if (ms->enter)
			ms->result = result;
	} else {
		const int ret = device_for_each_child(&partner->dev, ms,
				mode_selection_activate_altmode);
		if (!ret && ms->enter)
			ms->result = -ENODEV;
	}
}

/**
 * mode_selection_work() - Activate entry into the upcoming mode
 *
 * This function works in conjunction with `mode_selection_next()`.
 * It attempts to activate the next alternate mode in the selection sequence.
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

			if (!ms->enter || ms->result)
				mode_selection_next(partner, ms);
			else {
				ms->result = -ETIME;
				schedule_delayed_work(&partner->mode_selection_work,
					msecs_to_jiffies(mode_selection_timeout));
			}
		}
	}
	mutex_unlock(&partner->mode_sequence_lock);
}

static void mode_selection_init(struct typec_partner *partner)
{
	int i;

	for (i = 0; i < TYPEC_ALTMODE_MAX; i++) {
		partner->mode_states[i].mode = i;
		partner->mode_states[i].result = MODE_SELECTION_NO_RESULT;
	}

	kfifo_reset(&partner->mode_sequence);
	partner->active_mode = NULL;
}

int typec_mode_selection_create(struct typec_partner *partner)
{
	partner->mode_states = vmalloc(
		sizeof(struct mode_selection_state) * TYPEC_ALTMODE_MAX);
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
	struct typec_port *port = to_typec_port(partner->dev.parent);

	if (!partner->mode_states)
		return;

	if (mode < TYPEC_ALTMODE_MAX) {
		if (mode == TYPEC_ALTMODE_USB4) {
			if (!(port->cap->usb_capability & USB_CAPABILITY_USB4))
				return;
		}
		partner->mode_states[mode].enable = true;
	}
}

void typec_mode_selection_add_cable(struct typec_partner *partner,
		struct typec_cable *cable)
{
	const u32 id_header = cable->identity->id_header;
	const u32 vdo0 = cable->identity->vdo[0];
	const u32 vdo1 = cable->identity->vdo[1];
	const u32 type = PD_IDH_PTYPE(id_header);
	const u32 speed = VDO_TYPEC_CABLE_SPEED(vdo0);
	bool capable_dp = true;
	bool capable_tbt = false;
	bool capable_usb4 = false;

	if (!partner->mode_states)
		return;

	if (type == IDH_PTYPE_PCABLE) {
		capable_dp = (speed > CABLE_USB2_ONLY);
		capable_tbt = capable_dp;
		capable_usb4 = capable_dp;
	} else if (type == IDH_PTYPE_ACABLE) {
		const u32 version = VDO_TYPEC_CABLE_VERSION(vdo0);
		const bool usb4_support = VDO_TYPEC_CABLE_USB4_SUPP(vdo1);
		const bool modal_support = PD_IDH_MODAL_SUPP(id_header);

		capable_dp = modal_support;
		capable_tbt = true;
		capable_usb4 = (version == 3) ? usb4_support : modal_support;
	}

	if (capable_dp || capable_tbt || capable_usb4)
		dev_info(&partner->dev, "cable capabilities: %s %s %s\n",
			capable_dp ? altmode_names[TYPEC_ALTMODE_DP] : "",
			capable_tbt ? altmode_names[TYPEC_ALTMODE_TBT] : "",
			capable_usb4 ? altmode_names[TYPEC_ALTMODE_USB4] : "");
	partner->mode_states[TYPEC_ALTMODE_DP].cable_capability = capable_dp;
	partner->mode_states[TYPEC_ALTMODE_TBT].cable_capability = capable_tbt;
	partner->mode_states[TYPEC_ALTMODE_USB4].cable_capability = capable_usb4;
}

static void mode_selection_cancel_work(struct typec_partner *partner)
{
	/*
	 * mode_sequence_lock provides exclusive access to `mode_sequence` FIFO
	 * If the FIFO is empty, no further mode selection activities are expected
	 */
	mutex_lock(&partner->mode_sequence_lock);
	kfifo_reset(&partner->mode_sequence);
	mutex_unlock(&partner->mode_sequence_lock);

	cancel_delayed_work_sync(&partner->mode_selection_work);
}

void typec_mode_selection_destroy(struct typec_partner *partner)
{
	if (!partner->mode_states)
		return;

	mode_selection_cancel_work(partner);
	mutex_destroy(&partner->mode_sequence_lock);
	vfree(partner->mode_states);
	partner->mode_states = NULL;
}

/**
 * typec_mode_selection_start() - Starts the alternate mode selection process.
 *
 * This function populates a 'mode_sequence' FIFO with pointers to
 * `struct mode_selection_state` instances. The sequence is generated based on
 * partner/cable capabilities and prioritized according to the port's settings.
 */
int typec_mode_selection_start(struct typec_partner *partner)
{
	struct typec_port *port = to_typec_port(partner->dev.parent);
	int priorities[TYPEC_ALTMODE_MAX];
	bool pending_mode = true;
	int i;

	if (!partner->mode_states)
		return -ENOMEM;

	mutex_lock(&partner->mode_sequence_lock);
	if (!kfifo_is_empty(&partner->mode_sequence)) {
		mutex_unlock(&partner->mode_sequence_lock);
		return -EINPROGRESS;
	}
	if (partner->active_mode) {
		mutex_unlock(&partner->mode_sequence_lock);
		return -EALREADY;
	}

	mode_selection_init(partner);

	for (i = 0; i < TYPEC_ALTMODE_MAX; i++) {
		if (partner->mode_states[i].enable &&
			partner->mode_states[i].cable_capability)
			priorities[i] = port->altmode_priorities[i];
		else
			priorities[i] = MODE_PRIORITY_DISABLED;
	}

	while (pending_mode) {
		int mode = TYPEC_ALTMODE_MAX;
		int max_priority = INT_MAX;

		for (i = 0; i < TYPEC_ALTMODE_MAX; i++) {
			if (priorities[i] != MODE_PRIORITY_DISABLED &&
				priorities[i] < max_priority) {
				max_priority = priorities[i];
				mode = i;
			}
		}

		if (mode == TYPEC_ALTMODE_MAX)
			pending_mode = false;
		else {
			partner->mode_states[mode].enter = true;
			kfifo_put(&partner->mode_sequence, &partner->mode_states[mode]);
			priorities[mode] = MODE_PRIORITY_DISABLED;
		}
	}

	if (!kfifo_is_empty(&partner->mode_sequence))
		schedule_delayed_work(&partner->mode_selection_work, 0);
	mutex_unlock(&partner->mode_sequence_lock);

	return 0;
}

int typec_mode_selection_reset(struct typec_partner *partner)
{
	if (!partner->mode_states)
		return -ENOMEM;

	mode_selection_cancel_work(partner);

	if (partner->active_mode) {
		partner->active_mode->enter = false;
		mode_selection_activate_mode(partner, partner->active_mode);
	}
	mode_selection_init(partner);

	return 0;
}

int typec_mode_selection_get(struct typec_partner *partner, char *buf)
{
	ssize_t count = 0;
	int i;
	struct mode_selection_state *running_ms;

	if (!partner->mode_states)
		return -ENOMEM;

	mutex_lock(&partner->mode_sequence_lock);
	if (!kfifo_peek(&partner->mode_sequence, &running_ms))
		running_ms = NULL;

	for (i = 0; i < TYPEC_ALTMODE_MAX; i++) {
		struct mode_selection_state *ms = &partner->mode_states[i];

		if (ms->enable) {
			if (!ms->cable_capability)
				count += sysfs_emit_at(buf, count, "%s=nc ", altmode_names[i]);
			else if (ms == running_ms)
				count += sysfs_emit_at(buf, count, "%s=... ", altmode_names[i]);
			else if (ms->result == MODE_SELECTION_NO_RESULT)
				count += sysfs_emit_at(buf, count, "%s ", altmode_names[i]);
			else if (ms->result == 0)
				count += sysfs_emit_at(buf, count, "[%s] ", altmode_names[i]);
			else
				count += sysfs_emit_at(buf, count, "%s=%pe ", altmode_names[i],
					ERR_PTR(ms->result));
		}
	}
	mutex_unlock(&partner->mode_sequence_lock);

	if (count)
		count += sysfs_emit_at(buf, count, "\n");

	return count;
}
