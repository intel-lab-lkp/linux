// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Express Lane Margining at Receiver
 *
 * Copyright (C) 2026 Google LLC
 * Author: Priyank Rathod <rathodpriyank@google.com>
 *
 * Lane Margining at Receiver (PCIe Base Specification Revision 7.0,
 * sec 7.7.11 & sec 8.4.4) allows system software to determine the voltage
 * and timing margins of each physical lane on a PCIe link. The Extended
 * Capability (ID 0x27) is available for receivers operating at 16.0 GT/s
 * (Gen4) or higher data rates, and is mandatory for receivers operating at
 * 64.0 GT/s (Gen6) or higher data rates.
 *
 * This driver implements:
 *   - Probing Extended Capability ID 0x27 and Margining Port Capabilities.
 *   - Managing ASPM L0s/L1 link states during active margining with
 *     temporary inhibition (pci_aspm_inhibit).
 *   - PCIe Base Specification NO_CMD (0x7) clearing handshake per receiver
 *     and lane.
 *   - Caching receiver capabilities & step counts to avoid side-effects
 *     when setting to normal settings.
 *   - Handling Symmetric vs Independent Left/Right & Up/Down margin steps.
 *   - Runtime PM protection (D0 enforcement) during active margining.
 *   - Exposing per-device debugfs interfaces under /sys/kernel/debug/pci/.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kstrtox.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/string_choices.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "../pci.h"

/*
 * Margining Type (MTYPE) field encodings (bits 5:3) in Margining Lane Control
 * and Margining Lane Status registers per PCIe Base Specification
 * Revision 7.0:
 * - Section 7.7.11 "Lane Margining at the Receiver Extended Capability
 *   (ID 0x27)" (Margining Lane Control & Margining Lane Status Registers)
 * - Section 4.2.18.2 "Margin Command and Response Flow"
 *   (Table 4-77 "Margin Commands and Corresponding Responses")
 *
 * Encodings:
 *   001b (0x1) - Report Margin Control Capabilities
 *   010b (0x2) - Set Margining Parameters (Go to Normal Settings,
 *                Clear Error Log)
 *   011b (0x3) - Step Margin Timing
 *   100b (0x4) - Step Margin Voltage
 *   111b (0x7) - No Command
 *   (000b, 101b-110b are Reserved)
 */
#define LMR_TYPE_REPORT_CAPS		0x1	/* Report Capabilities */
#define LMR_TYPE_SET_PARAMS		0x2	/* Set Margining Parameters */
#define LMR_TYPE_TIMING			0x3	/* Step Margin Timing */
#define LMR_TYPE_VOLTAGE		0x4	/* Step Margin Voltage */
#define LMR_TYPE_NO_CMD			0x7	/* No Command */

/* Command Payloads per PCIe Base Specification Revision 7.0 Table 4-77 */
#define LMR_PAYLOAD_REPORT_CAPS		0x88	/* Report Capabilities */
#define LMR_PAYLOAD_REPORT_VOLT_STEPS	0x89	/* Report Voltage Steps */
#define LMR_PAYLOAD_REPORT_TIM_STEPS	0x8A	/* Report Timing Steps */
#define LMR_PAYLOAD_GO_TO_NORMAL	0x0F	/* Go to Normal Settings */
#define LMR_PAYLOAD_CLEAR_ERROR_LOG	0x55	/* Clear Error Log */
#define LMR_PAYLOAD_NO_CMD		0x9C	/* No Command */

/* LMR command timing parameters */
#define LMR_CMD_TIMEOUT_MS              150
#define LMR_CMD_SLEEP_MIN_US            100
#define LMR_CMD_SLEEP_MAX_US            250
#define LMR_ENABLE_TIMEOUT_MS           150
#define LMR_ENABLE_SLEEP_MIN_US         1000
#define LMR_ENABLE_SLEEP_MAX_US         2000

/*
 * LMR parameter limits per PCIe Base Specification Revision 7.0:
 * - Max lanes (32): sec 7.7.11 & Table 8-13 (MMaxLanes max 31)
 * - Receiver numbers 0..6: Table 4-76 (assignment) & Table 4-77 (valid for
 *   commands)
 * - Max timing step (63): sec 4.2.18.1.2, Table 4-77 (8Ah), & Table 8-13
 * - Max voltage step (127): sec 4.2.18.1.2, Table 4-77 (89h), & Table 8-13
 */
#define LMR_MAX_LANES			32
#define LMR_MAX_RX_NUM			6
#define LMR_MAX_TIMING_STEP		63
#define LMR_MAX_VOLTAGE_STEP		127

/* LMR PCIe generation numbers and helper */
#define LMR_GEN6                        6
#define LMR_GEN5                        5
#define LMR_GEN4                        4

#define LMR_SPEED_TO_GEN(speed) \
	((speed) >= PCIE_SPEED_64_0GT ? LMR_GEN6 : \
	 (speed) >= PCIE_SPEED_32_0GT ? LMR_GEN5 : \
	 LMR_GEN4)

/* LMR lane register stride */
#define LMR_LANE_REG_STRIDE             4

/* LMR receivers */
#define LMR_RX_LOCAL                    0

/*
 * Margining Payload field masks for Step Margin Timing and Step Margin Voltage
 * per PCIe Base Specification Revision 7.0 sec 4.2.18.1.2
 * ("Margin Payload for Step Margin Commands"):
 *
 * Step Margin Timing Payload:
 *   Bit 7:    Reserved (must be 0b)
 *   Bit 6:    Direction (0 = Right/Up, 1 = Left/Down)
 *   Bits 5:0: Margin Step (0..63)
 *
 * Step Margin Voltage Payload:
 *   Bit 7:    Direction (0 = Right/Up, 1 = Left/Down)
 *   Bits 6:0: Margin Step (0..127)
 */
#define LMR_TIMING_STEP_MASK		GENMASK(5, 0)
#define LMR_TIMING_DIR_MASK		BIT(6)
#define LMR_VOLTAGE_STEP_MASK		GENMASK(6, 0)
#define LMR_VOLTAGE_DIR_MASK		BIT(7)

/*
 * Margin Payload step direction field encodings per PCIe Base Specification
 * Revision 7.0 sec 4.2.18.1.2 ("Margin Payload for Step Margin Commands"):
 *
 * For timing:
 *   Bit 6: 0b = Right of normal setting (also 0b for symmetric margining)
 *          1b = Left of normal setting (when MIndLeftRightTiming is Set)
 * For voltage:
 *   Bit 7: 0b = Up from normal setting (also 0b for symmetric margining)
 *          1b = Down from normal setting (when MIndUpDownVoltage is Set)
 */
#define LMR_STEP_DIR_RIGHT_OR_UP	0
#define LMR_STEP_DIR_LEFT_OR_DOWN	1

/*
 * Report Margin Control Capabilities (Command 88h) response payload bit fields
 * per PCIe Base Specification Revision 7.0 Table 4-77 & Table 8-13:
 *   Bit 0:   MVoltageSupported (1 = Voltage margining supported;
 *            0 = Not supported)
 *   Bit 1:   MIndUpDownVoltage (1 = Independent Up/Down voltage supported;
 *            0 = Symmetric)
 *   Bit 2:   MIndLeftRightTiming (1 = Independent Left/Right timing
 *            supported; 0 = Symmetric)
 *   Bit 3:   MSampleReportingMethod (1 = Sampling rate supported;
 *            0 = Sample count supported)
 *   Bit 4:   MIndErrorSampler (1 = Independent error sampler;
 *            0 = Main data sampler)
 *   Bit 5:   MSampleMultipleReceivers (1 = Multiple receivers can be sampled
 *            concurrently; 0 = Only one receiver at a time)
 *   Bits 7:6: Reserved
 */
#define LMR_CAP_VOLTAGE_SUPPORTED	BIT(0)
#define LMR_CAP_IND_UP_DOWN_VOLTAGE	BIT(1)
#define LMR_CAP_IND_LEFT_RIGHT_TIMING	BIT(2)
#define LMR_CAP_SAMPLE_REPORT_METHOD	BIT(3)
#define LMR_CAP_IND_ERROR_SAMPLER	BIT(4)
#define LMR_CAP_SAMPLE_MULTIPLE_RECEIVERS BIT(5)

