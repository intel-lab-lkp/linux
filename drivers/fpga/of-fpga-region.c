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
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

/**
 * struct of_fpga_variant_priv - private data for FPGA Region variants
 * @fw_name: name of the firmware file containing the FPGA image
 * @fw_buf: buffer containing the cached firmware image
 * @fw_size: size in bytes of the buffer containing the cached firmware image
 */
struct of_fpga_variant_priv {
	char *fw_name;
	void *fw_buf;
	size_t fw_size;
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
 * or an error code if any of the bridges are not available.
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

		/* If any of the bridges are not available, give up */
		if (ret) {
			fpga_bridges_put(&region->bridge_list);
			return ret;
		}
	}

	return 0;
}

static int of_fpga_remove_variant_child(struct device *dev, void *data)
{
	struct device_node *variant_np = data;

	if (dev->of_node && dev->of_node->parent == variant_np) {
		if (dev_is_platform(dev)) {
			of_node_clear_flag(dev->of_node, OF_POPULATED);
			platform_device_unregister(to_platform_device(dev));
		}
	}
	return 0;
}

static int of_fpga_region_populate_variant(struct device *dev,
					   struct device_node *variant_np)
{
	struct device_node *child_np;

	for_each_available_child_of_node(variant_np, child_np) {
		if (!of_platform_device_create(child_np, NULL, dev))
			dev_warn(dev, "failed to create device node.\n");
	}

	return 0;
}

/**
 * of_fpga_region_apply_variant - apply a specific FPGA variant to the region
 * @region: FPGA region to be programmed
 * @variant: FPGA variant requested to be applied
 *
 * Retrieves the specific variant node from the fpga-variants container,
 * handles firmware loading (including lazy caching if firmware-cached is
 * set), programs the FPGA region with the variant, and finally populates
 * the platform devices for the child IPs.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
static int of_fpga_region_apply_variant(struct fpga_region *region,
					struct fpga_variant *variant)
{
	struct of_fpga_variant_priv *priv = variant->priv;
	struct device *dev = &region->dev;
	struct device_node *variants_np, *variant_np;
	const struct firmware *fw;
	struct fpga_image_info *info;
	int ret;

	variants_np = of_get_child_by_name(dev->of_node, "fpga-variants");
	if (!variants_np)
		return -ENODEV;

	variant_np = of_get_child_by_name(variants_np, variant->name);
	of_node_put(variants_np);
	if (!variant_np)
		return -ENODEV;

	info = fpga_image_info_alloc(dev);
	if (!info) {
		ret = -ENOMEM;
		goto err_put_node;
	}

	if (!of_property_read_bool(variant_np, "firmware-cached")) {
		info->firmware_name = devm_kstrdup(dev, priv->fw_name, GFP_KERNEL);
		if (!info->firmware_name) {
			ret = -ENOMEM;
			goto err_free_info;
		}
	} else if (priv->fw_buf) {
		info->buf = priv->fw_buf;
		info->count = priv->fw_size;
	} else {
		ret = request_firmware(&fw, priv->fw_name, dev);
		if (ret) {
			dev_err(dev, "failed to request firmware '%s'\n", priv->fw_name);
			goto err_free_info;
		}

		priv->fw_buf = devm_kmemdup(dev, fw->data, fw->size, GFP_KERNEL);
		priv->fw_size = fw->size;
		release_firmware(fw);

		if (!priv->fw_buf) {
			ret = -ENOMEM;
			goto err_free_info;
		}

		info->buf = priv->fw_buf;
		info->count = priv->fw_size;
	}

	/* Checked during probing */
	info->flags |= FPGA_MGR_PARTIAL_RECONFIG;

	of_property_read_u32(dev->of_node, "region-unfreeze-timeout-us",
			     &info->enable_timeout_us);
	of_property_read_u32(dev->of_node, "region-freeze-timeout-us",
			     &info->disable_timeout_us);
	of_property_read_u32(dev->of_node, "config-complete-timeout-us",
			     &info->config_complete_timeout_us);

	region->info = info;

	if (info->firmware_name || info->buf) {
		ret = fpga_region_program_fpga(region);
		if (ret) {
			dev_err(dev, "failed to program FPGA\n");
			goto err_clear_info;
		}
	}

	ret = of_fpga_region_populate_variant(dev, variant_np);
	if (ret) {
		dev_err(dev, "failed to populate variant IP nodes\n");
		goto err_clear_info;
	}

	of_node_put(variant_np);
	return 0;

err_clear_info:
	region->info = NULL;
