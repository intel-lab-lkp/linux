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
 *   - Managing ASPM L0s/L1 link states during active margining per spec.
 *   - PCIe r6.0 NO_CMD (0x7) clearing handshake per receiver and lane.
 *   - Caching receiver capabilities & step counts to avoid DEMARGIN side-effects.
 *   - Handling Symmetric vs Independent Left/Right & Up/Down margin steps.
 *   - Exposing per-device debugfs interfaces under /sys/kernel/debug/pci/.
 */

#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/seq_file.h>
#include <linux/bitfield.h>
#include <linux/debugfs.h>

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

/* LMR limits */
#define LMR_MAX_LANES                   32
#define LMR_MAX_RX_NUM                  6
#define LMR_MAX_TIMING_STEP             63
#define LMR_MAX_VOLTAGE_STEP            127

/* LMR PCIe generation numbers */
#define LMR_GEN6                        6
#define LMR_GEN5                        5
#define LMR_GEN4                        4

/* LMR lane register stride */
#define LMR_LANE_REG_STRIDE             4

/* LMR receivers and directions */
#define LMR_RX_LOCAL                    0
#define LMR_STEP_DIR_INCREASE           1
#define LMR_STEP_DIR_DECREASE           0

/* LMR step & direction encoding masks */
#define LMR_STEPS_MASK                  0x7F
#define LMR_TIMING_STEP_MASK            0x3F
#define LMR_TIMING_DIR_SHIFT            6
#define LMR_VOLTAGE_STEP_MASK           0x7F
#define LMR_VOLTAGE_DIR_SHIFT           7

/* LMR capabilities report bit fields */
#define LMR_CAP_MARGIN_HV               BIT(0)
#define LMR_CAP_MARGIN_EV_IV            BIT(1)
#define LMR_CAP_IND_LEFT_RIGHT_TIMING   BIT(2)
#define LMR_CAP_IND_UP_DOWN_VOLTAGE     BIT(3)
#define LMR_CAP_ERROR_SAMPLER           BIT(4)
#define LMR_CAP_SAMPLE_MULTIPLE_RX      BIT(5)

struct pci_margin_rx_info {
	bool caps_cached;
	u8 caps;
	u8 num_timing_steps;
	u8 num_voltage_steps;
};

struct pci_margin_lane {
	struct pci_margin_dev *mdev;
	int lane;
	u8 rx;
	int timing_val;
	int voltage_val;
	struct pci_margin_rx_info rx_info[LMR_MAX_RX_NUM + 1];
};

struct pci_margin_dev {
	struct pci_dev *dev;
	u16 cap;
	struct dentry *debugfs;
	struct mutex lock;
	int num_lanes;
	struct pci_margin_lane *lanes;
	bool enabled;
};

#if IS_ENABLED(CONFIG_DEBUG_FS)
static struct dentry *pci_debugfs_root_dir;

static struct dentry *get_pci_debugfs_root(void)
{
	if (!pci_debugfs_root_dir)
		pci_debugfs_root_dir = debugfs_lookup("pci", NULL);
	if (!pci_debugfs_root_dir)
		pci_debugfs_root_dir = debugfs_create_dir("pci", NULL);
	return pci_debugfs_root_dir;
}
#endif

/*
 * pci_lmr_run_cmd() - Issue LMR command to Lane Control and wait for Status.
 * Must be called with mdev->lock held.
 */
