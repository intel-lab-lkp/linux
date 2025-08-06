// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Duje Mihanović <duje@dujemihanovic.xyz>
 */

#include <linux/clk.h>
#include <linux/container_of.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/of_clk.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/units.h>

#include <dt-bindings/power/marvell,pxa1908-power.h>

/* VPU, GPU, ISP */
#define APMU_PWR_CTRL_REG	0xd8
#define APMU_PWR_BLK_TMR_REG	0xdc
#define APMU_PWR_STATUS_REG	0xf0

/* DSI */
#define APMU_DEBUG		0x88
#define DSI_PHY_DVM_MASK	BIT(31)

#define POWER_ON_LATENCY_US	300
#define POWER_OFF_LATENCY_US	20

struct pxa1908_pd_ctrl {
	struct genpd_onecell_data onecell_data;
	struct regmap *base;
	struct generic_pm_domain *domains[];
};

struct pxa1908_pd_data {
	u32 reg_clk_res_ctrl;
	u32 hw_mode;
	u32 pwr_state;
	bool keep_on;
	int id;
};

struct pxa1908_pd {
	const struct pxa1908_pd_data data;
	struct generic_pm_domain genpd;
	struct clk_bulk_data *clks;
	struct device *dev;
	bool initialized;
	int num_clks;
};

static bool pxa1908_pd_is_on(struct pxa1908_pd *pd)
{
	struct pxa1908_pd_ctrl *ctrl = dev_get_drvdata(pd->dev);

	return regmap_test_bits(ctrl->base, APMU_PWR_STATUS_REG, pd->data.pwr_state);
}

static int pxa1908_pd_power_on(struct generic_pm_domain *genpd)
{
	struct pxa1908_pd *pd = container_of(genpd, struct pxa1908_pd, genpd);
	struct pxa1908_pd_ctrl *ctrl = dev_get_drvdata(pd->dev);
	const struct pxa1908_pd_data *data = &pd->data;
	unsigned int status;
	int ret = 0;

	if (pd->clks)
		ret = clk_bulk_prepare_enable(pd->num_clks, pd->clks);

	regmap_set_bits(ctrl->base, data->reg_clk_res_ctrl, data->hw_mode);
	if (data->id != PXA1908_POWER_DOMAIN_ISP)
		regmap_write(ctrl->base, APMU_PWR_BLK_TMR_REG, 0x20001fff);
	regmap_set_bits(ctrl->base, APMU_PWR_CTRL_REG, data->pwr_state);

	usleep_range(POWER_ON_LATENCY_US, POWER_ON_LATENCY_US * 2);

	ret = regmap_read_poll_timeout(ctrl->base, APMU_PWR_STATUS_REG, status,
				       status & data->pwr_state, 6, 25 * USEC_PER_MSEC);
	if (ret == -ETIMEDOUT)
		dev_err(pd->dev, "timed out powering on domain '%s'\n", pd->genpd.name);

	if (pd->clks)
		clk_bulk_disable_unprepare(pd->num_clks, pd->clks);

	return ret;
}

static int pxa1908_pd_power_off(struct generic_pm_domain *genpd)
{
	struct pxa1908_pd *pd = container_of(genpd, struct pxa1908_pd, genpd);
	struct pxa1908_pd_ctrl *ctrl = dev_get_drvdata(pd->dev);
	const struct pxa1908_pd_data *data = &pd->data;
	unsigned int status;
	int ret;

	regmap_clear_bits(ctrl->base, APMU_PWR_CTRL_REG, data->pwr_state);

	usleep_range(POWER_OFF_LATENCY_US, POWER_OFF_LATENCY_US * 2);

	ret = regmap_read_poll_timeout(ctrl->base, APMU_PWR_STATUS_REG, status,
				       !(status & data->pwr_state), 6, 25 * USEC_PER_MSEC);
	if (ret == -ETIMEDOUT) {
		dev_err(pd->dev, "timed out powering off domain '%s'\n", pd->genpd.name);
		return ret;
	}

	regmap_clear_bits(ctrl->base, data->reg_clk_res_ctrl, data->hw_mode);

	return 0;
}

