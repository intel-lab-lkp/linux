// SPDX-License-Identifier: GPL-2.0
/*
 * Synopsys DesignWare PCIe controller debugfs driver
 *
 * Copyright (C) 2023 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Author: Shradha Todi <shradha.t@samsung.com>
 */

#include <linux/debugfs.h>

#include "pcie-designware.h"

#define RAS_DES_EVENT_COUNTER_CTRL_REG	0x8
#define RAS_DES_EVENT_COUNTER_DATA_REG	0xc
#define SD_STATUS_L1LANE_REG		0xb0
#define ERR_INJ_ENABLE_REG		0x30
#define ERR_INJ0_OFF			0x34

#define LANE_DETECT_SHIFT		17
#define LANE_DETECT_MASK		0x1
#define PIPE_RXVALID_SHIFT		18
#define PIPE_RXVALID_MASK		0x1

#define LANE_SELECT_SHIFT		8
#define LANE_SELECT_MASK		0xf
#define EVENT_COUNTER_STATUS_SHIFT	7
#define EVENT_COUNTER_STATUS_MASK	0x1
#define EVENT_COUNTER_ENABLE		(0x7 << 2)
#define PER_EVENT_OFF			(0x1 << 2)
#define PER_EVENT_ON			(0x3 << 2)

#define EINJ_COUNT_MASK			0xff
#define EINJ_TYPE_MASK			0xf
#define EINJ_TYPE_SHIFT			8
#define EINJ_INFO_MASK			0xfffff
#define EINJ_INFO_SHIFT			12

#define DWC_DEBUGFS_MAX			128

struct rasdes_info {
	/* to store rasdes capability offset */
	u32 ras_cap;
	struct mutex dbg_mutex;
	struct dentry *rasdes;
};

struct rasdes_priv {
	struct dw_pcie *pci;
	int idx;
};

struct event_counter {
	const char *name;
	/* values can be between 0-15 */
	u32 group_no;
	/* values can be between 0-32 */
	u32 event_no;
};

static const struct event_counter event_counters[] = {
	{"ebuf_overflow", 0x0, 0x0},
	{"ebuf_underrun", 0x0, 0x1},
	{"decode_err", 0x0, 0x2},
	{"running_disparity_err", 0x0, 0x3},
	{"skp_os_parity_err", 0x0, 0x4},
	{"sync_header_err", 0x0, 0x5},
	{"detect_ei_infer", 0x1, 0x5},
	{"receiver_err", 0x1, 0x6},
	{"rx_recovery_req", 0x1, 0x7},
	{"framing_err", 0x1, 0x9},
	{"deskew_err", 0x1, 0xa},
	{"bad_tlp", 0x2, 0x0},
	{"lcrc_err", 0x2, 0x1},
	{"bad_dllp", 0x2, 0x2},
};

struct err_inj {
	const char *name;
	/* values can be from group 0 - 6 */
	u32 err_inj_group;
	/* within each group there can be types */
	u32 err_inj_type;
	/* More details about the error */
	u32 err_inj_12_31;
};

static const struct err_inj err_inj_list[] = {
	{"tx_lcrc", 0x0, 0x0, 0x0},
	{"tx_ecrc", 0x0, 0x3, 0x0},
	{"rx_lcrc", 0x0, 0x8, 0x0},
	{"rx_ecrc", 0x0, 0xb, 0x0},
};

static ssize_t dbg_lane_detect_read(struct file *file, char __user *buf,
				    size_t count, loff_t *ppos)
{
	struct dw_pcie *pci = file->private_data;
	struct rasdes_info *rinfo = pci->dump_info;
	u32 val;
	ssize_t off = 0;
	char debugfs_buf[DWC_DEBUGFS_MAX];

	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap + SD_STATUS_L1LANE_REG);
	val = (val >> LANE_DETECT_SHIFT) & LANE_DETECT_MASK;
	if (val)
		off += scnprintf(debugfs_buf, DWC_DEBUGFS_MAX - off,
				 "Detected\n");
	else
		off += scnprintf(debugfs_buf, DWC_DEBUGFS_MAX - off,
				 "Undetected\n");

	return simple_read_from_buffer(buf, count, ppos, debugfs_buf, off);
}

