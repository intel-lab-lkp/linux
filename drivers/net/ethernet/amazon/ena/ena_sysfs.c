// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright 2015-2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/stat.h>
#include <linux/sysfs.h>

#include "ena_com.h"
#include "ena_netdev.h"
#include "ena_phc.h"
#include "ena_sysfs.h"

static ssize_t ena_phc_enable_set(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf,
				  size_t len)
{
	struct ena_adapter *adapter = dev_get_drvdata(dev);
	unsigned long phc_enable_val;
	int rc;

	if (!ena_com_phc_supported(adapter->ena_dev)) {
		netif_info(adapter, drv, adapter->netdev,
			   "Device doesn't support PHC");
		return -EOPNOTSUPP;
	}

	rc = kstrtoul(buf, 10, &phc_enable_val);
	if (rc < 0)
		return rc;

	if (phc_enable_val != 0 && phc_enable_val != 1)
		return -EINVAL;

	rtnl_lock();

	/* No change in state */
	if ((bool)phc_enable_val == ena_phc_is_enabled(adapter))
		goto out;

	ena_phc_enable(adapter, phc_enable_val);

	ena_destroy_device(adapter, false);
	rc = ena_restore_device(adapter);

out:
	rtnl_unlock();
	return rc ? rc : len;
}

#define ENA_PHC_ENABLE_STR_MAX_LEN 3

static ssize_t ena_phc_enable_get(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct ena_adapter *adapter = dev_get_drvdata(dev);

	return snprintf(buf, ENA_PHC_ENABLE_STR_MAX_LEN, "%u\n",
			ena_phc_is_enabled(adapter));
}

static DEVICE_ATTR(phc_enable, S_IRUGO | S_IWUSR, ena_phc_enable_get,
		   ena_phc_enable_set);

#define ENA_STAT_ENA_COM_PHC_ENTRY(stat) { \
	.name = #stat, \
	.stat_offset = offsetof(struct ena_com_stats_phc, stat) / sizeof(u64) \
}

const struct ena_stats ena_stats_ena_com_phc_strings[] = {
	ENA_STAT_ENA_COM_PHC_ENTRY(phc_cnt),
	ENA_STAT_ENA_COM_PHC_ENTRY(phc_exp),
	ENA_STAT_ENA_COM_PHC_ENTRY(phc_skp),
	ENA_STAT_ENA_COM_PHC_ENTRY(phc_err),
};

u16 ena_stats_array_ena_com_phc_size = ARRAY_SIZE(ena_stats_ena_com_phc_strings);

static ssize_t ena_phc_stats_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct ena_adapter *adapter = dev_get_drvdata(dev);
	int i, rc, chars_written = 0;

	if (!ena_phc_is_active(adapter))
		return 0;

	for (i = 0; i < ena_stats_array_ena_com_phc_size; i++) {
		const struct ena_stats *ena_stats;
		u64 *ptr;

		ena_stats = &ena_stats_ena_com_phc_strings[i];
		ptr = (u64 *)&adapter->ena_dev->phc.stats +
		      ena_stats->stat_offset;
		rc = snprintf(buf,
			      ETH_GSTRING_LEN + sizeof(u64),
			      "%s: %llu\n",
			      ena_stats->name,
			      *ptr);

		buf += rc;
		chars_written += rc;
	}

	return chars_written;
}

static DEVICE_ATTR(phc_stats, S_IRUGO, ena_phc_stats_show, NULL);

/******************************************************************************
 *****************************************************************************/
int ena_sysfs_init(struct device *dev)
{
	if (device_create_file(dev, &dev_attr_phc_enable))
		dev_err(dev, "Failed to create phc_enable sysfs entry");

	if (device_create_file(dev, &dev_attr_phc_stats))
		dev_err(dev, "Failed to create phc_stats sysfs entry");

	return 0;
}

/******************************************************************************
 *****************************************************************************/
void ena_sysfs_terminate(struct device *dev)
{
	device_remove_file(dev, &dev_attr_phc_enable);
	device_remove_file(dev, &dev_attr_phc_stats);
}
