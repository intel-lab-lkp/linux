// SPDX-License-Identifier: GPL-2.0
/*
 * TDX host user interface driver
 *
 * Copyright (C) 2025 Intel Corporation
 */

#include <linux/device/faux.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/pci.h>
#include <linux/pci-tsm.h>
#include <linux/tsm.h>

#include <asm/cpu_device_id.h>
#include <asm/tdx.h>
#include <asm/tdx_global_metadata.h>

static const struct x86_cpu_id tdx_host_ids[] = {
	X86_MATCH_FEATURE(X86_FEATURE_TDX_HOST_PLATFORM, NULL),
	{}
};
MODULE_DEVICE_TABLE(x86cpu, tdx_host_ids);

/*
 * The global pointer is for features which won't be affected by tdx_sysinfo
 * change after TDX Module update, e.g. TDX Connect, so could cache it. A
 * counterexample is the TDX Module version.
 */
static const struct tdx_sys_info *tdx_sysinfo;

struct tdx_tsm_link {
	struct pci_tsm_pf0 pci;
};

static struct tdx_tsm_link *to_tdx_tsm_link(struct pci_tsm *tsm)
{
	return container_of(tsm, struct tdx_tsm_link, pci.base_tsm);
}

static int tdx_tsm_link_connect(struct pci_dev *pdev)
{
	return -ENXIO;
}

static void tdx_tsm_link_disconnect(struct pci_dev *pdev)
{
}

static struct pci_tsm *tdx_tsm_link_pf0_probe(struct tsm_dev *tsm_dev,
					      struct pci_dev *pdev)
{
	int rc;

	struct tdx_tsm_link *tlink __free(kfree) = kzalloc_obj(*tlink);
	if (!tlink)
		return NULL;

	rc = pci_tsm_pf0_constructor(pdev, &tlink->pci, tsm_dev);
	if (rc)
		return NULL;

	return &no_free_ptr(tlink)->pci.base_tsm;
}

static void tdx_tsm_link_pf0_remove(struct pci_tsm *tsm)
{
	struct tdx_tsm_link *tlink = to_tdx_tsm_link(tsm);

	pci_tsm_pf0_destructor(&tlink->pci);
	kfree(tlink);
}

static struct pci_tsm *tdx_tsm_link_fn_probe(struct tsm_dev *tsm_dev,
					     struct pci_dev *pdev)
{
	int rc;

	struct pci_tsm *pci_tsm __free(kfree) = kzalloc_obj(*pci_tsm);
	if (!pci_tsm)
		return NULL;

	rc = pci_tsm_link_constructor(pdev, pci_tsm, tsm_dev);
	if (rc)
		return NULL;

	return no_free_ptr(pci_tsm);
}

static struct pci_tsm *tdx_tsm_link_probe(struct tsm_dev *tsm_dev,
					  struct pci_dev *pdev)
{
	if (is_pci_tsm_pf0(pdev))
		return tdx_tsm_link_pf0_probe(tsm_dev, pdev);

	return tdx_tsm_link_fn_probe(tsm_dev, pdev);
}

static void tdx_tsm_link_remove(struct pci_tsm *tsm)
{
	if (is_pci_tsm_pf0(tsm->pdev)) {
		tdx_tsm_link_pf0_remove(tsm);
		return;
	}

	/* for sub-functions */
	kfree(tsm);
}

static struct pci_tsm_ops tdx_tsm_link_ops = {
	.probe = tdx_tsm_link_probe,
	.remove = tdx_tsm_link_remove,
	.connect = tdx_tsm_link_connect,
	.disconnect = tdx_tsm_link_disconnect,
};

static void unregister_link_tsm(void *link)
{
	tsm_unregister(link);
}

static int __maybe_unused tdx_connect_init(struct device *dev)
{
	struct tsm_dev *link;
	int ret;

	if (!IS_ENABLED(CONFIG_TDX_CONNECT))
		return 0;

	if (!(tdx_sysinfo->features.tdx_features0 & TDX_FEATURES0_TDXCONNECT))
		return 0;

	link = tsm_register(dev, &tdx_tsm_link_ops);
	if (IS_ERR(link))
		return dev_err_probe(dev, PTR_ERR(link),
				     "failed to register TSM\n");

	return devm_add_action_or_reset(dev, unregister_link_tsm, link);
}

static int tdx_host_probe(struct faux_device *fdev)
{
	/* TODO: do tdx_connect_init() when it is fully implemented. */
	return 0;
}

static struct faux_device_ops tdx_host_ops = {
	.probe = tdx_host_probe,
};

static struct faux_device *fdev;

static int __init tdx_host_init(void)
{
	if (!x86_match_cpu(tdx_host_ids))
		return -ENODEV;

	tdx_sysinfo = tdx_get_sysinfo();
	if (!tdx_sysinfo)
		return -ENODEV;

	fdev = faux_device_create(KBUILD_MODNAME, NULL, &tdx_host_ops);
	if (!fdev)
		return -ENODEV;

	return 0;
}
module_init(tdx_host_init);

static void __exit tdx_host_exit(void)
{
	faux_device_destroy(fdev);
}
module_exit(tdx_host_exit);

MODULE_DESCRIPTION("TDX Host Services");
MODULE_LICENSE("GPL");