static ssize_t dbg_lane_detect_write(struct file *file, const char __user *buf,
				     size_t count, loff_t *ppos)
{
	struct dw_pcie *pci = file->private_data;
	struct rasdes_info *rinfo = pci->dump_info;
	u32 val;
	u32 lane;

	val = kstrtou32_from_user(buf, count, 0, &lane);
	if (val)
		return val;

	if (lane > 15)
		return -EINVAL;

	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap + SD_STATUS_L1LANE_REG);
	val &= ~LANE_SELECT_MASK;
	val |= lane;
	dw_pcie_writel_dbi(pci, rinfo->ras_cap + SD_STATUS_L1LANE_REG, val);

	return count;
}

static ssize_t dbg_rx_valid_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct dw_pcie *pci = file->private_data;
	struct rasdes_info *rinfo = pci->dump_info;
	u32 val;
	ssize_t off = 0;
	char debugfs_buf[DWC_DEBUGFS_MAX];

	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap + SD_STATUS_L1LANE_REG);
	val = (val >> PIPE_RXVALID_SHIFT) & PIPE_RXVALID_MASK;
	if (val)
		off += scnprintf(debugfs_buf, DWC_DEBUGFS_MAX - off,
				 "Valid\n");
	else
		off += scnprintf(debugfs_buf, DWC_DEBUGFS_MAX - off,
				 "Invalid\n");

	return simple_read_from_buffer(buf, count, ppos, debugfs_buf, off);
}

static ssize_t dbg_rx_valid_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	return dbg_lane_detect_write(file, buf, count, ppos);
}

static void set_event_number(struct rasdes_priv *pdata, struct dw_pcie *pci,
			     struct rasdes_info *rinfo)
{
	u32 val;

	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap +
				RAS_DES_EVENT_COUNTER_CTRL_REG);
	val &= ~EVENT_COUNTER_ENABLE;
	val &= ~(0xFFF << 16);
	val |= (event_counters[pdata->idx].group_no << 24);
	val |= (event_counters[pdata->idx].event_no << 16);
	dw_pcie_writel_dbi(pci, rinfo->ras_cap +
			   RAS_DES_EVENT_COUNTER_CTRL_REG, val);
}

static ssize_t cnt_en_read(struct file *file, char __user *buf, size_t count,
			   loff_t *ppos)
{
	struct rasdes_priv *pdata = file->private_data;
	struct dw_pcie *pci = pdata->pci;
	struct rasdes_info *rinfo = pci->dump_info;
	u32 val;
	ssize_t off = 0;
	char debugfs_buf[DWC_DEBUGFS_MAX];

	mutex_lock(&rinfo->dbg_mutex);
	set_event_number(pdata, pci, rinfo);
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap +
				RAS_DES_EVENT_COUNTER_CTRL_REG);
	mutex_unlock(&rinfo->dbg_mutex);
	val = (val >> EVENT_COUNTER_STATUS_SHIFT) & EVENT_COUNTER_STATUS_MASK;
	if (val)
		off += scnprintf(debugfs_buf, DWC_DEBUGFS_MAX - off,
				 "Enabled\n");
	else
		off += scnprintf(debugfs_buf, DWC_DEBUGFS_MAX - off,
				 "Disabled\n");

	return simple_read_from_buffer(buf, count, ppos, debugfs_buf, off);
}

static ssize_t cnt_en_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct rasdes_priv *pdata = file->private_data;
	struct dw_pcie *pci = pdata->pci;
	struct rasdes_info *rinfo = pci->dump_info;
	u32 val;
	u32 enable;

	val = kstrtou32_from_user(buf, count, 0, &enable);
	if (val)
		return val;

	mutex_lock(&rinfo->dbg_mutex);
	set_event_number(pdata, pci, rinfo);
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap +
				RAS_DES_EVENT_COUNTER_CTRL_REG);
	if (enable)
		val |= PER_EVENT_ON;
	else
		val |= PER_EVENT_OFF;

	dw_pcie_writel_dbi(pci, rinfo->ras_cap +
			   RAS_DES_EVENT_COUNTER_CTRL_REG, val);
	mutex_unlock(&rinfo->dbg_mutex);

	return count;
}

