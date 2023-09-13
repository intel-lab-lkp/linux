// SPDX-License-Identifier: GPL-2.0-only
/*
 * TI K3 Cortex-M4 Remote Processor(s) driver
 *
 * Copyright (C) 2021 Texas Instruments Incorporated - https://www.ti.com/
 *	Hari Nagalla <hnagalla@ti.com>
 */

#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/omap-mailbox.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include "omap_remoteproc.h"
#include "remoteproc_internal.h"
#include "ti_sci_proc.h"
#include "ti_k3_common.h"

/*
 * Power up the M4F remote processor.
 *
 * This function will be invoked only after the firmware for this rproc
 * was loaded, parsed successfully, and all of its resource requirements
 * were met. This callback is invoked only in remoteproc mode.
 */
static int k3_m4_rproc_start(struct rproc *rproc)
{
	struct k3_rproc *kproc = rproc->priv;
	u32 boot_addr;
	int ret;

	ret = k3_rproc_request_mbox(rproc);
	if (ret)
		return ret;

	boot_addr = rproc->bootaddr;
	ret = k3_rproc_release(kproc);
	if (ret)
		goto put_mbox;

	return 0;

put_mbox:
	mbox_free_channel(kproc->mbox);
	return ret;
}

/*
 * Stop the M4 remote processor.
 *
 * This function puts the M4 processor into reset, and finishes processing
 * of any pending messages. This callback is invoked only in remoteproc mode.
 */
static int k3_m4_rproc_stop(struct rproc *rproc)
{
	struct k3_rproc *kproc = rproc->priv;

	mbox_free_channel(kproc->mbox);

	k3_rproc_reset(kproc);

	return 0;
}

/*
 * Attach to a running M4 remote processor (IPC-only mode)
 *
 * This rproc attach callback only needs to request the mailbox, the remote
 * processor is already booted, so there is no need to issue any TI-SCI
 * commands to boot the M4 core. This callback is used only in IPC-only mode.
 */
static int k3_m4_rproc_attach(struct rproc *rproc)
{
	struct k3_rproc *kproc = rproc->priv;
	struct device *dev = kproc->dev;
	int ret;

	ret = k3_rproc_request_mbox(rproc);
	if (ret)
		return ret;

	dev_info(dev, "M4 initialized in IPC-only mode\n");
	return 0;
}

/*
 * Detach from a running M4 remote processor (IPC-only mode)
 *
 * This rproc detach callback performs the opposite operation to attach callback
 * and only needs to release the mailbox, the M4 core is not stopped and will
 * be left to continue to run its booted firmware. This callback is invoked only in
 * IPC-only mode.
 */
static int k3_m4_rproc_detach(struct rproc *rproc)
{
	struct k3_rproc *kproc = rproc->priv;
	struct device *dev = kproc->dev;

	mbox_free_channel(kproc->mbox);
	dev_info(dev, "M4 deinitialized in IPC-only mode\n");
	return 0;
}

static const struct rproc_ops k3_m4_rproc_ops = {
	.start		= k3_m4_rproc_start,
	.stop		= k3_m4_rproc_stop,
	.attach		= k3_m4_rproc_attach,
	.detach		= k3_m4_rproc_detach,
	.kick		= k3_rproc_kick,
	.da_to_va	= k3_rproc_da_to_va,
	.get_loaded_rsc_table = k3_get_loaded_rsc_table,
};

