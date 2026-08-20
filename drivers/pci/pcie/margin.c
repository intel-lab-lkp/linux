// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Express Lane Margining at Receiver
 *
 * Copyright (C) 2026 Google LLC
 * Author: Priyank Rathod <rathodpriyank@google.com>
 *
 * Lane Margining at Receiver (PCIe Base Specification r6.0, sec 8.4.4)
 * allows system software to determine the voltage and timing margins of
 * each physical lane on a PCIe link. The Extended Capability (ID 0x27)
 * is available for receivers operating at 16.0 GT/s (Gen4) or higher data
 * rates, and is mandatory for receivers operating at 64.0 GT/s (Gen6) or
 * higher data rates.
 *
 * This driver implements:
 *   - Probing Extended Capability ID 0x27 and Margining Port Capabilities.
 *   - Managing ASPM L0s/L1 link states during active margining with restoration.
 *   - PCIe r6.0 NO_CMD (0x7) clearing handshake per receiver and lane.
 *   - Caching receiver capabilities & step counts to avoid DEMARGIN side-effects.
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

/* Margin type encodings per PCIe Base Spec r6.0 sec 8.4.4 */
#define LMR_TYPE_DEMARGIN               0x0
#define LMR_TYPE_REPORT_CAPS            0x1
#define LMR_TYPE_REPORT_VOLTAGE_STEPS   0x2
#define LMR_TYPE_REPORT_TIMING_STEPS    0x3
#define LMR_TYPE_TIMING                 0x4
#define LMR_TYPE_VOLTAGE                0x5
#define LMR_TYPE_NO_CMD                 0x7

/* LMR command timing parameters */
#define LMR_CMD_TIMEOUT_MS              150
#define LMR_CMD_SLEEP_MIN_US            100
#define LMR_CMD_SLEEP_MAX_US            250
#define LMR_ENABLE_TIMEOUT_MS           150
#define LMR_ENABLE_SLEEP_MIN_US         1000
#define LMR_ENABLE_SLEEP_MAX_US         2000

/*
 * LMR limits:
 * Valid receiver numbers are 0 (local receiver) to 6 (up to 3 retimers)
 * per PCIe Base Specification r6.0 sec 8.4.4. Receiver number 7 is reserved.
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

/* LMR payload field masks per PCIe Base Spec r6.0 sec 8.4.4 */
#define LMR_STEPS_MASK			GENMASK(6, 0)
#define LMR_TIMING_STEP_MASK		GENMASK(5, 0)
#define LMR_TIMING_DIR_MASK		BIT(6)
#define LMR_VOLTAGE_STEP_MASK		GENMASK(6, 0)
#define LMR_VOLTAGE_DIR_MASK		BIT(7)

/* LMR capabilities report bit fields */
#define LMR_CAP_MARGIN_HV		BIT(0)
#define LMR_CAP_MARGIN_EV_IV		BIT(1)
#define LMR_CAP_IND_LEFT_RIGHT_TIMING	BIT(2)
#define LMR_CAP_IND_UP_DOWN_VOLTAGE	BIT(3)
#define LMR_CAP_ERROR_SAMPLER		BIT(4)
#define LMR_CAP_SAMPLE_MULTIPLE_RX	BIT(5)

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
 * @cap: Extended capability offset (PCI_EXT_CAP_ID_LMR)
 * @debugfs: Root debugfs dentry for this device
 * @lock: Mutex protecting LMR hardware access, active margining enablement,
 *        target receiver selection, lane margining steps, and ASPM state
 * @enabled: True if Lane Margining is currently enabled
 * @aspm_saved: True if original ASPM configuration has been saved
 * @saved_aspm: Saved ASPM control register bits for the device
 * @saved_parent_aspm: Saved ASPM control register bits for parent bridge
 * @num_lanes: Number of lanes on the link
 * @lanes: Flexible array of per-lane state structures
 */