static ssize_t cnt_lane_read(struct file *file, char __user *buf, size_t count,
			     loff_t *ppos)
{
	struct rasdes_priv *pdata = file->private_data;
	struct dw_pcie *pci = pdata->pci;
	struct rasdes_info *rinfo = pci->dump_info;
	u32 val;
	ssize_t off = 0;
	char debugfs_buf[DWC_DEBUGFS_MAX];

	mutex_lock(&rinfo->dbg_mutex);
	set_event_number(pdata, pci, rinfo);
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap +
				RAS_DES_EVENT_COUNTER_CTRL_REG);
	mutex_unlock(&rinfo->dbg_mutex);
	val = (val >> LANE_SELECT_SHIFT) & LANE_SELECT_MASK;
	off += scnprintf(debugfs_buf, DWC_DEBUGFS_MAX - off,
				 "Lane: %d\n", val);

	return simple_read_from_buffer(buf, count, ppos, debugfs_buf, off);
}

static ssize_t cnt_lane_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct rasdes_priv *pdata = file->private_data;
	struct dw_pcie *pci = pdata->pci;
	struct rasdes_info *rinfo = pci->dump_info;
	u32 val;
	u32 lane;

	val = kstrtou32_from_user(buf, count, 0, &lane);
	if (val)
		return val;

	if (lane > 15)
		return -EINVAL;

	mutex_lock(&rinfo->dbg_mutex);
	set_event_number(pdata, pci, rinfo);
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap +
				RAS_DES_EVENT_COUNTER_CTRL_REG);
	val &= ~(LANE_SELECT_MASK << LANE_SELECT_SHIFT);
	val |= (lane << LANE_SELECT_SHIFT);
	dw_pcie_writel_dbi(pci, rinfo->ras_cap +
			   RAS_DES_EVENT_COUNTER_CTRL_REG, val);
	mutex_unlock(&rinfo->dbg_mutex);

	return count;
}

static ssize_t cnt_val_read(struct file *file, char __user *buf, size_t count,
			    loff_t *ppos)
{
	struct rasdes_priv *pdata = file->private_data;
	struct dw_pcie *pci = pdata->pci;
	struct rasdes_info *rinfo = pci->dump_info;
	u32 val;
	ssize_t off = 0;
	char debugfs_buf[DWC_DEBUGFS_MAX];

	mutex_lock(&rinfo->dbg_mutex);
	set_event_number(pdata, pci, rinfo);
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap +
				RAS_DES_EVENT_COUNTER_DATA_REG);
	mutex_unlock(&rinfo->dbg_mutex);
	off += scnprintf(debugfs_buf, DWC_DEBUGFS_MAX - off,
				 "Value: %d\n", val);

	return simple_read_from_buffer(buf, count, ppos, debugfs_buf, off);
}

static ssize_t err_inj_read(struct file *file, char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct rasdes_priv *pdata = file->private_data;
	struct dw_pcie *pci = pdata->pci;
	struct rasdes_info *rinfo = pci->dump_info;
	u32 val;
	ssize_t off = 0;
	char debugfs_buf[DWC_DEBUGFS_MAX];

	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap + ERR_INJ0_OFF +
			(0x4 * err_inj_list[pdata->idx].err_inj_group));
	val &= EINJ_COUNT_MASK;
	off += scnprintf(debugfs_buf, DWC_DEBUGFS_MAX - off,
				 "Count: %d\n", val);

	return simple_read_from_buffer(buf, count, ppos, debugfs_buf, off);
}

static ssize_t err_inj_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct rasdes_priv *pdata = file->private_data;
	struct dw_pcie *pci = pdata->pci;
	struct rasdes_info *rinfo = pci->dump_info;
	u32 val;
	u32 counter;

	val = kstrtou32_from_user(buf, count, 0, &counter);
	if (val)
		return val;

	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap + ERR_INJ0_OFF +
			(0x4 * err_inj_list[pdata->idx].err_inj_group));
	val &= ~(EINJ_TYPE_MASK << EINJ_TYPE_SHIFT);
	val |= err_inj_list[pdata->idx].err_inj_type << EINJ_TYPE_SHIFT;
	val &= ~(EINJ_INFO_MASK << EINJ_INFO_SHIFT);
	val |= err_inj_list[pdata->idx].err_inj_12_31 << EINJ_INFO_SHIFT;
	val &= ~EINJ_COUNT_MASK;
	val |= counter;
	dw_pcie_writel_dbi(pci, rinfo->ras_cap + ERR_INJ0_OFF +
			(0x4 * err_inj_list[pdata->idx].err_inj_group), val);
	dw_pcie_writel_dbi(pci, rinfo->ras_cap + ERR_INJ_ENABLE_REG,
			   (0x1 << err_inj_list[pdata->idx].err_inj_group));

	return count;
}