/*
 * Step Margin Execution Status (Bits 7:6 of response payload per PCIe Base
 * Specification Revision 7.0 sec 4.2.18.1.1 "Step Margin Execution Status"):
 * 00b: Too many errors - Receiver autonomously went back to default settings
 * 01b: Set up for margin in progress
 * 10b: Margining in progress
 * 11b: NAK - Unsupported Lane Margining command was issued
 */
#define LMR_STS_EXEC_MASK		GENMASK(7, 6)
#define LMR_STS_EXEC_TOO_MANY_ERR	0x0
#define LMR_STS_EXEC_SETUP_IN_PROGRESS	0x1
#define LMR_STS_EXEC_IN_PROGRESS	0x2
#define LMR_STS_EXEC_NAK		0x3
#define LMR_STS_ERR_CNT_MASK		GENMASK(5, 0)

/**
 * struct pci_margin_rx_info - Cached Lane Margining receiver capabilities
 * @caps_cached: True if receiver capabilities and step limits are cached
 * @caps: Margining capabilities byte reported by receiver
 * @num_timing_steps: Maximum timing margin steps supported by receiver
 * @num_voltage_steps: Maximum voltage margin steps supported by receiver
 */
struct pci_margin_rx_info {
	bool caps_cached;
	u8 caps;
	u8 num_timing_steps;
	u8 num_voltage_steps;
};

/**
 * struct pci_margin_lane - Per-lane margining state
 * @mdev: Parent LMR margin device
 * @lane: Physical lane index (0..num_lanes - 1)
 * @rx: Selected target receiver number (0 = local, 1..6 = retimers)
 * @timing_val: Current applied timing margin step offset (+/-)
 * @voltage_val: Current applied voltage margin step offset (+/-)
 * @rx_info: Cached receiver capabilities per receiver number
 */
struct pci_margin_lane {
	struct pci_margin_dev *mdev;
	int lane;
	u8 rx;
	int timing_val;
	int voltage_val;
	struct pci_margin_rx_info rx_info[LMR_MAX_RX_NUM + 1];
};

/**
 * struct pci_margin_dev - PCIe Lane Margining device instance
 * @dev: Underlying PCI device
 * @partner: Connected link partner device across the PCIe link
 * @cap: Extended capability offset (PCI_EXT_CAP_ID_LMR)
 * @debugfs: Root debugfs dentry for this device
 * @lock: Mutex protecting LMR hardware access, active margining enablement,
 *        target receiver selection, and lane margining steps
 * @enabled: True if Lane Margining is currently enabled
 * @aspm_inhibited: True if ASPM is temporarily inhibited for active margining
 * @autonomous_saved: True if original autonomous width/speed configuration
 *                    has been saved
 * @saved_dsp_lnkctl_valid: True if Downstream Port Link Control was saved
 * @saved_dsp_lnkctl2_valid: True if Downstream Port Link Control 2 was saved
 * @saved_usp_lnkctl_valid: True if Upstream Port Link Control was saved
 * @saved_usp_lnkctl2_valid: True if Upstream Port Link Control 2 was saved
 * @saved_dsp_lnkctl: Saved Link Control register bits for Downstream Port
 * @saved_dsp_lnkctl2: Saved Link Control 2 register bits for Downstream Port
 * @saved_usp_lnkctl: Saved Link Control register bits for Upstream Port
 * @saved_usp_lnkctl2: Saved Link Control 2 register bits for Upstream Port
 * @num_lanes: Number of lanes on the link
 * @lanes: Flexible array of per-lane state structures
 */
struct pci_margin_dev {
	struct pci_dev *dev;
	struct pci_dev *partner;
	u16 cap;
	struct dentry *debugfs;
	struct mutex lock;
	bool enabled;
	bool aspm_inhibited;
	bool autonomous_saved;
	bool saved_dsp_lnkctl_valid;
	bool saved_dsp_lnkctl2_valid;
	bool saved_usp_lnkctl_valid;
	bool saved_usp_lnkctl2_valid;
	u16 saved_dsp_lnkctl;
	u16 saved_dsp_lnkctl2;
	u16 saved_usp_lnkctl;
	u16 saved_usp_lnkctl2;
	int num_lanes;
	struct pci_margin_lane lanes[] __counted_by(num_lanes);
};

#if IS_ENABLED(CONFIG_DEBUG_FS)
static DEFINE_MUTEX(pci_debugfs_root_lock);
static struct dentry *pci_debugfs_root_dir;

static struct dentry *get_pci_debugfs_root(void)
{
	mutex_lock(&pci_debugfs_root_lock);
	if (!pci_debugfs_root_dir)
		pci_debugfs_root_dir = debugfs_lookup("pci", NULL);
	if (!pci_debugfs_root_dir)
		pci_debugfs_root_dir = debugfs_create_dir("pci", NULL);
	mutex_unlock(&pci_debugfs_root_lock);
	return pci_debugfs_root_dir;
}
#endif

/*
 * pci_lmr_get_ports() - Identify Downstream and Upstream Port link partners
 * using the already tracked mdev->dev and mdev->partner devices.
 *
 * For Root Ports and Switch Downstream Ports, @dev is the Downstream Port and
 * @partner is the Upstream Port. For Endpoints and Switch Upstream Ports,
 * @partner is the Downstream Port and @dev is the Upstream Port.
 *
 * Context: Called with mdev->lock held and partner already established.
 * Does NOT acquire pci_bus_sem, preventing lock inversion deadlocks with
 * device_lock.
 */
static void pci_lmr_get_ports(struct pci_margin_dev *mdev,
			      struct pci_dev **downstream_port,
			      struct pci_dev **upstream_port)
{
	struct pci_dev *dev = mdev->dev;
	struct pci_dev *partner = mdev->partner;

	if (pcie_downstream_port(dev)) {
		*downstream_port = dev;
		*upstream_port = partner;
	} else {
		*downstream_port = partner;
		*upstream_port = dev;
	}
}



/*
 * Helpers to manage Autonomous Width/Speed transitions per PCIe Base
 * Specification Revision 7.0:
 * - Section 7.5.3.7 "Link Control Register" (Hardware Autonomous Width
 *   Disable, bit 9)
 * - Section 7.5.3.17 "Link Control 2 Register" (Hardware Autonomous Speed
 *   Disable, bit 5)
 * - Section 4.2.18.4 "Receiver Margin Testing Requirements"
 * - Section 8.4.4 "Lane Margining at the Receiver - Electrical Requirements"
 *
 * Both Downstream Port and Upstream Port must save and set Hardware Autonomous
 * Width Disable and Hardware Autonomous Speed Disable bits during margining to
 * guarantee that the link remains in a stable active L0 state.
 */
