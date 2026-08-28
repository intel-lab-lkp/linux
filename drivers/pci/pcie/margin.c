// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Express Lane Margining at Receiver
 *
 * Copyright (C) 2026 Google LLC
 * Author: Priyank Rathod <rathodpriyank@google.com>
 *
 * Lane Margining at Receiver (PCIe Base Specification Revision 7.0, sec 7.7.11 &
 * sec 8.4.4; r6.0 sec 7.7.10 & sec 8.4.4) allows system software to determine
 * the voltage and timing margins of each physical lane on a PCIe link. The
 * Extended Capability (ID 0x27) is available for receivers operating at 16.0 GT/s
 * (Gen4) or higher data rates, and is mandatory for receivers operating at 64.0 GT/s
 * (Gen6) or higher data rates.
 *
 * This driver implements:
 *   - Probing Extended Capability ID 0x27 and Margining Port Capabilities.
 *   - Managing ASPM L0s/L1 link states during active margining with restoration.
 *   - PCIe Base Specification NO_CMD (0x7) clearing handshake per receiver and lane.
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

#include "../pci.h"

/*
 * Margining Type (MTYPE) field encodings (bits 5:3) in Margining Lane Control
 * and Margining Lane Status registers per PCIe Base Specification Revision 7.0:
 * - Section 7.7.11 "Lane Margining at the Receiver Extended Capability (ID 0x27)"
 *   (Margining Lane Control Register & Margining Lane Status Register)
 *   [r6.0 Section 7.7.10]
 * - Section 4.2.18.2 "Margin Command and Response Flow"
 *   (Table 4-77 "Margin Commands and Corresponding Responses")
 *   [r6.0 Table 4-73]
 *
 * Encodings:
 *   001b (0x1) - Report Margin Control Capabilities
 *   010b (0x2) - Set Margining Parameters (Go to Normal Settings, Clear Error Log)
 *   011b (0x3) - Step Margin Timing
 *   100b (0x4) - Step Margin Voltage
 *   111b (0x7) - No Command
 *   (000b, 101b-110b are Reserved)
 */
#define LMR_TYPE_REPORT_CAPS 0x1 /* Report Margin Control Capabilities */
#define LMR_TYPE_SET_PARAMS 0x2 /* Set Margining Parameters */
#define LMR_TYPE_TIMING 0x3 /* Step Margin Timing */
#define LMR_TYPE_VOLTAGE 0x4 /* Step Margin Voltage */
#define LMR_TYPE_NO_CMD 0x7 /* No Command */

/* Command Payloads per PCIe Base Specification Revision 7.0 Table 4-77 (r6.0 Table 4-73) */
#define LMR_PAYLOAD_REPORT_CAPS 0x88 /* Report Margin Control Capabilities */
#define LMR_PAYLOAD_REPORT_VOLT_STEPS 0x89 /* Report Margining Voltage Steps */
#define LMR_PAYLOAD_REPORT_TIM_STEPS 0x8A /* Report Margining Timing Steps */
#define LMR_PAYLOAD_GO_TO_NORMAL 0x0F /* Go to Normal Settings */
#define LMR_PAYLOAD_CLEAR_ERROR_LOG 0x55 /* Clear Error Log */
#define LMR_PAYLOAD_NO_CMD 0x9C /* No Command */

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
 * - Receiver numbers 0..6: Table 4-76 (assignment) & Table 4-77 (valid for commands)
 * - Max timing step (63): sec 4.2.18.1.2, Table 4-77 (8Ah), & Table 8-13
 * - Max voltage step (127): sec 4.2.18.1.2, Table 4-77 (89h), & Table 8-13
 */
#define LMR_MAX_LANES                   32
#define LMR_MAX_RX_NUM                  6
#define LMR_MAX_TIMING_STEP             63
#define LMR_MAX_VOLTAGE_STEP            127

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

/* LMR receivers and directions */
#define LMR_RX_LOCAL                    0
#define LMR_STEP_DIR_INCREASE           1
#define LMR_STEP_DIR_DECREASE           0

/*
 * Margining Payload field masks for Step Margin Timing and Step Margin Voltage
 * per PCIe Base Specification Revision 7.0 sec 4.2.18.1.2
 * ("Margin Payload for Step Margin Commands"):
 *
 * Step Margin Timing Payload:
 *   Bit 7:    Reserved (must be 0b)
 *   Bit 6:    Direction (0 = Left/Decrease, 1 = Right/Increase)
 *   Bits 5:0: Margin Step (0..63)
 *
 * Step Margin Voltage Payload:
 *   Bit 7:    Direction (0 = Down/Decrease, 1 = Up/Increase)
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
 *   Bit 6: 0b = Right of normal setting (also 0b Reserved for symmetric)
 *          1b = Left of normal setting (when MIndLeftRightTiming is Set)
 * For voltage:
 *   Bit 7: 0b = Up from normal setting (also 0b Reserved for symmetric)
 *          1b = Down from normal setting (when MIndUpDownVoltage is Set)
 */
