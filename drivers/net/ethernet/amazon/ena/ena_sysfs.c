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

/******************************************************************************
 *****************************************************************************/
int ena_sysfs_init(struct device *dev)
{
	if (device_create_file(dev, &dev_attr_phc_enable))
		dev_err(dev, "Failed to create phc_enable sysfs entry");

	return 0;
}

/******************************************************************************
 *****************************************************************************/
void ena_sysfs_terminate(struct device *dev)
{
	device_remove_file(dev, &dev_attr_phc_enable);
}
