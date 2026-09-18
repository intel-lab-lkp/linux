// SPDX-License-Identifier: GPL-2.0
/*
 * R-Car X5H Module Controller
 *
 * Copyright (C) 2026 Glider bv
 */

#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/dev_printk.h>
#include <linux/device-id/of.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_clock.h>
#include <linux/pm_domain.h>
#include <linux/reset-controller.h>
#include <linux/reset.h>
#include <linux/scmi_protocol.h>
#include <linux/slab.h>

#include <dt-bindings/power/renesas,r8a78000-mdlc.h>

struct power_map_in {
	int hw_id;		/* Hardware power domain ID or -1 sentinel */
	u32 fw_id;		/* SCMI firmware power domain ID */
};

struct power_map {
	int hw_id;		/* Hardware power domain ID or -1 sentinel */
	u32 fw_id;		/* SCMI firmware power domain ID */
	struct generic_pm_domain *genpd;
};

struct mod_map_in {
	int hw_id;		/* Hardware module ID or -1 sentinel */
	u32 fw_id;		/* SCMI clock and reset IDs are identical */
};

struct mod_map {
	int hw_id;		/* Hardware module ID or -1 sentinel */
	u32 fw_id;		/* SCMI clock and reset IDs are identical */
	struct reset_control *rstc;
};

struct mdlc_info {
	u32 base;
	const struct power_map_in *power_map;
	const struct mod_map_in *mod_map;
};

struct fw_map {
	u32 impl_ver;
	const struct mdlc_info *info;
};

struct mdlc_data {
	const struct mdlc_info *default_info;
	const struct fw_map *fw_map;
};

/**
 * struct r8a78000_mdlc_priv - Module Controller Private Data
 *
 * @link: Link into list of MDLC instances
 * @genpd_data: PM domain provider data
 * @rcdev: Reset controller entity
 * @dev: MDLC device
 * @np: Device node in DT representing the MDLC
 * @scmi_clk_np: Device node in DT for the SCMI firmware clock protocol
 * @power_map: Mapping from hardware power domain IDs to SCMI power domains
 * @mod_map: Mapping from hardware module IDs to SCMI clocks and resets
 */
struct r8a78000_mdlc_priv {
	struct hlist_node link;
	struct genpd_onecell_data genpd_data;
	struct reset_controller_dev rcdev;
	struct device *dev;
	struct device_node *np;
	struct device_node *scmi_clk_np;
	const struct power_map *power_map;
	const struct mod_map *mod_map;
};

static struct generic_pm_domain *r8a78000_genpd_always_on;
static HLIST_HEAD(r8a78000_mdlc_list);
static DEFINE_MUTEX(r8a78000_mdlc_lock);	/* protects the two above */

static const struct power_map *power_map_find(const struct power_map *map,
					      u32 id)
{
	if (!map)
		return NULL;

	for (; map->hw_id >= 0; map++) {
		if (map->hw_id == id)
			return map;
	}

	return NULL;
}

static struct generic_pm_domain *r8a78000_genpd_xlate(
			const struct of_phandle_args *spec, void *data)
{
	struct r8a78000_mdlc_priv *priv = container_of(data,
					struct r8a78000_mdlc_priv, genpd_data);
	struct generic_pm_domain *genpd;
	struct device *dev = priv->dev;
	const struct power_map *map;
	u32 id;

	if (spec->args_count != 2)
		return ERR_PTR(-EINVAL);

	id = spec->args[0];

	if (id >= R8A78000_MDLC_PD_AON) {
		dev_dbg(dev,
			"Mapping HW power domain 0x%x to always-on domain\n",
			id);
		return r8a78000_genpd_always_on;
	}

	map = power_map_find(priv->power_map, id);
	if (!map) {
		dev_err(dev, "Unknown power domain 0x%x\n", id);
		return ERR_PTR(-ENOENT);
	}

	dev_dbg(dev, "Mapping HW power domain 0x%x to SCMI power domain %u\n",
		id, map->fw_id);

	genpd = map->genpd;

	return genpd;
}

#define rcdev_to_priv(_rcdev)	\
	container_of(_rcdev, struct r8a78000_mdlc_priv, rcdev)

static const struct mod_map *mod_map_find(const struct mod_map *map, u32 id)
{
	if (!map)
		return NULL;

	for (; map->hw_id >= 0; map++) {
		if (map->hw_id == id)
			return map;
	}

	return NULL;
}

static int r8a78000_mdlc_reset_xlate(struct reset_controller_dev *rcdev,
				     const struct of_phandle_args *spec)
{
	struct r8a78000_mdlc_priv *priv = rcdev_to_priv(rcdev);
	struct device *dev = priv->dev;
	const struct mod_map *map;
	u32 id;

	if (spec->args_count != 1)
		return -EINVAL;

	id = spec->args[0];

	map = mod_map_find(priv->mod_map, id);
	if (!map) {
		dev_err(dev, "Unknown reset 0x%x\n", id);
		return -ENOENT;
	}

	if (!map->rstc)
		dev_dbg(dev, "Ignoring HW reset 0x%x\n", id);
	else
		dev_dbg(dev, "Mapping HW reset 0x%x to SCMI reset %u\n", id,
			map->fw_id);

	return map - priv->mod_map;
}

#define DEFINE_MDLC_RESET_WRAPPER(_op, _ignore_eopnotsupp)		     \
	static int r8a78000_mdlc_ ## _op(struct reset_controller_dev *rcdev, \
					 unsigned long id)		     \
	{								     \
		struct r8a78000_mdlc_priv *priv = rcdev_to_priv(rcdev);	     \
		int ret;						     \
									     \
		ret = reset_control_ ## _op(priv->mod_map[id].rstc);	     \
		if (_ignore_eopnotsupp && ret == -EOPNOTSUPP) {		     \
			dev_dbg(priv->dev,				     \
				"%s: Ignoring unsupported reset %lu\n",	     \
				__func__, id);				     \
			return 0;					     \
		}							     \
									     \
		return ret;						     \
	}