static void pci_lmr_disable_autonomous(struct pci_margin_dev *mdev)
{
	struct pci_dev *downstream_port, *upstream_port;
	u16 lnkctl, lnkctl2;
	int ret;

	if (mdev->autonomous_saved)
		return;

	pci_lmr_get_ports(mdev, &downstream_port, &upstream_port);

	/* 1. Downstream Component (upstream_port): Save and Disable FIRST */
	if (upstream_port && pci_is_pcie(upstream_port) &&
	    !pci_dev_is_disconnected(upstream_port) &&
	    upstream_port->current_state == PCI_D0) {
		ret = pcie_capability_read_word(upstream_port, PCI_EXP_LNKCTL, &lnkctl);
		if (ret == PCIBIOS_SUCCESSFUL && !PCI_POSSIBLE_ERROR(lnkctl)) {
			mdev->saved_usp_lnkctl = lnkctl;
			mdev->saved_usp_lnkctl_valid = true;
			pcie_capability_set_word(upstream_port, PCI_EXP_LNKCTL,
						 PCI_EXP_LNKCTL_HAWD);
		}

		ret = pcie_capability_read_word(upstream_port, PCI_EXP_LNKCTL2, &lnkctl2);
		if (ret == PCIBIOS_SUCCESSFUL && !PCI_POSSIBLE_ERROR(lnkctl2)) {
			mdev->saved_usp_lnkctl2 = lnkctl2;
			mdev->saved_usp_lnkctl2_valid = true;
			pcie_capability_set_word(upstream_port, PCI_EXP_LNKCTL2,
						 PCI_EXP_LNKCTL2_HASD);
		}
	}

	/* 2. Upstream Component (downstream_port): Save and Disable SECOND */
	if (downstream_port && pci_is_pcie(downstream_port) &&
	    !pci_dev_is_disconnected(downstream_port) &&
	    downstream_port->current_state == PCI_D0) {
		ret = pcie_capability_read_word(downstream_port, PCI_EXP_LNKCTL, &lnkctl);
		if (ret == PCIBIOS_SUCCESSFUL && !PCI_POSSIBLE_ERROR(lnkctl)) {
			mdev->saved_dsp_lnkctl = lnkctl;
			mdev->saved_dsp_lnkctl_valid = true;
			pcie_capability_set_word(downstream_port, PCI_EXP_LNKCTL,
						 PCI_EXP_LNKCTL_HAWD);
		}

		ret = pcie_capability_read_word(downstream_port, PCI_EXP_LNKCTL2, &lnkctl2);
		if (ret == PCIBIOS_SUCCESSFUL && !PCI_POSSIBLE_ERROR(lnkctl2)) {
			mdev->saved_dsp_lnkctl2 = lnkctl2;
			mdev->saved_dsp_lnkctl2_valid = true;
			pcie_capability_set_word(downstream_port, PCI_EXP_LNKCTL2,
						 PCI_EXP_LNKCTL2_HASD);
		}
	}

	mdev->autonomous_saved = mdev->saved_usp_lnkctl_valid ||
				 mdev->saved_usp_lnkctl2_valid ||
				 mdev->saved_dsp_lnkctl_valid ||
				 mdev->saved_dsp_lnkctl2_valid;
}

static void pci_lmr_restore_autonomous(struct pci_margin_dev *mdev)
{
	struct pci_dev *downstream_port, *upstream_port;

	if (!mdev->autonomous_saved)
		return;

	pci_lmr_get_ports(mdev, &downstream_port, &upstream_port);

	/*
	 * PCIe Base Specification Revision 7.0 sec 7.5.3.7 & Table 7-24:
	 * 1. Upstream Component (downstream_port) restored FIRST.
	 */
	if (downstream_port && pci_is_pcie(downstream_port) &&
	    !pci_dev_is_disconnected(downstream_port) &&
	    downstream_port->current_state == PCI_D0) {
		if (mdev->saved_dsp_lnkctl_valid)
			pcie_capability_clear_and_set_word(
				downstream_port, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_HAWD,
				mdev->saved_dsp_lnkctl & PCI_EXP_LNKCTL_HAWD);
		if (mdev->saved_dsp_lnkctl2_valid)
			pcie_capability_clear_and_set_word(
				downstream_port, PCI_EXP_LNKCTL2, PCI_EXP_LNKCTL2_HASD,
				mdev->saved_dsp_lnkctl2 & PCI_EXP_LNKCTL2_HASD);
	}

	/*
	 * 2. Downstream Component (upstream_port) restored SECOND.
	 */
	if (upstream_port && pci_is_pcie(upstream_port) &&
	    !pci_dev_is_disconnected(upstream_port) &&
	    upstream_port->current_state == PCI_D0) {
		if (mdev->saved_usp_lnkctl_valid)
			pcie_capability_clear_and_set_word(
				upstream_port, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_HAWD,
				mdev->saved_usp_lnkctl & PCI_EXP_LNKCTL_HAWD);
		if (mdev->saved_usp_lnkctl2_valid)
			pcie_capability_clear_and_set_word(
				upstream_port, PCI_EXP_LNKCTL2, PCI_EXP_LNKCTL2_HASD,
				mdev->saved_usp_lnkctl2 & PCI_EXP_LNKCTL2_HASD);
	}

	mdev->saved_dsp_lnkctl_valid = false;
	mdev->saved_dsp_lnkctl2_valid = false;
	mdev->saved_usp_lnkctl_valid = false;
	mdev->saved_usp_lnkctl2_valid = false;
	mdev->autonomous_saved = false;
}

static inline u8 pci_lmr_sts_payload(u16 sts)
{
	return FIELD_GET(PCI_LMR_LANE_STS_PAYLOAD, sts);
}

/*
 * pci_lmr_get_active_lanes() - Determine the current number of active lanes
 * based on Negotiated Link Width (NLW) in Link Status Register.
 *
 * Per PCIe Base Specification Revision 7.0 sec 8.4.4, only active lanes
 * respond to margining commands. Inactive lanes do not assert Margining Ready,
 * causing command timeouts.
 *
 * If the link is down (NLW == 0 or configuration read fails), returns 0 to
 * prevent 150 ms per-lane timeout stalls on inactive/down hardware.
 */
static int pci_lmr_get_active_lanes(struct pci_margin_dev *mdev)
{
	struct pci_dev *dev = mdev->dev;
	u16 lnksta;
	int ret, nlw;

	ret = pcie_capability_read_word(dev, PCI_EXP_LNKSTA, &lnksta);
	if (ret == PCIBIOS_SUCCESSFUL && !PCI_POSSIBLE_ERROR(lnksta)) {
		nlw = FIELD_GET(PCI_EXP_LNKSTA_NLW, lnksta);
		if (nlw > 0 && nlw <= mdev->num_lanes)
			return nlw;
	}
	return 0;
}

/*
 * pci_lmr_run_cmd() - Issue LMR command to Lane Control and wait for Status.
 * Must be called with mdev->lock held.
 */
static int pci_lmr_run_cmd(struct pci_margin_dev *mdev, int lane, u8 rx, u8 type,
			   u8 usage, u8 payload, u16 *status_val)
{
	struct pci_dev *dev;
	u16 lmr, ctrl_offset, sts_offset;
	u16 ctrl, sts;
	unsigned long timeout;
	int ret;

	if (!mdev || lane < 0 || lane >= mdev->num_lanes || rx > LMR_MAX_RX_NUM)
		return -EINVAL;

	if (lane >= pci_lmr_get_active_lanes(mdev))
		return -ENODEV;

	dev = mdev->dev;
	lmr = mdev->cap;
	ctrl_offset = lmr + PCI_LMR_LANE_CTRL + LMR_LANE_REG_STRIDE * lane;
	sts_offset = lmr + PCI_LMR_LANE_STS + LMR_LANE_REG_STRIDE * lane;

	/*
	 * Per PCIe Base Specification Revision 7.0 sec 4.2.18.2 & Table 4-77,
	 * software must issue NO_CMD (0x7) with payload 0x9C targeting the
	 * specific receiver (rx) to clear MTYPE in Lane Status before issuing
	 * a subsequent command.
	 */
	if (type != LMR_TYPE_NO_CMD) {
		ctrl = FIELD_PREP(PCI_LMR_LANE_CTRL_RX_NUM, rx) |
		       FIELD_PREP(PCI_LMR_LANE_CTRL_MTYPE, LMR_TYPE_NO_CMD) |
		       FIELD_PREP(PCI_LMR_LANE_CTRL_USAGE, 0) |
		       FIELD_PREP(PCI_LMR_LANE_CTRL_PAYLOAD,
				  LMR_PAYLOAD_NO_CMD);

		ret = pci_write_config_word(dev, ctrl_offset, ctrl);
		if (ret != PCIBIOS_SUCCESSFUL)
			return pcibios_err_to_errno(ret);

		timeout = jiffies + msecs_to_jiffies(LMR_CMD_TIMEOUT_MS);
		while (1) {
			ret = pci_read_config_word(dev, sts_offset, &sts);
			if (ret != PCIBIOS_SUCCESSFUL)
				return pcibios_err_to_errno(ret);
			if (PCI_POSSIBLE_ERROR(sts))
				return -ENODEV;
			if (FIELD_GET(PCI_LMR_LANE_STS_MTYPE, sts) == LMR_TYPE_NO_CMD &&
			    FIELD_GET(PCI_LMR_LANE_STS_RX_NUM, sts) == rx)
				break;
			if (time_after(jiffies, timeout))
				return -ETIMEDOUT;
			usleep_range(LMR_CMD_SLEEP_MIN_US, LMR_CMD_SLEEP_MAX_US);
		}
	}

	ctrl = FIELD_PREP(PCI_LMR_LANE_CTRL_RX_NUM, rx) |
	       FIELD_PREP(PCI_LMR_LANE_CTRL_MTYPE, type) |
	       FIELD_PREP(PCI_LMR_LANE_CTRL_USAGE, usage) |
	       FIELD_PREP(PCI_LMR_LANE_CTRL_PAYLOAD, payload);

	ret = pci_write_config_word(dev, ctrl_offset, ctrl);
	if (ret != PCIBIOS_SUCCESSFUL)
		return pcibios_err_to_errno(ret);

	timeout = jiffies + msecs_to_jiffies(LMR_CMD_TIMEOUT_MS);
	while (1) {
		ret = pci_read_config_word(dev, sts_offset, &sts);
		if (ret != PCIBIOS_SUCCESSFUL)
			return pcibios_err_to_errno(ret);
		if (PCI_POSSIBLE_ERROR(sts))
			return -ENODEV;

		if (FIELD_GET(PCI_LMR_LANE_STS_MTYPE, sts) == type &&
		    FIELD_GET(PCI_LMR_LANE_STS_RX_NUM, sts) == rx) {
			if (status_val)
				*status_val = sts;
			return 0;
		}

		if (time_after(jiffies, timeout)) {
			/*
			 * Per PCIe Base Specification Revision 7.0 sec 4.2.18.2
			 * & Table 4-77, if receiver echoes NO_CMD (0x7) after
			 * command issuance, it indicates NAK.
			 */
			if (FIELD_GET(PCI_LMR_LANE_STS_MTYPE, sts) == LMR_TYPE_NO_CMD &&
			    FIELD_GET(PCI_LMR_LANE_STS_RX_NUM, sts) == rx)
				return -EOPNOTSUPP;
			break;
		}

		usleep_range(LMR_CMD_SLEEP_MIN_US, LMR_CMD_SLEEP_MAX_US);
	}

	return -ETIMEDOUT;
}

