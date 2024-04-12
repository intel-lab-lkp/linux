// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024 Intel Corporation. All rights reserved. */
#include <linux/tsm.h>
#include <linux/pci-tsm.h>
#include <asm/tdx.h>

static int tdx_tsm_add(struct pci_dev *pdev)
{
	return 0;
}

static void tdx_tsm_del(struct pci_dev *pdev)
{
}

static int tdx_tsm_exec(struct pci_dev *pdev, enum pci_tsm_cmd cmd)
{
	return -EOPNOTSUPP;
}

static const struct pci_tsm_ops tdx_pci_tsm_ops = {
	.add = tdx_tsm_add,
	.del = tdx_tsm_del,
	.exec = tdx_tsm_exec,
};

static void unregister_tsm(void *subsys)
{
	tsm_unregister(subsys);
}

static int tdx_tsm_probe(struct device *dev)
{
	struct tsm_subsys *subsys;

	subsys = tsm_register(dev, NULL, &tdx_pci_tsm_ops);
	if (IS_ERR(subsys)) {
		dev_err(dev, "failed to register TSM: (%pe)\n", subsys);
		return PTR_ERR(subsys);
	}

	return devm_add_action_or_reset(dev, unregister_tsm, subsys);
}

static struct device_driver tdx_tsm_driver = {
	.probe = tdx_tsm_probe,
	.bus = &tdx_subsys,
	.owner = THIS_MODULE,
	.name = KBUILD_MODNAME,
	.mod_name = KBUILD_MODNAME,
};

static int __init tdx_tsm_init(void)
{
	return driver_register(&tdx_tsm_driver);
}
module_init(tdx_tsm_init);

static void __exit tdx_tsm_exit(void)
{
	driver_unregister(&tdx_tsm_driver);
}
module_exit(tdx_tsm_exit);

MODULE_IMPORT_NS(TDX);
MODULE_LICENSE("GPL");
MODULE_ALIAS("tdx_tsm");
MODULE_DESCRIPTION("TDX TEE Security Manager");