DEFINE_MDLC_RESET_WRAPPER(reset, true)
DEFINE_MDLC_RESET_WRAPPER(assert, true)
DEFINE_MDLC_RESET_WRAPPER(deassert, true)
DEFINE_MDLC_RESET_WRAPPER(status, false)

static const struct reset_control_ops r8a78000_mdlc_reset_ops = {
	.reset = r8a78000_mdlc_reset,
	.assert = r8a78000_mdlc_assert,
	.deassert = r8a78000_mdlc_deassert,
	.status = r8a78000_mdlc_status,
};

static struct device_node *scmi_find_proto(struct device_node *scmi, u32 proto)
{
	for_each_available_child_of_node_scoped(scmi, child) {
		u32 reg;

		if (of_property_read_u32(child, "reg", &reg))
			continue;

		if (reg == proto)
			return_ptr(child);
	}

	return NULL;
}

static int r8a78000_mdlc_attach_dev(struct generic_pm_domain *domain,
				    struct device *dev)
{
	struct of_phandle_args pd_spec, scmi_spec;
	struct device_node *np = dev->of_node;
	struct r8a78000_mdlc_priv *priv;
	const struct mod_map *map;
	unsigned int id;
	struct clk *clk;
	int ret;

	ret = of_parse_phandle_with_args(np, "power-domains",
					 "#power-domain-cells", 0, &pd_spec);
	if (ret < 0)
		return ret;

	scoped_guard(mutex, &r8a78000_mdlc_lock) {
		hlist_for_each_entry(priv, &r8a78000_mdlc_list, link) {
			if (priv->np == pd_spec.np)
				break;
		}
	}

	if (!priv) {
		dev_err(dev, "%s: MDLC %pOF not found\n", __func__, pd_spec.np);
		of_node_put(pd_spec.np);
		return -ENODEV;
	}

	id = pd_spec.args[1];
	of_node_put(pd_spec.np);

	map = mod_map_find(priv->mod_map, id);
	if (!map) {
		dev_err(dev, "Unknown module 0x%x\n", id);
		return -ENOENT;
	}

	if (!priv->scmi_clk_np) {
		dev_dbg(dev, "Ignoring HW module 0x%x\n", id);
		return 0;
	}

	dev_dbg(dev, "Mapping HW module 0x%x to SCMI clock %u\n", id,
		map->fw_id);

	scmi_spec.np = priv->scmi_clk_np;
	scmi_spec.args_count = 1;
	scmi_spec.args[0] = map->fw_id;

	clk = of_clk_get_from_provider(&scmi_spec);
	if (IS_ERR(clk)) {
		dev_err(dev, "Cannot get SCMI clock %u: %pe\n", map->fw_id,
			clk);
		return PTR_ERR(clk);
	}

	dev_dbg(dev, "SCMI clock %u is %pC\n", map->fw_id, clk);

	if (!clk) {
		/* Ignore missing SCMI module clocks */
		return 0;
	}

	ret = pm_clk_create(dev);
	if (ret)
		goto fail_put;

	ret = pm_clk_add_clk(dev, clk);
	if (ret)
		goto fail_destroy;

	return 0;

fail_destroy:
	pm_clk_destroy(dev);
fail_put:
	clk_put(clk);
	return ret;
}

static void r8a78000_mdlc_detach_dev(struct generic_pm_domain *domain,
				     struct device *dev)
{
	if (!pm_clk_no_clocks(dev))
		pm_clk_destroy(dev);
}

static const struct power_map *fill_power_map(struct r8a78000_mdlc_priv *priv,
					      const struct power_map_in *map_in,
					      struct device_node *scmi_power_np)
{
	struct of_phandle_args scmi_spec;
	struct generic_pm_domain *genpd;
	struct device *dev = priv->dev;
	struct power_map *map;
	unsigned int i;

	if (!map_in)
		return NULL;

	for (i = 0; map_in[i].hw_id >= 0; i++) { }

	map = devm_kcalloc(dev, i + 1, sizeof(*map), GFP_KERNEL);
	if (!map)
		return ERR_PTR(-ENOMEM);

	for (i = 0; ; i++) {
		map[i].hw_id = map_in[i].hw_id;
		if (map[i].hw_id < 0)
			break;

		map[i].fw_id = map_in[i].fw_id;

		scmi_spec.np = scmi_power_np;
		scmi_spec.args_count = 1;
		scmi_spec.args[0] = map[i].fw_id;

		genpd = of_genpd_get_from_provider(&scmi_spec);
		if (IS_ERR(genpd))
			return dev_err_cast_probe(dev, genpd,
					"Failed to get SCMI power domain %u\n",
					map[i].fw_id);

		dev_dbg(dev, "SCMI power domain %u is %s\n", map[i].fw_id,
			genpd->name);

		map[i].genpd = genpd;

		/* Hook up clock domain support */
		genpd->attach_dev = r8a78000_mdlc_attach_dev;
		genpd->detach_dev = r8a78000_mdlc_detach_dev;
		/* Setting flags this late has no impact, but does not hurt */
		genpd->flags |= GENPD_FLAG_PM_CLK;
		genpd->dev_ops.stop = pm_clk_suspend;
		genpd->dev_ops.start = pm_clk_resume;
	}

	return map;
}

static const struct mod_map *fill_mod_map(struct r8a78000_mdlc_priv *priv,
					  const struct mod_map_in *map_in,
					  struct fwnode_handle *scmi_reset_fwnode)
{
	struct fwnode_reference_args scmi_spec;
	struct device *dev = priv->dev;
	struct reset_control *rstc;
	struct mod_map *map;
	unsigned int i;

	if (!map_in)
		return NULL;

	for (i = 0; map_in[i].hw_id >= 0; i++) { }

	map = devm_kcalloc(dev, i + 1, sizeof(*map), GFP_KERNEL);
	if (!map)
		return ERR_PTR(-ENOMEM);

