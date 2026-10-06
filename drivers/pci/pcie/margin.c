// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe Lane Margining at the Receiver
 *
 * Copyright (C) 2026 Google LLC
 * Author: Priyank Rathod <rathodpriyank@google.com>
 *
 * Lane Margining at the Receiver moves the sampling point of a receiver
 * away from its normal setting, in time or in voltage, while the link
 * stays up, and reports the errors the receiver sees.  It is available
 * on links running at 16.0 GT/s or faster.
 *
 * This file exposes the margining commands of the Lane Margining at the
 * Receiver Extended Capability in debugfs, one directory per port that
 * has the capability.  It does not run sweeps or interpret results;
 * that is left to user space.
 *
 * Commands always go through the capability of the port that owns the
 * debugfs directory.  Receiver 1 (the Downstream Port) and the retimer
 * receivers 2-5 are reached through the Downstream Port, and receiver 6
 * (the Upstream Port) through Function 0 of the device below it.  A
 * session selects one receiver, and only one session can be active on a
 * link at a time.
 *
 * Locking, outermost first:
 *
 *   device_lock() of both ends of the link, with pci_dev_trylock(),
 *   only while a session is started or stopped
 *     port->lock, one port at a time
 *       pci_lmr_mutex (leaf lock)
 *       pci_bus_sem (read) -> aspm_lock -> pcie_cap_lock in the ASPM API
 *
 * pci_lmr_find_partner() and runtime PM calls are made outside all of
 * the locks above.  port->state is changed only with both port->lock
 * and pci_lmr_mutex held, so holding either one is enough to read it.
 */

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/iopoll.h>
#include <linux/kstrtox.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/reboot.h>
#include <linux/rwsem.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/suspend.h>

#include "../pci.h"

/* Margin Type values in Margining Lane Control and Status */
#define LMR_TYPE_REPORT		1	/* Access Retimer/Receiver parameters */
#define LMR_TYPE_SET		2	/* Set margining parameters */
#define LMR_TYPE_TIMING		3	/* Step Margin Timing */
#define LMR_TYPE_VOLTAGE	4	/* Step Margin Voltage */
#define LMR_TYPE_NO_CMD		7	/* No Command */

/* Margin Payload values for LMR_TYPE_REPORT */
#define LMR_REPORT_CAPS		0x88	/* Margin Control Capabilities */
#define LMR_REPORT_VOLT_STEPS	0x89	/* MNumVoltageSteps */
#define LMR_REPORT_TIM_STEPS	0x8a	/* MNumTimingSteps */
#define LMR_REPORT_TIM_OFFSET	0x8b	/* MMaxTimingOffset */
#define LMR_REPORT_VOLT_OFFSET	0x8c	/* MMaxVoltageOffset */
#define LMR_REPORT_MAX_LANES	0x90	/* MMaxLanes */

/* Margin Payload values for LMR_TYPE_SET and LMR_TYPE_NO_CMD */
#define LMR_SET_NORMAL		0x0f	/* Go to Normal Settings */
#define LMR_NO_CMD		0x9c	/* No Command */

/* Report Margin Control Capabilities response */
#define LMR_CAP_VOLTAGE		BIT(0)	/* MVoltageSupported */
#define LMR_CAP_IND_UP_DOWN	BIT(1)	/* MIndUpDownVoltage */
#define LMR_CAP_IND_LEFT_RIGHT	BIT(2)	/* MIndLeftRightTiming */
#define LMR_CAP_SAMPLE_RATE	BIT(3)	/* MSampleReportingMethod */
#define LMR_CAP_IND_ERR_SAMPLER	BIT(4)	/* MIndErrorSampler */

/* Fields of the other Report responses */
#define LMR_RPT_TIM_STEPS	GENMASK(5, 0)
#define LMR_RPT_VOLT_STEPS	GENMASK(6, 0)
#define LMR_RPT_OFFSET		GENMASK(6, 0)
#define LMR_RPT_MAX_LANES	GENMASK(4, 0)

/* Step Margin command payload */
#define LMR_STEP_TIM		GENMASK(5, 0)
#define LMR_STEP_TIM_LEFT	BIT(6)
#define LMR_STEP_VOLT		GENMASK(6, 0)
#define LMR_STEP_VOLT_DOWN	BIT(7)

/* Step Margin response payload */
#define LMR_STEP_ERR_COUNT	GENMASK(5, 0)
#define LMR_STEP_STATUS		GENMASK(7, 6)
#define  LMR_STEP_TOO_MANY_ERR	0
#define  LMR_STEP_SETUP		1
#define  LMR_STEP_MARGINING	2
#define  LMR_STEP_NAK		3

#define LMR_RX_DP		1	/* Downstream Port receiver */
#define LMR_RX_UP		6	/* Upstream Port receiver */

#define LMR_MAX_WIDTH		32
#define LMR_POLL_US		1000
#define LMR_TIMEOUT_US		(100 * USEC_PER_MSEC)

/**
 * struct pci_lmr_rx - Parameters reported by the selected receiver
 * @valid: The fields below were read after the receiver was selected
 * @caps: Report Margin Control Capabilities response
 * @timing_steps: MNumTimingSteps
 * @max_timing_offset: MMaxTimingOffset
 * @voltage_steps: MNumVoltageSteps
 * @max_voltage_offset: MMaxVoltageOffset
 * @max_lanes: Number of lanes that can be margined at once (MMaxLanes + 1)
 */
struct pci_lmr_rx {
	bool valid;
	u8 caps;
	u8 timing_steps;
	u8 max_timing_offset;
	u8 voltage_steps;
	u8 max_voltage_offset;
	u8 max_lanes;
};

/**
 * struct pci_lmr_lane - One lane of a margining port
 * @port: Port the lane belongs to
 * @lane: Lane number
 * @touched: A step command was issued since the receiver was selected
 * @timing: Current timing offset in steps, negative to the left
 * @voltage: Current voltage offset in steps, negative down
 * @rx: Receiver parameters for this lane
 */
struct pci_lmr_lane {
	struct pci_lmr_port *port;
	unsigned int lane;
	bool touched;
	int timing;
	int voltage;
	struct pci_lmr_rx rx;
};

enum pci_lmr_state {
	LMR_IDLE,
	LMR_STARTING,
	LMR_ACTIVE,
};

