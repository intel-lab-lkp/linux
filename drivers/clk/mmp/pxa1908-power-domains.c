// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Duje Mihanović <duje@dujemihanovic.xyz>
 */

#include <linux/container_of.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/units.h>

#include <dt-bindings/power/marvell,pxa1908-power.h>

#include "clk.h"

/* VPU, GPU, ISP */
#define APMU_PWR_CTRL_REG	0xd8
#define APMU_PWR_BLK_TMR_REG	0xdc
#define APMU_PWR_STATUS_REG	0xf0

/* DSI */
#define APMU_DEBUG		0x88
#define DSI_PHY_DVM_MASK	BIT(31)

#define POWER_ON_LATENCY_US	300
#define POWER_OFF_LATENCY_US	20

#define NR_DOMAINS	5

struct pxa1908_pd_ctrl {
	struct genpd_onecell_data onecell_data;
	struct generic_pm_domain *domains[NR_DOMAINS];
	struct regmap *base;
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
	struct pxa1908_pd_ctrl *ctrl;
	struct generic_pm_domain genpd;
	struct device *dev;
	bool initialized;
	int num_clks;
};

static bool pxa1908_pd_is_on(struct pxa1908_pd *pd)
{
	struct pxa1908_pd_ctrl *ctrl = pd->ctrl;

	return regmap_test_bits(ctrl->base, APMU_PWR_STATUS_REG, pd->data.pwr_state);
}

static int pxa1908_pd_power_on(struct generic_pm_domain *genpd)
{
	struct pxa1908_pd *pd = container_of(genpd, struct pxa1908_pd, genpd);
	struct pxa1908_pd_ctrl *ctrl = pd->ctrl;
	const struct pxa1908_pd_data *data = &pd->data;
	unsigned int status;
	int ret = 0;

	regmap_set_bits(ctrl->base, data->reg_clk_res_ctrl, data->hw_mode);
	if (data->id != PXA1908_POWER_DOMAIN_ISP)
		regmap_write(ctrl->base, APMU_PWR_BLK_TMR_REG, 0x20001fff);
	regmap_set_bits(ctrl->base, APMU_PWR_CTRL_REG, data->pwr_state);

	usleep_range(POWER_ON_LATENCY_US, POWER_ON_LATENCY_US * 2);

	ret = regmap_read_poll_timeout(ctrl->base, APMU_PWR_STATUS_REG, status,
				       status & data->pwr_state, 6, 25 * USEC_PER_MSEC);
	if (ret == -ETIMEDOUT)
		dev_err(pd->dev, "timed out powering on domain '%s'\n", pd->genpd.name);

	return ret;
}

static int pxa1908_pd_power_off(struct generic_pm_domain *genpd)
{
	struct pxa1908_pd *pd = container_of(genpd, struct pxa1908_pd, genpd);
	struct pxa1908_pd_ctrl *ctrl = pd->ctrl;
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

	return regmap_clear_bits(ctrl->base, data->reg_clk_res_ctrl, data->hw_mode);
}

static inline int pxa1908_dsi_power_on(struct generic_pm_domain *genpd)
{
	struct pxa1908_pd *pd = container_of(genpd, struct pxa1908_pd, genpd);
	struct pxa1908_pd_ctrl *ctrl = pd->ctrl;

	return regmap_set_bits(ctrl->base, APMU_DEBUG, DSI_PHY_DVM_MASK);
}

static inline int pxa1908_dsi_power_off(struct generic_pm_domain *genpd)
{
	struct pxa1908_pd *pd = container_of(genpd, struct pxa1908_pd, genpd);
	struct pxa1908_pd_ctrl *ctrl = pd->ctrl;

	return regmap_clear_bits(ctrl->base, APMU_DEBUG, DSI_PHY_DVM_MASK);
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

static struct pxa1908_pd domains[NR_DOMAINS] = {
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

	for (int i = NR_DOMAINS - 1; i >= 0; i--) {
		pd = &domains[i];

		if (!pd->initialized)
			continue;

		ret = pm_genpd_remove(&pd->genpd);
		if (ret)
			dev_err(pd->dev, "failed to remove domain '%s': %d\n",
				pd->genpd.name, ret);
		if (pxa1908_pd_is_on(pd) && !pd->data.keep_on)
			pxa1908_pd_power_off(&pd->genpd);
	}
}

static int
pxa1908_pd_init(struct pxa1908_pd_ctrl *ctrl, int id, struct device *dev)
{
	struct pxa1908_pd *pd = &domains[id];
	int ret;

	pd->dev = dev;
	pd->ctrl = ctrl;
	ctrl->domains[id] = &pd->genpd;

	/* Make sure the state of the hardware is synced with the domain table above. */
	if (pd->data.keep_on) {
		ret = pd->genpd.power_on(&pd->genpd);
		if (ret) {
			dev_err(dev, "failed to power on domain '%s': %d\n", pd->genpd.name, ret);
			return ret;
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
				return ret;
			}
		}
	}

	ret = pm_genpd_init(&pd->genpd, NULL, !pd->data.keep_on);
	if (ret) {
		dev_err(dev, "domain '%s' failed to initialize: %d\n", pd->genpd.name, ret);
		return ret;
	}

	pd->initialized = true;

	return 0;
}

int pxa1908_pd_register(struct device *dev)
{
	struct pxa1908_pd_ctrl *ctrl;
	int ret;

	ctrl = devm_kzalloc(dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->base = syscon_node_to_regmap(dev->of_node);
	if (IS_ERR(ctrl->base)) {
		dev_err(dev, "no regmap available\n");
		return PTR_ERR(ctrl->base);
	}

	ctrl->onecell_data.domains = ctrl->domains;
	ctrl->onecell_data.num_domains = NR_DOMAINS;

	for (int i = 0; i < NR_DOMAINS; i++) {
		ret = pxa1908_pd_init(ctrl, i, dev);
		if (ret)
			goto err;
	}

	return of_genpd_add_provider_onecell(dev->of_node, &ctrl->onecell_data);

err:
	pxa1908_pd_cleanup(ctrl);
	return ret;
}