static int k3_m4_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const struct k3_rproc_dev_data *data;
	struct k3_rproc *kproc;
	struct rproc *rproc;
	const char *fw_name;
	bool r_state = false;
	bool p_state = false;
	int ret = 0;
	int ret1;

	data = of_device_get_match_data(dev);
	if (!data)
		return -ENODEV;

	ret = rproc_of_parse_firmware(dev, 0, &fw_name);
	if (ret) {
		dev_err(dev, "failed to parse firmware-name property, ret = %d\n",
			ret);
		return ret;
	}

	rproc = rproc_alloc(dev, dev_name(dev), &k3_m4_rproc_ops, fw_name,
			    sizeof(*kproc));
	if (!rproc)
		return -ENOMEM;

	rproc->has_iommu = false;
	rproc->recovery_disabled = true;
	if (data->uses_lreset) {
		rproc->ops->prepare = k3_rproc_prepare;
		rproc->ops->unprepare = k3_rproc_unprepare;
	}
	kproc = rproc->priv;
	kproc->rproc = rproc;
	kproc->dev = dev;
	kproc->data = data;

	kproc->ti_sci = ti_sci_get_by_phandle(np, "ti,sci");
	if (IS_ERR(kproc->ti_sci)) {
		ret = PTR_ERR(kproc->ti_sci);
		if (ret != -EPROBE_DEFER) {
			dev_err(dev, "failed to get ti-sci handle, ret = %d\n",
				ret);
		}
		kproc->ti_sci = NULL;
		goto free_rproc;
	}

	ret = of_property_read_u32(np, "ti,sci-dev-id", &kproc->ti_sci_id);
	if (ret) {
		dev_err(dev, "missing 'ti,sci-dev-id' property\n");
		goto put_sci;
	}

	kproc->reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(kproc->reset)) {
		ret = PTR_ERR(kproc->reset);
		dev_err(dev, "failed to get reset, status = %d\n", ret);
		goto put_sci;
	}

	kproc->tsp = k3_rproc_of_get_tsp(dev, kproc->ti_sci);
	if (IS_ERR(kproc->tsp)) {
		dev_err(dev, "failed to construct ti-sci proc control, ret = %d\n",
			ret);
		ret = PTR_ERR(kproc->tsp);
		goto put_sci;
	}

	ret = ti_sci_proc_request(kproc->tsp);
	if (ret < 0) {
		dev_err(dev, "ti_sci_proc_request failed, ret = %d\n", ret);
		goto free_tsp;
	}

	ret = k3_rproc_of_get_memories(pdev, kproc);
	if (ret)
		goto release_tsp;

	ret = k3_reserved_mem_init(kproc);
	if (ret) {
		dev_err(dev, "reserved memory init failed, ret = %d\n", ret);
		goto release_tsp;
	}

	ret = kproc->ti_sci->ops.dev_ops.is_on(kproc->ti_sci, kproc->ti_sci_id,
					       &r_state, &p_state);
	if (ret) {
		dev_err(dev, "failed to get initial state, mode cannot be determined, ret = %d\n",
			ret);
		goto release_mem;
	}

	/* configure devices for either remoteproc or IPC-only mode */
	if (p_state) {
		dev_info(dev, "configured M4 for IPC-only mode\n");
		rproc->state = RPROC_DETACHED;
		/* override rproc ops with only required IPC-only mode ops */
		rproc->ops->prepare = NULL;
		rproc->ops->unprepare = NULL;
		rproc->ops->start = NULL;
		rproc->ops->stop = NULL;
		rproc->ops->attach = k3_m4_rproc_attach;
		rproc->ops->detach = k3_m4_rproc_detach;
		rproc->ops->get_loaded_rsc_table = k3_get_loaded_rsc_table;
	} else {
		dev_info(dev, "configured M4 for remoteproc mode\n");
		/*
		 * ensure the M4 local reset is asserted to ensure the core
		 * doesn't execute bogus code in .prepare() when the module
		 * reset is released.
		 */
		if (data->uses_lreset) {
			ret = reset_control_status(kproc->reset);
			if (ret < 0) {
				dev_err(dev, "failed to get reset status, status = %d\n",
					ret);
				goto release_mem;
			} else if (ret == 0) {
				dev_warn(dev, "local reset is deasserted for device\n");
				k3_rproc_reset(kproc);
			}
		}
	}

	ret = rproc_add(rproc);
	if (ret) {
		dev_err(dev, "failed to add register device with remoteproc core, status = %d\n",
			ret);
		goto release_mem;
	}

	platform_set_drvdata(pdev, kproc);

	return 0;

release_mem:
	k3_reserved_mem_exit(kproc);
release_tsp:
	ret1 = ti_sci_proc_release(kproc->tsp);
	if (ret1)
		dev_err(dev, "failed to release proc, ret = %d\n", ret1);
free_tsp:
	kfree(kproc->tsp);
put_sci:
	ret1 = ti_sci_put_handle(kproc->ti_sci);
	if (ret1)
		dev_err(dev, "failed to put ti_sci handle, ret = %d\n", ret1);
free_rproc:
	rproc_free(rproc);
	return ret;
}

static int k3_m4_rproc_remove(struct platform_device *pdev)
{
	struct k3_rproc *kproc = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	int ret;

	rproc_del(kproc->rproc);

	ret = ti_sci_proc_release(kproc->tsp);
	if (ret)
		dev_err(dev, "failed to release proc, ret = %d\n", ret);

	kfree(kproc->tsp);

	ret = ti_sci_put_handle(kproc->ti_sci);
	if (ret)
		dev_err(dev, "failed to put ti_sci handle, ret = %d\n", ret);

	k3_reserved_mem_exit(kproc);
	rproc_free(kproc->rproc);

	return 0;
}

static const struct k3_rproc_mem_data am64_m4_mems[] = {
	{ .name = "iram", .dev_addr = 0x0 },
	{ .name = "dram", .dev_addr = 0x30000 },
};

static const struct k3_rproc_dev_data am64_m4_data = {
	.mems = am64_m4_mems,
	.num_mems = ARRAY_SIZE(am64_m4_mems),
	.boot_align_addr = SZ_1K,
	.uses_lreset = true,
};

static const struct of_device_id k3_m4_of_match[] = {
	{ .compatible = "ti,am64-m4fss", .data = &am64_m4_data, },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, k3_m4_of_match);

static struct platform_driver k3_m4_rproc_driver = {
	.probe	= k3_m4_rproc_probe,
	.remove	= k3_m4_rproc_remove,
	.driver	= {
		.name = "k3-m4-rproc",
		.of_match_table = k3_m4_of_match,
	},
};

module_platform_driver(k3_m4_rproc_driver);

MODULE_AUTHOR("Hari Nagalla <hnagalla@ti.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("TI K3 M4 Remoteproc driver");