/**
 * struct pci_lmr_port - A port that has the Lane Margining capability
 * @node: Entry in pci_lmr_ports
 * @dev: Device that has the capability
 * @dport: Downstream Port of the link; @dev itself on the Downstream Port side
 * @cap: Offset of the capability
 * @downstream: @dev is the Downstream Port of its link
 * @dir: debugfs directory
 * @lock: Serializes margining commands and protects the session fields
 * @state: Session state; see the locking notes at the top of the file
 * @partner: Other end of the link while a session is up (holds a reference)
 * @receiver: Receiver selected for the session
 * @retimers: Number of retimers detected when the session started
 * @lnksta: Speed and width fields of the Downstream Port's Link Status
 * @aspm: ASPM states that were enabled when the session started
 * @clkpm: CLKREQ# was enabled when the session started
 * @dp_hawd: HAWD bit of the Downstream Port's Link Control at session start
 * @dp_hasd: HASD bit of the Downstream Port's Link Control 2 at session start
 * @up_hawd: HAWD bit of the Upstream Port's Link Control at session start
 * @up_hasd: HASD bit of the Upstream Port's Link Control 2 at session start
 * @pm_gen: Value of pci_lmr_pm_gen when the session started
 * @nr_lanes: Maximum Link Width of @dev
 * @lanes: Per-lane state
 */
struct pci_lmr_port {
	struct list_head node;
	struct pci_dev *dev;
	struct pci_dev *dport;
	u16 cap;
	bool downstream;
	struct dentry *dir;
	struct mutex lock;		/* See the kernel-doc above */
	enum pci_lmr_state state;
	struct pci_dev *partner;
	u8 receiver;
	u8 retimers;
	u16 lnksta;
	u32 aspm;
	bool clkpm;
	u16 dp_hawd;
	u16 dp_hasd;
	u16 up_hawd;
	u16 up_hasd;
	unsigned int pm_gen;
	unsigned int nr_lanes;
	struct pci_lmr_lane lanes[] __counted_by(nr_lanes);
};

static DEFINE_MUTEX(pci_lmr_mutex);
static LIST_HEAD(pci_lmr_ports);	/* Protected by pci_lmr_mutex */
static atomic_t pci_lmr_pm_gen = ATOMIC_INIT(0);

/* The two ends of the link; only valid while @port->partner is set */
static struct pci_dev *pci_lmr_dp(struct pci_lmr_port *port)
{
	return port->downstream ? port->dev : port->partner;
}

static struct pci_dev *pci_lmr_up(struct pci_lmr_port *port)
{
	return port->downstream ? port->partner : port->dev;
}

static u8 pci_lmr_default_rx(struct pci_lmr_port *port)
{
	return port->downstream ? LMR_RX_DP : LMR_RX_UP;
}

static u16 pci_lmr_lane_sts(struct pci_lmr_port *port, unsigned int lane)
{
	int pos = port->cap + PCI_LMR_LANE_STS + lane * PCI_LMR_LANE_STRIDE;
	u16 sts;

	if (pci_read_config_word(port->dev, pos, &sts))
		return (u16)PCI_ERROR_RESPONSE;

	return sts;
}

/*
 * Write @cmd to Margining Lane Control and wait until the bits of
 * Margining Lane Status selected by @match are equal to those of @cmd.
 */
static int pci_lmr_write_wait(struct pci_lmr_port *port, unsigned int lane,
			      u16 cmd, u16 match, u16 *sts)
{
	int pos = port->cap + PCI_LMR_LANE_CTRL + lane * PCI_LMR_LANE_STRIDE;
	u16 val;
	int ret;

	ret = pci_write_config_word(port->dev, pos, cmd);
	if (ret)
		return pcibios_err_to_errno(ret);

	ret = read_poll_timeout(pci_lmr_lane_sts, val,
				PCI_POSSIBLE_ERROR(val) ||
				(val & match) == (cmd & match),
				LMR_POLL_US, LMR_TIMEOUT_US, false, port, lane);
	if (PCI_POSSIBLE_ERROR(val))
		return -ENODEV;
	if (ret)
		return ret;

	if (sts)
		*sts = val;

	return 0;
}

static u16 pci_lmr_word(u8 rx, u8 type, u8 payload)
{
	return FIELD_PREP(PCI_LMR_LANE_RX, rx) |
	       FIELD_PREP(PCI_LMR_LANE_TYPE, type) |
	       FIELD_PREP(PCI_LMR_LANE_PAYLOAD, payload);
}

static int pci_lmr_no_cmd(struct pci_lmr_port *port, unsigned int lane)
{
	u16 cmd = pci_lmr_word(0, LMR_TYPE_NO_CMD, LMR_NO_CMD);

	return pci_lmr_write_wait(port, lane, cmd, U16_MAX, NULL);
}

/*
 * Send one command to the selected receiver on @lane and return the
 * payload of its response.  A No Command is written first, as pcilmr
 * does, so the response cannot be confused with the one to an earlier
 * command.
 */
static int pci_lmr_cmd(struct pci_lmr_port *port, unsigned int lane,
		       u8 type, u8 payload, u8 *resp)
{
	u16 cmd, match, sts;
	int ret;

	lockdep_assert_held(&port->lock);

	ret = pci_lmr_no_cmd(port, lane);
	if (ret)
		return ret;

	cmd = pci_lmr_word(port->receiver, type, payload);
	match = PCI_LMR_LANE_RX | PCI_LMR_LANE_TYPE;
	if (type == LMR_TYPE_SET)
		match |= PCI_LMR_LANE_PAYLOAD;

	ret = pci_lmr_write_wait(port, lane, cmd, match, &sts);
	if (ret)
		return ret;

	if (resp)
		*resp = FIELD_GET(PCI_LMR_LANE_PAYLOAD, sts);

	return 0;
}

/* Go to Normal Settings returns both the timing and the voltage offset to 0 */
static int pci_lmr_go_normal(struct pci_lmr_lane *l)
{
	int ret;

	ret = pci_lmr_cmd(l->port, l->lane, LMR_TYPE_SET, LMR_SET_NORMAL, NULL);
	if (ret)
		return ret;

	l->timing = 0;
	l->voltage = 0;

	return pci_lmr_no_cmd(l->port, l->lane);
}

/* Return the lanes that may still be margined to normal settings */
static int pci_lmr_reset_lanes(struct pci_lmr_port *port)
{
	unsigned int i;
	int ret, err = 0;

	for (i = 0; i < port->nr_lanes; i++) {
		struct pci_lmr_lane *l = &port->lanes[i];

		if (!l->touched)
			continue;

		ret = pci_lmr_go_normal(l);
		if (!ret) {
			l->touched = false;
			continue;
		}

		err = err ?: ret;
		if (ret == -ENODEV)
			break;
		pci_warn(port->dev, "LMR: failed to reset lane %u (%d)\n",
			 i, ret);
	}

	return err;
}