static int pxa1908_dsi_power_on(struct generic_pm_domain *genpd)
{
	struct pxa1908_pd *pd = container_of(genpd, struct pxa1908_pd, genpd);
	struct pxa1908_pd_ctrl *ctrl = dev_get_drvdata(pd->dev);

	if (pd->clks) {
		int ret = clk_bulk_prepare_enable(pd->num_clks, pd->clks);

		if (ret) {
			dev_err(pd->dev, "failed to enable clocks for domain '%s': %d\n",
				pd->genpd.name, ret);
			return ret;
		}
	}

	regmap_set_bits(ctrl->base, APMU_DEBUG, DSI_PHY_DVM_MASK);

	return 0;
}

static int pxa1908_dsi_power_off(struct generic_pm_domain *genpd)
{
	struct pxa1908_pd *pd = container_of(genpd, struct pxa1908_pd, genpd);
	struct pxa1908_pd_ctrl *ctrl = dev_get_drvdata(pd->dev);

	regmap_clear_bits(ctrl->base, APMU_DEBUG, DSI_PHY_DVM_MASK);

	if (pd->clks)
		clk_bulk_disable_unprepare(pd->num_clks, pd->clks);

	return 0;
}

#define DOMAIN(_id, _name, ctrl, mode, state) \
	[_id] = { \
		.data = { \
			.reg_clk_res_ctrl = ctrl, \
			.hw_mode = BIT(mode), \
			.pwr_state = BIT(state), \
			.id = _id, \
		}, \
		.genpd = { \
			.name = _name, \
			.power_on = pxa1908_pd_power_on, \
			.power_off = pxa1908_pd_power_off, \
		}, \
	}

static struct pxa1908_pd domains[] = {
	DOMAIN(PXA1908_POWER_DOMAIN_VPU, "vpu", 0xa4, 19, 2),
	DOMAIN(PXA1908_POWER_DOMAIN_GPU, "gpu", 0xcc, 11, 0),
	DOMAIN(PXA1908_POWER_DOMAIN_GPU2D, "gpu2d", 0xf4, 11, 6),
	DOMAIN(PXA1908_POWER_DOMAIN_ISP, "isp", 0x38, 15, 4),
	[PXA1908_POWER_DOMAIN_DSI] = {
		.genpd = {
			.name = "dsi",
			.power_on = pxa1908_dsi_power_on,
			.power_off = pxa1908_dsi_power_off,
			/*
			 * TODO: There is no DSI driver written yet and until then we probably
			 * don't want to power off the DSI PHY ever.
			 */
			.flags = GENPD_FLAG_ALWAYS_ON,
		},
		.data = {
			/* See above. */
			.keep_on = true,
		},
	},
};

static void pxa1908_pd_cleanup(struct pxa1908_pd_ctrl *ctrl)
{
	struct pxa1908_pd *pd;
	int ret;

	for (int i = ARRAY_SIZE(domains) - 1; i >= 0; i--) {
		pd = &domains[i];

		if (!pd->initialized)
			continue;

		ret = pm_genpd_remove(&pd->genpd);
		if (ret)
			dev_err(pd->dev, "failed to remove domain '%s': %d\n",
				pd->genpd.name, ret);
		if (pxa1908_pd_is_on(pd) && !pd->data.keep_on)
			pxa1908_pd_power_off(&pd->genpd);

		clk_bulk_put_all(pd->num_clks, pd->clks);
	}
}

