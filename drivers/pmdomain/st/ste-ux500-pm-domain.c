// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2014 Linaro Ltd.
 *
 * Author: Ulf Hansson <ulf.hansson@linaro.org>
 *
 * Implements PM domains using the generic PM domain for ux500.
 */
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mfd/dbx500-prcmu.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/printk.h>
#include <linux/slab.h>

#include <dt-bindings/arm/ux500_pm_domains.h>

#define UX500_EPOD_NONE		NUM_EPOD_ID

/**
 * struct dbx500_powerdomain_info - dbx500 power domain information
 * @genpd: generic power domain
 * @epod_id: id for EPOD (power domain)
 * @is_ramret: RAM retention switch for EPOD (power domain)
 * @exclude_from_power_state: exclude domain from power state count
 */
struct dbx500_powerdomain_info {
	struct generic_pm_domain genpd;
	u16 epod_id;
	bool is_ramret;
	bool exclude_from_power_state;
};

static DEFINE_MUTEX(ux500_pd_lock);
static int power_state_active_cnt;
static bool epod_on[NUM_EPOD_ID];
static bool epod_ramret[NUM_EPOD_ID];

static void power_state_active_enable(void)
{
	power_state_active_cnt++;
}

static int power_state_active_disable(void)
{
	if (power_state_active_cnt <= 0) {
		pr_err("power state: unbalanced enable/disable calls\n");
		return -EINVAL;
	}

	power_state_active_cnt--;
	return 0;
}

static int enable_epod(u16 epod_id, bool ramret)
{
	int ret;

	if (ramret) {
		if (!epod_on[epod_id]) {
			ret = prcmu_set_epod(epod_id, EPOD_STATE_RAMRET);
			if (ret < 0)
				return ret;
		}
		epod_ramret[epod_id] = true;
	} else {
		ret = prcmu_set_epod(epod_id, EPOD_STATE_ON);
		if (ret < 0)
			return ret;
		epod_on[epod_id] = true;
	}

	return 0;
}

static int disable_epod(u16 epod_id, bool ramret)
{
	int ret;

	if (ramret) {
		if (!epod_on[epod_id]) {
			ret = prcmu_set_epod(epod_id, EPOD_STATE_OFF);
			if (ret < 0)
				return ret;
		}
		epod_ramret[epod_id] = false;
	} else {
		if (epod_ramret[epod_id]) {
			ret = prcmu_set_epod(epod_id, EPOD_STATE_RAMRET);
			if (ret < 0)
				return ret;
		} else {
			ret = prcmu_set_epod(epod_id, EPOD_STATE_OFF);
			if (ret < 0)
				return ret;
		}
		epod_on[epod_id] = false;
	}

	return 0;
}

static int pd_power_off(struct generic_pm_domain *domain)
{
	struct dbx500_powerdomain_info *info =
		container_of(domain, struct dbx500_powerdomain_info, genpd);
	int ret = 0;

	guard(mutex)(&ux500_pd_lock);
	if (info->epod_id < NUM_EPOD_ID)
		ret = disable_epod(info->epod_id, info->is_ramret);
	else if (!info->exclude_from_power_state)
		ret = power_state_active_disable();

	return ret;
}

static int pd_power_on(struct generic_pm_domain *domain)
{
	struct dbx500_powerdomain_info *info =
		container_of(domain, struct dbx500_powerdomain_info, genpd);
	int ret = 0;

	guard(mutex)(&ux500_pd_lock);
	if (info->epod_id < NUM_EPOD_ID)
		ret = enable_epod(info->epod_id, info->is_ramret);
	else if (!info->exclude_from_power_state)
		power_state_active_enable();

	return ret;
}

/*
 * Apart from these voltage domains there is also VSAFE which is always
 * on. Vape_esram0_pwr for eSRAM0 is connected to VSAFE.
 */
static struct dbx500_powerdomain_info ux500_pm_domain_vape = {
	/* Vape_pwr */
	.genpd = {
		.name = "VAPE",  /* 0.95 .. 1.20 V */
		.power_off = pd_power_off,
		.power_on = pd_power_on,
	},
	.epod_id = UX500_EPOD_NONE,
};

static struct dbx500_powerdomain_info ux500_pm_domain_varm = {
	.genpd = {
		.name = "VARM",
		.power_off = pd_power_off,
		.power_on = pd_power_on,
	},
	.epod_id = UX500_EPOD_NONE,
};