static void pci_lmr_forget_rx(struct pci_lmr_port *port)
{
	unsigned int i;

	for (i = 0; i < port->nr_lanes; i++) {
		port->lanes[i].touched = false;
		port->lanes[i].timing = 0;
		port->lanes[i].voltage = 0;
		port->lanes[i].rx.valid = false;
	}
}

static int pci_lmr_report(struct pci_lmr_lane *l, u8 payload, u8 *val)
{
	return pci_lmr_cmd(l->port, l->lane, LMR_TYPE_REPORT, payload, val);
}

static int pci_lmr_get_rx(struct pci_lmr_lane *l)
{
	struct pci_lmr_rx rx = {};
	u8 val;
	int ret;

	if (l->rx.valid)
		return 0;

	ret = pci_lmr_report(l, LMR_REPORT_CAPS, &rx.caps);
	if (ret)
		return ret;

	ret = pci_lmr_report(l, LMR_REPORT_TIM_STEPS, &val);
	if (ret)
		return ret;
	rx.timing_steps = FIELD_GET(LMR_RPT_TIM_STEPS, val);

	ret = pci_lmr_report(l, LMR_REPORT_TIM_OFFSET, &val);
	if (ret)
		return ret;
	rx.max_timing_offset = FIELD_GET(LMR_RPT_OFFSET, val);

	if (rx.caps & LMR_CAP_VOLTAGE) {
		ret = pci_lmr_report(l, LMR_REPORT_VOLT_STEPS, &val);
		if (ret)
			return ret;
		rx.voltage_steps = FIELD_GET(LMR_RPT_VOLT_STEPS, val);

		ret = pci_lmr_report(l, LMR_REPORT_VOLT_OFFSET, &val);
		if (ret)
			return ret;
		rx.max_voltage_offset = FIELD_GET(LMR_RPT_OFFSET, val);
	}

	ret = pci_lmr_report(l, LMR_REPORT_MAX_LANES, &val);
	if (ret)
		return ret;
	rx.max_lanes = FIELD_GET(LMR_RPT_MAX_LANES, val) + 1;

	ret = pci_lmr_no_cmd(l->port, l->lane);
	if (ret)
		return ret;

	rx.valid = true;
	l->rx = rx;

	return 0;
}

/*
 * Before each margining command, check that the link is still in the
 * state the session was set up in.
 */
static int pci_lmr_check(struct pci_lmr_port *port)
{
	struct pci_dev *dp, *up;
	u16 dp_ctl, up_ctl, lnksta, sts;

	lockdep_assert_held(&port->lock);

	if (port->state != LMR_ACTIVE)
		return -ENOTCONN;

	dp = pci_lmr_dp(port);
	up = pci_lmr_up(port);
	if (!pci_dev_is_added(port->partner) ||
	    pci_dev_is_disconnected(dp) || pci_dev_is_disconnected(up))
		return -ENODEV;

	/* Margining state is lost over system suspend */
	if (atomic_read(&pci_lmr_pm_gen) != port->pm_gen)
		return -EIO;

	if (pcie_capability_read_word(dp, PCI_EXP_LNKCTL, &dp_ctl) ||
	    pcie_capability_read_word(up, PCI_EXP_LNKCTL, &up_ctl) ||
	    pcie_capability_read_word(dp, PCI_EXP_LNKSTA, &lnksta) ||
	    pci_read_config_word(port->dev, port->cap + PCI_LMR_PORT_STS, &sts))
		return -ENODEV;

	if (PCI_POSSIBLE_ERROR(dp_ctl) || PCI_POSSIBLE_ERROR(up_ctl) ||
	    PCI_POSSIBLE_ERROR(lnksta) || PCI_POSSIBLE_ERROR(sts))
		return -ENODEV;

	/* ASPM was turned back on, e.g. through sysfs */
	if ((dp_ctl | up_ctl) & PCI_EXP_LNKCTL_ASPMC)
		return -EBUSY;

	if ((lnksta & (PCI_EXP_LNKSTA_CLS | PCI_EXP_LNKSTA_NLW)) !=
	    port->lnksta)
		return -EIO;

	if (!(sts & PCI_LMR_PORT_STS_MARGIN_READY))
		return -EIO;

	return 0;
}

static bool pci_lmr_margined(struct pci_lmr_lane *l)
{
	return l->timing || l->voltage;
}

/*
 * A receiver that reaches its error limit while dwelling on a step stops
 * margining and returns to normal settings on its own.  Check for that
 * before using the recorded offsets.
 */
static int pci_lmr_sync_lane(struct pci_lmr_lane *l)
{
	struct pci_lmr_port *port = l->port;
	u8 type, payload;
	u16 sts;

	if (!pci_lmr_margined(l))
		return 0;

	sts = pci_lmr_lane_sts(port, l->lane);
	if (PCI_POSSIBLE_ERROR(sts))
		return -ENODEV;

	type = FIELD_GET(PCI_LMR_LANE_TYPE, sts);
	payload = FIELD_GET(PCI_LMR_LANE_PAYLOAD, sts);
	if (FIELD_GET(PCI_LMR_LANE_RX, sts) == port->receiver &&
	    (type == LMR_TYPE_TIMING || type == LMR_TYPE_VOLTAGE) &&
	    FIELD_GET(LMR_STEP_STATUS, payload) == LMR_STEP_TOO_MANY_ERR)
		return pci_lmr_go_normal(l);

	return 0;
}

/*
 * The receiver limits how many lanes can be margined at once.  Count
 * the lanes that are margined now and use the smallest limit any of
 * them, or @l, reported.
 */
static int pci_lmr_lane_limit(struct pci_lmr_lane *l)
{
	struct pci_lmr_port *port = l->port;
	unsigned int i, count = 0, limit = l->rx.max_lanes;
	int ret;

	for (i = 0; i < port->nr_lanes; i++) {
		struct pci_lmr_lane *o = &port->lanes[i];

		if (o == l)
			continue;

		ret = pci_lmr_sync_lane(o);
		if (ret)
			return ret;

		if (!pci_lmr_margined(o))
			continue;

		count++;
		limit = min(limit, o->rx.max_lanes);
	}

	return count + 1 > limit ? -EBUSY : 0;
}