err_free_info:
	fpga_image_info_free(info);
err_put_node:
	of_node_put(variant_np);
	return ret;
}

/**
 * of_fpga_region_remove_variant - remove the current FPGA variant from the region
 * @region: FPGA region
 * @variant: FPGA variant being removed
 *
 * Unregisters all devices that were populared for the variant's IPs
 * during apply_variant() and frees the FPGA image.
 */
static void of_fpga_region_remove_variant(struct fpga_region *region,
					  struct fpga_variant *variant)
{
	struct device_node *variants_np, *variant_np;
	struct device *dev = &region->dev;

	variants_np = of_get_child_by_name(dev->of_node, "fpga-variants");
	if (variants_np) {
		variant_np = of_get_child_by_name(variants_np, variant->name);
		of_node_put(variants_np);

		if (variant_np) {
			device_for_each_child(dev, variant_np,
					      of_fpga_remove_variant_child);
			of_node_put(variant_np);
		}
	}

	if (region->info) {
		fpga_image_info_free(region->info);
		region->info = NULL;
	}
}

/**
 * child_regions_with_firmware - Used to check the child region info.
 * @overlay: device node of the overlay
 *
 * If the overlay adds child FPGA regions, they are not allowed to have
 * firmware-name property.
 *
 * Return: 0 for OK or -EINVAL if child FPGA region adds firmware-name.
 */
static int child_regions_with_firmware(struct device_node *overlay)
{
	struct device_node *child_region;
	const char *child_firmware_name;
	int ret = 0;

	of_node_get(overlay);

	child_region = of_find_matching_node(overlay, fpga_region_of_match);
	while (child_region) {
		if (!of_property_read_string(child_region, "firmware-name",
					     &child_firmware_name)) {
			ret = -EINVAL;
			break;
		}
		child_region = of_find_matching_node(child_region,
						     fpga_region_of_match);
	}

	of_node_put(child_region);

	if (ret)
		pr_err("firmware-name not allowed in child FPGA region: %pOF",
		       child_region);

	return ret;
}

/**
 * of_fpga_region_parse_ov - parse and check overlay applied to region
 *
 * @region: FPGA region
 * @overlay: overlay applied to the FPGA region
 *
 * Given an overlay applied to an FPGA region, parse the FPGA image specific
 * info in the overlay and do some checking.
 *
 * Return:
 *   NULL if overlay doesn't direct us to program the FPGA.
 *   fpga_image_info struct if there is an image to program.
 *   error code for invalid overlay.
 */
static struct fpga_image_info *
of_fpga_region_parse_ov(struct fpga_region *region,
			struct device_node *overlay)
{
	struct device *dev = &region->dev;
	struct fpga_image_info *info;
	const char *firmware_name;
	int ret;

	if (region->info) {
		dev_err(dev, "Region already has overlay applied.\n");
		return ERR_PTR(-EINVAL);
	}

	/*
	 * Reject overlay if child FPGA Regions added in the overlay have
	 * firmware-name property (would mean that an FPGA region that has
	 * not been added to the live tree yet is doing FPGA programming).
	 */
	ret = child_regions_with_firmware(overlay);
	if (ret)
		return ERR_PTR(ret);

	info = fpga_image_info_alloc(dev);
	if (!info)
		return ERR_PTR(-ENOMEM);

	info->overlay = overlay;

	/* Read FPGA region properties from the overlay */
	if (of_property_read_bool(overlay, "partial-fpga-config"))
		info->flags |= FPGA_MGR_PARTIAL_RECONFIG;

	if (of_property_read_bool(overlay, "external-fpga-config"))
		info->flags |= FPGA_MGR_EXTERNAL_CONFIG;

	if (of_property_read_bool(overlay, "encrypted-fpga-config"))
		info->flags |= FPGA_MGR_ENCRYPTED_BITSTREAM;

	if (!of_property_read_string(overlay, "firmware-name",
				     &firmware_name)) {
		info->firmware_name = devm_kstrdup(dev, firmware_name,
						   GFP_KERNEL);
		if (!info->firmware_name)
			return ERR_PTR(-ENOMEM);
	}

	of_property_read_u32(overlay, "region-unfreeze-timeout-us",
			     &info->enable_timeout_us);

	of_property_read_u32(overlay, "region-freeze-timeout-us",
			     &info->disable_timeout_us);

	of_property_read_u32(overlay, "config-complete-timeout-us",
			     &info->config_complete_timeout_us);

