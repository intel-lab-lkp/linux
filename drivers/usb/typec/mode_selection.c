// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Google LLC.
 */

#include <linux/usb/typec_altmode.h>
#include <linux/slab.h>
#include <linux/usb/pd_vdo.h>
#include <linux/kfifo.h>
#include <linux/list.h>
#include "mode_selection.h"
#include "class.h"

/* Timeout for a mode entry attempt, ms */
static const unsigned int mode_selection_timeout = 4000;
/* Delay between mode entry/exit attempts, ms */
static const unsigned int mode_selection_delay = 1000;
/* Maximum retries for mode entry on busy status */
static const unsigned int mode_entry_attempts = 4;

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
 * enum ms_state - Specific mode selection states
 * @MS_STATE_IDLE: The mode entry process has not started
 * @MS_STATE_INPROGRESS: The mode entry process is currently underway
 * @MS_STATE_ACTIVE: The mode has been successfully entered
 * @MS_STATE_CABLE_FAILED: The connected cable doesn't support the mode
 * @MS_STATE_TIMEOUT: Mode entry failed due to a timeout
 * @MS_STATE_FAILED: The mode driver reported the error
 */
enum ms_state {
	MS_STATE_IDLE = 0,
	MS_STATE_INPROGRESS,
	MS_STATE_ACTIVE,
	MS_STATE_CABLE_FAILED,
	MS_STATE_TIMEOUT,
	MS_STATE_FAILED,
	MS_STATE_MAX
};
static const char * const ms_state_strings[MS_STATE_MAX] = {
	[MS_STATE_IDLE] = "none",
	[MS_STATE_INPROGRESS] = "in progress",
	[MS_STATE_ACTIVE] = "active",
	[MS_STATE_CABLE_FAILED] = "cable failed",
	[MS_STATE_TIMEOUT] = "timeout",
	[MS_STATE_FAILED] = "failed",
};

/**
 * struct mode_selection_state - State tracking for a specific Type-C mode
 * @mode: The type of mode this instance represents
 * @name: Name string pointer
 * @priority: The mode priority. Higher values indicate a more preferred mode.
 * @list: List head to link this mode state into a prioritized list.
 * @partner_supported: Flag indicating if this mode is supported by the partner
 * @cable_supported: Flag indicating if this mode is supported by the cable
 * @enter: Flag indicating if the driver is currently attempting to enter or
 * exit the mode
 * @attempt_count: Number of times the driver has attempted to enter the mode
 * @state: The current mode selection state
 * @error: The outcome of the last attempt to enter the mode
 */
struct mode_selection_state {
	enum typec_mode_type mode;
	const char *name;
	int priority;
	struct list_head list;