static int pci_lmr_check_steps(struct pci_lmr_lane *l, bool timing, int steps)
{
	struct pci_lmr_rx *rx = &l->rx;
	unsigned int max;
	bool ind;

	if (timing) {
		max = rx->timing_steps;
		ind = rx->caps & LMR_CAP_IND_LEFT_RIGHT;
	} else {
		if (!(rx->caps & LMR_CAP_VOLTAGE))
			return -EOPNOTSUPP;
		max = rx->voltage_steps;
		ind = rx->caps & LMR_CAP_IND_UP_DOWN;
	}

	if (steps < 0 && !ind)
		return -EINVAL;

	if (steps < -(int)max || steps > (int)max)
		return -ERANGE;

	return 0;
}

/*
 * Issue a Step Margin command with a non-zero offset.  A NAK leaves the
 * receiver as it was.  When the receiver reports too many errors, it is
 * sent to normal settings, so that software and hardware agree.
 */
static int pci_lmr_send_step(struct pci_lmr_lane *l, bool timing, int steps)
{
	unsigned int mag = abs(steps);
	u8 type, payload, resp;
	int ret;

	if (timing) {
		type = LMR_TYPE_TIMING;
		payload = FIELD_PREP(LMR_STEP_TIM, mag);
		if (steps < 0)
			payload |= LMR_STEP_TIM_LEFT;
	} else {
		type = LMR_TYPE_VOLTAGE;
		payload = FIELD_PREP(LMR_STEP_VOLT, mag);
		if (steps < 0)
			payload |= LMR_STEP_VOLT_DOWN;
	}

	l->touched = true;
	ret = pci_lmr_cmd(l->port, l->lane, type, payload, &resp);
	if (ret)
		return ret;

	switch (FIELD_GET(LMR_STEP_STATUS, resp)) {
	case LMR_STEP_NAK:
		return -EOPNOTSUPP;
	case LMR_STEP_TOO_MANY_ERR:
		ret = pci_lmr_go_normal(l);
		return ret ?: -EIO;
	default:
		break;
	}

	if (timing)
		l->timing = steps;
	else
		l->voltage = steps;

	return 0;
}

static int pci_lmr_step(struct pci_lmr_lane *l, bool timing, int steps)
{
	struct pci_lmr_port *port = l->port;
	int other, ret;

	ret = pci_lmr_check(port);
	if (ret)
		return ret;

	ret = pci_lmr_get_rx(l);
	if (ret)
		return ret;

	ret = pci_lmr_check_steps(l, timing, steps);
	if (ret)
		return ret;

	ret = pci_lmr_sync_lane(l);
	if (ret)
		return ret;

	if (steps) {
		if (!pci_lmr_margined(l)) {
			ret = pci_lmr_lane_limit(l);
			if (ret)
				return ret;
		}
		return pci_lmr_send_step(l, timing, steps);
	}

	/*
	 * A Step Margin command with a payload of 0 does not move the
	 * receiver back.  Go to Normal Settings does, but for both offsets,
	 * so the other offset has to be applied again afterwards.
	 */
	other = timing ? l->voltage : l->timing;
	ret = pci_lmr_go_normal(l);
	if (ret)
		return ret;

	if (other)
		return pci_lmr_send_step(l, !timing, other);

	return 0;
}

static void pci_lmr_aspm_restore(struct pci_lmr_port *port)
{
	int state = port->aspm;

	if (!state)
		return;

	if (port->clkpm)
		state |= PCIE_LINK_STATE_CLKPM;

	pci_force_enable_link_state(pci_lmr_up(port), state);
}

/*
 * Turn off the ASPM states that are enabled on the link, using the
 * same calls as drivers that need ASPM off for a while.  Clock PM is
 * not passed to pci_disable_link_state(), because nothing but the sysfs
 * clkpm file clears a Clock PM disable again.  pcie_aspm_enabled() does
 * not report Clock PM, so it is added back on restore if CLKREQ# was
 * enabled.
 */
static int pci_lmr_aspm_off(struct pci_lmr_port *port)
{
	struct pci_dev *dp = pci_lmr_dp(port), *up = pci_lmr_up(port);
	u16 dp_ctl, up_ctl;
	int ret;

	ret = pcie_capability_read_word(up, PCI_EXP_LNKCTL, &up_ctl);
	if (ret || PCI_POSSIBLE_ERROR(up_ctl))
		return -ENODEV;

	port->clkpm = up_ctl & PCI_EXP_LNKCTL_CLKREQ_EN;
	port->aspm = pcie_aspm_enabled(up);
	if (port->aspm) {
		ret = pci_disable_link_state(up, port->aspm);
		if (ret)
			return ret;
	}

	/* ASPM may also have been left on by firmware */
	if (pcie_capability_read_word(dp, PCI_EXP_LNKCTL, &dp_ctl) ||
	    pcie_capability_read_word(up, PCI_EXP_LNKCTL, &up_ctl) ||
	    PCI_POSSIBLE_ERROR(dp_ctl) || PCI_POSSIBLE_ERROR(up_ctl)) {
		ret = -ENODEV;
		goto restore;
	}

	if ((dp_ctl | up_ctl) & PCI_EXP_LNKCTL_ASPMC) {
		ret = -EBUSY;
		goto restore;
	}

	return 0;

restore:
	pci_lmr_aspm_restore(port);
	return ret;
}

/*
 * Hardware must not change the link speed or width on its own while a
 * receiver is margined.  Set HAWD and HASD on both ends, the Upstream
 * Port first, and restore them in the opposite order.
 */
static int pci_lmr_autonomous_off(struct pci_lmr_port *port)
{
	struct pci_dev *dp = pci_lmr_dp(port), *up = pci_lmr_up(port);
	u16 dp_ctl, dp_ctl2, up_ctl, up_ctl2;

	if (pcie_capability_read_word(dp, PCI_EXP_LNKCTL, &dp_ctl) ||
	    pcie_capability_read_word(dp, PCI_EXP_LNKCTL2, &dp_ctl2) ||
	    pcie_capability_read_word(up, PCI_EXP_LNKCTL, &up_ctl) ||
	    pcie_capability_read_word(up, PCI_EXP_LNKCTL2, &up_ctl2))
		return -ENODEV;

	if (PCI_POSSIBLE_ERROR(dp_ctl) || PCI_POSSIBLE_ERROR(dp_ctl2) ||
	    PCI_POSSIBLE_ERROR(up_ctl) || PCI_POSSIBLE_ERROR(up_ctl2))
		return -ENODEV;

	port->dp_hawd = dp_ctl & PCI_EXP_LNKCTL_HAWD;
	port->dp_hasd = dp_ctl2 & PCI_EXP_LNKCTL2_HASD;
	port->up_hawd = up_ctl & PCI_EXP_LNKCTL_HAWD;
	port->up_hasd = up_ctl2 & PCI_EXP_LNKCTL2_HASD;

	pcie_capability_set_word(up, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_HAWD);
	pcie_capability_set_word(up, PCI_EXP_LNKCTL2, PCI_EXP_LNKCTL2_HASD);
	pcie_capability_set_word(dp, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_HAWD);
	pcie_capability_set_word(dp, PCI_EXP_LNKCTL2, PCI_EXP_LNKCTL2_HASD);

	return 0;
}