	/* If overlay is not programming the FPGA, don't need FPGA image info */
	if (!info->firmware_name) {
		ret = 0;
		goto ret_no_info;
	}

	/*
	 * If overlay informs us FPGA was externally programmed, specifying
	 * firmware here would be ambiguous.
	 */
	if (info->flags & FPGA_MGR_EXTERNAL_CONFIG) {
		dev_err(dev, "error: specified firmware and external-fpga-config");
		ret = -EINVAL;
		goto ret_no_info;
	}

	return info;
ret_no_info:
	fpga_image_info_free(info);
	return ERR_PTR(ret);
}

/**
 * of_fpga_region_notify_pre_apply - pre-apply overlay notification
 *
 * @region: FPGA region that the overlay was applied to
 * @nd: overlay notification data
 *
 * Called when an overlay targeted to an FPGA Region is about to be applied.
 * Parses the overlay for properties that influence how the FPGA will be
 * programmed and does some checking. If the checks pass, programs the FPGA.
 * If the checks fail, overlay is rejected and does not get added to the
 * live tree.
 *
 * Return: 0 for success or negative error code for failure.
 */
static int of_fpga_region_notify_pre_apply(struct fpga_region *region,
					   struct of_overlay_notify_data *nd)
{
	struct device *dev = &region->dev;
	struct fpga_image_info *info;
	int ret;

	info = of_fpga_region_parse_ov(region, nd->overlay);
	if (IS_ERR(info))
		return PTR_ERR(info);

	/* If overlay doesn't program the FPGA, accept it anyway. */
	if (!info)
		return 0;

	if (region->info) {
		dev_err(dev, "Region already has overlay applied.\n");
		return -EINVAL;
	}

	region->info = info;
	ret = fpga_region_program_fpga(region);
	if (ret) {
		/* error; reject overlay */
		fpga_image_info_free(info);
		region->info = NULL;
	}

	return ret;
}

/**
 * of_fpga_region_notify_post_remove - post-remove overlay notification
 *
 * @region: FPGA region that was targeted by the overlay that was removed
 * @nd: overlay notification data
 *
 * Called after an overlay has been removed if the overlay's target was a
 * FPGA region.
 */
static void of_fpga_region_notify_post_remove(struct fpga_region *region,
					      struct of_overlay_notify_data *nd)
{
	fpga_bridges_disable(&region->bridge_list);
	fpga_bridges_put(&region->bridge_list);
	fpga_image_info_free(region->info);
	region->info = NULL;
}

/**
 * of_fpga_region_notify - reconfig notifier for dynamic DT changes
 * @nb:		notifier block
 * @action:	notifier action
 * @arg:	reconfig data
 *
 * This notifier handles programming an FPGA when a "firmware-name" property is
 * added to an fpga-region.
 *
 * Return: NOTIFY_OK or error if FPGA programming fails.
 */
static int of_fpga_region_notify(struct notifier_block *nb,
				 unsigned long action, void *arg)
{
	struct of_overlay_notify_data *nd = arg;
	struct fpga_region *region;
	int ret;

	switch (action) {
	case OF_OVERLAY_PRE_APPLY:
		pr_debug("%s OF_OVERLAY_PRE_APPLY\n", __func__);
		break;
	case OF_OVERLAY_POST_APPLY:
		pr_debug("%s OF_OVERLAY_POST_APPLY\n", __func__);
		return NOTIFY_OK;       /* not for us */
	case OF_OVERLAY_PRE_REMOVE:
		pr_debug("%s OF_OVERLAY_PRE_REMOVE\n", __func__);
		return NOTIFY_OK;       /* not for us */
	case OF_OVERLAY_POST_REMOVE:
		pr_debug("%s OF_OVERLAY_POST_REMOVE\n", __func__);
		break;
	default:			/* should not happen */
		return NOTIFY_OK;
	}

	region = of_fpga_region_find(nd->target);
	if (!region)
		return NOTIFY_OK;

	ret = 0;
	switch (action) {
	case OF_OVERLAY_PRE_APPLY:
		ret = of_fpga_region_notify_pre_apply(region, nd);
		break;

	case OF_OVERLAY_POST_REMOVE:
		of_fpga_region_notify_post_remove(region, nd);
		break;
	}

	put_device(&region->dev);

	if (ret)
		return notifier_from_errno(ret);

	return NOTIFY_OK;
}

static struct notifier_block fpga_region_of_nb = {
	.notifier_call = of_fpga_region_notify,
};

