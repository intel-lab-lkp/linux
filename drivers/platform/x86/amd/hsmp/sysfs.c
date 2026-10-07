// SPDX-License-Identifier: GPL-2.0
/*
 * AMD HSMP sysfs support
 * Copyright (c) 2024, AMD.
 * All Rights Reserved.
 *
 * This file provides the per-socket sysfs attributes of the HSMP interface.
 */

#include <asm/amd/hsmp.h>

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/sysfs.h>

#include "hsmp.h"

struct hsmp_sys_attr {
	struct device_attribute dattr;
	u32 msg_id;
};

static ssize_t hsmp_metric_tbl_acpi_read(struct file *filp, struct kobject *kobj,
					 const struct bin_attribute *bin_attr, char *buf,
					 loff_t off, size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct hsmp_socket *sock = dev_get_drvdata(dev);

	/*
	 * metrics_bin is a sysfs binary attribute and is capped at PAGE_SIZE.
	 * It can therefore only carry the protocol version 6 metric table
	 * (struct hsmp_metric_table).  The larger tables defined from protocol
	 * version 7 onwards do not fit; userspace on those systems must read
	 * the snapshot through HSMP_IOCTL_GET_TELEMETRY_DATA on /dev/hsmp.
	 * Surface the unsupported case here as -EOPNOTSUPP rather than
	 * silently truncating the snapshot.
	 */
	if (get_hsmp_pdev()->proto_ver != HSMP_PROTO_VER6)
		return -EOPNOTSUPP;

	return hsmp_metric_tbl_read(sock, buf, count);
}

static umode_t hsmp_is_sock_attr_visible(struct kobject *kobj,
					 const struct bin_attribute *battr, int id)
{
	/*
	 * Keep metrics_bin visible on protocol version 7 and later as well,
	 * so that userspace which expects the file to exist gets a clear
	 * -EOPNOTSUPP from the read handler instead of -ENOENT, and is
	 * pointed at HSMP_IOCTL_GET_TELEMETRY_DATA as the supported path.
	 */
	if (get_hsmp_pdev()->proto_ver >= HSMP_PROTO_VER6)
		return battr->attr.mode;

	return 0;
}

static umode_t hsmp_is_sock_dev_attr_visible(struct kobject *kobj,
					     struct attribute *attr, int id)
{
	return attr->mode;
}

#define to_hsmp_sys_attr(_attr) container_of(_attr, struct hsmp_sys_attr, dattr)

static ssize_t hsmp_msg_resp32_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	struct hsmp_sys_attr *hattr = to_hsmp_sys_attr(attr);
	struct hsmp_socket *sock = dev_get_drvdata(dev);
	u32 data;
	int ret;

	ret = hsmp_msg_get_nargs(sock->sock_ind, hattr->msg_id, &data, 1);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", data);
}

#define DDR_MAX_BW_MASK		GENMASK(31, 20)
#define DDR_UTIL_BW_MASK	GENMASK(19, 8)
#define DDR_UTIL_BW_PERC_MASK	GENMASK(7, 0)
#define FW_VER_MAJOR_MASK	GENMASK(23, 16)
#define FW_VER_MINOR_MASK	GENMASK(15, 8)
#define FW_VER_DEBUG_MASK	GENMASK(7, 0)
#define FMAX_MASK		GENMASK(31, 16)
#define FMIN_MASK		GENMASK(15, 0)
#define FREQ_LIMIT_MASK		GENMASK(31, 16)
#define FREQ_SRC_IND_MASK	GENMASK(15, 0)

static ssize_t hsmp_ddr_max_bw_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	struct hsmp_sys_attr *hattr = to_hsmp_sys_attr(attr);
	struct hsmp_socket *sock = dev_get_drvdata(dev);
	u32 data;
	int ret;

	ret = hsmp_msg_get_nargs(sock->sock_ind, hattr->msg_id, &data, 1);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%lu\n", FIELD_GET(DDR_MAX_BW_MASK, data));
}

static ssize_t hsmp_ddr_util_bw_show(struct device *dev, struct device_attribute *attr,
				     char *buf)
{
	struct hsmp_sys_attr *hattr = to_hsmp_sys_attr(attr);
	struct hsmp_socket *sock = dev_get_drvdata(dev);
	u32 data;
	int ret;

	ret = hsmp_msg_get_nargs(sock->sock_ind, hattr->msg_id, &data, 1);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%lu\n", FIELD_GET(DDR_UTIL_BW_MASK, data));
}