#define dwc_debugfs_create(name)			\
debugfs_create_file(#name, 0644, rasdes_debug, pci,	\
			&dbg_ ## name ## _fops)

#define DWC_DEBUGFS_FOPS(name)					\
static const struct file_operations dbg_ ## name ## _fops = {	\
	.read = dbg_ ## name ## _read,				\
	.write = dbg_ ## name ## _write				\
}

DWC_DEBUGFS_FOPS(lane_detect);
DWC_DEBUGFS_FOPS(rx_valid);

static const struct file_operations cnt_en_ops = {
	.open = simple_open,
	.read = cnt_en_read,
	.write = cnt_en_write,
};

static const struct file_operations cnt_lane_ops = {
	.open = simple_open,
	.read = cnt_lane_read,
	.write = cnt_lane_write,
};

static const struct file_operations cnt_val_ops = {
	.open = simple_open,
	.read = cnt_val_read,
};

static const struct file_operations err_inj_ops = {
	.open = simple_open,
	.read = err_inj_read,
	.write = err_inj_write,
};

void dwc_pcie_rasdes_debugfs_deinit(struct dw_pcie *pci)
{
	struct rasdes_info *rinfo = pci->dump_info;

	debugfs_remove_recursive(rinfo->rasdes);
	mutex_destroy(&rinfo->dbg_mutex);
}

int dwc_pcie_rasdes_debugfs_init(struct dw_pcie *pci)
{
	struct device *dev = pci->dev;
	int ras_cap;
	struct rasdes_info *dump_info;
	char dirname[DWC_DEBUGFS_MAX];
	struct dentry *dir, *rasdes_debug, *rasdes_err_inj;
	struct dentry *rasdes_event_counter, *rasdes_events;
	int i;
	struct rasdes_priv *priv_tmp;

	ras_cap = dw_pcie_find_vsec_capability(pci, DW_PCIE_RAS_DES_CAP);
	if (!ras_cap) {
		dev_err(dev, "No RASDES capability available\n");
		return -ENODEV;
	}

	dump_info = devm_kzalloc(dev, sizeof(*dump_info), GFP_KERNEL);
	if (!dump_info)
		return -ENOMEM;

	/* Create main directory for each platform driver */
	sprintf(dirname, "pcie_dwc_%s", dev_name(dev));
	dir = debugfs_create_dir(dirname, NULL);

	/* Create subdirectories for Debug, Error injection, Statistics */
	rasdes_debug = debugfs_create_dir("rasdes_debug", dir);
	rasdes_err_inj = debugfs_create_dir("rasdes_err_inj", dir);
	rasdes_event_counter = debugfs_create_dir("rasdes_event_counter", dir);

	mutex_init(&dump_info->dbg_mutex);
	dump_info->ras_cap = ras_cap;
	dump_info->rasdes = dir;
	pci->dump_info = dump_info;

	/* Create debugfs files for Debug subdirectory */
	dwc_debugfs_create(lane_detect);
	dwc_debugfs_create(rx_valid);

	/* Create debugfs files for Error injection subdirectory */
	for (i = 0; i < ARRAY_SIZE(err_inj_list); i++) {
		priv_tmp = devm_kzalloc(dev, sizeof(*priv_tmp), GFP_KERNEL);
		if (!priv_tmp)
			goto err;

		priv_tmp->idx = i;
		priv_tmp->pci = pci;
		debugfs_create_file(err_inj_list[i].name, 0644,
				    rasdes_err_inj, priv_tmp, &err_inj_ops);
	}

	/* Create debugfs files for Statistical counter subdirectory */
	for (i = 0; i < ARRAY_SIZE(event_counters); i++) {
		priv_tmp = devm_kzalloc(dev, sizeof(*priv_tmp), GFP_KERNEL);
		if (!priv_tmp)
			goto err;

		priv_tmp->idx = i;
		priv_tmp->pci = pci;
		rasdes_events = debugfs_create_dir(event_counters[i].name,
						   rasdes_event_counter);
		if (event_counters[i].group_no == 0) {
			debugfs_create_file("lane_select", 0644, rasdes_events,
					    priv_tmp, &cnt_lane_ops);
		}
		debugfs_create_file("counter_value", 0644, rasdes_events, priv_tmp,
				    &cnt_val_ops);
		debugfs_create_file("counter_enable", 0444, rasdes_events, priv_tmp,
				    &cnt_en_ops);
	}

	return 0;
err:
	dwc_pcie_rasdes_debugfs_deinit(pci);
	return -ENOMEM;
}