static void pci_lmr_autonomous_restore(struct pci_lmr_port *port)
{
	struct pci_dev *dp = pci_lmr_dp(port), *up = pci_lmr_up(port);

	if (!pci_dev_is_disconnected(dp)) {
		pcie_capability_clear_and_set_word(dp, PCI_EXP_LNKCTL,
						   PCI_EXP_LNKCTL_HAWD,
						   port->dp_hawd);
		pcie_capability_clear_and_set_word(dp, PCI_EXP_LNKCTL2,
						   PCI_EXP_LNKCTL2_HASD,
						   port->dp_hasd);
	}

	if (!pci_dev_is_disconnected(up)) {
		pcie_capability_clear_and_set_word(up, PCI_EXP_LNKCTL,
						   PCI_EXP_LNKCTL_HAWD,
						   port->up_hawd);
		pcie_capability_clear_and_set_word(up, PCI_EXP_LNKCTL2,
						   PCI_EXP_LNKCTL2_HASD,
						   port->up_hasd);
	}
}

static u16 pci_lmr_port_sts(struct pci_lmr_port *port)
{
	u16 sts;

	if (pci_read_config_word(port->dev, port->cap + PCI_LMR_PORT_STS, &sts))
		return (u16)PCI_ERROR_RESPONSE;

	return sts;
}

static int pci_lmr_wait_ready(struct pci_lmr_port *port)
{
	u16 sts;
	int ret;

	ret = read_poll_timeout(pci_lmr_port_sts, sts,
				PCI_POSSIBLE_ERROR(sts) ||
				(sts & PCI_LMR_PORT_STS_MARGIN_READY),
				LMR_POLL_US, LMR_TIMEOUT_US, false, port);
	if (PCI_POSSIBLE_ERROR(sts))
		return -ENODEV;

	return ret;
}

/* Read the link state the session depends on, from the Downstream Port */
static int pci_lmr_read_link(struct pci_lmr_port *port)
{
	struct pci_dev *dp = pci_lmr_dp(port);
	u16 lnksta, lnksta2;

	if (pcie_capability_read_word(dp, PCI_EXP_LNKSTA, &lnksta) ||
	    pcie_capability_read_word(dp, PCI_EXP_LNKSTA2, &lnksta2) ||
	    PCI_POSSIBLE_ERROR(lnksta) || PCI_POSSIBLE_ERROR(lnksta2))
		return -ENODEV;

	if (!FIELD_GET(PCI_EXP_LNKSTA_NLW, lnksta))
		return -ENOLINK;

	/* Lane Margining at the Receiver is defined for 16.0 GT/s and up */
	if ((lnksta & PCI_EXP_LNKSTA_CLS) < PCI_EXP_LNKSTA_CLS_16_0GB)
		return -EOPNOTSUPP;

	port->lnksta = lnksta & (PCI_EXP_LNKSTA_CLS | PCI_EXP_LNKSTA_NLW);
	port->retimers = !!(lnksta2 & PCI_EXP_LNKSTA2_RETIMER) +
			 !!(lnksta2 & PCI_EXP_LNKSTA2_2RETIMERS);

	return 0;
}

/*
 * Find the device at the other end of the link and take a reference.
 * On the Downstream Port side that is Function 0 on the secondary bus.
 * The bus is looked up on the parent's list of child buses instead of
 * through ->subordinate, which is cleared only after the bus has been
 * removed from that list.
 */
static struct pci_dev *pci_lmr_find_partner(struct pci_lmr_port *port)
{
	struct pci_dev *dev = port->dev, *partner = NULL, *pdev;
	struct pci_bus *bus;

	if (!port->downstream)
		return pci_dev_get(port->dport);

	guard(rwsem_read)(&pci_bus_sem);
	list_for_each_entry(bus, &dev->bus->children, node) {
		if (bus->self != dev)
			continue;
		list_for_each_entry(pdev, &bus->devices, bus_list) {
			if (pdev->devfn == 0) {
				partner = pci_dev_get(pdev);
				break;
			}
		}
		break;
	}

	return partner;
}

/* Set up both ends of the link for margining */
static int pci_lmr_setup(struct pci_lmr_port *port)
{
	u16 cap, sts;
	int ret;

	ret = pci_lmr_read_link(port);
	if (ret)
		return ret;

	if (pci_read_config_word(port->dev, port->cap + PCI_LMR_PORT_CAP,
				 &cap) ||
	    pci_read_config_word(port->dev, port->cap + PCI_LMR_PORT_STS,
				 &sts) ||
	    PCI_POSSIBLE_ERROR(cap) || PCI_POSSIBLE_ERROR(sts))
		return -ENODEV;

	if ((cap & PCI_LMR_PORT_CAP_USES_SW_READY) &&
	    !(sts & PCI_LMR_PORT_STS_SW_READY))
		return -ETIMEDOUT;

	ret = pci_lmr_aspm_off(port);
	if (ret)
		return ret;

	ret = pci_lmr_autonomous_off(port);
	if (ret)
		goto aspm;

	ret = pci_lmr_wait_ready(port);
	if (ret)
		goto autonomous;

	return 0;

autonomous:
	pci_lmr_autonomous_restore(port);
aspm:
	pci_lmr_aspm_restore(port);
	return ret;
}

static void pci_lmr_set_state(struct pci_lmr_port *port,
			      enum pci_lmr_state state)
{
	lockdep_assert_held(&port->lock);
	lockdep_assert_held(&pci_lmr_mutex);

	port->state = state;
}

/* Claim the link for @port; another session on it gives -EBUSY */
static int pci_lmr_claim(struct pci_lmr_port *port, struct pci_dev *partner)
{
	struct pci_lmr_port *p;

	guard(mutex)(&port->lock);
	guard(mutex)(&pci_lmr_mutex);

	if (port->state != LMR_IDLE)
		return -EBUSY;

	list_for_each_entry(p, &pci_lmr_ports, node)
		if (p->dport == port->dport && p->state != LMR_IDLE)
			return -EBUSY;

	/*
	 * Removal clears the added flag before it takes the device lock,
	 * and runs pci_lmr_exit() after that.  Both device locks are held
	 * here, so neither end can reach pci_lmr_exit() until the session
	 * is either running or abandoned.
	 */
	if (!pci_dev_is_added(port->dev) || !pci_dev_is_added(partner))
		return -ENODEV;

	port->partner = partner;
	pci_lmr_set_state(port, LMR_STARTING);

	return 0;
}