#define LMR_STEP_DIR_RIGHT_OR_UP	0
#define LMR_STEP_DIR_LEFT_OR_DOWN	1

/*
 * Report Margin Control Capabilities (Command 88h) response payload bit fields
 * per PCIe Base Specification Revision 7.0 Table 4-77 & Table 8-13 (r6.0 Table 4-73 & Table 8-11):
 *   Bit 0:   MVoltageSupported (1 = Voltage margining supported; 0 = Not supported)
 *   Bit 1:   MIndUpDownVoltage (1 = Independent Up/Down voltage supported; 0 = Symmetric)
 *   Bit 2:   MIndLeftRightTiming (1 = Independent Left/Right timing supported; 0 = Symmetric)
 *   Bit 3:   MSampleReportingMethod (1 = Sampling rate supported; 0 = Sample count supported)
 *   Bit 4:   MIndErrorSampler (1 = Independent error sampler; 0 = Main data sampler)
 *   Bits 7:5: Reserved
 */
#define LMR_CAP_VOLTAGE_SUPPORTED BIT(0)
#define LMR_CAP_IND_UP_DOWN_VOLTAGE BIT(1)
#define LMR_CAP_IND_LEFT_RIGHT_TIMING	BIT(2)
#define LMR_CAP_SAMPLE_REPORT_METHOD BIT(3)
#define LMR_CAP_IND_ERROR_SAMPLER BIT(4)

/*
 * Step Margin Execution Status (Bits 7:6 of response payload per PCIe Base
 * Specification Revision 7.0 sec 4.2.18.1.1 "Step Margin Execution Status"):
 * 00b: Too many errors - Receiver autonomously went back to default settings
 * 01b: Set up for margin in progress
 * 10b: Margining in progress
 * 11b: NAK - Unsupported Lane Margining command was issued
 */
#define LMR_STS_EXEC_MASK GENMASK(7, 6)
#define LMR_STS_EXEC_TOO_MANY_ERR 0x0
#define LMR_STS_EXEC_SETUP_IN_PROGRESS 0x1
#define LMR_STS_EXEC_IN_PROGRESS 0x2
#define LMR_STS_EXEC_NAK 0x3
#define LMR_STS_ERR_CNT_MASK GENMASK(5, 0)

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
 *        target receiver selection, lane margining steps, and ASPM state
 * @enabled: True if Lane Margining is currently enabled
 * @aspm_saved: True if original ASPM configuration has been saved
 * @saved_dsp_aspm: Saved ASPM control register bits for Downstream Port
 * @saved_usp_aspm: Saved ASPM control register bits for Upstream Port
 * @autonomous_saved: True if original autonomous width/speed configuration has been saved
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
	bool aspm_saved;
	u16 saved_dsp_aspm;
	u16 saved_usp_aspm;
	bool autonomous_saved;
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
 * pci_lmr_get_link_partners() - Identify Downstream and Upstream Port link partners.
 *
 * For Root Ports and Switch Downstream Ports, @dev is the Downstream Port, and the
 * connected device on the secondary bus is the Upstream Port.
 * For Endpoints and Switch Upstream Ports, @dev is the Upstream Port, and the
 * upstream bridge is the Downstream Port.
 */
static void pci_lmr_get_link_partners(struct pci_dev *dev,
				      struct pci_dev **downstream_port,
				      struct pci_dev **upstream_port)
{
	if (pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT ||
	    pci_pcie_type(dev) == PCI_EXP_TYPE_DOWNSTREAM) {
		*downstream_port = dev;
		down_read(&pci_bus_sem);
		*upstream_port = dev->subordinate ?
			pci_dev_get(list_first_entry_or_null(&dev->subordinate->devices,
							     struct pci_dev, bus_list)) : NULL;
		up_read(&pci_bus_sem);
	} else {
		*downstream_port = pci_upstream_bridge(dev);
		*upstream_port = dev;
	}
}

