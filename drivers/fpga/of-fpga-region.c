// SPDX-License-Identifier: GPL-2.0
/*
 * FPGA Region - Device Tree support for FPGA programming under Linux
 *
 *  Copyright (C) 2013-2016 Altera Corporation
 *  Copyright (C) 2017 Intel Corporation
 */
#include <linux/fpga/fpga-bridge.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/fpga/fpga-region.h>
#include <linux/firmware.h>
#include <linux/fpga-region.h>
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/libfdt.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

/**
 * struct of_fpga_region_priv - Private data structure
 * image.
 * @dev:	Device data structure
 * @fw:		firmware of coeff table.
 * @path:	path of FPGA overlay image firmware file.
 * @ovcs_id:	overlay changeset id.
 */
struct of_fpga_region_priv {
	struct device *dev;
	const struct firmware *fw;
	int ovcs_id;
};

static const struct of_device_id fpga_region_of_match[] = {
	{ .compatible = "fpga-region", },
	{},
};
MODULE_DEVICE_TABLE(of, fpga_region_of_match);

/**
 * of_fpga_region_find - find FPGA region
 * @np: device node of FPGA Region
 *
 * Caller will need to put_device(&region->dev) when done.
 *
 * Return: FPGA Region struct or NULL
 */
static struct fpga_region *of_fpga_region_find(struct device_node *np)
{
	return fpga_region_class_find(NULL, np, device_match_of_node);
}

/**
 * of_fpga_region_get_mgr - get reference for FPGA manager
 * @np: device node of FPGA region
 *
 * Get FPGA Manager from "fpga-mgr" property or from ancestor region.
 *
 * Caller should call fpga_mgr_put() when done with manager.
 *
 * Return: fpga manager struct or IS_ERR() condition containing error code.
 */
static struct fpga_manager *of_fpga_region_get_mgr(struct device_node *np)
{
	struct device_node  *mgr_node;
	struct fpga_manager *mgr;

	of_node_get(np);
	while (np) {
		if (of_device_is_compatible(np, "fpga-region")) {
			mgr_node = of_parse_phandle(np, "fpga-mgr", 0);
			if (mgr_node) {
				mgr = of_fpga_mgr_get(mgr_node);
				of_node_put(mgr_node);
				of_node_put(np);
				return mgr;
			}
		}
		np = of_get_next_parent(np);
	}
	of_node_put(np);

	return ERR_PTR(-EINVAL);
}

/**
 * of_fpga_region_get_bridges - create a list of bridges
 * @region: FPGA region
 *
 * Create a list of bridges including the parent bridge and the bridges
 * specified by "fpga-bridges" property.  Note that the
 * fpga_bridges_enable/disable/put functions are all fine with an empty list
 * if that happens.
 *
 * Caller should call fpga_bridges_put(&region->bridge_list) when
 * done with the bridges.
 *
 * Return: 0 for success (even if there are no bridges specified)
 * or -EBUSY if any of the bridges are in use.
 */
static int of_fpga_region_get_bridges(struct fpga_region *region)
{
	struct device *dev = &region->dev;
	struct device_node *region_np = dev->of_node;
	struct fpga_image_info *info = region->info;
	struct device_node *br, *np, *parent_br = NULL;
	int i, ret;

	/* If parent is a bridge, add to list */
	ret = of_fpga_bridge_get_to_list(region_np->parent, info,
					 &region->bridge_list);

	/* -EBUSY means parent is a bridge that is under use. Give up. */
	if (ret == -EBUSY)
		return ret;

	/* Zero return code means parent was a bridge and was added to list. */
	if (!ret)
		parent_br = region_np->parent;

	/* If overlay has a list of bridges, use it. */
	br = of_parse_phandle(info->overlay, "fpga-bridges", 0);
	if (br) {
		of_node_put(br);
		np = info->overlay;
	} else {
		np = region_np;
	}

	for (i = 0; ; i++) {
		br = of_parse_phandle(np, "fpga-bridges", i);
		if (!br)
			break;

		/* If parent bridge is in list, skip it. */
		if (br == parent_br) {
			of_node_put(br);
			continue;
		}

		/* If node is a bridge, get it and add to list */
		ret = of_fpga_bridge_get_to_list(br, info,
						 &region->bridge_list);
		of_node_put(br);

		/* If any of the bridges are in use, give up */
		if (ret == -EBUSY) {
			fpga_bridges_put(&region->bridge_list);
			return -EBUSY;
		}
	}

	return 0;
}