static void pci_lmr_unclaim(struct pci_lmr_port *port)
{
	guard(mutex)(&port->lock);
	guard(mutex)(&pci_lmr_mutex);

	port->partner = NULL;
	pci_lmr_set_state(port, LMR_IDLE);
}

static int pci_lmr_activate(struct pci_lmr_port *port)
{
	int ret;

	guard(mutex)(&port->lock);

	port->receiver = pci_lmr_default_rx(port);
	port->pm_gen = atomic_read(&pci_lmr_pm_gen);
	pci_lmr_forget_rx(port);

	ret = pci_lmr_setup(port);
	if (ret)
		return ret;

	guard(mutex)(&pci_lmr_mutex);
	pci_lmr_set_state(port, LMR_ACTIVE);

	return 0;
}

static int pci_lmr_start(struct pci_lmr_port *port)
{
	struct pci_dev *partner, *dp, *up;
	int ret;

	partner = pci_lmr_find_partner(port);
	if (!partner)
		return -ENODEV;

	dp = port->downstream ? port->dev : partner;
	up = port->downstream ? partner : port->dev;

	ret = pm_runtime_resume_and_get(&dp->dev);
	if (ret)
		goto put_partner;
	ret = pm_runtime_resume_and_get(&up->dev);
	if (ret)
		goto put_dp;

	/*
	 * Probe, remove and reset hold the device lock and may wait for a
	 * lock taken further down this path, so never wait for it here.
	 */
	ret = -EBUSY;
	if (!pci_dev_trylock(dp))
		goto put_up;
	if (!pci_dev_trylock(up))
		goto unlock_dp;

	ret = pci_lmr_claim(port, partner);
	if (ret)
		goto unlock;

	ret = pci_lmr_activate(port);
	if (ret)
		goto unclaim;

	pci_dev_unlock(up);
	pci_dev_unlock(dp);

	/* The session keeps the partner reference and runtime PM usage */
	return 0;

unclaim:
	pci_lmr_unclaim(port);
unlock:
	pci_dev_unlock(up);
unlock_dp:
	pci_dev_unlock(dp);
put_up:
	pm_runtime_put_sync(&up->dev);
put_dp:
	pm_runtime_put_sync(&dp->dev);
put_partner:
	pci_dev_put(partner);
	return ret;
}

/*
 * End the session on @port and return its partner, whose reference and
 * runtime PM usage the caller drops.  When one end of the link is being
 * removed, the ASPM state of the link is left alone: the link state is
 * about to be freed.
 */
static struct pci_dev *pci_lmr_teardown(struct pci_lmr_port *port)
{
	struct pci_dev *partner = port->partner;
	bool removing;

	lockdep_assert_held(&port->lock);

	if (port->state != LMR_ACTIVE)
		return NULL;

	removing = !pci_dev_is_added(port->dev) || !pci_dev_is_added(partner);

	if (!pci_dev_is_disconnected(port->dev))
		pci_lmr_reset_lanes(port);
	pci_lmr_autonomous_restore(port);
	if (!removing)
		pci_lmr_aspm_restore(port);

	pci_lmr_forget_rx(port);
	scoped_guard(mutex, &pci_lmr_mutex) {
		port->partner = NULL;
		pci_lmr_set_state(port, LMR_IDLE);
	}

	return partner;
}

static void pci_lmr_release(struct pci_lmr_port *port, struct pci_dev *partner)
{
	pm_runtime_put_sync(&partner->dev);
	pm_runtime_put_sync(&port->dev->dev);
	pci_dev_put(partner);
}

static int pci_lmr_get_active_partner(struct pci_lmr_port *port,
				      struct pci_dev **partner)
{
	guard(mutex)(&port->lock);

	if (port->state == LMR_IDLE) {
		*partner = NULL;
		return 0;
	}
	if (port->state != LMR_ACTIVE)
		return -EBUSY;

	*partner = pci_dev_get(port->partner);
	return 0;
}

static struct pci_dev *pci_lmr_teardown_if_partner(struct pci_lmr_port *port,
						   struct pci_dev *partner)
{
	guard(mutex)(&port->lock);

	/* The session may have ended, or been replaced, meanwhile */
	if (port->partner != partner)
		return NULL;

	return pci_lmr_teardown(port);
}

static int pci_lmr_stop(struct pci_lmr_port *port)
{
	struct pci_dev *partner, *dp, *up, *ended;
	int ret;

	ret = pci_lmr_get_active_partner(port, &partner);
	if (ret || !partner)
		return ret;

	dp = port->downstream ? port->dev : partner;
	up = port->downstream ? partner : port->dev;

	ret = -EBUSY;
	if (!pci_dev_trylock(dp))
		goto put;
	if (!pci_dev_trylock(up))
		goto unlock_dp;

	ended = pci_lmr_teardown_if_partner(port, partner);
	if (ended)
		pci_lmr_release(port, ended);
	ret = 0;

	pci_dev_unlock(up);
unlock_dp:
	pci_dev_unlock(dp);
put:
	pci_dev_put(partner);
	return ret;
}

static struct pci_lmr_port *pci_lmr_find_port(struct pci_dev *dev)
{
	struct pci_lmr_port *port;

	lockdep_assert_held(&pci_lmr_mutex);

	list_for_each_entry(port, &pci_lmr_ports, node)
		if (port->dev == dev)
			return port;

	return NULL;
}

/* Find an active session that involves @dev, at either end of its link */
static struct pci_lmr_port *pci_lmr_find_session(struct pci_dev *dev)
{
	struct pci_lmr_port *port;

	lockdep_assert_held(&pci_lmr_mutex);

	list_for_each_entry(port, &pci_lmr_ports, node)
		if (port->state == LMR_ACTIVE &&
		    (port->dev == dev || port->partner == dev))
			return port;

	return NULL;
}

/**
 * pci_lmr_exit - End margining on a device that is being removed
 * @dev: PCI device
 *
 * Called from pci_destroy_dev() for every device.  Removes the debugfs
 * directory of @dev, waiting for file operations in progress, and ends
 * every session that involves @dev, whichever end of the link hosts it.
 * Removal of the two ends of a link is serialized by
 * pci_rescan_remove_lock, so a port found here cannot be freed under us.
 */