static void pci_lmr_put_link_partners(struct pci_dev *dev,
				      struct pci_dev *downstream_port,
				      struct pci_dev *upstream_port)
{
	if (pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT ||
	    pci_pcie_type(dev) == PCI_EXP_TYPE_DOWNSTREAM) {
		if (upstream_port)
			pci_dev_put(upstream_port);
	}
}

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

	if (pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT ||
	    pci_pcie_type(dev) == PCI_EXP_TYPE_DOWNSTREAM) {
		*downstream_port = dev;
		*upstream_port = partner;
	} else {
		*downstream_port = partner;
		*upstream_port = dev;
	}
}

/*
 * pci_lmr_aspm_inhibit() - Inhibit or restore ASPM L0s/L1 during active margining.
 * PCIe Base Specification Revision 7.0 sec 7.5.3.7 ("Link Control Register"):
 * - To disable/inhibit ASPM, software on Downstream Component (Endpoint / Upstream Port)
 *   must disable ASPM prior to disabling ASPM on Upstream Component (Root Port / Downstream Port).
 * - To enable/restore ASPM, software on Upstream Component (Root Port / Downstream Port)
 *   must enable ASPM prior to enabling ASPM on Downstream Component (Endpoint / Upstream Port).
 */
static void pci_lmr_aspm_inhibit(struct pci_margin_dev *mdev, bool inhibit)
{
	struct pci_dev *downstream_port, *upstream_port;
	struct pci_dev *partner = mdev->partner;
	u16 ctl;

	pci_lmr_get_ports(mdev, &downstream_port, &upstream_port);

	if (inhibit) {
		if (mdev->aspm_saved)
			return;

		/*
		 * If link partner already saved ASPM state, inherit it to
		 * prevent overwriting with 0.
		 */
		if (partner && partner->lmr && partner->lmr->aspm_saved) {
			mdev->saved_dsp_aspm = partner->lmr->saved_dsp_aspm;
			mdev->saved_usp_aspm = partner->lmr->saved_usp_aspm;
			mdev->aspm_saved = true;
			return;
		}

		/*
		 * 1. Downstream Component (upstream_port) must be disabled
		 * FIRST per sec 7.5.3.7.
		 */
		if (upstream_port && pci_is_pcie(upstream_port) &&
		    upstream_port->current_state == PCI_D0) {
			if (!pcie_capability_read_word(upstream_port, PCI_EXP_LNKCTL, &ctl)) {
				mdev->saved_usp_aspm = ctl & PCI_EXP_LNKCTL_ASPMC;
				pcie_capability_clear_word(upstream_port, PCI_EXP_LNKCTL,
							   PCI_EXP_LNKCTL_ASPMC);
			}
		}

		/*
		 * 2. Upstream Component (downstream_port) must be disabled
		 * SECOND per sec 7.5.3.7.
		 */
		if (downstream_port && pci_is_pcie(downstream_port) &&
		    downstream_port->current_state == PCI_D0) {
			if (!pcie_capability_read_word(downstream_port, PCI_EXP_LNKCTL, &ctl)) {
				mdev->saved_dsp_aspm = ctl & PCI_EXP_LNKCTL_ASPMC;
				pcie_capability_clear_word(downstream_port, PCI_EXP_LNKCTL,
							   PCI_EXP_LNKCTL_ASPMC);
			}
		}

		mdev->aspm_saved = true;

		/*
		 * Ensure link is settled in L0 mode per PCIe Base
		 * Specification Revision 7.0 sec 8.4.4.
		 */
		usleep_range(2000, 3000);
	} else {
		if (!mdev->aspm_saved)
			return;

		/*
		 * 1. Upstream Component (downstream_port) MUST be restored
		 * FIRST per sec 7.5.3.7.
		 */
		if (downstream_port && pci_is_pcie(downstream_port) &&
		    downstream_port->current_state == PCI_D0) {
			pcie_capability_clear_and_set_word(
				downstream_port, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_ASPMC,
				mdev->saved_dsp_aspm);
		}

		/*
		 * 2. Downstream Component (upstream_port) MUST be restored
		 * SECOND per sec 7.5.3.7.
		 */
		if (upstream_port && pci_is_pcie(upstream_port) &&
		    upstream_port->current_state == PCI_D0) {
			pcie_capability_clear_and_set_word(
				upstream_port, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_ASPMC,
				mdev->saved_usp_aspm);
		}

		mdev->aspm_saved = false;
	}
}

/*
 * pci_lmr_ensure_aspm_inhibited() - Verify and re-enforce ASPM inhibit state.
 *
 * Checks both Downstream Port and Upstream Port to guarantee that out-of-band
 * OS events (e.g. background power transitions or sysfs modifications) have
 * not unexpectedly re-enabled ASPM on either component. If ASPM was re-enabled,
 * re-inhibits it in the spec-mandated order and waits for the link to settle
 * in L0 before physical lane margin steps are executed.
 */
