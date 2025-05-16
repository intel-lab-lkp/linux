// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024 - 2025 Intel Corporation. All rights reserved. */

#define dev_fmt(fmt) "devsec: " fmt
#include <linux/platform_device.h>
#include <linux/pci-tsm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/tsm.h>
#include "devsec.h"

struct devsec_tsm_pf0 {
	struct pci_tsm_pf0 pci;
#define NR_TSM_STREAMS 4
};

static struct devsec_tsm_pf0 *to_devsec_tsm(struct pci_tsm *tsm)
{
	return container_of(tsm, struct devsec_tsm_pf0, pci.tsm);
}

static struct pci_tsm *devsec_tsm_pci_probe(struct pci_dev *pdev)
{
	int rc;

	if (pdev->sysdata != devsec_sysdata)
		return NULL;

	if (!is_pci_tsm_pf0(pdev))
		return NULL;

	struct devsec_tsm_pf0 *devsec_tsm __free(kfree) =
		kzalloc(sizeof(*devsec_tsm), GFP_KERNEL);
	if (!devsec_tsm)
		return NULL;

	rc = pci_tsm_pf0_initialize(pdev, &devsec_tsm->pci);
	if (rc)
		return NULL;

	pci_dbg(pdev, "tsm enabled\n");
	return &no_free_ptr(devsec_tsm)->pci.tsm;
}

static void devsec_tsm_pci_remove(struct pci_tsm *tsm)
{
	struct devsec_tsm_pf0 *devsec_tsm = to_devsec_tsm(tsm);

	pci_dbg(tsm->pdev, "tsm disabled\n");
	kfree(devsec_tsm);
}

/*
 * Reference consumer for a TSM driver "connect" operation callback. The
 * low-level TSM driver understands details about the platform the PCI
 * core does not, like number of available streams that can be
 * established per host bridge. The expected flow is:
 *
 * 1/ Allocate platform specific Stream resource (TSM specific)
 * 2/ Allocate Stream Ids in the endpoint and Root Port (PCI TSM helper)
 * 3/ Register Stream Ids for the consumed resources from the last 2
 *    steps to be accountable (via sysfs) to the admin (PCI TSM helper)
 * 4/ Register the Stream with the TSM core so that either PCI sysfs or
 *    TSM core sysfs can list the in-use resources (TSM core helper)
 * 5/ Configure IDE settings in the endpoint and Root Port (PCI TSM helper)
 * 6/ RPC call to TSM to perform IDE_KM and optionally enable the stream
 * (TSM Specific)
 * 7/ Enable the stream in the endpoint, and root port if TSM call did
 *    not already handle that (PCI TSM helper)
 *
 * The expectation is the helpers referenceed are convenience "library"
 * APIs for common operations, not a "midlayer" that enforces a specific
 * or use model sequencing.
 */
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

static void devsec_tsm_remove(void *tsm_core)
{
	tsm_unregister(tsm_core);
}

static int devsec_tsm_probe(struct platform_device *pdev)
{
	struct tsm_core_dev *tsm_core;

	tsm_core = tsm_register(&pdev->dev, NULL, &devsec_pci_ops);
	if (IS_ERR(tsm_core))
		return PTR_ERR(tsm_core);

	return devm_add_action_or_reset(&pdev->dev, devsec_tsm_remove,
					tsm_core);
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