static int pci_lmr_run_cmd(struct pci_dev *dev, int lane, u8 rx, u8 type,
			   u8 usage, u8 payload, u16 *status_val)
{
	u16 lmr = dev->lmr_cap;
	u16 ctrl_offset = lmr + PCI_LMR_LANE_CTRL + LMR_LANE_REG_STRIDE * lane;
	u16 sts_offset = lmr + PCI_LMR_LANE_STS + LMR_LANE_REG_STRIDE * lane;
	u16 ctrl, sts;
	unsigned long timeout;

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
			if (sts == 0xFFFF)
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
		if (sts == 0xFFFF)
			return -ENODEV;

		if (FIELD_GET(PCI_LMR_LANE_STS_MTYPE, sts) == type &&
		    FIELD_GET(PCI_LMR_LANE_STS_RX_NUM, sts) == rx) {
			if (status_val)
				*status_val = sts;
			return 0;
		}

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
	struct pci_margin_rx_info *info = &plane->rx_info[rx];
	u16 sts;
	int ret;

	if (info->caps_cached)
		return 0;

	/* Issuing REPORT_CAPS aborts any active margin per PCIe spec */
	pci_lmr_demargin_lane(plane);

	ret = pci_lmr_run_cmd(plane->mdev->dev, plane->lane, rx,
			      LMR_TYPE_REPORT_CAPS, 0, 0, &sts);
	if (ret)
		return ret;
	info->caps = FIELD_GET(PCI_LMR_LANE_STS_PAYLOAD, sts);

	ret = pci_lmr_run_cmd(plane->mdev->dev, plane->lane, rx,
			      LMR_TYPE_REPORT_TIMING_STEPS, 0, 0, &sts);
	if (ret)
		return ret;
	info->num_timing_steps = FIELD_GET(PCI_LMR_LANE_STS_PAYLOAD, sts) & LMR_STEPS_MASK;

	ret = pci_lmr_run_cmd(plane->mdev->dev, plane->lane, rx,
			      LMR_TYPE_REPORT_VOLTAGE_STEPS, 0, 0, &sts);
	if (ret)
		return ret;
	info->num_voltage_steps = FIELD_GET(PCI_LMR_LANE_STS_PAYLOAD, sts) & LMR_STEPS_MASK;

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
		   (cap & PCI_LMR_PORT_CAP_USES_SW_READY) ? "yes" : "no");
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
		   (sts & PCI_LMR_PORT_STS_MARGIN_READY) ? "yes" : "no");
	seq_printf(s, "  SW Ready: %s\n", (sts & PCI_LMR_PORT_STS_SW_READY) ? "yes" : "no");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(margin_port_status);

static int margin_enable_show(struct seq_file *s, void *v)
{
	struct pci_margin_dev *mdev = s->private;
	bool enabled;

	mutex_lock(&mdev->lock);
	enabled = mdev->enabled;
	mutex_unlock(&mdev->lock);

	seq_printf(s, "%d\n", enabled);
	return 0;
}

static ssize_t margin_enable_write(struct file *file, const char __user *user_buf,
			       size_t count, loff_t *ppos)
{
	struct pci_margin_dev *mdev = ((struct seq_file *)file->private_data)->private;
	struct pci_dev *dev = mdev->dev;
	unsigned long timeout;
	bool enable;
	int ret, i;
	u16 sts, cap;

	ret = kstrtobool_from_user(user_buf, count, &enable);
	if (ret)
		return ret;

	mutex_lock(&mdev->lock);

	if (mdev->enabled == enable)
		goto out;

	if (enable) {
		if (pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_CAP, &cap)) {
			ret = -EIO;
			goto out;
		}

		/*
		 * PCIe Base Spec r6.0: Link must be maintained in L0 during
		 * active margining. Disable ASPM L0s/L1.
		 */
		pci_disable_link_state(dev, PCIE_LINK_STATE_L0S | PCIE_LINK_STATE_L1);

		if (cap & PCI_LMR_PORT_CAP_USES_SW_READY) {
			if (pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, &sts)) {
				ret = -EIO;
				goto out;
			}
			sts |= PCI_LMR_PORT_STS_SW_READY;
			pci_write_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, sts);
		}

		timeout = jiffies + msecs_to_jiffies(LMR_ENABLE_TIMEOUT_MS);
		while (1) {
			if (pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, &sts)) {
				ret = -EIO;
				goto out;
			}
			if (sts == 0xFFFF) {
				ret = -ENODEV;
				goto out;
			}
			if (sts & PCI_LMR_PORT_STS_MARGIN_READY)
				break;
			if (time_after(jiffies, timeout)) {
				ret = -ETIMEDOUT;
				if (cap & PCI_LMR_PORT_CAP_USES_SW_READY) {
					sts &= ~PCI_LMR_PORT_STS_SW_READY;
					pci_write_config_word(dev,
							      mdev->cap + PCI_LMR_PORT_STS,
							      sts);
				}
				goto out;
			}
			usleep_range(LMR_ENABLE_SLEEP_MIN_US, LMR_ENABLE_SLEEP_MAX_US);
		}

		/* Cache capabilities for local receiver on all lanes */
		for (i = 0; i < mdev->num_lanes; i++) {
			mdev->lanes[i].rx = LMR_RX_LOCAL;
			pci_lmr_cache_rx_info(&mdev->lanes[i], LMR_RX_LOCAL);
		}
		mdev->enabled = true;
	} else {
		/* Demargin all lanes before clearing SW_READY */
		for (i = 0; i < mdev->num_lanes; i++)
			pci_lmr_demargin_lane(&mdev->lanes[i]);

		if (pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, &sts) == 0) {
			sts &= ~PCI_LMR_PORT_STS_SW_READY;
			pci_write_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, sts);
		}
		mdev->enabled = false;
	}