static void pci_lmr_ensure_aspm_inhibited(struct pci_margin_dev *mdev)
{
	struct pci_dev *downstream_port, *upstream_port;
	u16 dsp_ctl = 0, usp_ctl = 0;
	bool re_inhibit = false;

	pci_lmr_get_ports(mdev, &downstream_port, &upstream_port);

	if (upstream_port && pci_is_pcie(upstream_port)) {
		if (!pcie_capability_read_word(upstream_port, PCI_EXP_LNKCTL, &usp_ctl) &&
		    (usp_ctl & PCI_EXP_LNKCTL_ASPMC))
			re_inhibit = true;
	}

	if (downstream_port && pci_is_pcie(downstream_port)) {
		if (!pcie_capability_read_word(downstream_port, PCI_EXP_LNKCTL, &dsp_ctl) &&
		    (dsp_ctl & PCI_EXP_LNKCTL_ASPMC))
			re_inhibit = true;
	}

	if (re_inhibit) {
		pci_info_ratelimited(mdev->dev,
				     "ASPM re-enabled unexpectedly; re-enforcing ASPM inhibit for LMR\n");
		/* Disable Downstream Component first, Upstream Component second per sec 7.5.3.7 */
		if (upstream_port && pci_is_pcie(upstream_port))
			pcie_capability_clear_word(upstream_port, PCI_EXP_LNKCTL,
						   PCI_EXP_LNKCTL_ASPMC);
		if (downstream_port && pci_is_pcie(downstream_port))
			pcie_capability_clear_word(downstream_port, PCI_EXP_LNKCTL,
						   PCI_EXP_LNKCTL_ASPMC);
		/* Ensure link returns to and settles in L0 mode before proceeding */
		usleep_range(2000, 3000);
	}
}

/*
 * Helpers to manage Autonomous Width/Speed transitions per PCIe Base Specification Revision 7.0:
 * - Section 7.5.3.7 "Link Control Register" (Hardware Autonomous Width Disable, bit 9)
 * - Section 7.5.3.17 "Link Control 2 Register" (Hardware Autonomous Speed Disable, bit 5)
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
	struct pci_dev *partner = mdev->partner;
	u16 lnkctl, lnkctl2;

	if (mdev->autonomous_saved)
		return;

	/* If link partner already saved autonomous settings, inherit them */
	if (partner && partner->lmr && partner->lmr->autonomous_saved) {
		mdev->saved_dsp_lnkctl = partner->lmr->saved_dsp_lnkctl;
		mdev->saved_dsp_lnkctl2 = partner->lmr->saved_dsp_lnkctl2;
		mdev->saved_usp_lnkctl = partner->lmr->saved_usp_lnkctl;
		mdev->saved_usp_lnkctl2 = partner->lmr->saved_usp_lnkctl2;
		mdev->autonomous_saved = true;
		return;
	}

	pci_lmr_get_ports(mdev, &downstream_port, &upstream_port);

	/* 1. Downstream Component (upstream_port): Save and Disable FIRST */
	if (upstream_port && pci_is_pcie(upstream_port) &&
	    upstream_port->current_state == PCI_D0) {
		if (!pcie_capability_read_word(upstream_port, PCI_EXP_LNKCTL, &lnkctl)) {
			mdev->saved_usp_lnkctl = lnkctl;
			pcie_capability_set_word(upstream_port, PCI_EXP_LNKCTL,
						 PCI_EXP_LNKCTL_HAWD);
		}

		if (!pcie_capability_read_word(upstream_port, PCI_EXP_LNKCTL2, &lnkctl2)) {
			mdev->saved_usp_lnkctl2 = lnkctl2;
			pcie_capability_set_word(upstream_port, PCI_EXP_LNKCTL2,
						 PCI_EXP_LNKCTL2_HASD);
		}
	}

	/* 2. Upstream Component (downstream_port): Save and Disable SECOND */
	if (downstream_port && pci_is_pcie(downstream_port) &&
	    downstream_port->current_state == PCI_D0) {
		if (!pcie_capability_read_word(downstream_port, PCI_EXP_LNKCTL, &lnkctl)) {
			mdev->saved_dsp_lnkctl = lnkctl;
			pcie_capability_set_word(downstream_port, PCI_EXP_LNKCTL,
						 PCI_EXP_LNKCTL_HAWD);
		}

		if (!pcie_capability_read_word(downstream_port, PCI_EXP_LNKCTL2, &lnkctl2)) {
			mdev->saved_dsp_lnkctl2 = lnkctl2;
			pcie_capability_set_word(downstream_port, PCI_EXP_LNKCTL2,
						 PCI_EXP_LNKCTL2_HASD);
		}
	}

	mdev->autonomous_saved = true;
}