struct pci_margin_dev {
	struct pci_dev *dev;
	u16 cap;
	struct dentry *debugfs;
	struct mutex lock;
	bool enabled;
	bool aspm_saved;
	u16 saved_aspm;
	u16 saved_parent_aspm;
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
 * pci_lmr_disable_aspm() - Temporarily disable ASPM L0s/L1 during active
 * margining per PCIe Base Spec r6.0 sec 8.4.4, saving original ASPMC bits.
 */
static void pci_lmr_disable_aspm(struct pci_margin_dev *mdev)
{
	struct pci_dev *dev = mdev->dev;
	struct pci_dev *parent = pci_upstream_bridge(dev);
	u16 ctl;

	if (mdev->aspm_saved)
		return;

	if (!pcie_capability_read_word(dev, PCI_EXP_LNKCTL, &ctl)) {
		mdev->saved_aspm = ctl & PCI_EXP_LNKCTL_ASPMC;
		pcie_capability_clear_word(dev, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_ASPMC);
	}

	if (parent && pci_is_pcie(parent)) {
		if (!pcie_capability_read_word(parent, PCI_EXP_LNKCTL, &ctl)) {
			mdev->saved_parent_aspm = ctl & PCI_EXP_LNKCTL_ASPMC;
			pcie_capability_clear_word(parent, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_ASPMC);
		}
	}
	mdev->aspm_saved = true;
}

/*
 * pci_lmr_restore_aspm() - Restore original ASPM L0s/L1 state when margining
 * is disabled or torn down.
 */
static void pci_lmr_restore_aspm(struct pci_margin_dev *mdev)
{
	struct pci_dev *dev = mdev->dev;
	struct pci_dev *parent = pci_upstream_bridge(dev);

	if (!mdev->aspm_saved)
		return;

	pcie_capability_clear_and_set_word(dev, PCI_EXP_LNKCTL,
					   PCI_EXP_LNKCTL_ASPMC,
					   mdev->saved_aspm);
	if (parent && pci_is_pcie(parent))
		pcie_capability_clear_and_set_word(parent, PCI_EXP_LNKCTL,
						   PCI_EXP_LNKCTL_ASPMC,
						   mdev->saved_parent_aspm);
	mdev->aspm_saved = false;
}

static inline u8 pci_lmr_sts_payload(u16 sts)
{
	return FIELD_GET(PCI_LMR_LANE_STS_PAYLOAD, sts);
}

/*
 * pci_lmr_run_cmd() - Issue LMR command to Lane Control and wait for Status.
 * Must be called with mdev->lock held.
 */
static int pci_lmr_run_cmd(struct pci_dev *dev, int lane, u8 rx, u8 type,
			   u8 usage, u8 payload, u16 *status_val)
{
	u16 lmr, ctrl_offset, sts_offset;
	u16 ctrl, sts;
	unsigned long timeout;

	if (!dev || !dev->lmr || lane < 0 || lane >= dev->lmr->num_lanes ||
	    rx > LMR_MAX_RX_NUM)
		return -EINVAL;

	lmr = dev->lmr_cap;
	ctrl_offset = lmr + PCI_LMR_LANE_CTRL + LMR_LANE_REG_STRIDE * lane;
	sts_offset = lmr + PCI_LMR_LANE_STS + LMR_LANE_REG_STRIDE * lane;

	/*
	 * Per PCIe Base Spec r6.0 sec 8.4.4, software must issue NO_CMD (0x7)
	 * targeting the specific receiver (rx) to clear MTYPE in Lane Status
	 * before issuing a subsequent command.
	 */
	if (type != LMR_TYPE_NO_CMD) {
		ctrl = FIELD_PREP(PCI_LMR_LANE_CTRL_RX_NUM, rx) |
		       FIELD_PREP(PCI_LMR_LANE_CTRL_MTYPE, LMR_TYPE_NO_CMD) |
		       FIELD_PREP(PCI_LMR_LANE_CTRL_USAGE, 0) |
		       FIELD_PREP(PCI_LMR_LANE_CTRL_PAYLOAD, 0);

		pci_write_config_word(dev, ctrl_offset, ctrl);

		timeout = jiffies + msecs_to_jiffies(LMR_CMD_TIMEOUT_MS);
		while (1) {
			if (pci_read_config_word(dev, sts_offset, &sts))
				return -EIO;
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

	pci_write_config_word(dev, ctrl_offset, ctrl);

	timeout = jiffies + msecs_to_jiffies(LMR_CMD_TIMEOUT_MS);
	while (1) {
		if (pci_read_config_word(dev, sts_offset, &sts))
			return -EIO;
		if (PCI_POSSIBLE_ERROR(sts))
			return -ENODEV;

		if (FIELD_GET(PCI_LMR_LANE_STS_MTYPE, sts) == type &&
		    FIELD_GET(PCI_LMR_LANE_STS_RX_NUM, sts) == rx) {
			if (status_val)
				*status_val = sts;
			return 0;
		}

		/*
		 * Per PCIe Base Spec r6.0 sec 8.4.4, if receiver echoes NO_CMD (0x7)
		 * after a command was sent, it indicates command rejection/NAK.
		 */
		if (FIELD_GET(PCI_LMR_LANE_STS_MTYPE, sts) == LMR_TYPE_NO_CMD &&
		    FIELD_GET(PCI_LMR_LANE_STS_RX_NUM, sts) == rx)
			return -EOPNOTSUPP;

		if (time_after(jiffies, timeout))
			break;

		usleep_range(LMR_CMD_SLEEP_MIN_US, LMR_CMD_SLEEP_MAX_US);
	}

	return -ETIMEDOUT;
}

static int pci_lmr_demargin_lane(struct pci_margin_lane *plane)
{
	u16 sts;
	int ret;

	if (!plane || !plane->mdev)
		return -EINVAL;

	if (plane->timing_val == 0 && plane->voltage_val == 0)
		return 0;

	ret = pci_lmr_run_cmd(plane->mdev->dev, plane->lane, plane->rx,
			      LMR_TYPE_DEMARGIN, 0, 0, &sts);
	if (!ret) {
		plane->timing_val = 0;
		plane->voltage_val = 0;
	}
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

	/* Issuing REPORT_CAPS aborts any active margin per PCIe spec */
	ret = pci_lmr_demargin_lane(plane);
	if (ret)
		return ret;

	ret = pci_lmr_run_cmd(plane->mdev->dev, plane->lane, rx,
			      LMR_TYPE_REPORT_CAPS, 0, 0, &sts);
	if (ret)
		return ret;
	info->caps = pci_lmr_sts_payload(sts);

	ret = pci_lmr_run_cmd(plane->mdev->dev, plane->lane, rx,
			      LMR_TYPE_REPORT_TIMING_STEPS, 0, 0, &sts);
	if (ret)
		return ret;
	info->num_timing_steps = FIELD_GET(LMR_STEPS_MASK, pci_lmr_sts_payload(sts));

	ret = pci_lmr_run_cmd(plane->mdev->dev, plane->lane, rx,
			      LMR_TYPE_REPORT_VOLTAGE_STEPS, 0, 0, &sts);
	if (ret)
		return ret;
	info->num_voltage_steps = FIELD_GET(LMR_STEPS_MASK, pci_lmr_sts_payload(sts));

	info->caps_cached = true;
	return 0;
}

#if IS_ENABLED(CONFIG_DEBUG_FS)

static int margin_caps_show(struct seq_file *s, void *v)
{
	struct pci_margin_dev *mdev = s->private;
	struct pci_dev *dev = mdev->dev;
	u16 cap;

	if (pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_CAP, &cap))
		return -EIO;
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

	if (pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, &sts))
		return -EIO;
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

static void __pci_suspend_lmr_locked(struct pci_dev *dev)
{
	struct pci_margin_dev *mdev = dev->lmr;
	int i;
	u16 sts;

	if (!mdev || !mdev->enabled)
		return;

	for (i = 0; i < mdev->num_lanes; i++)
		pci_lmr_demargin_lane(&mdev->lanes[i]);

	if (pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, &sts) == 0) {
		sts &= ~PCI_LMR_PORT_STS_SW_READY;
		pci_write_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, sts);
	}
	pci_lmr_restore_aspm(mdev);
	pm_runtime_put(&dev->dev);
	mdev->enabled = false;
}