out:
	mutex_unlock(&mdev->lock);
	return ret ? ret : count;
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
	int rx;

	mutex_lock(&plane->mdev->lock);
	rx = plane->rx;
	mutex_unlock(&plane->mdev->lock);

	seq_printf(s, "%d\n", rx);
	return 0;
}

static ssize_t margin_lane_receiver_write(struct file *file, const char __user *user_buf,
				      size_t count, loff_t *ppos)
{
	struct pci_margin_lane *plane = ((struct seq_file *)file->private_data)->private;
	struct pci_margin_dev *mdev = plane->mdev;
	int ret;
	int rx;

	ret = kstrtoint_from_user(user_buf, count, 0, &rx);
	if (ret)
		return ret;

	if (rx < 0 || rx > LMR_MAX_RX_NUM)
		return -EINVAL;

	mutex_lock(&mdev->lock);
	if (plane->rx == rx)
		goto out;

	if (mdev->enabled) {
		/* Demargin previous receiver per single-receiver spec rule */
		pci_lmr_demargin_lane(plane);
		ret = pci_lmr_cache_rx_info(plane, rx);
		if (ret)
			goto out;
	}

	plane->rx = rx;
out:
	mutex_unlock(&mdev->lock);
	return ret ? ret : count;
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
	int ret = 0;
	u8 val;

	mutex_lock(&mdev->lock);
	if (!mdev->enabled) {
		ret = -EACCES;
		goto out;
	}

	ret = pci_lmr_cache_rx_info(plane, plane->rx);
	if (ret)
		goto out;

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
		   (val & LMR_CAP_SAMPLE_MULTIPLE_RX) ? "yes" : "no");
out:
	mutex_unlock(&mdev->lock);
	return ret;
}
DEFINE_SHOW_ATTRIBUTE(margin_lane_caps);

static int margin_lane_timing_steps_show(struct seq_file *s, void *v)
{
	struct pci_margin_lane *plane = s->private;
	struct pci_margin_dev *mdev = plane->mdev;
	struct pci_margin_rx_info *info;
	int ret = 0;

	mutex_lock(&mdev->lock);
	if (!mdev->enabled) {
		ret = -EACCES;
		goto out;
	}

	ret = pci_lmr_cache_rx_info(plane, plane->rx);
	if (!ret) {
		info = &plane->rx_info[plane->rx];
		seq_printf(s, "%d\n", info->num_timing_steps);
	}
out:
	mutex_unlock(&mdev->lock);
	return ret;
}
DEFINE_SHOW_ATTRIBUTE(margin_lane_timing_steps);

static int margin_lane_voltage_steps_show(struct seq_file *s, void *v)
{
	struct pci_margin_lane *plane = s->private;
	struct pci_margin_dev *mdev = plane->mdev;
	struct pci_margin_rx_info *info;
	int ret = 0;

	mutex_lock(&mdev->lock);
	if (!mdev->enabled) {
		ret = -EACCES;
		goto out;
	}

	ret = pci_lmr_cache_rx_info(plane, plane->rx);
	if (!ret) {
		info = &plane->rx_info[plane->rx];
		seq_printf(s, "%d\n", info->num_voltage_steps);
	}
out:
	mutex_unlock(&mdev->lock);
	return ret;
}
DEFINE_SHOW_ATTRIBUTE(margin_lane_voltage_steps);