static void pci_lmr_restore_autonomous(struct pci_margin_dev *mdev)
{
	struct pci_dev *downstream_port, *upstream_port;

	if (!mdev->autonomous_saved)
		return;

	pci_lmr_get_ports(mdev, &downstream_port, &upstream_port);

	/* 1. Upstream Component (downstream_port) restored FIRST */
	if (downstream_port && pci_is_pcie(downstream_port) &&
	    downstream_port->current_state == PCI_D0) {
		pcie_capability_clear_and_set_word(
			downstream_port, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_HAWD,
			mdev->saved_dsp_lnkctl & PCI_EXP_LNKCTL_HAWD);
		pcie_capability_clear_and_set_word(
			downstream_port, PCI_EXP_LNKCTL2, PCI_EXP_LNKCTL2_HASD,
			mdev->saved_dsp_lnkctl2 & PCI_EXP_LNKCTL2_HASD);
	}

	/* 2. Downstream Component (upstream_port) restored SECOND */
	if (upstream_port && pci_is_pcie(upstream_port) &&
	    upstream_port->current_state == PCI_D0) {
		pcie_capability_clear_and_set_word(
			upstream_port, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_HAWD,
			mdev->saved_usp_lnkctl & PCI_EXP_LNKCTL_HAWD);
		pcie_capability_clear_and_set_word(
			upstream_port, PCI_EXP_LNKCTL2, PCI_EXP_LNKCTL2_HASD,
			mdev->saved_usp_lnkctl2 & PCI_EXP_LNKCTL2_HASD);
	}

	mdev->autonomous_saved = false;
}

static inline u8 pci_lmr_sts_payload(u16 sts)
{
	return FIELD_GET(PCI_LMR_LANE_STS_PAYLOAD, sts);
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

	dev = mdev->dev;
	lmr = mdev->cap;
	ctrl_offset = lmr + PCI_LMR_LANE_CTRL + LMR_LANE_REG_STRIDE * lane;
	sts_offset = lmr + PCI_LMR_LANE_STS + LMR_LANE_REG_STRIDE * lane;

	/*
	 * Per PCIe Base Specification Revision 7.0 sec 4.2.18.2 & Table 4-77
	 * (r6.0 Table 4-73), software must issue NO_CMD (0x7) with payload
	 * 0x9C targeting the specific receiver (rx) to clear MTYPE in Lane
	 * Status before issuing a subsequent command.
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
			 * & Table 4-77 (r6.0 Table 4-73), if receiver echoes
			 * NO_CMD (0x7) after command issuance, it indicates NAK.
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
 * per PCIe Base Specification Revision 7.0 sec 4.2.18.2 & Table 4-77 (r6.0 Table 4-73).
 * Issues Set Margining Parameters (MTYPE 010b) with "Go to Normal Settings" (Payload 0x0F).
 */
static int pci_lmr_clear_to_normal_lane(struct pci_margin_lane *plane)
{
	u16 sts;
	int ret;

	if (!plane || !plane->mdev)
		return -EINVAL;

	ret = pci_lmr_run_cmd(plane->mdev, plane->lane, plane->rx,
			      LMR_TYPE_SET_PARAMS, 0, LMR_PAYLOAD_GO_TO_NORMAL,
			      &sts);
	if (ret)
		return ret;

	plane->timing_val = 0;
	plane->voltage_val = 0;
	return 0;
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

	/* Report Voltage Steps: MTYPE 001b, Payload 0x89 */
	ret = pci_lmr_run_cmd(plane->mdev, plane->lane, rx,
			      LMR_TYPE_REPORT_CAPS, 0,
			      LMR_PAYLOAD_REPORT_VOLT_STEPS, &sts);
	if (ret)
		return ret;
	info->num_voltage_steps = FIELD_GET(LMR_VOLTAGE_STEP_MASK, pci_lmr_sts_payload(sts));

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

	/* Wake the hardware and hold the PM reference before accessing registers */
	ret = pm_runtime_resume_and_get(&dev->dev);
	if (ret < 0)
		return ret;

	ret = pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_CAP, &cap);
	pm_runtime_put_sync(&dev->dev);

	if (ret != PCIBIOS_SUCCESSFUL)
		return pcibios_err_to_errno(ret);

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

	/* Wake the hardware and hold the PM reference before accessing registers */
	ret = pm_runtime_resume_and_get(&dev->dev);
	if (ret < 0)
		return ret;

	ret = pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, &sts);
	pm_runtime_put_sync(&dev->dev);

	if (ret != PCIBIOS_SUCCESSFUL)
		return pcibios_err_to_errno(ret);

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

	guard(mutex)(&mdev->lock);
	seq_printf(s, "%d\n", mdev->enabled);
	return 0;
}