/**
 * of_fpga_region_post_remove - post-remove overlay
 *
 * @region: FPGA region that was targeted by the overlay that was removed
 *
 * Called after an overlay has been removed if the overlay's target was a
 * FPGA region.
 */
static void of_fpga_region_post_remove(struct fpga_region *region)
{
	fpga_bridges_disable(&region->bridge_list);
	fpga_bridges_put(&region->bridge_list);
	fpga_image_info_free(region->info);
	region->info = NULL;
}

static int of_fpga_region_status(struct fpga_region *region)
{
	struct of_fpga_region_priv *ovcs = region->priv;

	if (ovcs->ovcs_id)
		return FPGA_REGION_HAS_PL;

	return FPGA_REGION_EMPTY;
}

/*
 * FPGA DTBO Parser
 *
 * This file contains the implementation of a function to parse Device Tree
 * Blob Overlay (DTBO) files used for dynamic reconfiguration of  FPGAs in
 * Linux. The `parse_dtbo()` function is responsible for:
 *
 * - Validating the DTBO header
 * - Extracting fixups and resolving symbolic paths to actual device nodes
 * - Identifying the FPGA region targeted by the overlay
 * - Allocating and populating the `fpga_image_info` structure with
 *   relevant configuration such as flags, firmware name, and timeout values
 * - Retrieving and associating any fpga-bridges specified in the overlay
 *
 * This function leverages the Flattened Device Tree (FDT) and OF (Open Firmware)
 * APIs to interpret the overlay and prepare the FPGA Manager for a runtime
 * configuration update. It is intended for use in dynamic reconfiguration
 * scenarios where full/partial bitstreams are applied using overlay files.
 *
 * Returns 0 on success or a negative error code on failure.
 */