	bool partner_supported;
	bool cable_supported;
	bool enter;
	int attempt_count;
	enum ms_state state;
	int error;
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

/* -------------------------------------------------------------------------- */
/* partner 'mod_selection' attribute */

/**
 * mode_selection_next() - Process mode selection results and schedule next
 * action
 * @partner: pointer to the partner structure
 * @ms: pointer to active mode_selection_state object that is on top in
 * mode_sequence FIFO.
 *
 * The mutex protecting the mode_sequence FIFO must be held by the caller
 * when invoking this function.
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

	__must_hold(&partner->mode_sequence_lock)
{
	if (!ms->enter) {
		kfifo_skip(&partner->mode_sequence);
	} else if (ms->state == MS_STATE_INPROGRESS && !ms->error) {
		ms->state = MS_STATE_ACTIVE;
		partner->active_mode = ms;
		kfifo_reset(&partner->mode_sequence);
	} else {
		if (ms->error) {
			ms->state = MS_STATE_FAILED;
			dev_dbg(&partner->dev, "%s: entry mode error %pe\n",
				ms->name, ERR_PTR(ms->error));
		}
		if (ms->error != -EBUSY || ms->attempt_count >= mode_entry_attempts)
			ms->enter = false;
	}

	if (!kfifo_is_empty(&partner->mode_sequence))
		schedule_delayed_work(&partner->mode_selection_work,
			msecs_to_jiffies(mode_selection_delay));
}

static void mode_selection_complete(struct typec_partner *partner,
			const enum typec_mode_type mode, const int error)
{
	struct mode_selection_state *ms;

	mutex_lock(&partner->mode_sequence_lock);
	if (kfifo_peek(&partner->mode_sequence, &ms)) {
		if (ms->mode == mode && ms->state == MS_STATE_INPROGRESS) {
			ms->error = error;
			cancel_delayed_work(&partner->mode_selection_work);
			mode_selection_next(partner, ms);
		}
	}
	mutex_unlock(&partner->mode_sequence_lock);
}

void typec_mode_selection_altmode_complete(struct typec_altmode *alt,
				const int error)
{
	mode_selection_complete(to_typec_partner(alt->dev.parent),
		typec_svid_to_altmode(alt->svid), error);
}
EXPORT_SYMBOL_GPL(typec_mode_selection_altmode_complete);

void typec_mode_selection_usb4_complete(struct typec_partner *partner,
				const int error)
{
	mode_selection_complete(partner, TYPEC_USB4_MODE, error);
}
EXPORT_SYMBOL_GPL(typec_mode_selection_usb4_complete);

static void mode_selection_activate_usb4_mode(struct typec_partner *partner,
	struct mode_selection_state *ms)
{
	struct typec_port *port = to_typec_port(partner->dev.parent);
	int error = -EOPNOTSUPP;

	if (port->ops && port->ops->enter_usb_mode) {
		if (ms->enter && port->usb_mode != USB_MODE_USB4)
			error = -EPERM;
		else
			error = port->ops->enter_usb_mode(port,
				ms->enter ? USB_MODE_USB4 : USB_MODE_USB3);
	}

	if (ms->enter)
		ms->error = error;
}

static int mode_selection_activate_altmode(struct device *dev, void *data)
{
	struct typec_altmode *alt = to_typec_altmode(dev);
	struct mode_selection_state *ms = (struct mode_selection_state *)data;
	int error = -ENODEV;
	int ret = 0;

	if (!strcmp(dev->type->name, ALTERNATE_MODE_DEVICE_TYPE_NAME)) {
		if (ms->mode == typec_svid_to_altmode(alt->svid)) {
			if (alt->ops && alt->ops->activate)
				error = alt->ops->activate(alt, ms->enter);
			else
				error = -EOPNOTSUPP;
			ret = 1;
		}
	}

	if (ms->enter)
		ms->error = error;

	return ret;
}

static void mode_selection_activate_mode(struct typec_partner *partner,
	struct mode_selection_state *ms)
{
	if (ms->enter)
		ms->attempt_count++;

	if (ms->mode == TYPEC_USB4_MODE)
		mode_selection_activate_usb4_mode(partner, ms);
	else
		device_for_each_child(&partner->dev, ms,
			mode_selection_activate_altmode);
}

/**
 * mode_selection_work() - Activate entry into the upcoming mode
 * @work: work structure
 *
 * This function works in conjunction with `mode_selection_next()`.
 * It attempts to activate the next mode in the selection sequence.
 *
 * If the mode activation (`mode_selection_activate_mode()`) fails,
 * `mode_selection_next()` will be called to initiate a new selection cycle.
 *
 * Otherwise, the state is set to MS_STATE_INPROGRESS, and
 * `mode_selection_work()` is scheduled for a subsequent entry after a timeout
 * period. The alternate mode driver is expected to call back with the actual
 * mode entry result. Upon this callback, `mode_selection_next()` will determine
 * the subsequent mode and re-schedule `mode_selection_work()`.
 */
static void mode_selection_work(struct work_struct *work)
{
	struct typec_partner *partner = container_of(work, struct typec_partner,
						  mode_selection_work.work);
	struct mode_selection_state *ms;

	mutex_lock(&partner->mode_sequence_lock);
	if (kfifo_peek(&partner->mode_sequence, &ms)) {
		if (ms->state == MS_STATE_INPROGRESS) {
			ms->state = MS_STATE_TIMEOUT;
			mode_selection_next(partner, ms);
		} else {
			mode_selection_activate_mode(partner, ms);

			if (ms->enter && !ms->error) {
				ms->state = MS_STATE_INPROGRESS;
				schedule_delayed_work(&partner->mode_selection_work,
					msecs_to_jiffies(mode_selection_timeout));
			} else
				mode_selection_next(partner, ms);
		}
	}
	mutex_unlock(&partner->mode_sequence_lock);
}

static void mode_selection_clear_results(struct typec_partner *partner)
{
	struct typec_port *port = to_typec_port(partner->dev.parent);
	struct mode_selection_state *ms;

	list_for_each_entry(ms, &port->mode_list, list) {
		ms->enter = true;
		ms->state = MS_STATE_IDLE;
		ms->error = 0;
		ms->attempt_count = 0;
	}

	kfifo_reset(&partner->mode_sequence);
	partner->active_mode = NULL;
}

void typec_mode_selection_add_partner(struct typec_partner *partner)
{
	struct typec_port *port = to_typec_port(partner->dev.parent);
	struct mode_selection_state *ms;

	list_for_each_entry(ms, &port->mode_list, list) {
		ms->partner_supported = false;
		ms->cable_supported = false;
	}

	INIT_KFIFO(partner->mode_sequence);
	mutex_init(&partner->mode_sequence_lock);
	mode_selection_clear_results(partner);
	INIT_DELAYED_WORK(&partner->mode_selection_work, mode_selection_work);
}

void typec_mode_selection_add_mode(struct typec_partner *partner,
		const enum typec_mode_type mode)
{
	struct typec_port *port = to_typec_port(partner->dev.parent);
	struct mode_selection_state *ms;

	list_for_each_entry(ms, &port->mode_list, list) {
		if (ms->mode == mode) {
			ms->partner_supported = true;
			break;
		}
	}
}

void typec_mode_selection_add_cable(struct typec_partner *partner,
		struct typec_cable *cable)
{
	struct typec_port *port = to_typec_port(partner->dev.parent);
	struct mode_selection_state *ms;
	const u32 id_header = cable->identity->id_header;
	const u32 vdo1 = cable->identity->vdo[0];
	const u32 type = PD_IDH_PTYPE(id_header);
	const u32 speed = VDO_TYPEC_CABLE_SPEED(vdo1);
	/*
	 * Some USB devices supporting DisplayPort lack valid cable VDO.
	 * Allow only DP mode in this case.
	 */
	bool capability[TYPEC_MODE_MAX] = {
		[TYPEC_DP_ALTMODE] = true,
		[TYPEC_TBT_ALTMODE] = false,
		[TYPEC_USB4_MODE] = false,
	};

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