static void pci_lmr_disable_locked(struct pci_margin_dev *mdev)
{
	struct pci_dev *dev;
	int i, ret;
	u16 sts;

	if (!mdev)
		return;

	lockdep_assert_held(&mdev->lock);

	if (!mdev->enabled)
		return;

	dev = mdev->dev;

	for (i = 0; i < mdev->num_lanes; i++)
		pci_lmr_clear_to_normal_lane(&mdev->lanes[i]);

	ret = pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, &sts);
	if (ret == PCIBIOS_SUCCESSFUL) {
		sts &= ~PCI_LMR_PORT_STS_SW_READY;
		pci_write_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, sts);
	}
	pci_lmr_aspm_inhibit(mdev, false);
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
	int ret, i;

	lockdep_assert_held(&mdev->lock);

	/* Ensure device is powered (D0) before reading configuration registers */
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
	pcie_capability_read_word(dev, PCI_EXP_LNKSTA, &lnksta);
	if ((lnksta & PCI_EXP_LNKSTA_CLS) < PCI_EXP_LNKSTA_CLS_16_0GB) {
		ret = -EOPNOTSUPP;
		goto err_partner_rpm;
	}

	ret = pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_CAP, &cap);
	if (ret != PCIBIOS_SUCCESSFUL) {
		ret = pcibios_err_to_errno(ret);
		goto err_partner_rpm;
	}

	/* Disable Autonomous Width and Speed transitions */
	pci_lmr_disable_autonomous(mdev);

	/* Inhibit ASPM L0s/L1 during margining with restoration path */
	pci_lmr_aspm_inhibit(mdev, true);

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

	/* Cache capabilities for configured receiver on all lanes */
	for (i = 0; i < mdev->num_lanes; i++) {
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
		if (clean_ret == PCIBIOS_SUCCESSFUL) {
			clean_sts &= ~PCI_LMR_PORT_STS_SW_READY;
			pci_write_config_word(dev, mdev->cap + PCI_LMR_PORT_STS,
					      clean_sts);
		}
	}