/*
 * pci_lmr_clear_to_normal_lane() - Clear lane margin back to normal settings
 * per PCIe Base Specification Revision 7.0 sec 4.2.18.2 & Table 4-77.
 * Issues Set Margining Parameters (MTYPE 010b) with "Go to Normal Settings"
 * (Payload 0x0F).
 */
static int pci_lmr_clear_to_normal_lane(struct pci_margin_lane *plane)
{
	u16 sts;
	int ret;

	if (!plane || !plane->mdev)
		return -EINVAL;

	if (plane->lane >= pci_lmr_get_active_lanes(plane->mdev)) {
		plane->timing_val = 0;
		plane->voltage_val = 0;
		return 0;
	}

	ret = pci_lmr_run_cmd(plane->mdev, plane->lane, plane->rx,
			      LMR_TYPE_SET_PARAMS, 0, LMR_PAYLOAD_GO_TO_NORMAL,
			      &sts);
	plane->timing_val = 0;
	plane->voltage_val = 0;
	return ret;
}

static int pci_lmr_cache_rx_info(struct pci_margin_lane *plane, u8 rx)
{
	struct pci_margin_rx_info *info;
	u16 sts;
	int ret;

	if (!plane || rx > LMR_MAX_RX_NUM)
		return -EINVAL;

	info = &plane->rx_info[rx];

	if (info->caps_cached)
		return 0;

	/* Issuing REPORT_CAPS aborts active margin; clear to normal settings */
	ret = pci_lmr_clear_to_normal_lane(plane);
	if (ret)
		return ret;

	/* Report Capabilities: MTYPE 001b, Payload 0x88 */
	ret = pci_lmr_run_cmd(plane->mdev, plane->lane, rx,
			      LMR_TYPE_REPORT_CAPS, 0, LMR_PAYLOAD_REPORT_CAPS,
			      &sts);
	if (ret)
		return ret;
	info->caps = pci_lmr_sts_payload(sts);

	/* Report Timing Steps: MTYPE 001b, Payload 0x8A */
	ret = pci_lmr_run_cmd(plane->mdev, plane->lane, rx,
			      LMR_TYPE_REPORT_CAPS, 0,
			      LMR_PAYLOAD_REPORT_TIM_STEPS, &sts);
	if (ret)
		return ret;
	info->num_timing_steps = FIELD_GET(LMR_TIMING_STEP_MASK, pci_lmr_sts_payload(sts));

	/*
	 * Report Voltage Steps: MTYPE 001b, Payload 0x89.
	 * Only query if receiver supports voltage margining. Per PCIe Base
	 * Specification Revision 7.0 Table 4-77, receivers lacking voltage
	 * margining support (MVoltageSupported = 0b) will NAK this command.
	 */
	if (info->caps & LMR_CAP_VOLTAGE_SUPPORTED) {
		ret = pci_lmr_run_cmd(plane->mdev, plane->lane, rx,
				      LMR_TYPE_REPORT_CAPS, 0,
				      LMR_PAYLOAD_REPORT_VOLT_STEPS, &sts);
		if (ret)
			return ret;
		info->num_voltage_steps = FIELD_GET(LMR_VOLTAGE_STEP_MASK,
						    pci_lmr_sts_payload(sts));
	} else {
		info->num_voltage_steps = 0;
	}

	info->caps_cached = true;
	return 0;
}

#if IS_ENABLED(CONFIG_DEBUG_FS)

static int margin_caps_show(struct seq_file *s, void *v)
{
	struct pci_margin_dev *mdev = s->private;
	struct pci_dev *dev = mdev->dev;
	u16 cap;
	int ret;

	/*
	 * Wake the hardware and hold the PM reference before accessing
	 * registers
	 */
	ret = pm_runtime_resume_and_get(&dev->dev);
	if (ret < 0)
		return ret;

	ret = pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_CAP, &cap);
	pm_runtime_put_sync(&dev->dev);

	if (ret != PCIBIOS_SUCCESSFUL)
		return pcibios_err_to_errno(ret);
	if (PCI_POSSIBLE_ERROR(cap))
		return -ENODEV;

	seq_printf(s, "Port Capabilities: %#06x\n", cap);
	seq_printf(s, "  Uses SW Ready: %s\n",
		   str_yes_no(cap & PCI_LMR_PORT_CAP_USES_SW_READY));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(margin_caps);

static int margin_port_status_show(struct seq_file *s, void *v)
{
	struct pci_margin_dev *mdev = s->private;
	struct pci_dev *dev = mdev->dev;
	u16 sts;
	int ret;

	/*
	 * Wake the hardware and hold the PM reference before accessing
	 * registers
	 */
	ret = pm_runtime_resume_and_get(&dev->dev);
	if (ret < 0)
		return ret;

	ret = pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, &sts);
	pm_runtime_put_sync(&dev->dev);

	if (ret != PCIBIOS_SUCCESSFUL)
		return pcibios_err_to_errno(ret);
	if (PCI_POSSIBLE_ERROR(sts))
		return -ENODEV;

	seq_printf(s, "Port Status: %#06x\n", sts);
	seq_printf(s, "  Margining Ready: %s\n",
		   str_yes_no(sts & PCI_LMR_PORT_STS_MARGIN_READY));
	seq_printf(s, "  SW Ready: %s\n",
		   str_yes_no(sts & PCI_LMR_PORT_STS_SW_READY));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(margin_port_status);

static int margin_enable_show(struct seq_file *s, void *v)
{
	struct pci_margin_dev *mdev = s->private;

	if (pci_dev_is_disconnected(mdev->dev))
		return -ENODEV;

	guard(mutex)(&mdev->lock);
	seq_printf(s, "%d\n", mdev->enabled);
	return 0;
}

