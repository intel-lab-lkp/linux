// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */

#include <linux/bits.h>
#include <linux/netdevice.h>
#include <linux/sprintf.h>
#include <linux/stdarg.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <net/pon.h>

#include "pon.h"

#define PON_S(x)	BIT(PON_PLOAM_STATE_##x)

/* The activation edges the ITU-T state machine permits, indexed by the state
 * being left.
 *
 * One table serves every mode. G.984.3 ranges in O3 where G.9807.1 merges O2
 * and O3 into one state and a MAC has one register code for the merged pair,
 * so a driver reports O2 and then O4. Both edges are legal here because what
 * is being checked is whether a driver reported something impossible, not
 * whether it conforms to one mode's text. A per mode split is worth adding
 * the day a second MAC needs it.
 *
 * O1 is reachable from every state: a loss of signal, a deactivation or the
 * end of TO2 returns the ONU to it. O7 is reachable from O1 to O5, where the
 * OLT's Disable_Serial_Number stops the ONU, but not from O6, which leaves
 * only for O5 or O1. O7 itself leaves only for O1, when the OLT enables the
 * ONU again (G.9807.1 Table C.12.4).
 */
static const u32 pon_state_legal[] = {
	[PON_PLOAM_STATE_UNKNOWN] = PON_S(O1) | PON_S(O2) | PON_S(O3) |
				    PON_S(O4) | PON_S(O5) | PON_S(O6) |
				    PON_S(O7),
	[PON_PLOAM_STATE_O1]	  = PON_S(O2) | PON_S(O7),
	[PON_PLOAM_STATE_O2]	  = PON_S(O1) | PON_S(O3) | PON_S(O4) |
				    PON_S(O7),
	[PON_PLOAM_STATE_O3]	  = PON_S(O1) | PON_S(O4) | PON_S(O7),
	[PON_PLOAM_STATE_O4]	  = PON_S(O1) | PON_S(O2) | PON_S(O5) |
				    PON_S(O7),
	[PON_PLOAM_STATE_O5]	  = PON_S(O1) | PON_S(O6) | PON_S(O7),
	[PON_PLOAM_STATE_O6]	  = PON_S(O1) | PON_S(O5),
	[PON_PLOAM_STATE_O7]	  = PON_S(O1),
};

static const char *const pon_state_names[] = {
	[PON_PLOAM_STATE_UNKNOWN] = "unknown",
	[PON_PLOAM_STATE_O1]	  = "O1",
	[PON_PLOAM_STATE_O2]	  = "O2",
	[PON_PLOAM_STATE_O3]	  = "O3",
	[PON_PLOAM_STATE_O4]	  = "O4",
	[PON_PLOAM_STATE_O5]	  = "O5",
	[PON_PLOAM_STATE_O6]	  = "O6",
	[PON_PLOAM_STATE_O7]	  = "O7",
};

/**
 * pon_state_edge_legal() - test an activation edge against the state machine
 * @from:	the state being left, enum pon_ploam_state
 * @to:		the state being entered, enum pon_ploam_state
 *
 * The edges follow the ONU activation cycle state transition table, ITU-T
 * G.9807.1 clause C.12.1.4.3, Table C.12.4, over the states of Table C.12.1.
 *
 * Return: true when pon_state_legal permits the edge, false otherwise or
 * when @from is out of range.
 */
static bool pon_state_edge_legal(u32 from, u32 to)
{
	if (from >= ARRAY_SIZE(pon_state_legal))
		return false;

	return !!(pon_state_legal[from] & BIT(to));
}

/**
 * pon_dev_state_name() - the name of an activation state in the device's mode
 * @pdev:	PON device structure
 * @state:	the state, enum pon_ploam_state, in range
 *
 * ITU-T G.9807.1 Table C.12.1 has one Serial Number state, O2-3, which a
 * driver reports as O2. Only a G-PON device has an O2 of its own.
 *
 * Context: Called with @pdev->lock held.
 * Return: "O2-3" for O2 in a mode other than G-PON, otherwise the name in
 * pon_state_names.
 */
static const char *pon_dev_state_name(const struct pon_dev *pdev, u32 state)
{
	if (state == PON_PLOAM_STATE_O2 && pdev->mode != PON_MODE_GPON)
		return "O2-3";

	return pon_state_names[state];
}

/**
 * pon_netdev_carrier_set() - set the carrier of one network device
 * @dev:	the network device
 * @up:		true for carrier on, false for carrier off
 */
static void pon_netdev_carrier_set(struct net_device *dev, bool up)
{
	if (up)
		netif_carrier_on(dev);
	else
		netif_carrier_off(dev);
}

/**
 * pon_dev_gem_bound() - test whether a GEM port can carry traffic
 * @pdev:	PON device structure
 *
 * Without the driver's tcont_channel callback, any GEM port is enough.
 *
 * Context: Called with @pdev->lock held.
 * Return: true when a GEM port rides a T-CONT that has a transmit channel,
 * false otherwise.
 */