static int of_fpga_region_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *var_np, *base_np = NULL, *variants_np = NULL;
	struct fpga_region *region;
	struct fpga_manager *mgr;
	struct fpga_region_info info = { 0 };
	struct of_fpga_variant_priv *priv;
	const char *base_variant_name = NULL;
	const char *fw_name = NULL;
	bool has_base_variant = false;
	bool base_variant_found = false;
	bool is_base = false;
	int ret;

	/* Find the FPGA mgr specified by region or parent region. */
	mgr = of_fpga_region_get_mgr(np);
	if (IS_ERR(mgr))
		return -EPROBE_DEFER;

	/* Register variants support only if the device tree is well-formed */
	if (!of_property_read_string(np, "base-variant", &base_variant_name))
		has_base_variant = true;

	if (has_base_variant) {
		if (!of_property_read_bool(np, "partial-fpga-config")) {
			dev_err(dev, "Region '%s' is missing 'partial-fpga-config' property.\n",
				np->name);
			ret = -EINVAL;
			goto eprobe_mgr_put;
		}

		variants_np = of_get_child_by_name(np, "fpga-variants");
		if (!variants_np) {
			dev_err(dev, "Missing 'fpga-variants' container node.\n");
			ret = -EINVAL;
			goto eprobe_mgr_put;
		}

		for_each_available_child_of_node(variants_np, var_np) {
			if (!of_property_present(var_np, "firmware-name")) {
				dev_err(dev, "Variant '%s' is missing 'firmware-name'.\n",
					var_np->name);
				of_node_put(var_np);
				ret = -EINVAL;
				goto eprobe_mgr_put;
			}

			if (!strcmp(var_np->name, base_variant_name))
				base_variant_found = true;
		}

		if (!base_variant_found) {
			dev_err(dev, "base-variant '%s' does not match.\n",
				base_variant_name);
			ret = -EINVAL;
			goto eprobe_mgr_put;
		}

		info.apply_variant = of_fpga_region_apply_variant;
		info.remove_variant = of_fpga_region_remove_variant;
	}

	info.mgr = mgr;
	info.get_bridges = of_fpga_region_get_bridges;

	region = fpga_region_register_full(dev, &info);
	if (IS_ERR(region)) {
		ret = PTR_ERR(region);
		goto eprobe_mgr_put;
	}
	if (has_base_variant && variants_np) {
		for_each_available_child_of_node(variants_np, var_np) {
			priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
			if (!priv) {
				of_node_put(var_np);
				ret = -ENOMEM;
				goto eprobe_region_unregister;
			}

			of_property_read_string(var_np, "firmware-name", &fw_name);
			priv->fw_name = devm_kstrdup(dev, fw_name, GFP_KERNEL);
			if (!priv->fw_name) {
				of_node_put(var_np);
				ret = -ENOMEM;
				goto eprobe_region_unregister;
			}

			is_base = !strcmp(var_np->name, base_variant_name);
			if (is_base)
				base_np = of_node_get(var_np);

			ret = fpga_region_add_variant(region, var_np->name, is_base, priv);
			if (ret) {
				dev_err(dev, "Cannot add variant '%s'.\n", var_np->name);
				of_node_put(var_np);
				goto eprobe_region_unregister;
			}
		}
		of_node_put(variants_np);
	}

	of_platform_populate(np, fpga_region_of_match, NULL, &region->dev);

	if (base_np) {
		of_fpga_region_populate_variant(&region->dev, base_np);
		of_node_put(base_np);
	}

	platform_set_drvdata(pdev, region);
	dev_info(dev, "FPGA Region probed\n");

	return 0;

eprobe_region_unregister:
	fpga_region_unregister(region);
eprobe_mgr_put:
	of_node_put(base_np);
	of_node_put(variants_np);
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
	int ret;

	ret = of_overlay_notifier_register(&fpga_region_of_nb);
	if (ret)
		return ret;

	ret = platform_driver_register(&of_fpga_region_driver);
	if (ret)
		goto err_plat;

	return 0;

err_plat:
	of_overlay_notifier_unregister(&fpga_region_of_nb);
	return ret;
}

static void __exit of_fpga_region_exit(void)
{
	platform_driver_unregister(&of_fpga_region_driver);
	of_overlay_notifier_unregister(&fpga_region_of_nb);
}

subsys_initcall(of_fpga_region_init);
module_exit(of_fpga_region_exit);

MODULE_DESCRIPTION("FPGA Region");
MODULE_AUTHOR("Alan Tull <atull@kernel.org>");
MODULE_LICENSE("GPL v2");
