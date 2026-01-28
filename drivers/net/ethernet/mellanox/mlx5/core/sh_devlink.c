// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#include <linux/mlx5/driver.h>
#include <net/devlink.h>

#include "sh_devlink.h"

static const struct devlink_ops mlx5_shd_ops = {
};

int mlx5_shd_init(struct mlx5_core_dev *dev)
{
	struct pci_dev *pdev = dev->pdev;
	unsigned int vpd_size, kw_len;
	struct devlink *devlink;
	const char *sn;
	u8 *vpd_data;
	int err = 0;
	char *end;
	int start;

	if (!mlx5_core_is_pf(dev))
		return 0;

	vpd_data = pci_vpd_alloc(pdev, &vpd_size);
	if (IS_ERR(vpd_data)) {
		err = PTR_ERR(vpd_data);
		return err == -ENODEV ? 0 : err;
	}
	start = pci_vpd_find_ro_info_keyword(vpd_data, vpd_size, "V3", &kw_len);
	if (start < 0) {
		/* Fall-back to SN for older devices. */
		start = pci_vpd_find_ro_info_keyword(vpd_data, vpd_size,
						     PCI_VPD_RO_KEYWORD_SERIALNO,
						     &kw_len);
		if (start < 0) {
			err = -ENOENT;
			goto out;
		}
	}
	sn = kstrndup(vpd_data + start, kw_len, GFP_KERNEL);
	if (!sn) {
		err = -ENOMEM;
		goto out;
	}
	/* Firmware may return spaces at the end of the string, strip it. */
	end = strchrnul(sn, ' ');
	*end = '\0';

	/* Get or create shared devlink instance */
	devlink = devlink_shd_get(sn, &mlx5_shd_ops, 0);
	kfree(sn);
	if (!devlink) {
		err = -ENOMEM;
		goto out;
	}

	dev->shd = devlink;
out:
	kfree(vpd_data);
	return err;
}

void mlx5_shd_uninit(struct mlx5_core_dev *dev)
{
	if (!dev->shd)
		return;

	devlink_shd_put(dev->shd);
}