static int parse_dtbo(const struct firmware *fw)
{
	int fixups_off, prop_off, overlay_off, prop_len, fw_name_len, ret;
	const char *prop_name, *symbol_path, *fw_name, *name, *value;
	struct device_node *symbols_node = NULL;
	struct device_node *fpga_node = NULL;
	struct device_node *br_node = NULL;
	const struct fdt_property *prop;
	struct fpga_image_info *info;
	struct fpga_region *region;
	const fdt32_t *val;

	/* Validate DTBO header */
	if (!fw || fdt_check_header(fw->data) < 0) {
		pr_err("%s: Invalid DTBO file\n", __func__);
		return -EINVAL;
	}

	/* Locate __fixups__ node */
	fixups_off = fdt_path_offset((void *)fw->data, "/__fixups__");
	if (fixups_off < 0) {
		pr_err("%s: __fixups__ node not found\n", __func__);
		return -EINVAL;
	}

	/* Retrieve the first property under __fixups__ */
	prop_off = fdt_first_property_offset((void *)fw->data, fixups_off);
	if (prop_off < 0) {
		pr_info("%s: No properties in __fixups__\n", __func__);
		return -ENOENT;
	}

	prop = fdt_get_property_by_offset((void *)fw->data, prop_off, &prop_len);
	if (!prop) {
		pr_err("%s: Failed to get first __fixups__ property\n", __func__);
		return -ENOENT;
	}

	prop_name = fdt_string((void *)fw->data, fdt32_to_cpu(prop->nameoff));

	/* Locate __symbols__ node */
	symbols_node = of_find_node_by_path("/__symbols__");
	if (!symbols_node) {
		pr_err("%s: Missing __symbols__ node\n", __func__);
		return -ENODEV;
	}

	/* Resolve symbolic path to FPGA node */
	symbol_path = of_get_property(symbols_node, prop_name, NULL);
	if (!symbol_path) {
		pr_err("%s: Symbol '%s' not found in __symbols__\n", __func__, prop_name);
		goto err_put_symbols;
	}

	/* Retrieve FPGA region associated with the node */
	fpga_node = of_find_node_by_path(symbol_path);
	if (!fpga_node) {
		pr_err("%s: FPGA node not found at path: %s\n", __func__, symbol_path);
		goto err_put_symbols;
	}

	/* Retrieve FPGA region associated with the node */
	region = of_fpga_region_find(fpga_node);
	if (!region) {
		pr_err("%s: FPGA region not found for: %s\n", __func__, symbol_path);
		goto err_put_fpga;
	}

	/* Allocate FPGA image info structure */
	info = fpga_image_info_alloc(&region->dev);
	if (!info) {
		ret = -ENOMEM;
		goto err_put_fpga;
	}

	/* Locate /fragment@0/__overlay__ node in the overlay */
	overlay_off = fdt_path_offset((void *)fw->data, "/fragment@0/__overlay__");
	if (overlay_off < 0) {
		pr_err("%s: Missing /fragment@0/__overlay__\n", __func__);
		ret = -ENOENT;
		goto err_free_info;
	}

	/* Parse optional configuration flags */
	if (fdt_getprop(fw->data, overlay_off, "partial-fpga-config", NULL))
		info->flags |= FPGA_MGR_PARTIAL_RECONFIG;

	if (fdt_getprop(fw->data, overlay_off, "external-fpga-config", NULL))
		info->flags |= FPGA_MGR_EXTERNAL_CONFIG;

	if (fdt_getprop(fw->data, overlay_off, "encrypted-fpga-config", NULL))
		info->flags |= FPGA_MGR_ENCRYPTED_BITSTREAM;

	/* Retrieve firmware-name property */
	fw_name = fdt_getprop(fw->data, overlay_off, "firmware-name", &fw_name_len);
	if (fw_name) {
		info->firmware_name = devm_kstrdup(&region->dev, fw_name, GFP_KERNEL);
		if (!info->firmware_name) {
			ret = -ENOMEM;
			goto err_free_info;
		}
	}

	/* Parse optional timeout properties */
	val = fdt_getprop(fw->data, overlay_off, "region-unfreeze-timeout-us", &fw_name_len);
	if (val && fw_name_len == sizeof(fdt32_t))
		info->enable_timeout_us = fdt32_to_cpu(*val);

	val = fdt_getprop(fw->data, overlay_off, "region-freeze-timeout-us", &fw_name_len);
	if (val && fw_name_len == sizeof(fdt32_t))
		info->disable_timeout_us = fdt32_to_cpu(*val);

	val = fdt_getprop(fw->data, overlay_off, "config-complete-timeout-us", &fw_name_len);
	if (val && fw_name_len == sizeof(fdt32_t))
		info->config_complete_timeout_us = fdt32_to_cpu(*val);

	/* Attach parsed image info to the FPGA region */
	region->info = info;

	/* Handle optional fpga-bridges references */
	fdt_for_each_property_offset(prop_off, (void *)fw->data, fixups_off) {
		prop = fdt_get_property_by_offset((void *)fw->data, prop_off, NULL);
		if (!prop)
			continue;

		name = fdt_string((void *)fw->data, fdt32_to_cpu(prop->nameoff));
		value = prop->data;

		if (!name || !value)
			continue;

		if (strstr(value, "fpga-bridges")) {
			symbol_path = of_get_property(symbols_node, name, NULL);
			if (!symbol_path) {
				pr_err("%s: Missing symbol for bridge: %s\n", __func__, name);
				ret = -ENODEV;
				goto err_put_symbols;
			}

			br_node = of_find_node_by_path(symbol_path);
			if (!br_node) {
				pr_err("%s: Bridge node not found at: %s\n", __func__, symbol_path);
				ret = -ENODEV;
				goto err_put_symbols;
			}

			ret = of_fpga_bridge_get_to_list(br_node, info, &region->bridge_list);
			of_node_put(br_node);

			if (ret == -EBUSY) {
				fpga_bridges_put(&region->bridge_list);
				goto err_put_symbols;
			}
		}
	}

	of_node_put(fpga_node);
	of_node_put(symbols_node);

	return 0;

err_free_info:
	fpga_image_info_free(info);
err_put_fpga:
	of_node_put(fpga_node);
err_put_symbols:
	of_node_put(symbols_node);

	return ret;
}