err_aspm:
	pci_lmr_aspm_inhibit(mdev, false);
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
	struct pci_dev *downstream_port, *upstream_port;
	bool enable;
	int ret;

	ret = kstrtobool_from_user(user_buf, count, &enable);
	if (ret)
		return ret;

	pci_lmr_get_link_partners(dev, &downstream_port, &upstream_port);

	/* Strict hierarchical lock order: Downstream Port (parent) before Upstream Port (child) */
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

	pci_lmr_put_link_partners(dev, downstream_port, upstream_port);

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

	guard(mutex)(&plane->mdev->lock);
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
	 * Revision 7.0 sec 4.2.18.1 & Table 4-76 (r6.0 Table 4-72);
	 * 7 is reserved.
	 */
	if (rx > LMR_MAX_RX_NUM)
		return -EINVAL;

	guard(mutex)(&mdev->lock);
	if (plane->rx == rx)
		return count;

	if (mdev->enabled) {
		/* Clear previous receiver to normal settings per single-receiver rule */
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

	guard(mutex)(&mdev->lock);
	if (!mdev->enabled)
		return -EBUSY;

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
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(margin_lane_caps);

static int margin_lane_steps_show(struct seq_file *s, u8 type)
{
	struct pci_margin_lane *plane = s->private;
	struct pci_margin_dev *mdev = plane->mdev;
	struct pci_margin_rx_info *info;
	int ret;

	guard(mutex)(&mdev->lock);
	if (!mdev->enabled)
		return -EBUSY;

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
 * If the target receiver uses an independent error sampler (MIndErrorSampler == 1b),
 * margining will not produce errors in the live data stream, and multiple receivers
 * may be margined concurrently. If MIndErrorSampler is 0b (main data sampler),
 * software must ensure that no other receiver on any lane is currently margined.
 */
static bool pci_lmr_check_sample_multiple_rx(struct pci_margin_dev *mdev,
					     struct pci_margin_lane *plane)
{
	struct pci_margin_rx_info *info = &plane->rx_info[plane->rx];
	int i;

	/* If receiver has an independent error sampler, concurrent margining is permitted */
	if (info->caps & LMR_CAP_IND_ERROR_SAMPLER)
		return true;

	for (i = 0; i < mdev->num_lanes; i++) {
		struct pci_margin_lane *other = &mdev->lanes[i];
		struct pci_margin_rx_info *other_info;

		if (i == plane->lane)
			continue;

		other_info = &other->rx_info[other->rx];
		/*
		 * For receivers using the main data sampler, reject only if another lane
		 * is actively margining a DIFFERENT receiver that ALSO uses the main data sampler.
		 */
		if (other->rx != plane->rx &&
		    !(other_info->caps & LMR_CAP_IND_ERROR_SAMPLER) &&
		    (other->timing_val != 0 || other->voltage_val != 0))
			return false;
	}
	return true;
}

static ssize_t margin_lane_step_write(struct file *file, const char __user *user_buf,
				      size_t count, u8 type)
{
	struct seq_file *s = file->private_data;
	struct pci_margin_lane *plane = s->private;
	struct pci_margin_dev *mdev = plane->mdev;
	struct pci_margin_rx_info *info;
	u8 step, dir, payload;
	int max_step, val, ret;
	u16 sts;
	u8 caps;

	ret = kstrtoint_from_user(user_buf, count, 0, &val);
	if (ret)
		return ret;

	guard(mutex)(&mdev->lock);
	if (!mdev->enabled)
		return -EBUSY;

	/*
	 * Ensure ASPM remains inhibited on both link partners before issuing
	 * margin steps. PCIe Base Specification Revision 7.0 sec 8.4.4 requires
	 * the link to stay in L0.
	 */
	pci_lmr_ensure_aspm_inhibited(mdev);

	if (val == 0) {
		/* Step this specific axis to 0 without resetting the orthogonal axis */
		if (type == LMR_TYPE_TIMING) {
			if (plane->voltage_val == 0) {
				ret = pci_lmr_clear_to_normal_lane(plane);
			} else {
				ret = pci_lmr_run_cmd(mdev, plane->lane, plane->rx,
						      LMR_TYPE_TIMING, 0, 0, &sts);
				if (!ret)
					plane->timing_val = 0;
			}
		} else {
			if (plane->timing_val == 0) {
				ret = pci_lmr_clear_to_normal_lane(plane);
			} else {
				ret = pci_lmr_run_cmd(mdev, plane->lane, plane->rx,
						      LMR_TYPE_VOLTAGE, 0, 0, &sts);
				if (!ret)
					plane->voltage_val = 0;
			}
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
			dir = LMR_STEP_DIR_LEFT_OR_DOWN; /* 1b: Left */
		} else {
			step = val;
			dir = LMR_STEP_DIR_RIGHT_OR_UP; /* 0b: Right or Symmetric (Reserved 0b) */
		}
		max_step = info->num_timing_steps;
		if (step > max_step)
			return -EINVAL;

		payload = FIELD_PREP(LMR_TIMING_DIR_MASK, dir) |
			  FIELD_PREP(LMR_TIMING_STEP_MASK, step);
		ret = pci_lmr_run_cmd(mdev, plane->lane, plane->rx,
				      LMR_TYPE_TIMING, 0, payload, &sts);
		if (ret)
			return ret;
		if (FIELD_GET(LMR_STS_EXEC_MASK, pci_lmr_sts_payload(sts)) ==
		    LMR_STS_EXEC_NAK)
			return -EOPNOTSUPP;
		if (FIELD_GET(LMR_STS_EXEC_MASK, pci_lmr_sts_payload(sts)) ==
		    LMR_STS_EXEC_TOO_MANY_ERR) {
			plane->timing_val = 0;
			plane->voltage_val = 0;
			return -EIO;
		}
		plane->timing_val = val;
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
			dir = LMR_STEP_DIR_LEFT_OR_DOWN; /* 1b: Down */
		} else {
			step = val;
			dir = LMR_STEP_DIR_RIGHT_OR_UP; /* 0b: Up or Symmetric (Reserved 0b) */
		}
		max_step = info->num_voltage_steps;
		if (step > max_step)
			return -EINVAL;

		payload = FIELD_PREP(LMR_VOLTAGE_DIR_MASK, dir) |
			  FIELD_PREP(LMR_VOLTAGE_STEP_MASK, step);
		ret = pci_lmr_run_cmd(mdev, plane->lane, plane->rx,
				      LMR_TYPE_VOLTAGE, 0, payload, &sts);
		if (ret)
			return ret;
		if (FIELD_GET(LMR_STS_EXEC_MASK, pci_lmr_sts_payload(sts)) ==
		    LMR_STS_EXEC_NAK)
			return -EOPNOTSUPP;
		if (FIELD_GET(LMR_STS_EXEC_MASK, pci_lmr_sts_payload(sts)) ==
		    LMR_STS_EXEC_TOO_MANY_ERR) {
			plane->timing_val = 0;
			plane->voltage_val = 0;
			return -EIO;
		}
		plane->voltage_val = val;
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

	guard(mutex)(&plane->mdev->lock);
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
	 * For devices associated with an Upstream Port (Endpoints),
	 * the Lane Margining Extended Capability must be implemented in
	 * Function 0 (and only Function 0).
	 */
	if (pci_is_pcie(dev) && pci_pcie_type(dev) == PCI_EXP_TYPE_ENDPOINT &&
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
	 * Maximum Link Width (MLW) per PCIe Base Specification Revision 7.0 sec 7.5.3.6
	 * ("Link Capabilities Register", bits 9:4).
	 */
	ret = pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &lnkcap);
	if (ret != PCIBIOS_SUCCESSFUL)
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

	pci_margin_debugfs_init(mdev);

	dev->lmr = mdev;

	pci_dbg(dev, "Lane Margining at Receiver (Gen%u) Capability detected\n",
		LMR_SPEED_TO_GEN(speed));
}

void pci_lmr_exit(struct pci_dev *dev)
{
	struct pci_margin_dev *mdev;

	if (!dev || !dev->lmr)
		return;

	mdev = dev->lmr;

	/* 1. Tear down user-facing debugfs files FIRST to prevent concurrent access */
	pci_margin_debugfs_remove(mdev);

	/* 2. Disarm dev->lmr under device_lock to serialize with pci_reset_lmr */
	pci_dev_lock(dev);
	mdev = dev->lmr;
	if (!mdev) {
		pci_dev_unlock(dev);
		return;
	}
	scoped_guard(mutex, &mdev->lock) {
		dev->lmr = NULL;
		pci_lmr_disable_locked(mdev);
	}
	pci_dev_unlock(dev);

	/* 3. Safe to destroy structures */
	mutex_destroy(&mdev->lock);
	kfree(mdev);
}

void pci_suspend_lmr(struct pci_dev *dev)
{
	struct pci_margin_dev *mdev = dev->lmr;

	if (!dev || !mdev)
		return;

	guard(mutex)(&mdev->lock);
	pci_lmr_disable_locked(mdev);
}

void pci_reset_lmr(struct pci_dev *dev)
{
	struct pci_margin_dev *mdev;
	u16 sts;
	int ret, i;

	if (!dev || pci_dev_is_removed(dev))
		return;

	device_lock_assert(&dev->dev);

	mdev = dev->lmr;
	if (!mdev)
		return;

	guard(mutex)
		(&mdev->lock);
	if (mdev->enabled) {
		for (i = 0; i < mdev->num_lanes; i++) {
			/*
			 * FLR does not reset Physical Layer registers like LMR.
			 * Return physical samplers to nominal in hardware to prevent
			 * persistent receiver eye skew.
			 */
			pci_lmr_clear_to_normal_lane(&mdev->lanes[i]);
			mdev->lanes[i].timing_val = 0;
			mdev->lanes[i].voltage_val = 0;
		}

		/* Clear SW_READY in hardware to reset margining state machine */
		ret = pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, &sts);
		if (ret == PCIBIOS_SUCCESSFUL) {
			sts &= ~PCI_LMR_PORT_STS_SW_READY;
			pci_write_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, sts);
		}

		/* Restore original hardware ASPM before saved states can seal the leak */
		pci_lmr_aspm_inhibit(mdev, false);
		pci_lmr_restore_autonomous(mdev);

		if (mdev->partner) {
			/*
			 * Drop remote partner's PM reference and schedule idle check
			 * asynchronously so the partner does not remain stranded in
			 * RPM_ACTIVE (D0) indefinitely.
			 */
			pm_runtime_put(&mdev->partner->dev);
			pci_dev_put(mdev->partner);
			mdev->partner = NULL;
		}

		/*
		 * Decrement runtime PM usage counter without triggering synchronous
		 * suspend, ensuring the device remains in D0 during pci_save_state()
		 * and the subsequent reset sequence.
		 */
		pm_runtime_put_noidle(&dev->dev);
		mdev->enabled = false;
	}
}