static ssize_t margin_lane_timing_write(struct file *file, const char __user *user_buf,
				    size_t count, loff_t *ppos)
{
	struct pci_margin_lane *plane = ((struct seq_file *)file->private_data)->private;
	struct pci_margin_dev *mdev = plane->mdev;
	struct pci_margin_rx_info *info;
	int val, ret;
	u8 step, dir;
	u16 sts;
	u8 caps;

	ret = kstrtoint_from_user(user_buf, count, 0, &val);
	if (ret)
		return ret;

	if (val > LMR_MAX_TIMING_STEP || val < -LMR_MAX_TIMING_STEP)
		return -EINVAL;

	mutex_lock(&mdev->lock);
	if (!mdev->enabled) {
		ret = -EACCES;
		goto out;
	}

	if (val == 0) {
		ret = pci_lmr_demargin_lane(plane);
		goto out;
	}

	ret = pci_lmr_cache_rx_info(plane, plane->rx);
	if (ret)
		goto out;

	info = &plane->rx_info[plane->rx];
	caps = info->caps;

	if (val < 0) {
		step = -val;
		if (!(caps & LMR_CAP_IND_LEFT_RIGHT_TIMING)) {
			/* Symmetric margining requires dir=1 per spec */
			ret = -EINVAL;
			goto out;
		}
		dir = LMR_STEP_DIR_DECREASE;
	} else {
		step = val;
		dir = LMR_STEP_DIR_INCREASE;
	}

	if (step > info->num_timing_steps) {
		ret = -EINVAL;
		goto out;
	}

	ret = pci_lmr_run_cmd(mdev->dev, plane->lane, plane->rx,
			      LMR_TYPE_TIMING, 0,
			      (step & LMR_TIMING_STEP_MASK) | (dir << LMR_TIMING_DIR_SHIFT), &sts);
	if (ret)
		goto out;

	/* Record actual step count applied by hardware */
	step = FIELD_GET(PCI_LMR_LANE_STS_PAYLOAD, sts) & LMR_TIMING_STEP_MASK;
	plane->timing_val = (dir == LMR_STEP_DIR_DECREASE) ? -step : step;

out:
	mutex_unlock(&mdev->lock);
	return ret ? ret : count;
}

static int margin_lane_timing_show(struct seq_file *s, void *v)
{
	struct pci_margin_lane *plane = s->private;
	int timing_val;

	mutex_lock(&plane->mdev->lock);
	timing_val = plane->timing_val;
	mutex_unlock(&plane->mdev->lock);

	seq_printf(s, "%d\n", timing_val);
	return 0;
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
	struct pci_margin_lane *plane = ((struct seq_file *)file->private_data)->private;
	struct pci_margin_dev *mdev = plane->mdev;
	struct pci_margin_rx_info *info;
	int val, ret;
	u8 step, dir;
	u16 sts;
	u8 caps;

	ret = kstrtoint_from_user(user_buf, count, 0, &val);
	if (ret)
		return ret;

	if (val > LMR_MAX_VOLTAGE_STEP || val < -LMR_MAX_VOLTAGE_STEP)
		return -EINVAL;

	mutex_lock(&mdev->lock);
	if (!mdev->enabled) {
		ret = -EACCES;
		goto out;
	}

	if (val == 0) {
		ret = pci_lmr_demargin_lane(plane);
		goto out;
	}

	ret = pci_lmr_cache_rx_info(plane, plane->rx);
	if (ret)
		goto out;

	info = &plane->rx_info[plane->rx];
	caps = info->caps;

	if (val < 0) {
		step = -val;
		if (!(caps & LMR_CAP_IND_UP_DOWN_VOLTAGE)) {
			/* Symmetric voltage margining requires dir=1 per spec */
			ret = -EINVAL;
			goto out;
		}
		dir = 0;
	} else {
		step = val;
		dir = 1;
	}

	if (step > info->num_voltage_steps) {
		ret = -EINVAL;
		goto out;
	}

	ret = pci_lmr_run_cmd(mdev->dev, plane->lane, plane->rx,
			      LMR_TYPE_VOLTAGE, 0,
			      (step & LMR_VOLTAGE_STEP_MASK) |
			      (dir << LMR_VOLTAGE_DIR_SHIFT), &sts);
	if (ret)
		goto out;

	step = FIELD_GET(PCI_LMR_LANE_STS_PAYLOAD, sts) & LMR_VOLTAGE_STEP_MASK;
	plane->voltage_val = (dir == 0) ? -step : step;

out:
	mutex_unlock(&mdev->lock);
	return ret ? ret : count;
}