static struct dbx500_powerdomain_info ux500_pm_domain_vmodem = {
	.genpd = {
		.name = "VMODEM",
		.power_off = pd_power_off,
		.power_on = pd_power_on,
	},
	.epod_id = UX500_EPOD_NONE,
};

static struct dbx500_powerdomain_info ux500_pm_domain_vpll = {
	.genpd = {
		.name = "VPLL", /* 1.8 V */
		.power_off = pd_power_off,
		.power_on = pd_power_on,
	},
	.epod_id = UX500_EPOD_NONE,
};

/*
 * CHECKME: as these are used directly by peripherals as regulators,
 * perhaps they should stay in the regulator subsystem?
 */
static struct dbx500_powerdomain_info ux500_pm_domain_vsmps1 = {
	.genpd = {
		.name = "VSMPS1", /* Also called VIO (1.2V) */
		.power_off = pd_power_off,
		.power_on = pd_power_on,
	},
	.epod_id = UX500_EPOD_NONE,
};

static struct dbx500_powerdomain_info ux500_pm_domain_vsmps2 = {
	.genpd = {
		.name = "VSMPS2", /* Also called VIO (1.8V) */
		.power_off = pd_power_off,
		.power_on = pd_power_on,
	},
	.epod_id = UX500_EPOD_NONE,
	.exclude_from_power_state = true,
};

static struct dbx500_powerdomain_info ux500_pm_domain_vsmps3 = {
	.genpd = {
		.name = "VSMPS3",
		.power_off = pd_power_off,
		.power_on = pd_power_on,
	},
	.epod_id = UX500_EPOD_NONE,
};

static struct dbx500_powerdomain_info ux500_pm_domain_vrf1 = {
	.genpd = {
		.name = "VRF1",
		.power_off = pd_power_off,
		.power_on = pd_power_on,
	},
	.epod_id = UX500_EPOD_NONE,
};

/* The following are technically children of VAPE */
static struct dbx500_powerdomain_info ux500_pm_domain_sva_mmdsp = {
	/* Vape_SVA_MMDSP_pwr */
	.genpd = {
		.name = "SVA_MMDSP",
		.power_off = pd_power_off,
		.power_on = pd_power_on,
	},
	.epod_id = EPOD_ID_SVAMMDSP,
};

static struct dbx500_powerdomain_info ux500_pm_domain_sva_pipe = {
	/* Vape_SVA_pwr */
	.genpd = {
		.name = "SVA_PIPE",
		.power_off = pd_power_off,
		.power_on = pd_power_on,
	},
	.epod_id = EPOD_ID_SVAPIPE,
};

static struct dbx500_powerdomain_info ux500_pm_domain_sia_mmdsp = {
	/* Vape_SIA_MMDSP_pwr */
	.genpd = {
		.name = "SIA_MMDSP",
		.power_off = pd_power_off,
		.power_on = pd_power_on,
	},
	.epod_id = EPOD_ID_SIAMMDSP,
};

static struct dbx500_powerdomain_info ux500_pm_domain_sia_pipe = {
	/* Vape_SIA_pwr */
	.genpd = {
		.name = "SIA_PIPE",
		.power_off = pd_power_off,
		.power_on = pd_power_on,
	},
	.epod_id = EPOD_ID_SIAPIPE,
};

static struct dbx500_powerdomain_info ux500_pm_domain_sga = {
	/* Vape_SGA_pwr */
	.genpd = {
		.name = "SGA",
		.power_off = pd_power_off,
		.power_on = pd_power_on,
	},
	.epod_id = EPOD_ID_SGA,
};

static struct dbx500_powerdomain_info ux500_pm_domain_b2r2_mcde = {
	/* Vape_DSS_pwr DSS (display subsystem) */
	.genpd = {
		.name = "B2R2_MCDE",
		.power_off = pd_power_off,
		.power_on = pd_power_on,
	},
	.epod_id = EPOD_ID_B2R2_MCDE,
};

static struct dbx500_powerdomain_info ux500_pm_domain_esram_12 = {
	/* Vape_esram0_pwr, Vape_esram1_pwr */
	.genpd = {
		.name = "ESRAM_12",
		.power_off = pd_power_off,
		.power_on = pd_power_on,
	},
	.epod_id = EPOD_ID_ESRAM12,
};