static ssize_t hsmp_ddr_util_bw_perc_show(struct device *dev, struct device_attribute *attr,
					  char *buf)
{
	struct hsmp_sys_attr *hattr = to_hsmp_sys_attr(attr);
	struct hsmp_socket *sock = dev_get_drvdata(dev);
	u32 data;
	int ret;

	ret = hsmp_msg_get_nargs(sock->sock_ind, hattr->msg_id, &data, 1);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%lu\n", FIELD_GET(DDR_UTIL_BW_PERC_MASK, data));
}

static ssize_t hsmp_msg_fw_ver_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	struct hsmp_sys_attr *hattr = to_hsmp_sys_attr(attr);
	struct hsmp_socket *sock = dev_get_drvdata(dev);
	u32 data;
	int ret;

	ret = hsmp_msg_get_nargs(sock->sock_ind, hattr->msg_id, &data, 1);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%lu.%lu.%lu\n",
			  FIELD_GET(FW_VER_MAJOR_MASK, data),
			  FIELD_GET(FW_VER_MINOR_MASK, data),
			  FIELD_GET(FW_VER_DEBUG_MASK, data));
}

static ssize_t hsmp_fclk_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct hsmp_sys_attr *hattr = to_hsmp_sys_attr(attr);
	struct hsmp_socket *sock = dev_get_drvdata(dev);
	u32 data[2];
	int ret;

	ret = hsmp_msg_get_nargs(sock->sock_ind, hattr->msg_id, data, 2);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", data[0]);
}

static ssize_t hsmp_mclk_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct hsmp_sys_attr *hattr = to_hsmp_sys_attr(attr);
	struct hsmp_socket *sock = dev_get_drvdata(dev);
	u32 data[2];
	int ret;

	ret = hsmp_msg_get_nargs(sock->sock_ind, hattr->msg_id, data, 2);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", data[1]);
}

static ssize_t hsmp_clk_fmax_show(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	struct hsmp_sys_attr *hattr = to_hsmp_sys_attr(attr);
	struct hsmp_socket *sock = dev_get_drvdata(dev);
	u32 data;
	int ret;

	ret = hsmp_msg_get_nargs(sock->sock_ind, hattr->msg_id, &data, 1);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%lu\n", FIELD_GET(FMAX_MASK, data));
}

static ssize_t hsmp_clk_fmin_show(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	struct hsmp_sys_attr *hattr = to_hsmp_sys_attr(attr);
	struct hsmp_socket *sock = dev_get_drvdata(dev);
	u32 data;
	int ret;

	ret = hsmp_msg_get_nargs(sock->sock_ind, hattr->msg_id, &data, 1);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%lu\n", FIELD_GET(FMIN_MASK, data));
}

static ssize_t hsmp_freq_limit_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	struct hsmp_sys_attr *hattr = to_hsmp_sys_attr(attr);
	struct hsmp_socket *sock = dev_get_drvdata(dev);
	u32 data;
	int ret;

	ret = hsmp_msg_get_nargs(sock->sock_ind, hattr->msg_id, &data, 1);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%lu\n", FIELD_GET(FREQ_LIMIT_MASK, data));
}

static const char * const freqlimit_srcnames[] = {
	"cHTC-Active",
	"PROCHOT",
	"TDC limit",
	"PPT Limit",
	"OPN Max",
	"Reliability Limit",
	"APML Agent",
	"HSMP Agent",
};

static ssize_t hsmp_freq_limit_source_show(struct device *dev, struct device_attribute *attr,
					   char *buf)
{
	struct hsmp_sys_attr *hattr = to_hsmp_sys_attr(attr);
	struct hsmp_socket *sock = dev_get_drvdata(dev);
	unsigned int index;
	int len = 0;
	u16 src_ind;
	u32 data;
	int ret;

	ret = hsmp_msg_get_nargs(sock->sock_ind, hattr->msg_id, &data, 1);
	if (ret)
		return ret;

	src_ind = FIELD_GET(FREQ_SRC_IND_MASK, data);
	for (index = 0; index < ARRAY_SIZE(freqlimit_srcnames); index++) {
		if (!src_ind)
			break;
		if (src_ind & 1)
			len += sysfs_emit_at(buf, len, "%s\n", freqlimit_srcnames[index]);
		src_ind >>= 1;
	}
	return len;
}