static ssize_t margin_enable_write(struct file *file, const char __user *user_buf,
			       size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct pci_margin_dev *mdev = s->private;
	struct pci_dev *dev = mdev->dev;
	unsigned long timeout;
	u16 sts, cap, lnksta;
	bool enable;
	int ret, i;

	ret = kstrtobool_from_user(user_buf, count, &enable);
	if (ret)
		return ret;

	guard(mutex)(&mdev->lock);

	if (mdev->enabled == enable)
		return count;

	if (!enable) {
		__pci_suspend_lmr_locked(dev);
		return count;
	}

	/* PCIe r6.0 sec 8.4.4: LMR is physically undefined below 16.0 GT/s (Gen4) */
	pcie_capability_read_word(dev, PCI_EXP_LNKSTA, &lnksta);
	if ((lnksta & PCI_EXP_LNKSTA_CLS) < PCI_EXP_LNKSTA_CLS_16_0GB)
		return -EOPNOTSUPP;

	if (pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_CAP, &cap))
		return -EIO;

	/* Prevent D3hot runtime PM transition during active margining */
	ret = pm_runtime_resume_and_get(&dev->dev);
	if (ret < 0)
		return ret;

	/* Disable ASPM L0s/L1 during margining with restoration path */
	pci_lmr_disable_aspm(mdev);

	/* Ensure link is settled in L0 mode per PCIe r6.0 sec 8.4.4 */
	usleep_range(2000, 3000);

	if (cap & PCI_LMR_PORT_CAP_USES_SW_READY) {
		if (pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, &sts)) {
			ret = -EIO;
			goto err_enable;
		}
		sts |= PCI_LMR_PORT_STS_SW_READY;
		pci_write_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, sts);
	}

	timeout = jiffies + msecs_to_jiffies(LMR_ENABLE_TIMEOUT_MS);
	while (1) {
		if (pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, &sts)) {
			ret = -EIO;
			goto err_enable;
		}
		if (PCI_POSSIBLE_ERROR(sts)) {
			ret = -ENODEV;
			goto err_enable;
		}
		if (sts & PCI_LMR_PORT_STS_MARGIN_READY)
			break;
		if (time_after(jiffies, timeout)) {
			ret = -ETIMEDOUT;
			goto err_enable;
		}
		usleep_range(LMR_ENABLE_SLEEP_MIN_US, LMR_ENABLE_SLEEP_MAX_US);
	}

	/* Cache capabilities for configured receiver on all lanes */
	for (i = 0; i < mdev->num_lanes; i++) {
		ret = pci_lmr_cache_rx_info(&mdev->lanes[i],
					    mdev->lanes[i].rx);
		if (ret)
			goto err_enable;
	}
	mdev->enabled = true;
	return count;

