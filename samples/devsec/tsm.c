/* SPDX-License-Identifier: GPL-2.0-only */
// Copyright(c) 2024 Intel Corporation. All rights reserved.

#define dev_fmt(fmt) "devsec: " fmt
#include <linux/platform_device.h>
#include <linux/pci-tsm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/tsm.h>
#include "devsec.h"

struct devsec_dsm {
	struct pci_dsm pci;
};

static struct devsec_dsm *to_devsec_dsm(struct pci_dsm *dsm)
{
	return container_of(dsm, struct devsec_dsm, pci);
}

static struct pci_dsm *devsec_tsm_pci_probe(struct pci_dev *pdev)
{
	struct devsec_dsm *devsec_dsm;

	if (pdev->sysdata != devsec_sysdata)
		return NULL;

	devsec_dsm = kzalloc(sizeof(*devsec_dsm), GFP_KERNEL);
	if (!devsec_dsm)
		return NULL;

	devsec_dsm->pci.pdev = pdev;
	pci_dbg(pdev, "tsm enabled\n");
	return &devsec_dsm->pci;
}

static void devsec_tsm_pci_remove(struct pci_dsm *dsm)
{
	struct devsec_dsm *devsec_dsm = to_devsec_dsm(dsm);

	pci_dbg(dsm->pdev, "tsm disabled\n");
	kfree(devsec_dsm);
}

static int devsec_tsm_connect(struct pci_dev *pdev)
{
	return -ENXIO;
}

static void devsec_tsm_disconnect(struct pci_dev *pdev)
{
}

static const struct pci_tsm_ops devsec_pci_ops = {
	.probe = devsec_tsm_pci_probe,
	.remove = devsec_tsm_pci_remove,
	.connect = devsec_tsm_connect,
	.disconnect = devsec_tsm_disconnect,
};

static void devsec_tsm_remove(void *tsm)
{
	tsm_unregister(tsm);
}

static int devsec_tsm_probe(struct platform_device *pdev)
{
	struct tsm_subsys *tsm;

	tsm = tsm_register(&pdev->dev, NULL, &devsec_pci_ops);
	if (IS_ERR(tsm))
		return PTR_ERR(tsm);

	return devm_add_action_or_reset(&pdev->dev, devsec_tsm_remove,
					tsm);
}

static struct platform_driver devsec_tsm_driver = {
	.driver = {
		.name = "devsec_tsm",
	},
};

static struct platform_device *devsec_tsm;

static int __init devsec_tsm_init(void)
{
	struct platform_device_info devsec_tsm_info = {
		.name = "devsec_tsm",
		.id = -1,
	};
	int rc;

	devsec_tsm = platform_device_register_full(&devsec_tsm_info);
	if (IS_ERR(devsec_tsm))
		return PTR_ERR(devsec_tsm);

	rc = platform_driver_probe(&devsec_tsm_driver, devsec_tsm_probe);
	if (rc)
		platform_device_unregister(devsec_tsm);
	return rc;
}
module_init(devsec_tsm_init);

static void __exit devsec_tsm_exit(void)
{
	platform_driver_unregister(&devsec_tsm_driver);
	platform_device_unregister(devsec_tsm);
}
module_exit(devsec_tsm_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Device Security Sample Infrastructure: Platform TSM Driver");