static void pci_lmr_disable_locked(struct pci_margin_dev *mdev)
{
	struct pci_dev *dev;
	int active_lanes, i, ret;
	u16 sts;

	if (!mdev)
		return;

	lockdep_assert_held(&mdev->lock);

	if (!mdev->enabled)
		return;

	dev = mdev->dev;
	active_lanes = pci_lmr_get_active_lanes(mdev);

	for (i = 0; i < active_lanes; i++)
		pci_lmr_clear_to_normal_lane(&mdev->lanes[i]);

	for (i = 0; i < mdev->num_lanes; i++) {
		int r;

		mdev->lanes[i].timing_val = 0;
		mdev->lanes[i].voltage_val = 0;
		for (r = 0; r <= LMR_MAX_RX_NUM; r++)
			mdev->lanes[i].rx_info[r].caps_cached = false;
	}

	ret = pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, &sts);
	if (ret == PCIBIOS_SUCCESSFUL && !PCI_POSSIBLE_ERROR(sts)) {
		sts &= ~PCI_LMR_PORT_STS_SW_READY;
		pci_write_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, sts);
	}

	if (mdev->aspm_inhibited) {
		pci_aspm_inhibit_locked(dev, false);
		mdev->aspm_inhibited = false;
	}

	pci_lmr_restore_autonomous(mdev);

	if (mdev->partner) {
		pm_runtime_put_sync(&mdev->partner->dev);
		pci_dev_put(mdev->partner);
		mdev->partner = NULL;
	}

	pm_runtime_put_sync(&dev->dev);
	mdev->enabled = false;
}

static int pci_lmr_enable_locked(struct pci_margin_dev *mdev,
				 struct pci_dev *downstream_port,
				 struct pci_dev *upstream_port)
{
	struct pci_dev *dev = mdev->dev;
	struct pci_dev *partner = NULL;
	unsigned long timeout;
	u16 sts, cap, lnksta;
	int active_lanes, ret, i;

	lockdep_assert_held(&mdev->lock);

	/*
	 * Ensure device is powered (D0) before reading configuration
	 * registers
	 */
	ret = pm_runtime_resume_and_get(&dev->dev);
	if (ret < 0)
		return ret;

	partner = (dev == downstream_port) ? upstream_port : downstream_port;

	/* Prevent concurrent LMR on both ends of the same link */
	if (partner && partner->lmr && partner->lmr->enabled) {
		ret = -EBUSY;
		goto err_rpm;
	}

	if (partner) {
		ret = pm_runtime_resume_and_get(&partner->dev);
		if (ret < 0)
			goto err_rpm;
		mdev->partner = pci_dev_get(partner);
	}

	/*
	 * PCIe Base Specification Revision 7.0 sec 8.4.4: LMR is physically
	 * undefined below 16.0 GT/s. Even if a device supports Gen4+, if the
	 * link is currently trained and operating at Gen1..Gen3 speeds
	 * (< 16.0 GT/s) in Link Status Register (sec 7.5.3.8, Current Link
	 * Speed), reject margining.
	 */
	ret = pcie_capability_read_word(dev, PCI_EXP_LNKSTA, &lnksta);
	if (ret) {
		ret = pcibios_err_to_errno(ret);
		goto err_partner_rpm;
	}
	if (PCI_POSSIBLE_ERROR(lnksta)) {
		ret = -ENODEV;
		goto err_partner_rpm;
	}
	if ((lnksta & PCI_EXP_LNKSTA_CLS) < PCI_EXP_LNKSTA_CLS_16_0GB) {
		ret = -EOPNOTSUPP;
		goto err_partner_rpm;
	}

	active_lanes = FIELD_GET(PCI_EXP_LNKSTA_NLW, lnksta);
	if (active_lanes == 0 || active_lanes > mdev->num_lanes)
		active_lanes = mdev->num_lanes;

	ret = pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_CAP, &cap);
	if (ret != PCIBIOS_SUCCESSFUL) {
		ret = pcibios_err_to_errno(ret);
		goto err_partner_rpm;
	}
	if (PCI_POSSIBLE_ERROR(cap)) {
		ret = -ENODEV;
		goto err_partner_rpm;
	}

	/* Disable Autonomous Width and Speed transitions */
	pci_lmr_disable_autonomous(mdev);

	/*
	 * Inhibit ASPM during margining via the ASPM driver API so the link
	 * remains continuously in L0. pci_bus_sem is held by caller.
	 */
	ret = pci_aspm_inhibit_locked(dev, true);
	if (ret && ret != -EPERM)
		goto err_autonomous;
	if (!ret)
		mdev->aspm_inhibited = true;

	if (cap & PCI_LMR_PORT_CAP_USES_SW_READY) {
		ret = pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, &sts);
		if (ret != PCIBIOS_SUCCESSFUL) {
			ret = pcibios_err_to_errno(ret);
			goto err_aspm;
		}
		sts |= PCI_LMR_PORT_STS_SW_READY;
		pci_write_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, sts);
	}

	timeout = jiffies + msecs_to_jiffies(LMR_ENABLE_TIMEOUT_MS);
	while (1) {
		ret = pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, &sts);
		if (ret != PCIBIOS_SUCCESSFUL) {
			ret = pcibios_err_to_errno(ret);
			goto err_sw_ready;
		}
		if (PCI_POSSIBLE_ERROR(sts)) {
			ret = -ENODEV;
			goto err_sw_ready;
		}
		if (sts & PCI_LMR_PORT_STS_MARGIN_READY)
			break;
		if (time_after(jiffies, timeout)) {
			ret = -ETIMEDOUT;
			goto err_sw_ready;
		}
		usleep_range(LMR_ENABLE_SLEEP_MIN_US, LMR_ENABLE_SLEEP_MAX_US);
	}

	/* Cache capabilities for configured receiver on all active lanes */
	for (i = 0; i < active_lanes; i++) {
		ret = pci_lmr_cache_rx_info(&mdev->lanes[i], mdev->lanes[i].rx);
		if (ret)
			goto err_sw_ready;
	}
	mdev->enabled = true;
	return 0;

err_sw_ready:
	if (cap & PCI_LMR_PORT_CAP_USES_SW_READY) {
		u16 clean_sts;
		int clean_ret;

		clean_ret = pci_read_config_word(
			dev, mdev->cap + PCI_LMR_PORT_STS, &clean_sts);
		if (clean_ret == PCIBIOS_SUCCESSFUL && !PCI_POSSIBLE_ERROR(clean_sts)) {
			clean_sts &= ~PCI_LMR_PORT_STS_SW_READY;
			pci_write_config_word(dev, mdev->cap + PCI_LMR_PORT_STS,
					      clean_sts);
		}
	}
err_aspm:
	if (mdev->aspm_inhibited) {
		pci_aspm_inhibit_locked(dev, false);
		mdev->aspm_inhibited = false;
	}
err_autonomous:
	pci_lmr_restore_autonomous(mdev);
err_partner_rpm:
	if (mdev->partner) {
		pm_runtime_put_sync(&mdev->partner->dev);
		pci_dev_put(mdev->partner);
		mdev->partner = NULL;
	}
err_rpm:
	pm_runtime_put_sync(&dev->dev);
	return ret;
}

static ssize_t margin_enable_write(struct file *file,
				   const char __user *user_buf, size_t count,
				   loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct pci_margin_dev *mdev = s->private;
	struct pci_dev *dev = mdev->dev;
	struct pci_dev *downstream_port = NULL, *upstream_port = NULL;
	bool enable;
	int ret;

	ret = kstrtobool_from_user(user_buf, count, &enable);
	if (ret)
		return ret;

	if (enable) {
		ret = pcie_get_link_endpoints(dev, &downstream_port, &upstream_port);
		if (ret)
			return ret;
	} else {
		struct pci_dev *partner;

		/*
		 * When disabling LMR, always operate on the link partner that
		 * was saved when LMR was enabled (mdev->partner). If a hot-swap
		 * occurred while LMR was active, pcie_get_link_endpoints()
		 * would resolve to the newly connected device, causing
		 * pci_lmr_disable_locked() to restore registers on the old
		 * partner without holding its device_lock.
		 */
		mutex_lock(&mdev->lock);
		if (!mdev->enabled) {
			mutex_unlock(&mdev->lock);
			return count;
		}
		partner = mdev->partner;
		if (partner)
			pci_dev_get(partner);
		mutex_unlock(&mdev->lock);

		if (pcie_downstream_port(dev)) {
			downstream_port = pci_dev_get(dev);
			upstream_port = partner;
		} else {
			downstream_port = partner;
			upstream_port = pci_dev_get(dev);
		}
	}

	/*
	 * Canonical PCI locking hierarchy:
	 *   pci_bus_sem -> Downstream Port (parent) -> Upstream Port (child) -> mdev->lock.
	 * Holding pci_bus_sem allows pci_aspm_inhibit_locked() to safely execute
	 * without lock inversion deadlocks.
	 */
	down_read(&pci_bus_sem);
	if (downstream_port)
		pci_dev_lock(downstream_port);
	if (upstream_port && upstream_port != downstream_port)
		pci_dev_lock(upstream_port);

	mutex_lock(&mdev->lock);

	if (mdev->enabled == enable) {
		ret = count;
	} else if (!enable) {
		pci_lmr_disable_locked(mdev);
		ret = count;
	} else {
		ret = pci_lmr_enable_locked(mdev, downstream_port, upstream_port);
		if (!ret)
			ret = count;
	}

	mutex_unlock(&mdev->lock);

	if (upstream_port && upstream_port != downstream_port)
		pci_dev_unlock(upstream_port);
	if (downstream_port)
		pci_dev_unlock(downstream_port);
	up_read(&pci_bus_sem);

	pcie_put_link_endpoints(downstream_port, upstream_port);

	return ret;
}

