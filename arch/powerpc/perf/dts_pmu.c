// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <asm/reg.h>
#include <asm/dts_pmu.h>


extern void unregister_power_pmu(struct power_pmu *pmu);
static u32 pmu_dts_nr_pmc;

u32 mmcr_regs_sprs[MAX_MMCR];
int mmcr_count;

struct pmu_dts_event {
	struct device_attribute attr;
	char name[32];
	char config[32];
};

static struct pmu_dts_event *dts_events[MAX_DTS_EVENTS];
static struct attribute *pmu_dts_events_attrs[MAX_DTS_EVENTS + 1];
static int dts_event_count;

static ssize_t pmu_dts_event_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct pmu_dts_event *evt =
			container_of(attr, struct pmu_dts_event, attr);

	return sprintf(buf, "%s\n", evt->config);
}

/* Format attributes */
PMU_FORMAT_ATTR(event, "config:0-59");
PMU_FORMAT_ATTR(pmcsel, "config1:18-25");

static struct attribute *pmu_dts_format_attrs[] = {
	&format_attr_event.attr,
	&format_attr_pmcsel.attr,
	NULL,
};

static struct attribute_group pmu_dts_format_group = {
	.name = "format",
	.attrs = pmu_dts_format_attrs,
};

static ssize_t nr_pmc_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", pmu_dts_nr_pmc);
}

static DEVICE_ATTR_RO(nr_pmc);

static struct attribute *pmu_dts_dt_attrs[] = {
	&dev_attr_nr_pmc.attr,
	NULL,
};

static struct attribute_group pmu_dts_dt_group = {
	.name = "dt",
	.attrs = pmu_dts_dt_attrs,
};

static struct attribute_group pmu_dts_events_group = {
	.name = "events",
	.attrs = pmu_dts_events_attrs,
};

static const struct attribute_group *pmu_dts_attr_groups[] = {
	&pmu_dts_events_group,
	&pmu_dts_format_group,
	&pmu_dts_dt_group,
	NULL,
};

static struct power_pmu dts_pmu = {
	.name           = "cpu_dts",
	.n_counter      = MAX_PMU_COUNTERS,
	.attr_groups    = pmu_dts_attr_groups,
};

/* Device Tree match */
static const struct of_device_id pmu_dts_of_match[] = {
	{ .compatible = "ibm,power-pmu" },
	{ }
};
MODULE_DEVICE_TABLE(of, pmu_dts_of_match);

/* Probe function */
static int pmu_dts_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node, *child, *events_np;
	struct device_node *sprs_np, *mmcr_np, *mmcr_child;
	struct pmu_dts_event *evt;
	u64 code;
	u32 code32, sprn;
	u32 code64[2];
	u32 code128[4];
	int cells;
	const char *str;

	pr_info("PMU DTS probe node = %s\n", np->full_name);

	if (!of_property_present(np, "nr_pmc")) {

		for_each_child_of_node(np, child) {
			pr_info("child node = %s\n", child->full_name);

			if (of_property_present(child, "nr_pmc")) {
				np = child;
				break;
			}
		}
	}

	if (of_property_read_u32(np, "nr_pmc", &pmu_dts_nr_pmc)) {
		pr_err("pmu_dts: nr_pmc not found in %s\n", np->full_name);
		return -EINVAL;
	}

	if (!of_property_read_string(np, "pmu-name", &str))
		pr_info("PMU Name: %s\n", str);

	if (!of_property_read_string(np, "pmu-version", &str))
		pr_info("PMU Version: %s\n", str);

	if (!of_property_read_string(np, "platform", &str))
		pr_info("Platform: %s\n", str);

	sprs_np = of_get_child_by_name(np, "sprs");
	if (!sprs_np) {
		pr_err("pmu_dts: no sprs node\n");
		return -EINVAL;
	}

	mmcr_np = of_get_child_by_name(sprs_np, "mmcr");
	if (!mmcr_np) {
		pr_err("pmu_dts: no mmcr node\n");
		return -EINVAL;
	}

	mmcr_count = 0;
	for_each_child_of_node(mmcr_np, mmcr_child) {

		if (of_property_read_u32(mmcr_child, "sprn", &sprn))
			continue;

		mmcr_regs_sprs[mmcr_count++] = sprn;
		pr_info("pmu_dts: MMCR[%d] = %u (%s)\n", mmcr_count - 1,
				sprn, mmcr_child->name);

		if (mmcr_count >= MAX_MMCR)
			break;
	}

	if (!mmcr_count) {
		pr_err("pmu_dts: no MMCR SPRs found\n");
		return -EINVAL;
	}

	/* Parse events */
	events_np = of_get_child_by_name(np, "events");
	if (!events_np) {
		pr_err("pmu_dts: no events node found\n");
		return -EINVAL;
	}

	dts_event_count = 0;
	for_each_child_of_node(events_np, child) {
		if (!of_device_is_available(child))
			continue;

		cells = of_property_count_u32_elems(child, "event_code");
		if (cells == 1) {
			if (of_property_read_u32(child, "event_code", &code32))
				continue;

			code = code32;

		} else if (cells == 2) {
			if (of_property_read_u32_array(child, "event_code", code64, 2))
				continue;
			code = ((u64)code64[0] << 32) | code64[1];

		} else if (cells == 4) {
			if (of_property_read_u32_array(child, "event_code", code128, 4))
				continue;
			code = ((u64)code128[1] << 32) | code128[3];

		} else {
			pr_warn("pmu_dts: invalid event_code for %s\n", child->name);
			continue;
		}

		evt = kzalloc(sizeof(*evt), GFP_KERNEL);
		if (!evt)
			continue;

		snprintf(evt->name, sizeof(evt->name), "%s", child->name);
		snprintf(evt->config, sizeof(evt->config), "event=0x%llx", code);

		sysfs_attr_init(&evt->attr.attr);
		evt->attr.attr.name = evt->name;
		evt->attr.attr.mode = 0444;
		evt->attr.show = pmu_dts_event_show;
		dts_events[dts_event_count] = evt;
		pmu_dts_events_attrs[dts_event_count] = &evt->attr.attr;
		dts_event_count++;

		if (dts_event_count >= MAX_DTS_EVENTS)
			break;
	}
	pmu_dts_events_attrs[dts_event_count] = NULL;

	/* Register PMU */
	pr_info("pmu_dts: registering PMU\n");
	return register_power_pmu(&dts_pmu);
}

/* Platform driver */
static struct platform_driver pmu_dts_driver = {
	.probe = pmu_dts_probe,
	.driver = {
		.name = "pmu_dts",
		.of_match_table = pmu_dts_of_match,
	},
};

static int __init pmu_dts_init(void)
{
	pr_info("pmu_dts: init\n");
	return platform_driver_register(&pmu_dts_driver);
}

static void __exit pmu_dts_exit(void)
{
	pr_info("pmu_dts: exit\n");
	platform_driver_unregister(&pmu_dts_driver);
	unregister_power_pmu(&dts_pmu);
}
module_init(pmu_dts_init);
module_exit(pmu_dts_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shivani Nittor");
MODULE_DESCRIPTION("PMU DTS driver");