static struct dbx500_powerdomain_info ux500_pm_domain_esram_34 = {
	/* Vape_esram3_pwr, Vape_esram4_pwr */
	.genpd = {
		.name = "ESRAM_34",
		.power_off = pd_power_off,
		.power_on = pd_power_on,
	},
	.epod_id = EPOD_ID_ESRAM34,
};

static struct generic_pm_domain *ux500_pm_domains[NR_DOMAINS] = {
	[DOMAIN_VAPE] = &ux500_pm_domain_vape.genpd,
	[DOMAIN_VARM] = &ux500_pm_domain_varm.genpd,
	[DOMAIN_VMODEM] = &ux500_pm_domain_vmodem.genpd,
	[DOMAIN_VPLL] = &ux500_pm_domain_vpll.genpd,
	[DOMAIN_VSMPS1] = &ux500_pm_domain_vsmps1.genpd,
	[DOMAIN_VSMPS2] = &ux500_pm_domain_vsmps2.genpd,
	[DOMAIN_VSMPS3] = &ux500_pm_domain_vsmps3.genpd,
	[DOMAIN_VRF1] = &ux500_pm_domain_vrf1.genpd,
	[DOMAIN_SVA_MMDSP] = &ux500_pm_domain_sva_mmdsp.genpd,
	[DOMAIN_SVA_PIPE] = &ux500_pm_domain_sva_pipe.genpd,
	[DOMAIN_SIA_MMDSP] = &ux500_pm_domain_sia_mmdsp.genpd,
	[DOMAIN_SIA_PIPE] = &ux500_pm_domain_sia_pipe.genpd,
	[DOMAIN_SGA] = &ux500_pm_domain_sga.genpd,
	[DOMAIN_B2R2_MCDE] = &ux500_pm_domain_b2r2_mcde.genpd,
	[DOMAIN_ESRAM_12] = &ux500_pm_domain_esram_12.genpd,
	[DOMAIN_ESRAM_34] = &ux500_pm_domain_esram_34.genpd,
};

static const struct of_device_id ux500_pm_domain_matches[] = {
	{ .compatible = "stericsson,ux500-pm-domains", },
	{ },
};

static int ux500_pm_domain_add_subdomain(struct generic_pm_domain *domain)
{
	return pm_genpd_add_subdomain(&ux500_pm_domain_vape.genpd, domain);
}

static int ux500_pm_domains_add_subdomains(void)
{
	int ret;

	ret = ux500_pm_domain_add_subdomain(&ux500_pm_domain_sva_mmdsp.genpd);
	if (ret)
		return ret;

	ret = ux500_pm_domain_add_subdomain(&ux500_pm_domain_sva_pipe.genpd);
	if (ret)
		return ret;

	ret = ux500_pm_domain_add_subdomain(&ux500_pm_domain_sia_mmdsp.genpd);
	if (ret)
		return ret;

	ret = ux500_pm_domain_add_subdomain(&ux500_pm_domain_sia_pipe.genpd);
	if (ret)
		return ret;

	ret = ux500_pm_domain_add_subdomain(&ux500_pm_domain_sga.genpd);
	if (ret)
		return ret;

	return ux500_pm_domain_add_subdomain(&ux500_pm_domain_b2r2_mcde.genpd);
}

static int ux500_pm_domains_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct genpd_onecell_data *genpd_data;
	int i;
	int ret;

	if (!np)
		return -ENODEV;

	genpd_data = kzalloc_obj(*genpd_data);
	if (!genpd_data)
		return -ENOMEM;

	genpd_data->domains = ux500_pm_domains;
	genpd_data->num_domains = ARRAY_SIZE(ux500_pm_domains);

	for (i = 0; i < ARRAY_SIZE(ux500_pm_domains); ++i)
		pm_genpd_init(ux500_pm_domains[i], NULL, true);

	ret = ux500_pm_domains_add_subdomains();
	if (ret)
		return ret;

	of_genpd_add_provider_onecell(np, genpd_data);
	return 0;
}

static struct platform_driver ux500_pm_domains_driver = {
	.probe  = ux500_pm_domains_probe,
	.driver = {
		.name = "ux500_pm_domains",
		.of_match_table = ux500_pm_domain_matches,
	},
};

static int __init ux500_pm_domains_init(void)
{
	return platform_driver_register(&ux500_pm_domains_driver);
}
arch_initcall(ux500_pm_domains_init);