	for (i = 0; ; i++) {
		map[i].hw_id = map_in[i].hw_id;
		if (map[i].hw_id < 0)
			break;

		map[i].fw_id = map_in[i].fw_id;

		if (!scmi_reset_fwnode)
			continue;

		scmi_spec.fwnode = scmi_reset_fwnode;
		scmi_spec.nargs = 1;
		scmi_spec.args[0] = map[i].fw_id;

		rstc = reset_control_get_from_provider_exclusive(&scmi_spec);
		if (IS_ERR(rstc))
			return dev_err_cast_probe(dev, rstc,
					"Failed to get SCMI reset %u\n",
					map[i].fw_id);

		map[i].rstc = rstc;
	}

	return map;
}

static void r8a78000_mdlc_unlink(void *data)
{
	struct r8a78000_mdlc_priv *priv = data;

	scoped_guard(mutex, &r8a78000_mdlc_lock) {
		hlist_del(&priv->link);
	}
}

static void r8a78000_genpd_del_provider(void *data)
{
	of_genpd_del_provider(data);
}

static int r8a78000_genpd_always_on_singleton(struct device *dev)
{
	struct generic_pm_domain *genpd;
	int ret;

	guard(mutex)(&r8a78000_mdlc_lock);

	if (r8a78000_genpd_always_on)
		return 0;

	genpd = kzalloc_obj(*genpd);
	if (!genpd)
		return -ENOMEM;

	genpd->name = "always-on";
	genpd->attach_dev = r8a78000_mdlc_attach_dev;
	genpd->detach_dev = r8a78000_mdlc_detach_dev;
	genpd->flags |= GENPD_FLAG_PM_CLK;

	ret = pm_genpd_init(genpd, &pm_domain_always_on_gov, false);
	if (ret) {
		kfree(genpd);
		return dev_err_probe(dev, ret,
				     "Failed to create always-on domain\n");
	}

	r8a78000_genpd_always_on = genpd;
	return 0;
}

static int r8a78000_mdlc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *scmi __free(device_node) = NULL;
	struct device_node *scmi_power_np = NULL;
	const struct mdlc_data *mdlc_data;
	struct device_node *scmi_reset_np;
	struct device_node *scmi_clk_np;
	struct r8a78000_mdlc_priv *priv;
	struct scmi_base_info version;
	const struct mdlc_info *info;
	const struct fw_map *fw_map;
	struct resource *res;
	int ret;

	ret = r8a78000_genpd_always_on_singleton(dev);
	if (ret)
		return ret;

	mdlc_data = of_device_get_match_data(dev);
	if (!mdlc_data)
		return -ENODEV;

	info = mdlc_data->default_info;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->np = np;

	scmi = of_parse_phandle(dev->of_node, "renesas,scmi-firmware", 0);
	if (!scmi) {
		dev_dbg(dev, "Cannot find SCMI firmware node\n");
		goto fallback;
	}

	if (!of_device_is_available(scmi)) {
		dev_dbg(dev, "SCMI firmware node is not available\n");
		goto fallback;
	}

	scmi_power_np = scmi_find_proto(scmi, SCMI_PROTOCOL_POWER);
	if (!scmi_power_np) {
		dev_dbg(dev,
			"Cannot find SCMI power domain management protocol\n");
		goto fallback;
	}

	scmi_clk_np = scmi_find_proto(scmi, SCMI_PROTOCOL_CLOCK);
	if (!scmi_clk_np) {
		dev_dbg(dev, "Cannot find SCMI clock management protocol\n");
		goto fallback;
	}

	scmi_reset_np = scmi_find_proto(scmi, SCMI_PROTOCOL_RESET);
	if (!scmi_reset_np) {
		dev_dbg(dev, "Cannot find SCMI reset management protocol\n");
		goto fallback;
	}

	ret = scmi_get_base_info(scmi, &version);
	if (ret == -EPROBE_DEFER)
		return dev_err_probe(dev, ret, "SCMI provider not ready\n");
	if (ret) {
		dev_dbg(dev, "SCMI is not available\n");
		goto fallback;
	}

	if (strcmp(version.vendor_id, "Renesas") ||
	    strcmp(version.sub_vendor_id, "None")) {
		dev_warn(dev, "Unsupported SCMI firmware %s/%s\n",
			 version.vendor_id, version.sub_vendor_id);
		goto fallback;
	}

	for (fw_map = mdlc_data->fw_map; fw_map->info; fw_map++) {
		if (fw_map->impl_ver == version.impl_ver)
			break;
	}

	if (!fw_map->info) {
		dev_warn(dev, "Unsupported SCMI firmware version 0x%08x\n",
			 version.impl_ver);
		goto fallback;
	}

	priv->scmi_clk_np = scmi_clk_np;
	info = fw_map->info;

