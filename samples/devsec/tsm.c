/* SPDX-License-Identifier: GPL-2.0-only */
// Copyright(c) 2024 Intel Corporation. All rights reserved.

#define dev_fmt(fmt) "devsec: " fmt
#include <linux/platform_device.h>
#include <linux/pci-tsm.h>
#include <linux/pci-ide.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/tsm.h>
#include "devsec.h"

#define DEVSEC_NR_IDE_STREAMS 4

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

/* protected by tsm_ops lock */
static DECLARE_BITMAP(devsec_stream_ids, DEVSEC_NR_IDE_STREAMS);
static struct devsec_stream_info {
	struct pci_dev *pdev;
	struct pci_ide ide;
} devsec_streams[DEVSEC_NR_IDE_STREAMS];

static int devsec_tsm_connect(struct pci_dev *pdev)
{
	struct pci_ide *ide;
	int rc, stream_id;

	stream_id =
		find_first_zero_bit(devsec_stream_ids, DEVSEC_NR_IDE_STREAMS);
	if (stream_id == DEVSEC_NR_IDE_STREAMS)
		return -EBUSY;
	set_bit(stream_id, devsec_stream_ids);
	ide = &devsec_streams[stream_id].ide;
	pci_ide_stream_probe(pdev, ide);

	ide->stream_id = stream_id;
	rc = pci_ide_stream_setup(pdev, ide, PCI_IDE_SETUP_ROOT_PORT);
	if (rc)
		return rc;
	rc = tsm_register_ide_stream(pdev, ide);
	if (rc)
		goto err;

	devsec_streams[stream_id].pdev = pdev;
	pci_ide_enable_stream(pdev, ide);
	return 0;
err:
	pci_ide_stream_teardown(pdev, ide, PCI_IDE_SETUP_ROOT_PORT);
	return rc;
}

static void devsec_tsm_disconnect(struct pci_dev *pdev)
{
	struct pci_ide *ide;
	int i;

	for_each_set_bit(i, devsec_stream_ids, DEVSEC_NR_IDE_STREAMS)
		if (devsec_streams[i].pdev == pdev)
			break;

	if (i >= DEVSEC_NR_IDE_STREAMS)
		return;

	ide = &devsec_streams[i].ide;
	pci_ide_disable_stream(pdev, ide);
	tsm_unregister_ide_stream(ide);
	pci_ide_stream_teardown(pdev, ide, PCI_IDE_SETUP_ROOT_PORT);
	devsec_streams[i].pdev = NULL;
	clear_bit(i, devsec_stream_ids);
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

static void set_nr_ide_streams(int nr)
{
	struct pci_dev *pdev = NULL;

	for_each_pci_dev(pdev) {
		struct pci_host_bridge *hb;

		if (pdev->sysdata != devsec_sysdata)
			continue;
		hb = pci_find_host_bridge(pdev->bus);
		if (hb->nr_ide_streams >= 0)
			continue;
		pci_set_nr_ide_streams(hb, nr);
	}
}

static void devsec_tsm_ide_teardown(void *data)
{
	set_nr_ide_streams(-1);
}

static int devsec_tsm_probe(struct platform_device *pdev)
{
	struct tsm_subsys *tsm;
	int rc;

	tsm = tsm_register(&pdev->dev, NULL, &devsec_pci_ops);
	if (IS_ERR(tsm))
		return PTR_ERR(tsm);

	rc = devm_add_action_or_reset(&pdev->dev, devsec_tsm_remove, tsm);
	if (rc)
		return rc;

	set_nr_ide_streams(DEVSEC_NR_IDE_STREAMS);

	return devm_add_action_or_reset(&pdev->dev, devsec_tsm_ide_teardown,
					NULL);
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

MODULE_IMPORT_NS(PCI_IDE);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Device Security Sample Infrastructure: Platform TSM Driver");