void pci_lmr_exit(struct pci_dev *dev)
{
	struct pci_lmr_port *port, *p;
	struct pci_dev *ended;

	scoped_guard(mutex, &pci_lmr_mutex)
		port = pci_lmr_find_port(dev);

	if (port)
		debugfs_remove(port->dir);

	for (;;) {
		scoped_guard(mutex, &pci_lmr_mutex)
			p = pci_lmr_find_session(dev);
		if (!p)
			break;

		scoped_guard(mutex, &p->lock)
			ended = pci_lmr_teardown(p);
		if (ended)
			pci_lmr_release(p, ended);
	}

	if (!port)
		return;

	scoped_guard(mutex, &pci_lmr_mutex)
		list_del(&port->node);
	mutex_destroy(&port->lock);
	kfree(port);
}

static int enable_show(struct seq_file *s, void *unused)
{
	struct pci_lmr_port *port = s->private;

	guard(mutex)(&port->lock);
	seq_printf(s, "%d\n", port->state == LMR_ACTIVE);

	return 0;
}

static ssize_t enable_write(struct file *file, const char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct pci_lmr_port *port = s->private;
	bool enable;
	int ret;

	ret = kstrtobool_from_user(ubuf, count, &enable);
	if (ret)
		return ret;

	if (enable) {
		scoped_guard(mutex, &port->lock)
			if (port->state == LMR_ACTIVE)
				return count;
		ret = pci_lmr_start(port);
	} else {
		ret = pci_lmr_stop(port);
	}

	return ret ?: count;
}
DEFINE_SHOW_STORE_ATTRIBUTE(enable);

static int receiver_show(struct seq_file *s, void *unused)
{
	struct pci_lmr_port *port = s->private;

	guard(mutex)(&port->lock);
	seq_printf(s, "%u\n", port->state == LMR_ACTIVE ? port->receiver :
		   pci_lmr_default_rx(port));

	return 0;
}

static bool pci_lmr_rx_valid(struct pci_lmr_port *port, u8 rx)
{
	if (!port->downstream)
		return rx == LMR_RX_UP;

	/* Each retimer has two receivers, numbered from 2 */
	return rx >= LMR_RX_DP && rx <= LMR_RX_DP + 2 * port->retimers;
}

static ssize_t receiver_write(struct file *file, const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct pci_lmr_port *port = s->private;
	u8 rx;
	int ret;

	ret = kstrtou8_from_user(ubuf, count, 10, &rx);
	if (ret)
		return ret;

	guard(mutex)(&port->lock);

	ret = pci_lmr_check(port);
	if (ret)
		return ret;

	if (!pci_lmr_rx_valid(port, rx))
		return -EINVAL;

	if (rx == port->receiver)
		return count;

	/* Leave no lane margined by the old receiver */
	ret = pci_lmr_reset_lanes(port);
	if (ret)
		return ret;

	pci_lmr_forget_rx(port);
	port->receiver = rx;

	return count;
}
DEFINE_SHOW_STORE_ATTRIBUTE(receiver);

static int port_show(struct seq_file *s, void *unused)
{
	struct pci_lmr_port *port = s->private;
	struct pci_dev *dev = port->dev;
	u16 cap, sts;
	int ret;

	ret = pm_runtime_resume_and_get(&dev->dev);
	if (ret)
		return ret;

	if (pci_read_config_word(dev, port->cap + PCI_LMR_PORT_CAP, &cap) ||
	    pci_read_config_word(dev, port->cap + PCI_LMR_PORT_STS, &sts) ||
	    PCI_POSSIBLE_ERROR(cap) || PCI_POSSIBLE_ERROR(sts))
		ret = -ENODEV;

	pm_runtime_put(&dev->dev);
	if (ret)
		return ret;

	seq_printf(s, "uses_driver_software: %d\n",
		   !!(cap & PCI_LMR_PORT_CAP_USES_SW_READY));
	seq_printf(s, "margining_ready: %d\n",
		   !!(sts & PCI_LMR_PORT_STS_MARGIN_READY));
	seq_printf(s, "software_ready: %d\n",
		   !!(sts & PCI_LMR_PORT_STS_SW_READY));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(port);

static int caps_show(struct seq_file *s, void *unused)
{
	struct pci_lmr_lane *l = s->private;
	struct pci_lmr_port *port = l->port;
	struct pci_lmr_rx *rx = &l->rx;
	int ret;

	guard(mutex)(&port->lock);

	ret = pci_lmr_check(port);
	if (ret)
		return ret;

	ret = pci_lmr_get_rx(l);
	if (ret)
		return ret;

	seq_printf(s, "receiver: %u\n", port->receiver);
	seq_printf(s, "capabilities: %#04x\n", rx->caps);
	seq_printf(s, "voltage_supported: %d\n",
		   !!(rx->caps & LMR_CAP_VOLTAGE));
	seq_printf(s, "independent_up_down_voltage: %d\n",
		   !!(rx->caps & LMR_CAP_IND_UP_DOWN));
	seq_printf(s, "independent_left_right_timing: %d\n",
		   !!(rx->caps & LMR_CAP_IND_LEFT_RIGHT));
	seq_printf(s, "sample_reporting_method: %s\n",
		   rx->caps & LMR_CAP_SAMPLE_RATE ? "rate" : "count");
	seq_printf(s, "independent_error_sampler: %d\n",
		   !!(rx->caps & LMR_CAP_IND_ERR_SAMPLER));
	seq_printf(s, "timing_steps: %u\n", rx->timing_steps);
	seq_printf(s, "max_timing_offset: %u\n", rx->max_timing_offset);
	seq_printf(s, "voltage_steps: %u\n", rx->voltage_steps);
	seq_printf(s, "max_voltage_offset: %u\n", rx->max_voltage_offset);
	seq_printf(s, "max_lanes: %u\n", rx->max_lanes);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(caps);

static ssize_t pci_lmr_step_write(struct file *file, const char __user *ubuf,
				  size_t count, bool timing)
{
	struct seq_file *s = file->private_data;
	struct pci_lmr_lane *l = s->private;
	int steps, ret;

	ret = kstrtoint_from_user(ubuf, count, 10, &steps);
	if (ret)
		return ret;

	guard(mutex)(&l->port->lock);

	ret = pci_lmr_step(l, timing, steps);

	return ret ?: count;
}

static int margin_timing_show(struct seq_file *s, void *unused)
{
	struct pci_lmr_lane *l = s->private;

	guard(mutex)(&l->port->lock);
	seq_printf(s, "%d\n", l->timing);

	return 0;
}

static ssize_t margin_timing_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	return pci_lmr_step_write(file, ubuf, count, true);
}
DEFINE_SHOW_STORE_ATTRIBUTE(margin_timing);

static int margin_voltage_show(struct seq_file *s, void *unused)
{
	struct pci_lmr_lane *l = s->private;

	guard(mutex)(&l->port->lock);
	seq_printf(s, "%d\n", l->voltage);

	return 0;
}

static ssize_t margin_voltage_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	return pci_lmr_step_write(file, ubuf, count, false);
}
DEFINE_SHOW_STORE_ATTRIBUTE(margin_voltage);