err_enable:
	if (cap & PCI_LMR_PORT_CAP_USES_SW_READY) {
		if (pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, &sts) == 0) {
			sts &= ~PCI_LMR_PORT_STS_SW_READY;
			pci_write_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, sts);
		}
	}
	pci_lmr_restore_aspm(mdev);
	pm_runtime_put(&dev->dev);
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

	/* Valid receiver numbers are 0..6 per PCIe r6.0 sec 8.4.4; 7 is reserved */
	if (rx > LMR_MAX_RX_NUM)
		return -EINVAL;

	guard(mutex)(&mdev->lock);
	if (plane->rx == rx)
		return count;

	if (mdev->enabled) {
		/* Demargin previous receiver per single-receiver spec rule */
		ret = pci_lmr_demargin_lane(plane);
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
		return -EACCES;

	ret = pci_lmr_cache_rx_info(plane, plane->rx);
	if (ret)
		return ret;

	info = &plane->rx_info[plane->rx];
	val = info->caps;
	seq_printf(s, "Lane %d Rx %d Capabilities: %#02x\n", plane->lane, plane->rx, val);
	seq_printf(s, "  Margining H/V: %s\n", (val & LMR_CAP_MARGIN_HV) ? "both" : "either");
	seq_printf(s, "  Margining eV/iV: %s\n",
		   (val & LMR_CAP_MARGIN_EV_IV) ? "both (close & open)" : "one (close only)");
	seq_printf(s, "  Left/Right: %s\n", (val & LMR_CAP_IND_LEFT_RIGHT_TIMING) ? "both" : "one");
	seq_printf(s, "  Up/Down: %s\n", (val & LMR_CAP_IND_UP_DOWN_VOLTAGE) ? "both" : "one");
	seq_printf(s, "  Error Sampler: %s\n",
		   (val & LMR_CAP_ERROR_SAMPLER) ? "yes" : "no (main sampler)");
	seq_printf(s, "  Sample Multiple Receivers: %s\n",
		   str_yes_no(val & LMR_CAP_SAMPLE_MULTIPLE_RX));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(margin_lane_caps);

static int margin_lane_steps_show(struct seq_file *s, bool is_voltage)
{
	struct pci_margin_lane *plane = s->private;
	struct pci_margin_dev *mdev = plane->mdev;
	struct pci_margin_rx_info *info;
	int ret;

	guard(mutex)(&mdev->lock);
	if (!mdev->enabled)
		return -EACCES;

	ret = pci_lmr_cache_rx_info(plane, plane->rx);
	if (ret)
		return ret;

	info = &plane->rx_info[plane->rx];
	seq_printf(s, "%d\n", is_voltage ? info->num_voltage_steps : info->num_timing_steps);
	return 0;
}

static int margin_lane_timing_steps_show(struct seq_file *s, void *v)
{
	return margin_lane_steps_show(s, false);
}
DEFINE_SHOW_ATTRIBUTE(margin_lane_timing_steps);

static int margin_lane_voltage_steps_show(struct seq_file *s, void *v)
{
	return margin_lane_steps_show(s, true);
}
DEFINE_SHOW_ATTRIBUTE(margin_lane_voltage_steps);

static bool pci_lmr_check_sample_multiple_rx(struct pci_margin_dev *mdev,
					     struct pci_margin_lane *plane)
{
	struct pci_margin_rx_info *info = &plane->rx_info[plane->rx];
	int i;

	if (info->caps & LMR_CAP_SAMPLE_MULTIPLE_RX)
		return true;

	for (i = 0; i < mdev->num_lanes; i++) {
		struct pci_margin_lane *other = &mdev->lanes[i];

		if (i == plane->lane)
			continue;
		if (other->rx == plane->rx &&
		    (other->timing_val != 0 || other->voltage_val != 0))
			return false;
	}
	return true;
}

static ssize_t margin_lane_step_write(struct file *file, const char __user *user_buf,
				      size_t count, bool is_voltage)
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

	max_step = is_voltage ? LMR_MAX_VOLTAGE_STEP : LMR_MAX_TIMING_STEP;
	if (val > max_step || val < -max_step)
		return -EINVAL;

	guard(mutex)(&mdev->lock);
	if (!mdev->enabled)
		return -EACCES;

	if (val == 0)
		return pci_lmr_demargin_lane(plane) ?: count;

	ret = pci_lmr_cache_rx_info(plane, plane->rx);
	if (ret)
		return ret;

	if (!pci_lmr_check_sample_multiple_rx(mdev, plane))
		return -EBUSY;

	info = &plane->rx_info[plane->rx];
	caps = info->caps;

	if (val < 0) {
		step = -val;
		if (is_voltage) {
			/* Symmetric voltage margining requires dir=1 per spec */
			if (!(caps & LMR_CAP_IND_UP_DOWN_VOLTAGE))
				return -EINVAL;
			dir = 0;
		} else {
			/* Symmetric timing margining requires dir=1 per spec */
			if (!(caps & LMR_CAP_IND_LEFT_RIGHT_TIMING))
				return -EINVAL;
			dir = LMR_STEP_DIR_DECREASE;
		}
	} else {
		step = val;
		dir = is_voltage ? 1 : LMR_STEP_DIR_INCREASE;
	}

	max_step = is_voltage ? info->num_voltage_steps : info->num_timing_steps;
	if (step > max_step)
		return -EINVAL;

	if (is_voltage) {
		payload = FIELD_PREP(LMR_VOLTAGE_DIR_MASK, dir) |
			  FIELD_PREP(LMR_VOLTAGE_STEP_MASK, step);
		ret = pci_lmr_run_cmd(mdev->dev, plane->lane, plane->rx,
				      LMR_TYPE_VOLTAGE, 0, payload, &sts);
		if (ret)
			return ret;
		step = FIELD_GET(LMR_VOLTAGE_STEP_MASK, pci_lmr_sts_payload(sts));
		plane->voltage_val = (dir == 0) ? -step : step;
	} else {
		payload = FIELD_PREP(LMR_TIMING_DIR_MASK, dir) |
			  FIELD_PREP(LMR_TIMING_STEP_MASK, step);
		ret = pci_lmr_run_cmd(mdev->dev, plane->lane, plane->rx,
				      LMR_TYPE_TIMING, 0, payload, &sts);
		if (ret)
			return ret;
		step = FIELD_GET(LMR_TIMING_STEP_MASK, pci_lmr_sts_payload(sts));
		plane->timing_val = (dir == LMR_STEP_DIR_DECREASE) ? -step : step;
	}

	return count;
}