static int margin_enable_open(struct inode *inode, struct file *file)
{
	return single_open(file, margin_enable_show, inode->i_private);
}

static const struct file_operations margin_enable_fops = {
	.open = margin_enable_open,
	.read = seq_read,
	.write = margin_enable_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int margin_lane_receiver_show(struct seq_file *s, void *v)
{
	struct pci_margin_lane *plane = s->private;
	struct pci_margin_dev *mdev = plane->mdev;

	if (pci_dev_is_disconnected(mdev->dev))
		return -ENODEV;

	guard(mutex)(&mdev->lock);
	if (mdev->enabled && plane->lane >= pci_lmr_get_active_lanes(mdev))
		return -ENODEV;

	seq_printf(s, "%d\n", plane->rx);
	return 0;
}

static ssize_t margin_lane_receiver_write(struct file *file, const char __user *user_buf,
					  size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct pci_margin_lane *plane = s->private;
	struct pci_margin_dev *mdev = plane->mdev;
	int ret;
	u8 rx;

	ret = kstrtou8_from_user(user_buf, count, 0, &rx);
	if (ret)
		return ret;

	/*
	 * Valid receiver numbers are 0..6 per PCIe Base Specification
	 * Revision 7.0 sec 4.2.18.1 & Table 4-76; 7 is reserved.
	 */
	if (rx > LMR_MAX_RX_NUM)
		return -EINVAL;

	guard(mutex)(&mdev->lock);
	if (plane->rx == rx)
		return count;

	if (mdev->enabled) {
		/*
		 * Clear previous receiver to normal settings per
		 * single-receiver rule
		 */
		ret = pci_lmr_clear_to_normal_lane(plane);
		if (ret)
			return ret;
		ret = pci_lmr_cache_rx_info(plane, rx);
		if (ret)
			return ret;
	}

	plane->rx = rx;
	return count;
}

static int margin_lane_receiver_open(struct inode *inode, struct file *file)
{
	return single_open(file, margin_lane_receiver_show, inode->i_private);
}

static const struct file_operations margin_lane_receiver_fops = {
	.open = margin_lane_receiver_open,
	.read = seq_read,
	.write = margin_lane_receiver_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int margin_lane_caps_show(struct seq_file *s, void *v)
{
	struct pci_margin_lane *plane = s->private;
	struct pci_margin_dev *mdev = plane->mdev;
	struct pci_margin_rx_info *info;
	int ret;
	u8 val;

	if (pci_dev_is_disconnected(mdev->dev))
		return -ENODEV;

	guard(mutex)(&mdev->lock);
	if (!mdev->enabled)
		return -EBUSY;

	if (plane->lane >= pci_lmr_get_active_lanes(mdev))
		return -ENODEV;

	ret = pci_lmr_cache_rx_info(plane, plane->rx);
	if (ret)
		return ret;

	info = &plane->rx_info[plane->rx];
	val = info->caps;
	seq_printf(s, "Lane %d Rx %d Capabilities: %#02x\n", plane->lane, plane->rx, val);
	seq_printf(s, "  Voltage Supported: %s\n",
		   str_yes_no(val & LMR_CAP_VOLTAGE_SUPPORTED));
	seq_printf(s, "  Left/Right: %s\n",
		   (val & LMR_CAP_IND_LEFT_RIGHT_TIMING) ? "independent" : "symmetric");
	seq_printf(s, "  Up/Down: %s\n",
		   (val & LMR_CAP_IND_UP_DOWN_VOLTAGE) ? "independent" : "symmetric");
	seq_printf(s, "  Error Sampler: %s\n",
		   (val & LMR_CAP_IND_ERROR_SAMPLER) ? "independent" :
						       "main sampler");
	seq_printf(s, "  Sample Reporting: %s\n",
		   (val & LMR_CAP_SAMPLE_REPORT_METHOD) ? "rate" : "count");
	seq_printf(s, "  Sample Multiple Receivers: %s\n",
		   str_yes_no(val & LMR_CAP_SAMPLE_MULTIPLE_RECEIVERS));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(margin_lane_caps);

static int margin_lane_steps_show(struct seq_file *s, u8 type)
{
	struct pci_margin_lane *plane = s->private;
	struct pci_margin_dev *mdev = plane->mdev;
	struct pci_margin_rx_info *info;
	int ret;

	if (pci_dev_is_disconnected(mdev->dev))
		return -ENODEV;

	guard(mutex)(&mdev->lock);
	if (!mdev->enabled)
		return -EBUSY;

	if (plane->lane >= pci_lmr_get_active_lanes(mdev))
		return -ENODEV;

	ret = pci_lmr_cache_rx_info(plane, plane->rx);
	if (ret)
		return ret;

	info = &plane->rx_info[plane->rx];
	seq_printf(s, "%d\n", (type == LMR_TYPE_VOLTAGE) ?
		   info->num_voltage_steps : info->num_timing_steps);
	return 0;
}

static int margin_lane_timing_steps_show(struct seq_file *s, void *v)
{
	return margin_lane_steps_show(s, LMR_TYPE_TIMING);
}
DEFINE_SHOW_ATTRIBUTE(margin_lane_timing_steps);

static int margin_lane_voltage_steps_show(struct seq_file *s, void *v)
{
	return margin_lane_steps_show(s, LMR_TYPE_VOLTAGE);
}
DEFINE_SHOW_ATTRIBUTE(margin_lane_voltage_steps);

/*
 * pci_lmr_check_sample_multiple_rx() - Check multi-receiver concurrency.
 * Per PCIe Base Specification Revision 7.0 sec 4.2.18.2 & sec 8.4.4:
 * "For Receivers where MIndErrorSampler is 0b, at most one such Receiver is
 * permitted to be margined at a time. However, margining may be performed on
 * multiple Lanes simultaneously, as long as it is within the maximum number of
 * Lanes the device supports."
 *
 * In addition, Command 88h response payload Bit 5 (MSampleMultipleReceivers)
 * indicates whether multiple receivers can be sampled concurrently. Both
 * MIndErrorSampler (Bit 4) and MSampleMultipleReceivers (Bit 5) must be set on
 * the targeted receiver and all active receivers to allow concurrent margining
 * across different receivers.
 */
static bool pci_lmr_check_sample_multiple_rx(struct pci_margin_dev *mdev,
					     struct pci_margin_lane *plane)
{
	struct pci_margin_rx_info *info = &plane->rx_info[plane->rx];
	bool plane_concurrent;
	int i;

	plane_concurrent = (info->caps & LMR_CAP_IND_ERROR_SAMPLER) &&
			   (info->caps & LMR_CAP_SAMPLE_MULTIPLE_RECEIVERS);

	for (i = 0; i < mdev->num_lanes; i++) {
		struct pci_margin_lane *other = &mdev->lanes[i];
		struct pci_margin_rx_info *other_info;
		bool other_concurrent;

		if (i == plane->lane)
			continue;

		if (other->timing_val == 0 && other->voltage_val == 0)
			continue;

		if (other->rx == plane->rx)
			continue;

		other_info = &other->rx_info[other->rx];
		other_concurrent = (other_info->caps & LMR_CAP_IND_ERROR_SAMPLER) &&
				   (other_info->caps & LMR_CAP_SAMPLE_MULTIPLE_RECEIVERS);

		/*
		 * If either the targeted receiver or any currently active
		 * receiver lacks an independent error sampler (MIndErrorSampler
		 * == 0b) or does not support concurrent receiver margining
		 * (MSampleMultipleReceivers == 0b), disallow concurrent
		 * margining across different receivers.
		 */
		if (!plane_concurrent || !other_concurrent)
			return false;
	}
	return true;
}

static int pci_lmr_check_exec_status(struct pci_margin_lane *plane, u16 sts)
{
	u8 exec = FIELD_GET(LMR_STS_EXEC_MASK, pci_lmr_sts_payload(sts));

	if (exec == LMR_STS_EXEC_NAK)
		return -EOPNOTSUPP;
	if (exec == LMR_STS_EXEC_TOO_MANY_ERR) {
		plane->timing_val = 0;
		plane->voltage_val = 0;
		return -EIO;
	}
	return 0;
}

static int pci_lmr_issue_step(struct pci_margin_lane *plane, u8 type, int val)
{
	struct pci_margin_dev *mdev = plane->mdev;
	u8 step, dir, payload;
	u16 sts;
	int ret;

	if (type == LMR_TYPE_TIMING) {
		if (val < 0) {
			step = -val;
			dir = LMR_STEP_DIR_LEFT_OR_DOWN;
		} else {
			step = val;
			dir = LMR_STEP_DIR_RIGHT_OR_UP;
		}
		payload = FIELD_PREP(LMR_TIMING_DIR_MASK, dir) |
			  FIELD_PREP(LMR_TIMING_STEP_MASK, step);
		ret = pci_lmr_run_cmd(mdev, plane->lane, plane->rx,
				      LMR_TYPE_TIMING, 0, payload, &sts);
		if (ret)
			return ret;
		ret = pci_lmr_check_exec_status(plane, sts);
		if (ret)
			return ret;
		plane->timing_val = val;
	} else {
		if (val < 0) {
			step = -val;
			dir = LMR_STEP_DIR_LEFT_OR_DOWN;
		} else {
			step = val;
			dir = LMR_STEP_DIR_RIGHT_OR_UP;
		}
		payload = FIELD_PREP(LMR_VOLTAGE_DIR_MASK, dir) |
			  FIELD_PREP(LMR_VOLTAGE_STEP_MASK, step);
		ret = pci_lmr_run_cmd(mdev, plane->lane, plane->rx,
				      LMR_TYPE_VOLTAGE, 0, payload, &sts);
		if (ret)
			return ret;
		ret = pci_lmr_check_exec_status(plane, sts);
		if (ret)
			return ret;
		plane->voltage_val = val;
	}
	return 0;
}

static ssize_t margin_lane_step_write(struct file *file, const char __user *user_buf,
				      size_t count, u8 type)
{
	struct seq_file *s = file->private_data;
	struct pci_margin_lane *plane = s->private;
	struct pci_margin_dev *mdev = plane->mdev;
	struct pci_margin_rx_info *info;
	int max_step, val, ret;
	u8 step, caps;
	u16 sts;

	ret = kstrtoint_from_user(user_buf, count, 0, &val);
	if (ret)
		return ret;

	guard(mutex)(&mdev->lock);
	if (!mdev->enabled)
		return -EBUSY;

	/*
	 * Reject step operations if the target lane exceeds the currently
	 * negotiated/active link width (e.g. down-trained link) or if the
	 * link is down / device disconnected.
	 */
	ret = pcie_capability_read_word(mdev->dev, PCI_EXP_LNKSTA, &sts);
	if (ret != PCIBIOS_SUCCESSFUL)
		return pcibios_err_to_errno(ret);
	if (PCI_POSSIBLE_ERROR(sts))
		return -ENODEV;
	{
		u16 nlw = FIELD_GET(PCI_EXP_LNKSTA_NLW, sts);

		if (nlw == 0 || plane->lane >= nlw)
			return -ENODEV;
	}

	if (val == 0) {
		/*
		 * Per PCIe Base Specification Revision 7.0 Table 4-77, Step
		 * Margin commands with a 0 payload are NO-OPs in hardware.
		 * Returning an axis to nominal (0) requires issuing "Go to
		 * Normal Settings" (LMR_PAYLOAD_GO_TO_NORMAL / 0x0F). If the
		 * orthogonal axis is non-zero, clear both axes to normal first,
		 * then re-apply the orthogonal displacement to prevent hardware
		 * state drift.
		 */
		if (type == LMR_TYPE_TIMING) {
			int saved_voltage = plane->voltage_val;

			ret = pci_lmr_clear_to_normal_lane(plane);
			if (!ret && saved_voltage != 0)
				ret = pci_lmr_issue_step(plane, LMR_TYPE_VOLTAGE, saved_voltage);
		} else {
			int saved_timing = plane->timing_val;

			ret = pci_lmr_clear_to_normal_lane(plane);
			if (!ret && saved_timing != 0)
				ret = pci_lmr_issue_step(plane, LMR_TYPE_TIMING, saved_timing);
		}
		return ret ? ret : count;
	}

	ret = pci_lmr_cache_rx_info(plane, plane->rx);
	if (ret)
		return ret;

	if (!pci_lmr_check_sample_multiple_rx(mdev, plane))
		return -EBUSY;

	info = &plane->rx_info[plane->rx];
	caps = info->caps;

	switch (type) {
	case LMR_TYPE_TIMING:
		if (val < -LMR_MAX_TIMING_STEP || val > LMR_MAX_TIMING_STEP)
			return -EINVAL;
		if (val < 0) {
			if (!(caps & LMR_CAP_IND_LEFT_RIGHT_TIMING))
				return -EINVAL;
			step = -val;
		} else {
			step = val;
		}
		max_step = info->num_timing_steps;
		if (step > max_step)
			return -EINVAL;

		ret = pci_lmr_issue_step(plane, LMR_TYPE_TIMING, val);
		if (ret)
			return ret;
		break;

	case LMR_TYPE_VOLTAGE:
		if (!(caps & LMR_CAP_VOLTAGE_SUPPORTED))
			return -EOPNOTSUPP;
		if (val < -LMR_MAX_VOLTAGE_STEP || val > LMR_MAX_VOLTAGE_STEP)
			return -EINVAL;
		if (val < 0) {
			if (!(caps & LMR_CAP_IND_UP_DOWN_VOLTAGE))
				return -EINVAL;
			step = -val;
		} else {
			step = val;
		}
		max_step = info->num_voltage_steps;
		if (step > max_step)
			return -EINVAL;

		ret = pci_lmr_issue_step(plane, LMR_TYPE_VOLTAGE, val);
		if (ret)
			return ret;
		break;

	default:
		return -EINVAL;
	}

	return count;
}

static ssize_t margin_lane_timing_write(struct file *file, const char __user *user_buf,
					size_t count, loff_t *ppos)
{
	return margin_lane_step_write(file, user_buf, count, LMR_TYPE_TIMING);
}

static int margin_lane_step_show(struct seq_file *s, u8 type)
{
	struct pci_margin_lane *plane = s->private;
	struct pci_margin_dev *mdev = plane->mdev;

	if (pci_dev_is_disconnected(mdev->dev))
		return -ENODEV;

	guard(mutex)(&plane->mdev->lock);
	if (mdev->enabled && plane->lane >= pci_lmr_get_active_lanes(mdev))
		return -ENODEV;

	seq_printf(s, "%d\n", (type == LMR_TYPE_VOLTAGE) ?
		   plane->voltage_val : plane->timing_val);
	return 0;
}

static int margin_lane_timing_show(struct seq_file *s, void *v)
{
	return margin_lane_step_show(s, LMR_TYPE_TIMING);
}

static int margin_lane_timing_open(struct inode *inode, struct file *file)
{
	return single_open(file, margin_lane_timing_show, inode->i_private);
}

static const struct file_operations margin_lane_timing_fops = {
	.open = margin_lane_timing_open,
	.read = seq_read,
	.write = margin_lane_timing_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static ssize_t margin_lane_voltage_write(struct file *file, const char __user *user_buf,
					 size_t count, loff_t *ppos)
{
	return margin_lane_step_write(file, user_buf, count, LMR_TYPE_VOLTAGE);
}

static int margin_lane_voltage_show(struct seq_file *s, void *v)
{
	return margin_lane_step_show(s, LMR_TYPE_VOLTAGE);
}

static int margin_lane_voltage_open(struct inode *inode, struct file *file)
{
	return single_open(file, margin_lane_voltage_show, inode->i_private);
}

static const struct file_operations margin_lane_voltage_fops = {
	.open = margin_lane_voltage_open,
	.read = seq_read,
	.write = margin_lane_voltage_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static void pci_margin_debugfs_init(struct pci_margin_dev *mdev)
{
	struct pci_dev *dev = mdev->dev;
	struct dentry *parent;
	char dirname[64];
	int i;

	parent = get_pci_debugfs_root();
	scnprintf(dirname, sizeof(dirname), "pcie_lmr_%s", dev_name(&dev->dev));
	mdev->debugfs = debugfs_create_dir(dirname, parent);

	debugfs_create_file("capabilities", 0444, mdev->debugfs, mdev, &margin_caps_fops);
	debugfs_create_file("port_status", 0444, mdev->debugfs, mdev, &margin_port_status_fops);
	debugfs_create_file("enable", 0644, mdev->debugfs, mdev, &margin_enable_fops);

	for (i = 0; i < mdev->num_lanes; i++) {
		struct pci_margin_lane *plane = &mdev->lanes[i];
		struct dentry *lane_dir;
		char lane_name[16];

		scnprintf(lane_name, sizeof(lane_name), "lane%d", i);
		lane_dir = debugfs_create_dir(lane_name, mdev->debugfs);

		debugfs_create_file("receiver", 0644, lane_dir, plane, &margin_lane_receiver_fops);
		debugfs_create_file("caps", 0444, lane_dir, plane, &margin_lane_caps_fops);
		debugfs_create_file("num_timing_steps", 0444, lane_dir, plane,
				    &margin_lane_timing_steps_fops);
		debugfs_create_file("num_voltage_steps", 0444, lane_dir, plane,
				    &margin_lane_voltage_steps_fops);
		debugfs_create_file("margin_timing", 0644, lane_dir, plane,
				    &margin_lane_timing_fops);
		debugfs_create_file("margin_voltage", 0644, lane_dir, plane,
				    &margin_lane_voltage_fops);
	}
}

static void pci_margin_debugfs_remove(struct pci_margin_dev *mdev)
{
	debugfs_remove_recursive(mdev->debugfs);
}

#else
static inline void pci_margin_debugfs_init(struct pci_margin_dev *mdev) { }
static inline void pci_margin_debugfs_remove(struct pci_margin_dev *mdev) { }
#endif

void pci_lmr_init(struct pci_dev *dev)
{
	struct pci_margin_dev *mdev;
	enum pci_bus_speed speed;
	u32 lnkcap;
	u16 lmr;
	int num_lanes, ret, i;

	if (WARN_ON_ONCE(!dev) || !pci_is_pcie(dev))
		return;

	/*
	 * Per PCIe Base Specification Revision 7.0 sec 7.7.11:
	 * Lane Margining at Receiver is physically undefined and not applicable
	 * to Root Complex Integrated Endpoints, Root Complex Event Collectors,
	 * or PCIe-to-PCI/PCI-X Bridges.
	 */
	if (pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END ||
	    pci_pcie_type(dev) == PCI_EXP_TYPE_RC_EC ||
	    pci_pcie_type(dev) == PCI_EXP_TYPE_PCI_BRIDGE)
		return;

	/*
	 * Per PCIe Base Specification Revision 7.0 sec 7.7.11:
	 * For devices associated with an Upstream Port (Endpoints,
	 * Legacy Endpoints, and Switch Upstream Ports), the Lane Margining
	 * Extended Capability must be implemented in Function 0 (and only
	 * Function 0).
	 */
	if ((pci_pcie_type(dev) == PCI_EXP_TYPE_ENDPOINT ||
	     pci_pcie_type(dev) == PCI_EXP_TYPE_LEG_END ||
	     pci_pcie_type(dev) == PCI_EXP_TYPE_UPSTREAM) &&
	    PCI_FUNC(dev->devfn) != 0)
		return;

	speed = pcie_get_speed_cap(dev);
	if (speed < PCIE_SPEED_16_0GT || speed == PCI_SPEED_UNKNOWN)
		return;

	lmr = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_LMR);
	if (!lmr) {
		if (speed >= PCIE_SPEED_64_0GT)
			pci_warn(dev,
				 "Missing Lane Margining at Receiver Capability (mandatory for Gen6+)\n");
		else
			pci_dbg(dev,
				"Optional Lane Margining at Receiver Capability not found\n");
		return;
	}

	/*
	 * Determine link width: read Maximum Link Width (MLW) from Link
	 * Capabilities. Sizing data structures and debugfs interfaces to MLW
	 * ensures all lanes can be margined if the link up-trains dynamically.
	 * Dynamic Negotiated Link Width (NLW) is queried at runtime via
	 * pci_lmr_get_active_lanes().
	 */
	ret = pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &lnkcap);
	if (ret != PCIBIOS_SUCCESSFUL || PCI_POSSIBLE_ERROR(lnkcap))
		return;
	num_lanes = FIELD_GET(PCI_EXP_LNKCAP_MLW, lnkcap);
	if (num_lanes == 0 || num_lanes > LMR_MAX_LANES) {
		pci_warn(dev, "Invalid link width %d for LMR\n", num_lanes);
		return;
	}

	dev->lmr_cap = lmr;

	mdev = kzalloc(struct_size(mdev, lanes, num_lanes), GFP_KERNEL);
	if (!mdev)
		return;

	mdev->num_lanes = num_lanes;
	mdev->dev = dev;
	mdev->cap = lmr;
	mutex_init(&mdev->lock);

	for (i = 0; i < num_lanes; i++) {
		mdev->lanes[i].mdev = mdev;
		mdev->lanes[i].lane = i;
		mdev->lanes[i].rx = LMR_RX_LOCAL;
	}

	dev->lmr = mdev;
	pci_margin_debugfs_init(mdev);

	pci_dbg(dev, "Lane Margining at Receiver (Gen%u) Capability detected\n",
		LMR_SPEED_TO_GEN(speed));
}

void pci_lmr_exit(struct pci_dev *dev)
{
	struct pci_margin_dev *mdev;
	struct pci_dev *partner;
	struct pci_dev *downstream_port, *upstream_port;

	if (!dev || !dev->lmr)
		return;

	mdev = dev->lmr;

	/*
	 * 1. Tear down user-facing debugfs files FIRST to prevent concurrent
	 *    access. debugfs_remove_recursive() flushes active file operations.
	 */
	pci_margin_debugfs_remove(mdev);

	/*
	 * 2. Acquire locks in canonical PCI hierarchy:
	 *    pci_bus_sem -> Downstream Port (parent) -> Upstream Port (child) -> mdev->lock.
	 *
	 * Both ends of the link must hold their device_lock during teardown
	 * because pci_lmr_disable_locked() restores autonomous width and speed
	 * registers on both ports.
	 */
	mutex_lock(&mdev->lock);
	partner = mdev->partner;
	if (partner)
		pci_dev_get(partner);
	mutex_unlock(&mdev->lock);

	if (pcie_downstream_port(dev)) {
		downstream_port = dev;
		upstream_port = partner;
	} else {
		downstream_port = partner;
		upstream_port = dev;
	}

	down_read(&pci_bus_sem);
	if (downstream_port)
		pci_dev_lock(downstream_port);
	if (upstream_port && upstream_port != downstream_port)
		pci_dev_lock(upstream_port);

	mutex_lock(&mdev->lock);
	dev->lmr = NULL;
	if (mdev->enabled)
		pci_lmr_disable_locked(mdev);
	mutex_unlock(&mdev->lock);

	if (upstream_port && upstream_port != downstream_port)
		pci_dev_unlock(upstream_port);
	if (downstream_port)
		pci_dev_unlock(downstream_port);
	up_read(&pci_bus_sem);

	pci_dev_put(partner);

	/* 3. Safe to destroy structures */
	mutex_destroy(&mdev->lock);
	kfree(mdev);
}