static const char * const pci_lmr_step_status[] = {
	[LMR_STEP_TOO_MANY_ERR]	= "too_many_errors",
	[LMR_STEP_SETUP]	= "setup",
	[LMR_STEP_MARGINING]	= "margining",
	[LMR_STEP_NAK]		= "nak",
};

/* Show the receiver's response to the last step command, if any */
static int status_show(struct seq_file *s, void *unused)
{
	struct pci_lmr_lane *l = s->private;
	struct pci_lmr_port *port = l->port;
	u8 type, payload;
	u16 sts;

	guard(mutex)(&port->lock);

	if (port->state != LMR_ACTIVE)
		return -ENOTCONN;

	if (atomic_read(&pci_lmr_pm_gen) != port->pm_gen)
		return -EIO;

	sts = pci_lmr_lane_sts(port, l->lane);
	if (PCI_POSSIBLE_ERROR(sts))
		return -ENODEV;

	type = FIELD_GET(PCI_LMR_LANE_TYPE, sts);
	payload = FIELD_GET(PCI_LMR_LANE_PAYLOAD, sts);
	if (FIELD_GET(PCI_LMR_LANE_RX, sts) != port->receiver ||
	    (type != LMR_TYPE_TIMING && type != LMR_TYPE_VOLTAGE)) {
		seq_puts(s, "idle\n");
		return 0;
	}

	seq_printf(s, "%s %s %lu\n",
		   type == LMR_TYPE_TIMING ? "timing" : "voltage",
		   pci_lmr_step_status[FIELD_GET(LMR_STEP_STATUS, payload)],
		   FIELD_GET(LMR_STEP_ERR_COUNT, payload));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static void pci_lmr_debugfs_init(struct pci_lmr_port *port)
{
	char name[32];
	unsigned int i;

	snprintf(name, sizeof(name), "pcie_lmr_%s", pci_name(port->dev));
	port->dir = debugfs_create_dir(name, NULL);

	debugfs_create_file("enable", 0600, port->dir, port, &enable_fops);
	debugfs_create_file("receiver", 0600, port->dir, port, &receiver_fops);
	debugfs_create_file("port", 0400, port->dir, port, &port_fops);

	for (i = 0; i < port->nr_lanes; i++) {
		struct pci_lmr_lane *l = &port->lanes[i];
		struct dentry *dir;

		snprintf(name, sizeof(name), "lane%u", i);
		dir = debugfs_create_dir(name, port->dir);

		debugfs_create_file("caps", 0400, dir, l, &caps_fops);
		debugfs_create_file("margin_timing", 0600, dir, l,
				    &margin_timing_fops);
		debugfs_create_file("margin_voltage", 0600, dir, l,
				    &margin_voltage_fops);
		debugfs_create_file("status", 0400, dir, l, &status_fops);
	}
}

/**
 * pci_lmr_init - Set up margining for a device that has the capability
 * @dev: PCI device
 *
 * The capability is used on Downstream Ports and on Function 0 of the
 * device below a Downstream Port.
 */
void pci_lmr_init(struct pci_dev *dev)
{
	struct pci_lmr_port *port;
	struct pci_dev *dport;
	unsigned int i, width;
	u32 lnkcap;
	u16 cap;

	if (!pci_is_pcie(dev) || dev->is_virtfn)
		return;

	if (pcie_downstream_port(dev)) {
		dport = dev;
	} else {
		if (dev->devfn != 0)
			return;
		dport = pci_upstream_bridge(dev);
		if (!dport || !pci_is_pcie(dport) ||
		    !pcie_downstream_port(dport))
			return;
	}

	cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_LMR);
	if (!cap)
		return;

	if (pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &lnkcap))
		return;

	width = FIELD_GET(PCI_EXP_LNKCAP_MLW, lnkcap);
	if (!width || width > LMR_MAX_WIDTH)
		return;

	port = kzalloc(struct_size(port, lanes, width), GFP_KERNEL);
	if (!port)
		return;

	port->nr_lanes = width;
	port->dev = dev;
	port->dport = dport;
	port->cap = cap;
	port->downstream = dport == dev;
	mutex_init(&port->lock);
	for (i = 0; i < width; i++) {
		port->lanes[i].port = port;
		port->lanes[i].lane = i;
	}

	pci_lmr_debugfs_init(port);

	guard(mutex)(&pci_lmr_mutex);
	list_add_tail(&port->node, &pci_lmr_ports);
}

/*
 * Receivers lose their margining state over system suspend.  Commands
 * on a session that was active before a suspend fail with -EIO, and the
 * session has to be stopped and started again.
 */
static int pci_lmr_pm_notify(struct notifier_block *nb, unsigned long action,
			     void *data)
{
	switch (action) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		atomic_inc(&pci_lmr_pm_gen);
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block pci_lmr_pm_nb = {
	.notifier_call = pci_lmr_pm_notify,
};

/* Return margined lanes to normal settings before reboot or kexec */
static int pci_lmr_reboot_notify(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	struct pci_lmr_port *port;

	guard(mutex)(&pci_lmr_mutex);
	list_for_each_entry(port, &pci_lmr_ports, node) {
		if (port->state != LMR_ACTIVE)
			continue;
		if (!mutex_trylock(&port->lock))
			continue;
		if (port->state == LMR_ACTIVE) {
			if (!pci_dev_is_disconnected(port->dev))
				pci_lmr_reset_lanes(port);
			pci_lmr_autonomous_restore(port);
		}
		mutex_unlock(&port->lock);
	}

	return NOTIFY_DONE;
}

static struct notifier_block pci_lmr_reboot_nb = {
	.notifier_call = pci_lmr_reboot_notify,
};

static int __init pci_lmr_pm_init(void)
{
	register_reboot_notifier(&pci_lmr_reboot_nb);
	return register_pm_notifier(&pci_lmr_pm_nb);
}
late_initcall(pci_lmr_pm_init);