static bool pon_dev_gem_bound(struct pon_dev *pdev)
{
	struct pon_gem *gem;

	if (list_empty(&pdev->gems))
		return false;

	if (!pdev->ops->tcont_channel)
		return true;

	list_for_each_entry(gem, &pdev->gems, list)
		if (pon_gem_channel(pdev, gem) >= 0)
			return true;

	return false;
}

/**
 * pon_dev_carrier_update() - set the carrier of the PON netdevs from the state
 * @pdev:	PON device structure
 *
 * The carrier is up in O5 or O6 while a conduit that is up is paired with
 * the instance and at least one GEM port rides a T-CONT that has a transmit
 * channel, as the driver's tcont_channel callback answers. That includes
 * the default alloc-id, which carries data too and which the driver binds
 * without an alloc-id event. Without the callback a GEM port is enough.
 * The GEM network devices follow the data netdev.
 *
 * Called on every change that can alter the answer: an activation edge, an
 * alloc-id event, a T-CONT or a GEM port that is set or deleted and a
 * conduit that pairs, goes up, goes down or leaves.
 */
void pon_dev_carrier_update(struct pon_dev *pdev)
{
	struct pon_gem *gem;
	bool up;

	lockdep_assert_held(&pdev->lock);

	up = (pdev->ploam == PON_PLOAM_STATE_O5 ||
	      pdev->ploam == PON_PLOAM_STATE_O6) && pon_dev_gem_bound(pdev) &&
	     pon_conduit_running(pdev);

	pon_netdev_carrier_set(pdev->main_netdev, up);
	list_for_each_entry(gem, &pdev->gems, list)
		if (gem->gem_netdev)
			pon_netdev_carrier_set(gem->gem_netdev, up);
}

/**
 * pon_dev_log_pop() - take the oldest recorded line out of the ring
 * @pdev:	PON device structure
 * @line:	filled with the line, PON_LOG_LINE_LEN bytes
 * @level:	set to the printk level of the line
 * @dropped:	set to the lines a full ring dropped before this one
 *
 * Context: Process context. Takes @pdev->lock.
 * Return: true when @line holds a line, false when the ring is empty.
 */
static bool pon_dev_log_pop(struct pon_dev *pdev, char *line,
			    const char **level, unsigned int *dropped)
{
	bool found;

	mutex_lock(&pdev->lock);
	*dropped = pdev->log_dropped;
	pdev->log_dropped = 0;
	found = pdev->log_count;
	if (found) {
		strscpy(line, pdev->log_lines[pdev->log_head],
			PON_LOG_LINE_LEN);
		*level = pdev->log_levels[pdev->log_head];
		pdev->log_head = (pdev->log_head + 1) % PON_LOG_LINES;
		pdev->log_count--;
	}
	mutex_unlock(&pdev->lock);

	return found;
}

/**
 * pon_dev_log_work() - print the lines pon_dev_log() recorded
 * @work:	the instance's @log_work
 *
 * Prints the lines in the order they were recorded, each at its own level
 * with the name of the data network device. Where a full ring dropped lines,
 * one line at info level with their count comes first, because they were
 * older. The instance lock is held only to take a line out, never across a
 * print.
 *
 * Context: Process context, on system_dfl_wq. Takes @pdev->lock.
 */
static void pon_dev_log_work(struct work_struct *work)
{
	struct pon_dev *pdev = container_of(work, struct pon_dev, log_work);
	char line[PON_LOG_LINE_LEN];
	unsigned int dropped;
	const char *level;
	bool found;

	do {
		found = pon_dev_log_pop(pdev, line, &level, &dropped);
		if (dropped)
			netdev_info(pdev->main_netdev,
				    "%u log lines dropped\n", dropped);
		if (found)
			netdev_printk(level, pdev->main_netdev, "%s\n", line);
	} while (found);
}

/**
 * pon_log_init() - prepare the log of a new PON device
 * @pdev:	PON device structure
 *
 * Context: From pon_dev_create(), before the device is published.
 */
void pon_log_init(struct pon_dev *pdev)
{
	INIT_WORK(&pdev->log_work, pon_dev_log_work);
}

/**
 * pon_dev_log_record() - record one line for the log work
 * @pdev:	PON device structure
 * @level:	printk level of the line, such as KERN_INFO
 * @fmt:	printf format of the line, without a newline
 * @args:	the arguments of @fmt
 *
 * Context: Called with @pdev->lock held.
 */
static __printf(3, 0) void pon_dev_log_record(struct pon_dev *pdev,
					      const char *level,
					      const char *fmt, va_list args)
{
	unsigned int slot;

	lockdep_assert_held(&pdev->lock);

	if (pdev->going_away)
		return;

	if (pdev->log_count == PON_LOG_LINES) {
		pdev->log_head = (pdev->log_head + 1) % PON_LOG_LINES;
		pdev->log_count--;
		pdev->log_dropped++;
	}

	slot = (pdev->log_head + pdev->log_count) % PON_LOG_LINES;
	vsnprintf(pdev->log_lines[slot], PON_LOG_LINE_LEN, fmt, args);
	pdev->log_levels[slot] = level;
	pdev->log_count++;

	queue_work(system_dfl_wq, &pdev->log_work);
}