static const struct bin_attribute  hsmp_metric_tbl_attr = {
	.attr = { .name = HSMP_METRICS_TABLE_NAME, .mode = 0444},
	.read = hsmp_metric_tbl_acpi_read,
	.size = sizeof(struct hsmp_metric_table),
};

static const struct bin_attribute *hsmp_attr_list[] = {
	&hsmp_metric_tbl_attr,
	NULL
};

#define HSMP_DEV_ATTR(_name, _msg_id, _show, _mode)	\
static struct hsmp_sys_attr hattr_##_name = {		\
	.dattr = __ATTR(_name, _mode, _show, NULL),	\
	.msg_id = _msg_id,				\
}

HSMP_DEV_ATTR(c0_residency_input, HSMP_GET_C0_PERCENT, hsmp_msg_resp32_show, 0444);
HSMP_DEV_ATTR(prochot_status, HSMP_GET_PROC_HOT, hsmp_msg_resp32_show, 0444);
HSMP_DEV_ATTR(smu_fw_version, HSMP_GET_SMU_VER, hsmp_msg_fw_ver_show, 0444);
HSMP_DEV_ATTR(protocol_version, HSMP_GET_PROTO_VER, hsmp_msg_resp32_show, 0444);
HSMP_DEV_ATTR(cclk_freq_limit_input, HSMP_GET_CCLK_THROTTLE_LIMIT, hsmp_msg_resp32_show, 0444);
HSMP_DEV_ATTR(ddr_max_bw, HSMP_GET_DDR_BANDWIDTH, hsmp_ddr_max_bw_show, 0444);
HSMP_DEV_ATTR(ddr_utilised_bw_input, HSMP_GET_DDR_BANDWIDTH, hsmp_ddr_util_bw_show, 0444);
HSMP_DEV_ATTR(ddr_utilised_bw_perc_input, HSMP_GET_DDR_BANDWIDTH, hsmp_ddr_util_bw_perc_show, 0444);
HSMP_DEV_ATTR(fclk_input, HSMP_GET_FCLK_MCLK, hsmp_fclk_show, 0444);
HSMP_DEV_ATTR(mclk_input, HSMP_GET_FCLK_MCLK, hsmp_mclk_show, 0444);
HSMP_DEV_ATTR(clk_fmax, HSMP_GET_SOCKET_FMAX_FMIN, hsmp_clk_fmax_show, 0444);
HSMP_DEV_ATTR(clk_fmin, HSMP_GET_SOCKET_FMAX_FMIN, hsmp_clk_fmin_show, 0444);
HSMP_DEV_ATTR(pwr_current_active_freq_limit, HSMP_GET_SOCKET_FREQ_LIMIT,
	      hsmp_freq_limit_show, 0444);
HSMP_DEV_ATTR(pwr_current_active_freq_limit_source, HSMP_GET_SOCKET_FREQ_LIMIT,
	      hsmp_freq_limit_source_show, 0444);

static struct attribute *hsmp_dev_attr_list[] = {
	&hattr_c0_residency_input.dattr.attr,
	&hattr_prochot_status.dattr.attr,
	&hattr_smu_fw_version.dattr.attr,
	&hattr_protocol_version.dattr.attr,
	&hattr_cclk_freq_limit_input.dattr.attr,
	&hattr_ddr_max_bw.dattr.attr,
	&hattr_ddr_utilised_bw_input.dattr.attr,
	&hattr_ddr_utilised_bw_perc_input.dattr.attr,
	&hattr_fclk_input.dattr.attr,
	&hattr_mclk_input.dattr.attr,
	&hattr_clk_fmax.dattr.attr,
	&hattr_clk_fmin.dattr.attr,
	&hattr_pwr_current_active_freq_limit.dattr.attr,
	&hattr_pwr_current_active_freq_limit_source.dattr.attr,
	NULL
};

static const struct attribute_group hsmp_attr_grp = {
	.bin_attrs = hsmp_attr_list,
	.attrs = hsmp_dev_attr_list,
	.is_bin_visible = hsmp_is_sock_attr_visible,
	.is_visible = hsmp_is_sock_dev_attr_visible,
};

/*
 * Per-socket attribute groups. Every show/read handler expects the drvdata of
 * the device carrying the group to be the struct hsmp_socket it reports on.
 */
const struct attribute_group *hsmp_sock_groups[] = {
	&hsmp_attr_grp,
	NULL
};
EXPORT_SYMBOL_NS_GPL(hsmp_sock_groups, "AMD_HSMP");
