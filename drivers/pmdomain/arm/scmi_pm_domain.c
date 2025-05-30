// SPDX-License-Identifier: GPL-2.0
/*
 * SCMI Generic power domain support.
 *
 * Copyright (C) 2018-2021 ARM Ltd.
 */

#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pm_domain.h>
#include <linux/scmi_protocol.h>
#include <linux/of.h>


static const struct scmi_power_proto_ops *power_ops;

struct scmi_pm_domain {
	struct generic_pm_domain genpd;
	const struct scmi_protocol_handle *ph;
	const char *name;
	u32 domain;
};

#define to_scmi_pd(gpd) container_of(gpd, struct scmi_pm_domain, genpd)

static int scmi_pd_power(struct generic_pm_domain *domain, bool power_on)
{
	u32 state;
	struct scmi_pm_domain *pd = to_scmi_pd(domain);

	if (power_on)
		state = SCMI_POWER_STATE_GENERIC_ON;
	else
		state = SCMI_POWER_STATE_GENERIC_OFF;

	return power_ops->state_set(pd->ph, pd->domain, state);
}

static int scmi_pd_power_on(struct generic_pm_domain *domain)
{
	return scmi_pd_power(domain, true);
}

static int scmi_pd_power_off(struct generic_pm_domain *domain)
{
	return scmi_pd_power(domain, false);
}

static int scmi_pm_domain_probe(struct scmi_device *sdev)
{
	struct device *dev = &sdev->dev;
	struct device_node *np;
	struct scmi_pm_domain *scmi_pd;
	struct of_phandle_args args;
	const struct scmi_handle *handle = sdev->handle;
	struct scmi_protocol_handle *ph;
	struct genpd_onecell_data *scmi_pd_data;
	struct generic_pm_domain **domains;
	int max_id = -1;
	int index, num_domains;
	ktime_t start_time = ktime_get();
	unsigned long *domain_ids;

	dev_err(dev, "Starting optimized SCMI power domain probe\n");

	if (!handle)
		return -ENODEV;

	power_ops = handle->devm_protocol_get(sdev, SCMI_PROTOCOL_POWER, &ph);
	if (IS_ERR(power_ops))
		return PTR_ERR(power_ops);

	num_domains = power_ops->num_domains_get(ph);
	if (num_domains < 0) {
		dev_err(dev, "number of domains not found\n");
		return num_domains;
	}

	domain_ids = devm_bitmap_zalloc(dev, num_domains, GFP_KERNEL);
	if (!domain_ids)
		return -ENOMEM;

	/* Find referenced domain IDs and mark them in bitmap */
	for_each_node_with_property(np, "power-domains") {
		index = 0;
		while (!of_parse_phandle_with_args(np, "power-domains",
					"#power-domain-cells",
					index, &args)) {
			if (args.args_count >= 1 && args.np == dev->of_node) {
				int id = args.args[0];
				if (id < num_domains) {
					set_bit(id, domain_ids);
					max_id = max(max_id, id);
					dev_dbg(dev, "Found power domain reference %d from node %pOF\n",
							id, np);
				}
			}
			of_node_put(args.np);
			index++;
		}
	}

	if (max_id < 0) {
		dev_warn(dev, "No power domains referenced in device tree\n");
		/* Create provider anyway as domains might be referenced later */
		max_id = 0;
	}

	dev_warn(dev, "Highest referenced domain ID: %d\n", max_id);

	scmi_pd_data = devm_kzalloc(dev, sizeof(*scmi_pd_data), GFP_KERNEL);
	if (!scmi_pd_data)
		return -ENOMEM;

	domains = devm_kcalloc(dev, max_id + 1, sizeof(*domains), GFP_KERNEL);
	if (!domains)
		return -ENOMEM;

	scmi_pd = devm_kcalloc(dev, max_id + 1, sizeof(*scmi_pd), GFP_KERNEL);
	if (!scmi_pd)
		return -ENOMEM;

	/* Initialize only referenced domains */
	for_each_set_bit(index, domain_ids, num_domains) {
		u32 state;

		if (power_ops->state_get(ph, index, &state)) {
			dev_err(dev, "Domain %d not available\n", index);
			continue;
		}

		dev_warn(dev, "Initializing referenced domain %d\n", index);

		scmi_pd->domain = index;
		scmi_pd->ph = ph;
		scmi_pd->name = power_ops->name_get(ph, index);
		scmi_pd->genpd.name = scmi_pd->name;
		scmi_pd->genpd.power_off = scmi_pd_power_off;
		scmi_pd->genpd.power_on = scmi_pd_power_on;
		scmi_pd->genpd.flags = GENPD_FLAG_ACTIVE_WAKEUP;

		if (state == SCMI_POWER_STATE_GENERIC_ON) {
			dev_warn(dev, "Domain %d is ON, registering state\n", index);
			power_ops->state_set(ph, index, state);
		}

		pm_genpd_init(&scmi_pd->genpd, NULL,
			      state == SCMI_POWER_STATE_GENERIC_OFF);

		domains[index] = &scmi_pd->genpd;
		scmi_pd++;
	}

	scmi_pd_data->domains = domains;
	scmi_pd_data->num_domains = max_id + 1;

	dev_set_drvdata(dev, scmi_pd_data);

	dev_err(dev, "SCMI power domains probe completed in %lld us\n",
			ktime_us_delta(ktime_get(), start_time));

	return of_genpd_add_provider_onecell(dev->of_node, scmi_pd_data);
}

static void scmi_pm_domain_remove(struct scmi_device *sdev)
{
	int i;
	struct genpd_onecell_data *scmi_pd_data;
	struct device *dev = &sdev->dev;
	struct device_node *np = dev->of_node;

	of_genpd_del_provider(np);

	scmi_pd_data = dev_get_drvdata(dev);
	for (i = 0; i < scmi_pd_data->num_domains; i++) {
		if (!scmi_pd_data->domains[i])
			continue;
		pm_genpd_remove(scmi_pd_data->domains[i]);
	}
}

static const struct scmi_device_id scmi_id_table[] = {
	{ SCMI_PROTOCOL_POWER, "genpd" },
	{ },
};
MODULE_DEVICE_TABLE(scmi, scmi_id_table);

static struct scmi_driver scmi_power_domain_driver = {
	.name = "scmi-power-domain",
	.probe = scmi_pm_domain_probe,
	.remove = scmi_pm_domain_remove,
	.id_table = scmi_id_table,
};
module_scmi_driver(scmi_power_domain_driver);

MODULE_AUTHOR("Sudeep Holla <sudeep.holla@arm.com>");
MODULE_DESCRIPTION("ARM SCMI power domain driver");
MODULE_LICENSE("GPL v2");