/**
 * pon_dev_log_level() - log one line at a given level from the core
 * @pdev:	PON device structure
 * @level:	printk level of the line, such as KERN_ERR
 * @fmt:	printf format of the line, without a newline
 *
 * As pon_dev_log(), with the level of the line chosen by the caller.
 *
 * Context: Called with @pdev->lock held.
 */
void pon_dev_log_level(struct pon_dev *pdev, const char *level,
		       const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	pon_dev_log_record(pdev, level, fmt, args);
	va_end(args);
}

/**
 * pon_dev_log() - log one line from the instance's context
 * @pdev:	PON device structure
 * @fmt:	printf format of the line, without a newline
 *
 * Records the line and leaves the print to a work item on system_dfl_wq. A
 * print to a serial console blocks for milliseconds, while the instance's
 * context answers the OLT within a deadline. The caller never waits for it.
 * The lines are printed at info level with the name of the data network
 * device, in the order they were recorded, the activation edges of
 * pon_dev_state_report() among them. The ring holds PON_LOG_LINES lines.
 * When it is full the oldest line is dropped and counted. The count is
 * printed in its place. A line is cut to PON_LOG_LINE_LEN - 1 characters.
 * Nothing is recorded once pon_dev_unregister() has begun.
 *
 * Context: Called with @pdev->lock held.
 */
void pon_dev_log(struct pon_dev *pdev, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	pon_dev_log_record(pdev, KERN_INFO, fmt, args);
	va_end(args);
}
EXPORT_SYMBOL_GPL(pon_dev_log);

/**
 * pon_dev_state_report() - report the activation state the MAC reached
 * @pdev:	PON device structure
 * @state:	the new state, enum pon_ploam_state
 *
 * The driver computes the state. The core owns, validates and publishes it.
 * Call with @pdev->lock held, from the instance's work or from a pon_dev_ops
 * handler.
 *
 * A repeat of the state already published is a no-op: this is a level, not an
 * edge. An edge the standard does not permit is published anyway, because the
 * hardware is the truth and a core that refused it would publish a state the
 * ONU is not in. It is warned about instead. Every edge that is published
 * gets one line in the kernel log at info level through pon_dev_log().
 *
 * Return: 0, -ENODEV once pon_dev_unregister() has begun, or -EINVAL for a
 * value out of range or an illegal edge. An illegal edge is published anyway
 * and the return value is for the driver author and for a selftest. A value
 * out of range is not a state and is refused before anything is published.
 */
int pon_dev_state_report(struct pon_dev *pdev, enum pon_ploam_state state)
{
	u32 old = pdev->ploam;
	int err = 0;

	lockdep_assert_held(&pdev->lock);

	if (pdev->going_away)
		return -ENODEV;

	if (state > PON_PLOAM_STATE_O7) {
		netdev_warn(pdev->main_netdev,
			    "activation state %u is not a state\n", state);
		return -EINVAL;
	}

	if (state == old)
		return 0;

	if (!pon_state_edge_legal(old, state)) {
		net_warn_ratelimited("%s: activation went O%u to O%u, which the standard does not permit\n",
				     netdev_name(pdev->main_netdev), old,
				     state);
		err = -EINVAL;
	}

	WRITE_ONCE(pdev->ploam, state);
	pon_dev_log(pdev, "PLOAM state %s -> %s",
		    pon_dev_state_name(pdev, old),
		    pon_dev_state_name(pdev, state));

	/* Carrier before the notification, so a daemon that reads both sees a
	 * netdev that agrees with the state. rtnetlink and generic netlink
	 * have no ordering between them, so this is best effort.
	 */
	pon_dev_carrier_update(pdev);
	pon_nl_notify_ploam(pdev);

	return err;
}
EXPORT_SYMBOL_GPL(pon_dev_state_report);

/**
 * pon_dev_event() - report a discrete event
 * @pdev:	PON device structure
 * @ev:		what happened and the alloc-id it names
 *
 * Runs in the instance's context, with its lock held.
 *
 * A PON_EVENT_TYPE_TCONT_ALLOC event means the driver bound the alloc-id to
 * a channel, so the GEM ports that ride it carry traffic from then on: the
 * carrier may rise. A PON_EVENT_TYPE_TCONT_DEALLOC event means the driver
 * released the channel: the carrier may fall. An event that arrives once
 * pon_dev_unregister() has begun is dropped.
 */
void pon_dev_event(struct pon_dev *pdev, const struct pon_event *ev)
{
	lockdep_assert_held(&pdev->lock);

	if (pdev->going_away)
		return;

	if (ev->type == PON_EVENT_TYPE_TCONT_ALLOC ||
	    ev->type == PON_EVENT_TYPE_TCONT_DEALLOC)
		pon_dev_carrier_update(pdev);
}
EXPORT_SYMBOL_GPL(pon_dev_event);