static int margin_lane_voltage_show(struct seq_file *s, void *v)
{
	struct pci_margin_lane *plane = s->private;
	int voltage_val;

	mutex_lock(&plane->mdev->lock);
	voltage_val = plane->voltage_val;
	mutex_unlock(&plane->mdev->lock);

	seq_printf(s, "%d\n", voltage_val);
	return 0;
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
	snprintf(dirname, sizeof(dirname), "pcie_lmr_%s", dev_name(&dev->dev));
	mdev->debugfs = debugfs_create_dir(dirname, parent);
	if (!mdev->debugfs)
		return;

	debugfs_create_file("capabilities", 0444, mdev->debugfs, mdev, &margin_caps_fops);
	debugfs_create_file("port_status", 0444, mdev->debugfs, mdev, &margin_port_status_fops);
	debugfs_create_file("enable", 0644, mdev->debugfs, mdev, &margin_enable_fops);

	for (i = 0; i < mdev->num_lanes; i++) {
		struct pci_margin_lane *plane = &mdev->lanes[i];
		struct dentry *lane_dir;
		char lane_name[16];

		snprintf(lane_name, sizeof(lane_name), "lane%d", i);
		lane_dir = debugfs_create_dir(lane_name, mdev->debugfs);
		if (!lane_dir)
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
	u16 lmr;
	u16 lnkcap;
	int i;

	if (!pci_is_pcie(dev))
		return;

	speed = pcie_get_speed_cap(dev);

	lmr = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_LMR);
	if (!lmr) {
		if (speed == PCIE_SPEED_64_0GT)
			pci_warn(dev,
				 "Missing Lane Margining at Receiver Capability (mandatory for Gen6+)\n");
		else if (speed >= PCIE_SPEED_16_0GT && speed != PCI_SPEED_UNKNOWN)
			pci_dbg(dev,
				"Optional Lane Margining at Receiver Capability not found\n");
		return;
	}

	if (speed < PCIE_SPEED_16_0GT && speed != PCI_SPEED_UNKNOWN)
		return;

	dev->lmr_cap = lmr;

	mdev = kzalloc_obj(*mdev, GFP_KERNEL);
	if (!mdev)
		return;

	mdev->dev = dev;
	mdev->cap = lmr;
	mutex_init(&mdev->lock);

	pcie_capability_read_word(dev, PCI_EXP_LNKCAP, &lnkcap);
	mdev->num_lanes = FIELD_GET(PCI_EXP_LNKCAP_MLW, lnkcap);

	if (mdev->num_lanes == 0 || mdev->num_lanes > LMR_MAX_LANES) {
		pci_warn(dev, "Invalid link width %d for LMR\n", mdev->num_lanes);
		goto err_free_mdev;
	}

	mdev->lanes = kcalloc(mdev->num_lanes, sizeof(*mdev->lanes), GFP_KERNEL);
	if (!mdev->lanes)
		goto err_free_mdev;

	for (i = 0; i < mdev->num_lanes; i++) {
		mdev->lanes[i].mdev = mdev;
		mdev->lanes[i].lane = i;
		mdev->lanes[i].rx = LMR_RX_LOCAL;
	}

	pci_margin_debugfs_init(mdev);

	dev->lmr = mdev;

	pci_info(dev, "Lane Margining at Receiver (Gen%u) Capability detected\n",
		 speed == PCIE_SPEED_64_0GT ? LMR_GEN6 :
		 speed == PCIE_SPEED_32_0GT ? LMR_GEN5 :
		 LMR_GEN4);
	return;

err_free_mdev:
	mutex_destroy(&mdev->lock);
	kfree(mdev);
}

void pci_lmr_exit(struct pci_dev *dev)
{
	struct pci_margin_dev *mdev = dev->lmr;
	int i;

	if (!mdev)
		return;

	mutex_lock(&mdev->lock);
	if (mdev->enabled) {
		for (i = 0; i < mdev->num_lanes; i++)
			pci_lmr_demargin_lane(&mdev->lanes[i]);
	}
	mutex_unlock(&mdev->lock);

	pci_margin_debugfs_remove(mdev);
	mutex_destroy(&mdev->lock);
	kfree(mdev->lanes);
	kfree(mdev);
	dev->lmr = NULL;
}

void pci_suspend_lmr(struct pci_dev *dev)
{
	struct pci_margin_dev *mdev = dev->lmr;
	int i;
	u16 sts;

	if (!mdev)
		return;

	mutex_lock(&mdev->lock);
	if (mdev->enabled) {
		for (i = 0; i < mdev->num_lanes; i++)
			pci_lmr_demargin_lane(&mdev->lanes[i]);

		if (pci_read_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, &sts) == 0) {
			sts &= ~PCI_LMR_PORT_STS_SW_READY;
			pci_write_config_word(dev, mdev->cap + PCI_LMR_PORT_STS, sts);
		}
		mdev->enabled = false;
	}
	mutex_unlock(&mdev->lock);
}
