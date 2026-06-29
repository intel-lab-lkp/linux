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
#include "isa207-common.h"

extern void unregister_power_pmu(struct power_pmu *pmu);
static u32 pmu_dts_nr_pmc;
struct dts_field_map pmcsel_map;
struct dts_field_map pmc_map;
struct dts_field_map field_maps[MAX_FIELDS];
int field_count;

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

static int dts_compute_mmcr(u64 event[], int n_ev,
					unsigned int hwc[], struct mmcr_regs *mmcr,
					struct perf_event *pevents[], u32 flags)
{
	int ret;

	ret = compute_mmcr_dts(event, n_ev, hwc, mmcr, pevents, flags);
	if (!ret)
		mmcr->mmcr0 |= MMCR0_C56RUN;
	return ret;
}

static const unsigned int dts_event_alternatives[][MAX_ALT] = {
	{ 0x600f4, 0x1001e },
};

static int dts_get_alternatives(u64 event, unsigned int flags, u64 alt[])
{
	int num_alt = 0;

	num_alt = isa207_get_alternatives(event, alt,
					  ARRAY_SIZE(dts_event_alternatives), flags,
					  dts_event_alternatives);

	return num_alt;
}

static struct power_pmu dts_pmu = {
	.name                   = "cpu_dts",
	.n_counter              = MAX_PMU_COUNTERS,
	.attr_groups            = pmu_dts_attr_groups,
	.add_fields             = ISA207_ADD_FIELDS,
	.test_adder             = ISA207_TEST_ADDER,
	.group_constraint_mask  = CNST_CACHE_PMC4_MASK,
	.group_constraint_val   = CNST_CACHE_PMC4_VAL,
	.compute_mmcr           = dts_compute_mmcr,
	// .config_bhrb         = power10_config_bhrb,
	// .bhrb_filter_map     = power10_bhrb_filte-r_map,
	.get_alternatives       = dts_get_alternatives,
	.get_mem_data_src       = isa207_get_mem_data_src,
	.get_mem_weight         = isa207_get_mem_weight,
	.disable_pmc            = isa207_disable_pmc,
	.flags                  = PPMU_HAS_SIER | PPMU_ARCH_207S |
					PPMU_ARCH_31 | PPMU_HAS_ATTR_CONFIG1 |
					PPMU_P10,
	.attr_groups            = pmu_dts_attr_groups,
	//.bhrb_nr              = 32,
	.capabilities           = PERF_PMU_CAP_EXTENDED_REGS,
	//.check_attr_config    = power10_check_attr_config,
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
	struct device_node *fmt_np, *field_np;
	u32 bits[2], pgm[2];
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
	fmt_np = of_get_child_by_name(np, "evt_code_format");
	if (!fmt_np) {
		pr_err("pmu_dts: no evt_code_format node\n");
		return -EINVAL;
	}

	field_count = 0;
	for_each_child_of_node(fmt_np, field_np) {

		struct dts_field_map *f = &field_maps[field_count];

		printk("field_np->name is %s\n", field_np->name);
		snprintf(f->name, sizeof(f->name), "%s", field_np->name);
		f->is_pmc = false;
		f->use_target_field_shift = false;

		/* Identify PMC field */
		if (!strcmp(field_np->name, "PMCx"))
			f->is_pmc = true;

		if (of_property_read_u32_array(field_np, "bits", bits, 2))
			continue;

		f->bits_start = bits[0];
		f->bits_end   = bits[1];

		if (of_property_read_u32(field_np, "mmcr", &f->mmcr))
			continue;

	/* Check target_field_shift-based mapping */
		if (!f->is_pmc && f->mmcr != 4) {
			if (!of_property_read_u32(field_np, "target_field_base",
					&f->target_field_base)) {
				of_property_read_u32(field_np, "target_field_shift",
					&f->target_field_shift);
				f->use_target_field_shift = true;

			} else {
				if (!of_property_read_u32_array(field_np, "target_fields", pgm, 2))
					f->pgm_start = pgm[0];
				else
					f->pgm_start = 0;
			}

		} else {
			/* MMCRA or PMC field → no target_field_shift */
			if (!of_property_read_u32_array(field_np, "target_fields", pgm, 2))
				f->pgm_start = pgm[0];
			else
				f->pgm_start = 0;

			f->use_target_field_shift = false;
		}

		field_count++;
		if (field_count >= MAX_FIELDS)
			break;
	}

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