fallback:
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	for (; info->base; info++) {
		if (info->base == res->start)
			break;
	}

	if (!info->base) {
		dev_dbg(dev, "Unsupported MDLC instance 0x%pa\n", &res->start);
		return -ENODEV;
	}

	/*
	 * We cannot do lazy look-up in r8a78000_genpd_xlate(), as that
	 * function is called with of_genpd_mutex already held.
	 */
	priv->power_map = fill_power_map(priv, info->power_map, scmi_power_np);
	if (IS_ERR(priv->power_map))
		return PTR_ERR(priv->power_map);

	/*
	 * We cannot do lazy look-up in r8a78000_mdlc_reset_xlate(), as that
	 * function is called with reset_list_mutex already held.
	 */
	priv->mod_map = fill_mod_map(priv, info->mod_map,
				     of_fwnode_handle(scmi_reset_np));
	if (IS_ERR(priv->mod_map))
		return PTR_ERR(priv->mod_map);

	scoped_guard(mutex, &r8a78000_mdlc_lock) {
		hlist_add_head(&priv->link, &r8a78000_mdlc_list);
	}

	ret = devm_add_action_or_reset(dev, r8a78000_mdlc_unlink, priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add action\n");

	/* Note that no actual domains are registered, just need translation */
	priv->genpd_data.xlate = r8a78000_genpd_xlate;
	ret = of_genpd_add_provider_onecell(np, &priv->genpd_data);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to register genpd provider\n");

	ret = devm_add_action_or_reset(dev, r8a78000_genpd_del_provider, np);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to add unregister action\n");

	priv->rcdev.ops = &r8a78000_mdlc_reset_ops;
	priv->rcdev.of_node = np;
	priv->rcdev.of_reset_n_cells = 1;
	priv->rcdev.of_xlate = r8a78000_mdlc_reset_xlate;

	ret = devm_reset_controller_register(dev, &priv->rcdev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to register reset controller\n");

	return 0;
}

static const struct mod_map_in r8a78000_mdlc_perw_mod_default[] = {
	{ 0x54 },	/* HSCIF0 */
	{ -1 }
};

static const struct mdlc_info r8a78000_mdlc_default[] = {
	{
		.base = 0xc05d0000 /* mdlc_perw */,
		.mod_map = r8a78000_mdlc_perw_mod_default,
	},
	{ /* sentinel */ }
};

// FIXME We don't need all of them from the start; only add when used/tested
static const struct power_map_in r8a78000_mdlc_pere_power_fw_4_28_0[] = {
	{ 0, 12 },	/* PD_UFS0 */
	{ 1, 13 },	/* PD_UFS1 */
	{ -1 }
};

static const struct mod_map_in r8a78000_mdlc_pere_mod_fw_4_28_0[] = {
	{ 0x30, 197 },	/* PERE_GPIODM0 */
	// No CLOCK_ATTRIBUTES { 0x31, 198 },	/* PERE_GPIODM1 */
	// No CLOCK_ATTRIBUTES { 0x32, 199 },	/* PERE_GPIODM2 */
	// No CLOCK_ATTRIBUTES { 0x33, 200 },	/* PERE_GPIODM3 */
	{ 0x40, 201 },	/* RPC */
	{ 0x60, 202 },	/* UFS0 */
	{ 0x61, 203 },	/* UFS1 */
	{ 0x70, 204 },	/* SDHI0 */
	{ -1 }
};

static const struct mod_map_in r8a78000_mdlc_perw_mod_fw_4_28_0[] = {
	{ 0x30, 205 },	/* PERW_GPIODM0 */
	// No CLOCK_ATTRIBUTES { 0x31, 206 },	/* PERW_GPIODM1 */
	// No CLOCK_ATTRIBUTES { 0x32, 207 },	/* PERW_GPIODM2 */
	// No CLOCK_ATTRIBUTES { 0x33, 208 },	/* PERW_GPIODM3 */
	{ 0x40, 209 },	/* SCIF0 */
	{ 0x41, 210 },	/* SCIF1 */
	{ 0x42, 211 },	/* SCIF3 */
	{ 0x43, 212 },	/* SCIF4 */
	{ 0x44, 213 },	/* I2C1 */
	{ 0x45, 214 },	/* I2C2 */
	{ 0x46, 215 },	/* I2C3 */
	{ 0x47, 216 },	/* I2C4 */
	{ 0x48, 217 },	/* I2C5 */
	{ 0x49, 218 },	/* I2C6 */
	{ 0x4a, 219 },	/* I2C7 */
	{ 0x4b, 220 },	/* I2C8 */
	{ 0x4c, 221 },	/* I3C0 */
	{ 0x4d, 222 },	/* I3C1 */
	{ 0x4e, 223 },	/* I3C2 */
	{ 0x4f, 224 },	/* MSI4 */
	{ 0x50, 225 },	/* MSI5 */
	{ 0x51, 226 },	/* MSI6 */
	{ 0x52, 227 },	/* MSI7 */
	/*
	 * HSCIF0 is protected:
	 *   - CLOCK_ATTRIBUTES is not supported, so clk is NULL
	 *   - Reset operations fail with -EOPNOTSUPP
	 */
	{ 0x54, 228 },	/* HSCIF0 */
	{ 0x55, 229 },	/* HSCIF1 */
	{ 0x56, 230 },	/* HSCIF2 */
	{ 0x57, 231 },	/* HSCIF3 */
	{ 0x58, 232 },	/* DRI00 */
	{ 0x59, 233 },	/* DRI01 */
	{ 0x5a, 234 },	/* DRI10 */
	{ 0x5b, 235 },	/* DRI11 */
	{ 0x5c, 236 },	/* DRI20 */
	{ 0x5d, 237 },	/* DRI21 */
	{ 0x5e, 238 },	/* DRI30 */
	{ 0x5f, 239 },	/* DRI31 */
	{ 0x60, 240 },	/* DRI40 */
	{ 0x61, 241 },	/* DRI41 */
	{ 0x62, 242 },	/* DRI50 */
	{ 0x63, 243 },	/* DRI51 */
	{ 0x64, 244 },	/* DRI60 */
	{ 0x65, 245 },	/* DRI61 */
	{ 0x66, 246 },	/* DRI70 */
	{ 0x67, 247 },	/* DRI71 */
	{ 0x70, 248 },	/* PWM0 */
	{ 0x72, 249 },	/* TMU1 */
	{ 0x73, 250 },	/* TMU2 */
	{ 0x74, 251 },	/* TMU3 */
	{ 0x75, 252 },	/* TMU4 */
	{ 0x76, 253 },	/* TPU0 */
	{ 0x90, 254 },	/* ADG0 */
	{ 0x91, 255 },	/* ADG1 */
	{ 0x92, 256 },	/* SSI0 */
	{ 0x93, 257 },	/* SSI00 */
	{ 0x94, 258 },	/* SSI01 */
	{ 0x95, 259 },	/* SSI02 */
	{ 0x96, 260 },	/* SSI03 */
	{ 0x97, 261 },	/* SSI04 */
	{ 0x98, 262 },	/* SSI05 */
	{ 0x99, 263 },	/* SSI06 */
	{ 0x9a, 264 },	/* SSI07 */
	{ 0x9b, 265 },	/* SSI08 */
	{ 0x9c, 266 },	/* SSI09 */
	{ 0x9d, 267 },	/* SSI1 */
	{ 0x9e, 268 },	/* SSI10 */
	{ 0x9f, 269 },	/* SSI11 */
	{ 0xa0, 270 },	/* SSI12 */
	{ 0xa1, 271 },	/* SSI13 */
	{ 0xa2, 272 },	/* SSI14 */
	{ 0xa3, 273 },	/* SSI15 */
	{ 0xa4, 274 },	/* SSI16 */
	{ 0xa5, 275 },	/* SSI17 */
	{ 0xa6, 276 },	/* SSI18 */
	{ 0xa7, 277 },	/* SSI19 */
	{ 0xa8, 278 },	/* SCU0 */
	{ 0xa9, 279 },	/* SRC00 */
	{ 0xaa, 280 },	/* SRC01 */
	{ 0xab, 281 },	/* SRC02 */
	{ 0xac, 282 },	/* SRC03 */
	{ 0xad, 283 },	/* SRC04 */
	{ 0xae, 284 },	/* SRC05 */
	{ 0xaf, 285 },	/* SRC06 */
	{ 0xb0, 286 },	/* SRC07 */
	{ 0xb1, 287 },	/* SRC08 */
	{ 0xb2, 288 },	/* SRC09 */
	{ 0xb3, 289 },	/* SCU00 */
	{ 0xb4, 290 },	/* SCU01 */
	{ 0xb5, 291 },	/* DVC00 */
	{ 0xb6, 292 },	/* DVC01 */
	{ 0xb7, 293 },	/* SCU1 */
	{ 0xb8, 294 },	/* SRC10 */
	{ 0xb9, 295 },	/* SRC11 */
	{ 0xba, 296 },	/* SRC12 */
	{ 0xbb, 297 },	/* SRC13 */
	{ 0xbc, 298 },	/* SRC14 */
	{ 0xbd, 299 },	/* SRC15 */
	{ 0xbe, 300 },	/* SRC16 */
	{ 0xbf, 301 },	/* SRC17 */
	{ 0xc0, 302 },	/* SRC18 */
	{ 0xc1, 303 },	/* SRC19 */
	{ 0xc2, 304 },	/* SCU10 */
	{ 0xc3, 305 },	/* SCU11 */
	{ 0xc4, 306 },	/* DVC10 */
	{ 0xc5, 307 },	/* DVC11 */
	{ 0xc6, 308 },	/* APD00 */
	{ 0xc7, 309 },	/* APD01 */
	{ 0xc8, 310 },	/* APD10 */
	{ 0xc9, 311 },	/* APD11 */
	{ 0xca, 312 },	/* APD02 */
	{ 0xcb, 313 },	/* APD12 */
	{ -1 }
};

static const struct mdlc_info r8a78000_mdlc_fw_4_28_0[] = {
	{
		.base = 0xc3060000 /* mdlc_vipn */,
		/* FIXME .power_map = r8a78000_mdlc_vipn_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_vipn_mod_fw_4_28_0, */
	}, {
		.base = 0xc3460000 /* mdlc_vips */,
		/* FIXME .power_map = r8a78000_mdlc_vips_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_vips_mod_fw_4_28_0, */
	}, {
		.base = 0xc5000000 /* mdlc_vio */,
		/* FIXME .power_map = r8a78000_mdlc_vio_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_vio_mod_fw_4_28_0, */
	}, {
		.base = 0xc08f0000 /* mdlc_pere */,
		.power_map = r8a78000_mdlc_pere_power_fw_4_28_0,
		.mod_map = r8a78000_mdlc_pere_mod_fw_4_28_0,
	}, {
		.base = 0xc05d0000 /* mdlc_perw */,
		.mod_map = r8a78000_mdlc_perw_mod_fw_4_28_0,
// FIXME We don't need all of them from the start; only add when used/tested
#if 0
	}, {
		.base = 0xe8000000 /* mdlc_ddr0 */,
	}, {
		.base = 0xe8080000 /* mdlc_ddr1 */,
	}, {
		.base = 0xe8100000 /* mdlc_ddr2 */,
	}, {
		.base = 0xe8180000 /* mdlc_ddr3 */,
	}, {
		.base = 0xe8200000 /* mdlc_ddr4 */,
	}, {
		.base = 0xe8280000 /* mdlc_ddr5 */,
	}, {
		.base = 0xe8300000 /* mdlc_ddr6 */,
	}, {
		.base = 0xe8380000 /* mdlc_ddr7 */,
	}, {
		.base = 0xc9c90000 /* mdlc_hscn */,
		/* FIXME .power_map = r8a78000_mdlc_hscn_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_hscn_mod_fw_4_28_0, */
	}, {
		.base = 0x19440000 /* mdlc_rt */,
		/* FIXME .power_map = r8a78000_mdlc_rt_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_rt_mod_fw_4_28_0, */
	}, {
		.base = 0xc6480000 /* mdlc_top */,
		/* FIXME .mod_map = r8a78000_mdlc_top_mod_fw_4_28_0, */
	}, {
		.base = 0xde200000 /* mdlc_hscs */,
		/* FIXME .power_map = r8a78000_mdlc_hscs_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_hscs_mod_fw_4_28_0, */
	}, {
		.base = 0xc1990000 /* mdlc_imn */,
		/* FIXME .power_map = r8a78000_mdlc_imn_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_imn_mod_fw_4_28_0, */
	}, {
		.base = 0xc1d90000 /* mdlc_ims */,
		/* FIXME .power_map = r8a78000_mdlc_ims_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_ims_mod_fw_4_28_0, */
	}, {
		.base = 0xcb510000 /* mdlc_gpc */,
		/* FIXME .power_map = r8a78000_mdlc_gpc_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_gpc_mod_fw_4_28_0, */
	}, {
		.base = 0xcbe90000 /* mdlc_dsp */,
		/* FIXME .power_map = r8a78000_mdlc_dsp_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_dsp_mod_fw_4_28_0, */
	}, {
		.base = 0xe9980000 /* mdlc_mm */,
		/* FIXME .mod_map = r8a78000_mdlc_mm_mod_fw_4_28_0, */
	}, {
		.base = 0xd2c30000 /* mdlc_npu0 */,
		/* FIXME .power_map = r8a78000_mdlc_npu0_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_npu0_mod_fw_4_28_0, */
	}, {
		.base = 0xd6c30000 /* mdlc_npu1 */,
		/* FIXME .power_map = r8a78000_mdlc_npu1_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_npu1_mod_fw_4_28_0, */
	}, {
		.base = 0xca410000 /* mdlc_cmnn */,
		/* FIXME .power_map = r8a78000_mdlc_cmnn_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_cmnn_mod_fw_4_28_0, */
	}, {
		.base = 0xca510000 /* mdlc_cmns */,
		/* FIXME .power_map = r8a78000_mdlc_cmns_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_cmns_mod_fw_4_28_0, */
	}, {
		.base = 0xc1330000 /* mdlc_scp */,
		/* FIXME .mod_map = r8a78000_mdlc_scp_mod_fw_4_28_0, */
	}, {
		.base = 0xc1338000 /* mdlc_aon */,
		/* FIXME .mod_map = r8a78000_mdlc_aon_mod_fw_4_28_0, */
#endif
	},
	{ /* sentinel */ }
};

static const struct power_map_in r8a78000_mdlc_vio_power_fw_4_31_0[] = {
	{ 0, 4 },	/* PD_VIO0 */
	{ 1, 5 },	/* PD_VIO1 */
	{ 2, 6 },	/* PD_VIO2 */
	{ 3, 7 },	/* PD_VIO3 */
	{ 7, 11 },	/* PD_VIO4 */
	{ -1 }
};
static const struct mod_map_in r8a78000_mdlc_vio_mod_fw_4_31_0[] = {
	{ 0x70, 26 },	/* VSPD0 */
	{ 0x71, 27 },	/* VSPD1 */
	{ 0x72, 28 },	/* VSPD2 */
	{ 0x73, 29 },	/* VSPD3 */
	{ 0x74, 30 },	/* VSPD4 */
	{ 0x75, 31 },	/* VSPDB0 */
	{ 0x76, 32 },	/* VSPDB1 */
	{ 0x77, 33 },	/* VSPDB2 */
	{ 0x78, 34 },	/* VSPDB3 */
	{ 0x79, 35 },	/* VSPDB4 */
	{ 0x7e, 40 },	/* FCPVD0 */
	{ 0x7f, 41 },	/* FCPVD1 */
	{ 0x80, 42 },	/* FCPVD2 */
	{ 0x81, 43 },	/* FCPVD3 */
	{ 0x82, 44 },	/* FCPVD4 */
	{ 0x83, 45 },	/* FCPVD5 */
	{ 0x84, 46 },	/* FCPVD6 */
	{ 0x85, 47 },	/* FCPVD7 */
	{ 0x86, 48 },	/* FCPVD8 */
	{ 0x87, 49 },	/* FCPVD9 */
	{ 0xf9, 163 },	/* VSPB0 */
	{ 0xfa, 164 },	/* VSPB1 */
	{ 0xfb, 165 },	/* VSPB2 */
	{ 0xfc, 166 },	/* VSPB3 */
	{ 0xfd, 167 },	/* VSPB4 */
	{ 0xfe, 168 },	/* VSPI0 */
	{ 0xff, 169 },	/* VSPI1 */
	{ 0x100, 170 },	/* VSPI2 */
	{ 0x101, 171 },	/* VSPI3 */
	{ 0x102, 172 },	/* FCPVB0 */
	{ 0x103, 173 },	/* FCPVB1 */
	{ 0x104, 174 },	/* FCPVB2 */
	{ 0x105, 175 },	/* FCPVB3 */
	{ 0x106, 176 },	/* FCPVB4 */
	{ 0x107, 177 },	/* FCPVI0 */
	{ 0x108, 178 },	/* FCPVI1 */
	{ 0x109, 179 },	/* FCPVI2 */
	{ 0x10a, 180 },	/* FCPVI3 */
	{ -1 }
};

static const struct mod_map_in r8a78000_mdlc_pere_mod_fw_4_31_0[] = {
	{ 0x30, 193 },	/* PERE_GPIODM0 */
	// No CLOCK_ATTRIBUTES { 0x31, 194 },	/* PERE_GPIODM1 */
	// No CLOCK_ATTRIBUTES { 0x32, 195 },	/* PERE_GPIODM2 */
	// No CLOCK_ATTRIBUTES { 0x33, 196 },	/* PERE_GPIODM3 */
	{ 0x40, 197 },	/* RPC */
	{ 0x60, 198 },	/* UFS0 */
	{ 0x61, 199 },	/* UFS1 */
	{ 0x70, 200 },	/* SDHI0 */
	{ -1 }
};

static const struct mod_map_in r8a78000_mdlc_perw_mod_fw_4_31_0[] = {
	{ 0x30, 201 },	/* PERW_GPIODM0 */
	// No CLOCK_ATTRIBUTES { 0x31, 202 },	/* PERW_GPIODM1 */
	// No CLOCK_ATTRIBUTES { 0x32, 203 },	/* PERW_GPIODM2 */
	// No CLOCK_ATTRIBUTES { 0x33, 204 },	/* PERW_GPIODM3 */
	{ 0x40, 205 },	/* SCIF0 */
	{ 0x41, 206 },	/* SCIF1 */
	{ 0x42, 207 },	/* SCIF3 */
	{ 0x43, 208 },	/* SCIF4 */
	{ 0x44, 209 },	/* I2C1 */
	{ 0x45, 210 },	/* I2C2 */
	{ 0x46, 211 },	/* I2C3 */
	{ 0x47, 212 },	/* I2C4 */
	{ 0x48, 213 },	/* I2C5 */
	{ 0x49, 214 },	/* I2C6 */
	{ 0x4a, 215 },	/* I2C7 */
	{ 0x4b, 216 },	/* I2C8 */
	{ 0x4c, 217 },	/* I3C0 */
	{ 0x4d, 218 },	/* I3C1 */
	{ 0x4e, 219 },	/* I3C2 */
	{ 0x4f, 220 },	/* MSI4 */
	{ 0x50, 221 },	/* MSI5 */
	{ 0x51, 222 },	/* MSI6 */
	{ 0x52, 223 },	/* MSI7 */
	{ 0x54, 224 },	/* HSCIF0 */
	{ 0x55, 225 },	/* HSCIF1 */
	{ 0x56, 226 },	/* HSCIF2 */
	{ 0x57, 227 },	/* HSCIF3 */
	{ 0x58, 228 },	/* DRI00 */
	{ 0x59, 229 },	/* DRI01 */
	{ 0x5a, 230 },	/* DRI10 */
	{ 0x5b, 231 },	/* DRI11 */
	{ 0x5c, 232 },	/* DRI20 */
	{ 0x5d, 233 },	/* DRI21 */
	{ 0x5e, 234 },	/* DRI30 */
	{ 0x5f, 235 },	/* DRI31 */
	{ 0x60, 236 },	/* DRI40 */
	{ 0x61, 237 },	/* DRI41 */
	{ 0x62, 238 },	/* DRI50 */
	{ 0x63, 239 },	/* DRI51 */
	{ 0x64, 240 },	/* DRI60 */
	{ 0x65, 241 },	/* DRI61 */
	{ 0x66, 242 },	/* DRI70 */
	{ 0x67, 243 },	/* DRI71 */
	{ 0x70, 244 },	/* PWM0 */
	{ 0x72, 245 },	/* TMU1 */
	{ 0x73, 246 },	/* TMU2 */
	{ 0x74, 247 },	/* TMU3 */
	{ 0x75, 248 },	/* TMU4 */
	{ 0x76, 249 },	/* TPU0 */
	{ 0x90, 250 },	/* ADG0 */
	{ 0x91, 251 },	/* ADG1 */
	{ 0x92, 252 },	/* SSI0 */
	{ 0x93, 253 },	/* SSI00 */
	{ 0x94, 254 },	/* SSI01 */
	{ 0x95, 255 },	/* SSI02 */
	{ 0x96, 256 },	/* SSI03 */
	{ 0x97, 257 },	/* SSI04 */
	{ 0x98, 258 },	/* SSI05 */
	{ 0x99, 259 },	/* SSI06 */
	{ 0x9a, 260 },	/* SSI07 */
	{ 0x9b, 261 },	/* SSI08 */
	{ 0x9c, 262 },	/* SSI09 */
	{ 0x9d, 263 },	/* SSI1 */
	{ 0x9e, 264 },	/* SSI10 */
	{ 0x9f, 265 },	/* SSI11 */
	{ 0xa0, 266 },	/* SSI12 */
	{ 0xa1, 267 },	/* SSI13 */
	{ 0xa2, 268 },	/* SSI14 */
	{ 0xa3, 269 },	/* SSI15 */
	{ 0xa4, 270 },	/* SSI16 */
	{ 0xa5, 271 },	/* SSI17 */
	{ 0xa6, 272 },	/* SSI18 */
	{ 0xa7, 273 },	/* SSI19 */
	{ 0xa8, 274 },	/* SCU0 */
	{ 0xa9, 275 },	/* SRC00 */
	{ 0xaa, 276 },	/* SRC01 */
	{ 0xab, 277 },	/* SRC02 */
	{ 0xac, 278 },	/* SRC03 */
	{ 0xad, 279 },	/* SRC04 */
	{ 0xae, 280 },	/* SRC05 */
	{ 0xaf, 281 },	/* SRC06 */
	{ 0xb0, 282 },	/* SRC07 */
	{ 0xb1, 283 },	/* SRC08 */
	{ 0xb2, 284 },	/* SRC09 */
	{ 0xb3, 285 },	/* SCU00 */
	{ 0xb4, 286 },	/* SCU01 */
	{ 0xb5, 287 },	/* DVC00 */
	{ 0xb6, 288 },	/* DVC01 */
	{ 0xb7, 289 },	/* SCU1 */
	{ 0xb8, 290 },	/* SRC10 */
	{ 0xb9, 291 },	/* SRC11 */
	{ 0xba, 292 },	/* SRC12 */
	{ 0xbb, 293 },	/* SRC13 */
	{ 0xbc, 294 },	/* SRC14 */
	{ 0xbd, 295 },	/* SRC15 */
	{ 0xbe, 296 },	/* SRC16 */
	{ 0xbf, 297 },	/* SRC17 */
	{ 0xc0, 298 },	/* SRC18 */
	{ 0xc1, 299 },	/* SRC19 */
	{ 0xc2, 300 },	/* SCU10 */
	{ 0xc3, 301 },	/* SCU11 */
	{ 0xc4, 302 },	/* DVC10 */
	{ 0xc5, 303 },	/* DVC11 */
	{ 0xc6, 304 },	/* APD00 */
	{ 0xc7, 305 },	/* APD01 */
	{ 0xc8, 306 },	/* APD10 */
	{ 0xc9, 307 },	/* APD11 */
	{ 0xca, 308 },	/* APD02 */
	{ 0xcb, 309 },	/* APD12 */
	{ -1 }
};

static const struct mdlc_info r8a78000_mdlc_fw_4_31_0[] = {
	{
		.base = 0xc3060000 /* mdlc_vipn */,
		/* FIXME .power_map = r8a78000_mdlc_vipn_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_vipn_mod_fw_4_31_0, */
	}, {
		.base = 0xc3460000 /* mdlc_vips */,
		/* FIXME .power_map = r8a78000_mdlc_vips_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_vips_mod_fw_4_31_0, */
	}, {
		.base = 0xc5000000 /* mdlc_vio */,
		.power_map = r8a78000_mdlc_vio_power_fw_4_31_0,
		.mod_map = r8a78000_mdlc_vio_mod_fw_4_31_0,
	}, {
		.base = 0xc08f0000 /* mdlc_pere */,
		.power_map = r8a78000_mdlc_pere_power_fw_4_28_0,
		.mod_map = r8a78000_mdlc_pere_mod_fw_4_31_0,
	}, {
		.base = 0xc05d0000 /* mdlc_perw */,
		.mod_map = r8a78000_mdlc_perw_mod_fw_4_31_0,
// FIXME We don't need all of them from the start; only add when used/tested
#if 0
	}, {
		.base = 0xe8000000 /* mdlc_ddr0 */,
	}, {
		.base = 0xe8080000 /* mdlc_ddr1 */,
	}, {
		.base = 0xe8100000 /* mdlc_ddr2 */,
	}, {
		.base = 0xe8180000 /* mdlc_ddr3 */,
	}, {
		.base = 0xe8200000 /* mdlc_ddr4 */,
	}, {
		.base = 0xe8280000 /* mdlc_ddr5 */,
	}, {
		.base = 0xe8300000 /* mdlc_ddr6 */,
	}, {
		.base = 0xe8380000 /* mdlc_ddr7 */,
	}, {
		.base = 0xc9c90000 /* mdlc_hscn */,
		/* FIXME .power_map = r8a78000_mdlc_hscn_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_hscn_mod_fw_4_31_0, */
	}, {
		.base = 0x19440000 /* mdlc_rt */,
		/* FIXME .power_map = r8a78000_mdlc_rt_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_rt_mod_fw_4_31_0, */
	}, {
		.base = 0xc6480000 /* mdlc_top */,
		/* FIXME .mod_map = r8a78000_mdlc_top_mod_fw_4_31_0, */
	}, {
		.base = 0xde200000 /* mdlc_hscs */,
		/* FIXME .power_map = r8a78000_mdlc_hscs_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_hscs_mod_fw_4_31_0, */
	}, {
		.base = 0xc1990000 /* mdlc_imn */,
		/* FIXME .power_map = r8a78000_mdlc_imn_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_imn_mod_fw_4_31_0, */
	}, {
		.base = 0xc1d90000 /* mdlc_ims */,
		/* FIXME .power_map = r8a78000_mdlc_ims_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_ims_mod_fw_4_31_0, */
	}, {
		.base = 0xcb510000 /* mdlc_gpc */,
		/* FIXME .power_map = r8a78000_mdlc_gpc_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_gpc_mod_fw_4_31_0, */
	}, {
		.base = 0xcbe90000 /* mdlc_dsp */,
		/* FIXME .power_map = r8a78000_mdlc_dsp_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_dsp_mod_fw_4_31_0, */
	}, {
		.base = 0xe9980000 /* mdlc_mm */,
		/* FIXME .mod_map = r8a78000_mdlc_mm_mod_fw_4_31_0, */
	}, {
		.base = 0xd2c30000 /* mdlc_npu0 */,
		/* FIXME .power_map = r8a78000_mdlc_npu0_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_npu0_mod_fw_4_31_0, */
	}, {
		.base = 0xd6c30000 /* mdlc_npu1 */,
		/* FIXME .power_map = r8a78000_mdlc_npu1_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_npu1_mod_fw_4_31_0, */
	}, {
		.base = 0xca410000 /* mdlc_cmnn */,
		/* FIXME .power_map = r8a78000_mdlc_cmnn_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_cmnn_mod_fw_4_31_0, */
	}, {
		.base = 0xca510000 /* mdlc_cmns */,
		/* FIXME .power_map = r8a78000_mdlc_cmns_power_fw_4_28_0, */
		/* FIXME .mod_map = r8a78000_mdlc_cmns_mod_fw_4_31_0, */
	}, {
		.base = 0xc1330000 /* mdlc_scp */,
		/* FIXME .mod_map = r8a78000_mdlc_scp_mod_fw_4_31_0, */
	}, {
		.base = 0xc1338000 /* mdlc_aon */,
		/* FIXME .mod_map = r8a78000_mdlc_aon_mod_fw_4_31_0, */
#endif
	},
	{ /* sentinel */ }
};

static const struct fw_map r8a78000_mdlc_fw_map[] = {
	{ 0x010a0000, r8a78000_mdlc_fw_4_28_0 },	/* SCP FW SDKv4.28.0 */
	{ 0x010d0000, r8a78000_mdlc_fw_4_31_0 },	/* SCP FW SDKv4.31.0 */
	{ 0x010e0000, r8a78000_mdlc_fw_4_31_0 },	/* SCP FW SDKv4.32.0 */
	{ 0x01100000, r8a78000_mdlc_fw_4_31_0 },	/* SCP FW SDKv4.36.0 */
	{ /* sentinel */ }
};

static const struct mdlc_data r8a78000_mdlc_data = {
	.default_info = r8a78000_mdlc_default,
	.fw_map = r8a78000_mdlc_fw_map,
};

static const struct of_device_id r8a78000_mdlc_match[] = {
	{
		.compatible = "renesas,r8a78000-mdlc",
		.data = &r8a78000_mdlc_data,
	},
	{ /* sentinel */ }
};

static struct platform_driver r8a78000_mdlc_driver = {
	.probe = r8a78000_mdlc_probe,
	.driver = {
		.name = "r8a78000-mdlc",
		.of_match_table = r8a78000_mdlc_match,
		.suppress_bind_attrs = true,
	},
};

builtin_platform_driver(r8a78000_mdlc_driver)

MODULE_DESCRIPTION("R-Car X5H MDLC Driver");