	list_for_each_entry(ms, &port->mode_list, list)
		ms->cable_supported = capability[ms->mode];
}

void typec_mode_selection_remove_partner(struct typec_partner *partner)
{
	mutex_lock(&partner->mode_sequence_lock);
	kfifo_reset(&partner->mode_sequence);
	mutex_unlock(&partner->mode_sequence_lock);

	cancel_delayed_work_sync(&partner->mode_selection_work);
	mutex_destroy(&partner->mode_sequence_lock);
}

/**
 * typec_mode_selection_start() - Starts the mode selection process.
 * @partner: pointer to the partner structure
 *
 * This function populates a 'mode_sequence' FIFO with pointers to
 * `struct mode_selection_state` instances. The sequence is generated based on
 * partner/cable capabilities and prioritized according to the port's settings.
 */
int typec_mode_selection_start(struct typec_partner *partner)
{
	struct typec_port *port = to_typec_port(partner->dev.parent);
	struct mode_selection_state *ms;
	int ret = 0;

	mutex_lock(&partner->mode_sequence_lock);

	if (!kfifo_is_empty(&partner->mode_sequence))
		ret = -EINPROGRESS;
	else if (partner->active_mode)
		ret = -EALREADY;
	else {
		mode_selection_clear_results(partner);

		list_for_each_entry(ms, &port->mode_list, list) {
			if (ms->partner_supported) {
				if (ms->cable_supported)
					kfifo_put(&partner->mode_sequence, ms);
				else
					ms->state = MS_STATE_CABLE_FAILED;
			}
		}

		if (kfifo_peek(&partner->mode_sequence, &ms))
			schedule_delayed_work(&partner->mode_selection_work, 0);
	}

	mutex_unlock(&partner->mode_sequence_lock);

	return ret;
}

/**
 * typec_mode_selection_reset() - Reset the mode selection process.
 * @partner: pointer to the partner structure
 *
 * This function cancels ongoing mode selection and exits the currently active
 * mode, if present.
 * It returns -EINPROGRESS when a mode exit is already scheduled, or a mode
 * entry is ongoing, indicating that the reset cannot immediately complete.
 */
int typec_mode_selection_reset(struct typec_partner *partner)
{
	struct mode_selection_state *ms;

	mutex_lock(&partner->mode_sequence_lock);
	if (kfifo_peek(&partner->mode_sequence, &ms)) {
		kfifo_reset(&partner->mode_sequence);

		if (!ms->enter || ms->state != MS_STATE_IDLE) {
			ms->attempt_count = mode_entry_attempts;
			kfifo_put(&partner->mode_sequence, ms);
			mutex_unlock(&partner->mode_sequence_lock);

			return -EINPROGRESS;
		}
	}

	if (partner->active_mode) {
		partner->active_mode->enter = false;
		mode_selection_activate_mode(partner, partner->active_mode);
	}
	mode_selection_clear_results(partner);
	mutex_unlock(&partner->mode_sequence_lock);

	return 0;
}

int typec_mode_selection_get_active(struct typec_partner *partner, char *buf)
{
	struct typec_port *port = to_typec_port(partner->dev.parent);
	struct mode_selection_state *ms, *running_ms;
	ssize_t count = 0;

	mutex_lock(&partner->mode_sequence_lock);
	if (!kfifo_peek(&partner->mode_sequence, &running_ms))
		running_ms = NULL;

	list_for_each_entry(ms, &port->mode_list, list) {
		if (ms->partner_supported) {
			if (ms->state == MS_STATE_ACTIVE)
				count += sysfs_emit_at(buf, count, "[%s] ", ms->name);
			else if (ms == running_ms)
				count += sysfs_emit_at(buf, count, "(%s) ", ms->name);
			else
				count += sysfs_emit_at(buf, count, "%s ", ms->name);
		}
	}
	mutex_unlock(&partner->mode_sequence_lock);

	if (count)
		count += sysfs_emit_at(buf, count, "\n");

	return count;
}

int typec_mode_selection_get_result(struct typec_partner *partner,
		const enum typec_mode_type mode, char *buf)
{
	struct typec_port *port = to_typec_port(partner->dev.parent);
	struct mode_selection_state *ms;

	list_for_each_entry(ms, &port->mode_list, list)
		if (ms->mode == mode)
			return sysfs_emit(buf, "%s\n", ms_state_strings[ms->state]);

	return -EOPNOTSUPP;
}