static int
pxa1908_pd_init(struct pxa1908_pd_ctrl *ctrl, struct device_node *node, struct device *dev)
{
	struct pxa1908_pd *pd;
	int clk_idx = 0, ret;
	u32 id;

	ret = of_property_read_u32(node, "reg", &id);
	if (ret) {
		dev_err(dev, "failed to get domain id from reg: %d\n", ret);
		return ret;
	}

	if (id >= ARRAY_SIZE(domains)) {
		dev_err(dev, "invalid domain id %d\n", id);
		return ret;
	}

	pd = &domains[id];
	pd->dev = dev;
	pd->num_clks = of_clk_get_parent_count(node);
	ctrl->domains[id] = &pd->genpd;

	if (pd->num_clks > 0) {
		pd->clks = devm_kcalloc(dev, pd->num_clks, sizeof(*pd->clks), GFP_KERNEL);
		if (!pd->clks)
			return -ENOMEM;
	}

	for (int i = 0; i < pd->num_clks; i++) {
		struct clk *clk = of_clk_get(node, i);

		if (IS_ERR(clk)) {
			ret = PTR_ERR(clk);
			dev_err(dev, "failed to get clk for domain '%s': %d\n",
				pd->genpd.name, ret);
			goto err;
		}

		pd->clks[clk_idx++].clk = clk;
	}

	/* Make sure the state of the hardware is synced with the domain table above. */
	if (pd->data.keep_on) {
		ret = pd->genpd.power_on(&pd->genpd);
		if (ret) {
			dev_err(dev, "failed to power on domain '%s': %d\n", pd->genpd.name, ret);
			goto err;
		}
	} else {
		if (pxa1908_pd_is_on(pd)) {
			dev_warn(dev,
				 "domain '%s' is on despite being default off; powering off\n",
				 pd->genpd.name);

			ret = pxa1908_pd_power_off(&pd->genpd);
			if (ret) {
				dev_err(dev, "failed to power off domain '%s': %d\n",
					pd->genpd.name, ret);
				goto err;
			}
		}
	}

	ret = pm_genpd_init(&pd->genpd, NULL, !pd->data.keep_on);
	if (ret) {
		dev_err(dev, "domain '%s' failed to initialize: %d\n", pd->genpd.name, ret);
		goto err;
	}

	pd->initialized = true;

	return 0;

err:
	clk_bulk_put_all(pd->num_clks, pd->clks);
	return ret;
}

static int pxa1908_pd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pxa1908_pd_ctrl *ctrl;
	struct device_node *node;
	int ret;

	ctrl = devm_kzalloc(dev, struct_size(ctrl, domains, ARRAY_SIZE(domains)), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->base = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(ctrl->base)) {
		dev_err(dev, "no regmap available\n");
		return PTR_ERR(ctrl->base);
	}

	platform_set_drvdata(pdev, ctrl);

	ctrl->onecell_data.domains = ctrl->domains;
	ctrl->onecell_data.num_domains = ARRAY_SIZE(domains);

	for_each_available_child_of_node(dev->of_node, node) {
		ret = pxa1908_pd_init(ctrl, node, dev);
		if (ret)
			goto err;
	}

	return of_genpd_add_provider_onecell(dev->of_node, &ctrl->onecell_data);

err:
	pxa1908_pd_cleanup(ctrl);
	return ret;
}

static void pxa1908_pd_remove(struct platform_device *pdev)
{
	pxa1908_pd_cleanup(platform_get_drvdata(pdev));
}

static const struct of_device_id pxa1908_pd_match[] = {
	{
		.compatible = "marvell,pxa1908-power-controller",
	},
	{ }
};
MODULE_DEVICE_TABLE(of, pxa1908_pd_match);

static struct platform_driver pxa1908_pd_driver = {
	.probe = pxa1908_pd_probe,
	.remove = pxa1908_pd_remove,
	.driver = {
		.name = "pxa1908-power-controller",
		.of_match_table = pxa1908_pd_match,
	},
};
module_platform_driver(pxa1908_pd_driver);

MODULE_AUTHOR("Duje Mihanović <duje@dujemihanovic.xyz>");
MODULE_DESCRIPTION("Marvell PXA1908 power domain driver");
MODULE_LICENSE("GPL");