static int of_fpga_region_pre_config(struct fpga_region *region)
{
	struct of_fpga_region_priv *ovcs = region->priv;
	int err;

	/* if it's set do not allow changes */
	if (ovcs->ovcs_id)
		return -EPERM;

	err = request_firmware(&ovcs->fw, region->firmware_name, NULL);
	if (err)
		return err;

	err = parse_dtbo(ovcs->fw);
	if (err)
		return err;

	release_firmware(ovcs->fw);

	return 0;
}

static int of_fpga_region_post_config(struct fpga_region *region)
{
	struct of_fpga_region_priv *ovcs = region->priv;
	int err;

	/* if it's set do not allow changes */
	if (ovcs->ovcs_id)
		return -EPERM;

	err = request_firmware(&ovcs->fw, region->firmware_name, NULL);
	if (err != 0)
		goto out_err;

	err = of_overlay_fdt_apply((void *)ovcs->fw->data, ovcs->fw->size,
				   &ovcs->ovcs_id, NULL);
	if (err < 0) {
		pr_err("%s: Failed to create overlay (err=%d)\n",
		       __func__, err);
		release_firmware(ovcs->fw);
		goto out_err;
	}

	return 0;

out_err:
	ovcs->ovcs_id = 0;
	ovcs->fw = NULL;

	return err;
}

static int of_fpga_region_config_remove(struct fpga_region *region)
{
	struct of_fpga_region_priv *ovcs = region->priv;

	if (!ovcs->ovcs_id)
		return -EPERM;

	of_overlay_remove(&ovcs->ovcs_id);
	of_fpga_region_post_remove(region);
	release_firmware(ovcs->fw);

	ovcs->ovcs_id = 0;
	ovcs->fw = NULL;

	return 0;
}

static const struct fpga_region_ops region_ops = {
	.region_status = of_fpga_region_status,
	.region_pre_config = of_fpga_region_pre_config,
	.region_post_config = of_fpga_region_post_config,
	.region_remove = of_fpga_region_config_remove,
};

static int of_fpga_region_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct of_fpga_region_priv *priv;
	struct fpga_region *region;
	struct fpga_manager *mgr;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;

	/* Find the FPGA mgr specified by region or parent region. */
	mgr = of_fpga_region_get_mgr(np);
	if (IS_ERR(mgr))
		return -EPROBE_DEFER;

	region = fpga_region_register_with_ops(dev, mgr, &region_ops, priv,
					       of_fpga_region_get_bridges);
	if (IS_ERR(region)) {
		ret = PTR_ERR(region);
		goto eprobe_mgr_put;
	}

	of_platform_populate(np, fpga_region_of_match, NULL, &region->dev);
	platform_set_drvdata(pdev, region);

	dev_info(dev, "FPGA Region probed\n");

	return 0;

eprobe_mgr_put:
	fpga_mgr_put(mgr);
	return ret;
}

static void of_fpga_region_remove(struct platform_device *pdev)
{
	struct fpga_region *region = platform_get_drvdata(pdev);
	struct fpga_manager *mgr = region->mgr;

	fpga_region_unregister(region);
	fpga_mgr_put(mgr);
}

static struct platform_driver of_fpga_region_driver = {
	.probe = of_fpga_region_probe,
	.remove = of_fpga_region_remove,
	.driver = {
		.name	= "of-fpga-region",
		.of_match_table = of_match_ptr(fpga_region_of_match),
	},
};

/**
 * of_fpga_region_init - init function for fpga_region class
 * Creates the fpga_region class and registers a reconfig notifier.
 *
 * Return: 0 on success, negative error code otherwise.
 */
static int __init of_fpga_region_init(void)
{
	return platform_driver_register(&of_fpga_region_driver);
}

static void __exit of_fpga_region_exit(void)
{
	platform_driver_unregister(&of_fpga_region_driver);
}

subsys_initcall(of_fpga_region_init);
module_exit(of_fpga_region_exit);

MODULE_DESCRIPTION("FPGA Region");
MODULE_AUTHOR("Alan Tull <atull@kernel.org>");
MODULE_LICENSE("GPL v2");