static ssize_t margin_lane_timing_write(struct file *file, const char __user *user_buf,
					size_t count, loff_t *ppos)
{
	return margin_lane_step_write(file, user_buf, count, false);
}

static int margin_lane_step_show(struct seq_file *s, bool is_voltage)
{
	struct pci_margin_lane *plane = s->private;

	guard(mutex)(&plane->mdev->lock);
	seq_printf(s, "%d\n", is_voltage ? plane->voltage_val : plane->timing_val);
	return 0;
}

static int margin_lane_timing_show(struct seq_file *s, void *v)
{
	return margin_lane_step_show(s, false);
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
	return margin_lane_step_write(file, user_buf, count, true);
}

static int margin_lane_voltage_show(struct seq_file *s, void *v)
{
	return margin_lane_step_show(s, true);
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
	if (IS_ERR_OR_NULL(mdev->debugfs))
		return;

	debugfs_create_file("capabilities", 0444, mdev->debugfs, mdev, &margin_caps_fops);
	debugfs_create_file("port_status", 0444, mdev->debugfs, mdev, &margin_port_status_fops);
	debugfs_create_file("enable", 0644, mdev->debugfs, mdev, &margin_enable_fops);

	for (i = 0; i < mdev->num_lanes; i++) {
		struct pci_margin_lane *plane = &mdev->lanes[i];
		struct dentry *lane_dir;
		char lane_name[16];

		scnprintf(lane_name, sizeof(lane_name), "lane%d", i);
		lane_dir = debugfs_create_dir(lane_name, mdev->debugfs);
		if (IS_ERR_OR_NULL(lane_dir))
			continue;

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
	u16 lmr, lnksta;
	int num_lanes, i;

	if (!dev || !pci_is_pcie(dev))
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

	pcie_capability_read_word(dev, PCI_EXP_LNKSTA, &lnksta);
	num_lanes = FIELD_GET(PCI_EXP_LNKSTA_NLW, lnksta);
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

	pci_info(dev, "Lane Margining at Receiver (Gen%u) Capability detected\n",
		 LMR_SPEED_TO_GEN(speed));
}

void pci_lmr_exit(struct pci_dev *dev)
{
	struct pci_margin_dev *mdev = dev->lmr;

	if (!dev || !mdev)
		return;

	pci_suspend_lmr(dev);

	pci_margin_debugfs_remove(mdev);
	mutex_destroy(&mdev->lock);
	kfree(mdev);
	dev->lmr = NULL;
}

void pci_suspend_lmr(struct pci_dev *dev)
{
	struct pci_margin_dev *mdev = dev->lmr;

	if (!dev || !mdev)
		return;

	guard(mutex)(&mdev->lock);
	__pci_suspend_lmr_locked(dev);
}

void pci_reset_lmr(struct pci_dev *dev)
{
	pci_suspend_lmr(dev);
}
