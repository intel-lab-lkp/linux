// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, NVIDIA CORPORATION.
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include <soc/tegra/bpmp.h>
#include <soc/tegra/bpmp-abi.h>

#include "bpmp-private.h"

/* Documented sysfs / ABI bounds; firmware may still reject a request. */
#define TEGRA_BPMP_MBWT_INSTANCE_MAX	5U
#define TEGRA_BPMP_MBWT_VC_MAX		2U
#define TEGRA_BPMP_MBWT_BW_MIN		1U
#define TEGRA_BPMP_MBWT_BW_MAX		110U

struct tegra_bpmp_mbwt_sysfs {
	struct device_attribute dev_attr;
	struct tegra_bpmp *bpmp;
	/* Serializes bandwidth I/O. */
	struct mutex lock;
};

static struct tegra_bpmp_mbwt_sysfs *
tegra_bpmp_mbwt_sysfs_from_attr(struct device_attribute *attr)
{
	return container_of(attr, struct tegra_bpmp_mbwt_sysfs, dev_attr);
}

static int tegra_bpmp_mbwt_valid_tuple(unsigned int instance,
				       unsigned int vc_type,
				       unsigned int bandwidth)
{
	if (instance > TEGRA_BPMP_MBWT_INSTANCE_MAX)
		return -EINVAL;
	if (vc_type > TEGRA_BPMP_MBWT_VC_MAX)
		return -EINVAL;
	if (bandwidth < TEGRA_BPMP_MBWT_BW_MIN ||
	    bandwidth > TEGRA_BPMP_MBWT_BW_MAX)
		return -EINVAL;

	return 0;
}

static int tegra_bpmp_mbwt_parse(const char *buf, size_t count,
				 unsigned int *instance,
				 unsigned int *vc_type,
				 unsigned int *bandwidth)
{
	unsigned int values[3];
	char *copy, *cur, *tok;
	unsigned int i = 0;
	int err = 0;

	copy = kmemdup_nul(buf, count, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;

	cur = strim(copy);
	while ((tok = strsep(&cur, ",")) != NULL) {
		if (i >= ARRAY_SIZE(values)) {
			err = -EINVAL;
			goto out;
		}

		tok = strim(tok);
		if (!*tok) {
			err = -EINVAL;
			goto out;
		}

		err = kstrtou32(tok, 0, &values[i]);
		if (err)
			goto out;

		i++;
	}

	if (i != ARRAY_SIZE(values)) {
		err = -EINVAL;
		goto out;
	}

	*instance = values[0];
	*vc_type = values[1];
	*bandwidth = values[2];
	err = 0;

out:
	kfree(copy);
	return err;
}

static ssize_t bandwidth_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct tegra_bpmp_mbwt_sysfs *mbwt;
	unsigned int instance, vc_type, bandwidth;
	ssize_t len = 0;
	int err;

	mbwt = tegra_bpmp_mbwt_sysfs_from_attr(attr);

	mutex_lock(&mbwt->lock);
	for (instance = 0; instance <= TEGRA_BPMP_MBWT_INSTANCE_MAX; instance++) {
		for (vc_type = 0; vc_type <= TEGRA_BPMP_MBWT_VC_MAX; vc_type++) {
			err = tegra410_bpmp_mbwt_get(mbwt->bpmp, instance,
						     vc_type, &bandwidth);
			if (err) {
				mutex_unlock(&mbwt->lock);
				return err;
			}

			len += sysfs_emit_at(buf, len, "%u,%u,%u\n", instance,
					     vc_type, bandwidth);
		}
	}
	mutex_unlock(&mbwt->lock);

	return len;
}

static ssize_t bandwidth_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct tegra_bpmp_mbwt_sysfs *mbwt;
	unsigned int instance, vc_type, bandwidth;
	int err;

	err = tegra_bpmp_mbwt_parse(buf, count, &instance, &vc_type,
				    &bandwidth);
	if (err)
		return err;

	err = tegra_bpmp_mbwt_valid_tuple(instance, vc_type, bandwidth);
	if (err)
		return err;

	mbwt = tegra_bpmp_mbwt_sysfs_from_attr(attr);

	mutex_lock(&mbwt->lock);
	err = tegra410_bpmp_mbwt_set(mbwt->bpmp, instance, vc_type, bandwidth);
	mutex_unlock(&mbwt->lock);
	if (err)
		return err;

	return count;
}

static void tegra_bpmp_mbwt_sysfs_teardown(void *data)
{
	struct tegra_bpmp_mbwt_sysfs *mbwt = data;

	device_remove_file(mbwt->bpmp->dev, &mbwt->dev_attr);
}

int tegra_bpmp_sysfs_register(struct tegra_bpmp *bpmp)
{
	struct tegra_bpmp_mbwt_sysfs *mbwt;
	int err;

	if (!ACPI_HANDLE(bpmp->dev))
		return 0;

	if (!tegra_bpmp_mrq_is_supported(bpmp, MRQ_SOCHUB_MBWT))
		return 0;

	/*
	 * MRQ_QUERY_ABI only confirms that the MBWT MRQ is implemented. The
	 * firmware reports GET_BW / SET_BW support through the MBWT ABI query.
	 */
	if (!tegra410_bpmp_mbwt_cmd_is_supported(bpmp, CMD_SOCHUB_MBWT_GET_BW) ||
	    !tegra410_bpmp_mbwt_cmd_is_supported(bpmp, CMD_SOCHUB_MBWT_SET_BW))
		return 0;

	mbwt = devm_kzalloc(bpmp->dev, sizeof(*mbwt), GFP_KERNEL);
	if (!mbwt)
		return -ENOMEM;

	mbwt->bpmp = bpmp;
	mutex_init(&mbwt->lock);

	sysfs_attr_init(&mbwt->dev_attr.attr);
	mbwt->dev_attr.attr.name = "bandwidth";
	mbwt->dev_attr.attr.mode = 0644;
	mbwt->dev_attr.show = bandwidth_show;
	mbwt->dev_attr.store = bandwidth_store;

	err = device_create_file(bpmp->dev, &mbwt->dev_attr);
	if (err)
		return err;

	err = devm_add_action(bpmp->dev, tegra_bpmp_mbwt_sysfs_teardown, mbwt);
	if (err) {
		device_remove_file(bpmp->dev, &mbwt->dev_attr);
		return err;
	}

	return 0;
}
