// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the Uncore PCI PMUs in Fujitsu chips.
 *
 * See Documentation/admin-guide/perf/fujitsu_pci_pmu.rst for more details.
 *
 * This driver is based on drivers/perf/qcom_l3_pmu.c
 * Copyright (c) 2015-2017, The Linux Foundation. All rights reserved.
 * Copyright (c) 2024 Fujitsu. All rights reserved.
 */

#include <linux/acpi.h>
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/platform_device.h>

/*
 * General constants
 */

/* Number of counters on each PMU */
#define PCI_NUM_COUNTERS  8
/* Mask for the event type field within perf_event_attr.config and EVTYPE reg */
#define PCI_EVTYPE_MASK   0xFF

/*
 * Register offsets
 */

/* Perfmon registers */
#define PCI_PM_EVCNTR(__cntr) (0x000 + ((__cntr) & 0x7) * 8)
#define PCI_PM_CNTCTL(__cntr) (0x100 + ((__cntr) & 0x7) * 8)
#define PCI_PM_EVTYPE(__cntr) (0x200 + ((__cntr) & 0x7) * 8)
#define PCI_PM_CR         0x400
#define PCI_PM_CNTENSET   0x410
#define PCI_PM_CNTENCLR   0x418
#define PCI_PM_INTENSET   0x420
#define PCI_PM_INTENCLR   0x428
#define PCI_PM_OVSR       0x440

/*
 * Bit field definitions
 */

/* PCI_PM_CNTCTLx */
#define PMCNT_RESET           (0)

/* PCI_PM_EVTYPEx */
#define EVSEL(__val)          ((__val) & PCI_EVTYPE_MASK)

/* PCI_PM_CR */
#define PM_RESET              (1UL << 1)
#define PM_ENABLE             (1UL << 0)

/* PCI_PM_CNTENSET */
#define PMCNTENSET(__cntr)    (1UL << ((__cntr) & 0x7))

/* PCI_PM_CNTENCLR */
#define PMCNTENCLR(__cntr)    (1UL << ((__cntr) & 0x7))
#define PM_CNTENCLR_RESET     (0xFF)

/* PCI_PM_INTENSET */
#define PMINTENSET(__cntr)    (1UL << ((__cntr) & 0x7))

/* PCI_PM_INTENCLR */
#define PMINTENCLR(__cntr)    (1UL << ((__cntr) & 0x7))
#define PM_INTENCLR_RESET     (0xFF)

/* PCI_PM_OVSR */
#define PMOVSRCLR(__cntr)     (1UL << ((__cntr) & 0x7))
#define PMOVSRCLR_RESET       (0xFF)

/*
 * Events
 */

#define PCI_EVENT_PORT0_CYCLES		0x000
#define PCI_EVENT_PORT0_READ_COUNT	0x010
#define PCI_EVENT_PORT0_READ_COUNT_BUS	0x014
#define PCI_EVENT_PORT0_WRITE_COUNT	0x020
#define PCI_EVENT_PORT0_WRITE_COUNT_BUS	0x024
#define PCI_EVENT_PORT1_CYCLES		0x040
#define PCI_EVENT_PORT1_READ_COUNT	0x050
#define PCI_EVENT_PORT1_READ_COUNT_BUS	0x054
#define PCI_EVENT_PORT1_WRITE_COUNT	0x060
#define PCI_EVENT_PORT1_WRITE_COUNT_BUS	0x064
#define PCI_EVENT_EA_PCI		0x080

/*
 * Main PMU, inherits from the core perf PMU type
 */
struct pci_pmu {
	struct pmu		pmu;
	struct hlist_node	node;
	void __iomem		*regs;
	struct perf_event	*events[PCI_NUM_COUNTERS];
	unsigned long		used_mask[BITS_TO_LONGS(PCI_NUM_COUNTERS)];
	cpumask_t		cpumask;
};

#define to_pci_pmu(p) (container_of(p, struct pci_pmu, pmu))

/*
 * Implementation of standard counter operations
 */

static void fujitsu_pci_counter_start(struct perf_event *event)
{
	struct pci_pmu *pcipmu = to_pci_pmu(event->pmu);
	int idx = event->hw.idx;

	/* Initialize the hardware counter and reset prev_count*/
	local64_set(&event->hw.prev_count, 0);
	writeq_relaxed(0, pcipmu->regs + PCI_PM_EVCNTR(idx));

	/* Set the event type */
	writeq_relaxed(EVSEL(event->attr.config), pcipmu->regs + PCI_PM_EVTYPE(idx));

	/* Enable interrupt generation by this counter */
	writeq_relaxed(PMINTENSET(idx), pcipmu->regs + PCI_PM_INTENSET);

	/* Finally, enable the counter */
	writeq_relaxed(PMCNT_RESET, pcipmu->regs + PCI_PM_CNTCTL(idx));
	writeq_relaxed(PMCNTENSET(idx), pcipmu->regs + PCI_PM_CNTENSET);
}

static void fujitsu_pci_counter_stop(struct perf_event *event,
					      int flags)
{
	struct pci_pmu *pcipmu = to_pci_pmu(event->pmu);
	int idx = event->hw.idx;

	/* Disable the counter */
	writeq_relaxed(PMCNTENCLR(idx), pcipmu->regs + PCI_PM_CNTENCLR);

	/* Disable interrupt generation by this counter */
	writeq_relaxed(PMINTENCLR(idx), pcipmu->regs + PCI_PM_INTENCLR);
}

static void fujitsu_pci_counter_update(struct perf_event *event)
{
	struct pci_pmu *pcipmu = to_pci_pmu(event->pmu);
	int idx = event->hw.idx;
	u64 prev, new;

	do {
		prev = local64_read(&event->hw.prev_count);
		new = readq_relaxed(pcipmu->regs + PCI_PM_EVCNTR(idx));
	} while (local64_cmpxchg(&event->hw.prev_count, prev, new) != prev);

	local64_add(new - prev, &event->count);
}

/*
 * Top level PMU functions.
 */

static inline void fujitsu_pci__init(struct pci_pmu *pcipmu)
{
	int i;

	writeq_relaxed(PM_RESET, pcipmu->regs + PCI_PM_CR);

	writeq_relaxed(PM_CNTENCLR_RESET, pcipmu->regs + PCI_PM_CNTENCLR);
	writeq_relaxed(PM_INTENCLR_RESET, pcipmu->regs + PCI_PM_INTENCLR);
	writeq_relaxed(PMOVSRCLR_RESET, pcipmu->regs + PCI_PM_OVSR);

	for (i = 0; i < PCI_NUM_COUNTERS; ++i) {
		writeq_relaxed(PMCNT_RESET, pcipmu->regs + PCI_PM_CNTCTL(i));
		writeq_relaxed(EVSEL(0), pcipmu->regs + PCI_PM_EVTYPE(i));
	}

	/*
	 * Use writeq here to ensure all programming commands are done
	 *  before proceeding
	 */
	writeq(PM_ENABLE, pcipmu->regs + PCI_PM_CR);
}

static irqreturn_t fujitsu_pci__handle_irq(int irq_num, void *data)
{
	struct pci_pmu *pcipmu = data;
	/* Read the overflow status register */
	long status = readq_relaxed(pcipmu->regs + PCI_PM_OVSR);
	int idx;

	if (status == 0)
		return IRQ_NONE;

	/* Clear the bits we read on the overflow status register */
	writeq_relaxed(status, pcipmu->regs + PCI_PM_OVSR);

	for_each_set_bit(idx, &status, PCI_NUM_COUNTERS) {
		struct perf_event *event;

		event = pcipmu->events[idx];
		if (!event)
			continue;

		fujitsu_pci_counter_update(event);
	}

	return IRQ_HANDLED;
}

/*
 * Implementation of abstract pmu functionality required by
 * the core perf events code.
 */

static void fujitsu_pci__pmu_enable(struct pmu *pmu)
{
	struct pci_pmu *pcipmu = to_pci_pmu(pmu);

	/* Ensure the other programming commands are observed before enabling */
	wmb();

	writeq_relaxed(PM_ENABLE, pcipmu->regs + PCI_PM_CR);
}

static void fujitsu_pci__pmu_disable(struct pmu *pmu)
{
	struct pci_pmu *pcipmu = to_pci_pmu(pmu);

	writeq_relaxed(0, pcipmu->regs + PCI_PM_CR);

	/* Ensure the basic counter unit is stopped before proceeding */
	wmb();
}

/*
 * We must NOT create groups containing events from multiple hardware PMUs,
 * although mixing different software and hardware PMUs is allowed.
 */
static bool fujitsu_pci__validate_event_group(struct perf_event *event)
{
	struct perf_event *leader = event->group_leader;
	struct perf_event *sibling;
	int counters = 0;

	if (leader->pmu != event->pmu && !is_software_event(leader))
		return false;

	/* The sum of the counters used by the event and its leader event */
	counters = 2;

	for_each_sibling_event(sibling, leader) {
		if (is_software_event(sibling))
			continue;
		if (sibling->pmu != event->pmu)
			return false;
		counters += 1;
	}

	/*
	 * If the group requires more counters than the HW has, it
	 * cannot ever be scheduled.
	 */
	return counters <= PCI_NUM_COUNTERS;
}

static int fujitsu_pci__event_init(struct perf_event *event)
{
	struct pci_pmu *pcipmu = to_pci_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	/*
	 * Is the event for this PMU?
	 */
	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/*
	 * Sampling not supported since these events are not core-attributable.
	 */
	if (hwc->sample_period)
		return -EINVAL;

	/*
	 * Task mode not available, we run the counters as socket counters,
	 * not attributable to any CPU and therefore cannot attribute per-task.
	 */
	if (event->cpu < 0)
		return -EINVAL;

	/* Validate the group */
	if (!fujitsu_pci__validate_event_group(event))
		return -EINVAL;

	hwc->idx = -1;

	/*
	 * Many perf core operations (eg. events rotation) operate on a
	 * single CPU context. This is obvious for CPU PMUs, where one
	 * expects the same sets of events being observed on all CPUs,
	 * but can lead to issues for off-core PMUs, like this one, where
	 * each event could be theoretically assigned to a different CPU.
	 * To mitigate this, we enforce CPU assignment to one designated
	 * processor (the one described in the "cpumask" attribute exported
	 * by the PMU device). perf user space tools honor this and avoid
	 * opening more than one copy of the events.
	 */
	event->cpu = cpumask_first(&pcipmu->cpumask);

	return 0;
}

static void fujitsu_pci__event_start(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	hwc->state = 0;
	fujitsu_pci_counter_start(event);
}

static void fujitsu_pci__event_stop(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	if (hwc->state & PERF_HES_STOPPED)
		return;

	fujitsu_pci_counter_stop(event, flags);
	if (flags & PERF_EF_UPDATE)
		fujitsu_pci_counter_update(event);
	hwc->state |= PERF_HES_STOPPED | PERF_HES_UPTODATE;
}

static int fujitsu_pci__event_add(struct perf_event *event, int flags)
{
	struct pci_pmu *pcipmu = to_pci_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	int idx;

	/*
	 * Try to allocate a counter.
	 */
	idx = bitmap_find_free_region(pcipmu->used_mask, PCI_NUM_COUNTERS, 0);
	if (idx < 0)
		/* The counters are all in use. */
		return -EAGAIN;

	hwc->idx = idx;
	hwc->state = PERF_HES_STOPPED | PERF_HES_UPTODATE;
	pcipmu->events[idx] = event;

	if (flags & PERF_EF_START)
		fujitsu_pci__event_start(event, 0);

	/* Propagate changes to the userspace mapping. */
	perf_event_update_userpage(event);

	return 0;
}

static void fujitsu_pci__event_del(struct perf_event *event, int flags)
{
	struct pci_pmu *pcipmu = to_pci_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	/* Stop and clean up */
	fujitsu_pci__event_stop(event,  flags | PERF_EF_UPDATE);
	pcipmu->events[hwc->idx] = NULL;
	bitmap_release_region(pcipmu->used_mask, hwc->idx, 0);

	/* Propagate changes to the userspace mapping. */
	perf_event_update_userpage(event);
}

static void fujitsu_pci__event_read(struct perf_event *event)
{
	fujitsu_pci_counter_update(event);
}

/*
 * Add sysfs attributes
 *
 * We export:
 * - formats, used by perf user space and other tools to configure events
 * - events, used by perf user space and other tools to create events
 *   symbolically, e.g.:
 *     perf stat -a -e pci_iod0_pci0/event=0x24/ ls
 * - cpumask, used by perf user space and other tools to know on which CPUs
 *   to open the events
 */

/* formats */

#define PCI_PMU_FORMAT_ATTR(_name, _config)				      \
	(&((struct dev_ext_attribute[]) {				      \
		{ .attr = __ATTR(_name, 0444, device_show_string, NULL),      \
		  .var = (void *) _config, }				      \
	})[0].attr.attr)

static struct attribute *fujitsu_pci_pmu_formats[] = {
	PCI_PMU_FORMAT_ATTR(event, "config:0-7"),
	NULL,
};

static const struct attribute_group fujitsu_pci_pmu_format_group = {
	.name = "format",
	.attrs = fujitsu_pci_pmu_formats,
};

/* events */

static ssize_t pci_pmu_event_show(struct device *dev,
				     struct device_attribute *attr, char *page)
{
	struct perf_pmu_events_attr *pmu_attr;

	pmu_attr = container_of(attr, struct perf_pmu_events_attr, attr);
	return sysfs_emit(page, "event=0x%02llx\n", pmu_attr->id);
}

#define PCI_EVENT_ATTR(_name, _id)					     \
	PMU_EVENT_ATTR_ID(_name, pci_pmu_event_show, _id)

static struct attribute *fujitsu_pci_pmu_events[] = {
	PCI_EVENT_ATTR(pci-port0-cycles, PCI_EVENT_PORT0_CYCLES),
	PCI_EVENT_ATTR(pci-port0-read-count, PCI_EVENT_PORT0_READ_COUNT),
	PCI_EVENT_ATTR(pci-port0-read-count-bus, PCI_EVENT_PORT0_READ_COUNT_BUS),
	PCI_EVENT_ATTR(pci-port0-write-count, PCI_EVENT_PORT0_WRITE_COUNT),
	PCI_EVENT_ATTR(pci-port0-write-count-bus, PCI_EVENT_PORT0_WRITE_COUNT_BUS),
	PCI_EVENT_ATTR(pci-port1-cycles, PCI_EVENT_PORT1_CYCLES),
	PCI_EVENT_ATTR(pci-port1-read-count, PCI_EVENT_PORT1_READ_COUNT),
	PCI_EVENT_ATTR(pci-port1-read-count-bus, PCI_EVENT_PORT1_READ_COUNT_BUS),
	PCI_EVENT_ATTR(pci-port1-write-count, PCI_EVENT_PORT1_WRITE_COUNT),
	PCI_EVENT_ATTR(pci-port1-write-count_bus, PCI_EVENT_PORT1_WRITE_COUNT_BUS),
	PCI_EVENT_ATTR(ea-pci, PCI_EVENT_EA_PCI),
	NULL
};

static const struct attribute_group fujitsu_pci_pmu_events_group = {
	.name = "events",
	.attrs = fujitsu_pci_pmu_events,
};

/* cpumask */

static ssize_t cpumask_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct pci_pmu *pcipmu = to_pci_pmu(dev_get_drvdata(dev));

	return cpumap_print_to_pagebuf(true, buf, &pcipmu->cpumask);
}

static DEVICE_ATTR_RO(cpumask);

static struct attribute *fujitsu_pci_pmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static const struct attribute_group fujitsu_pci_pmu_cpumask_attr_group = {
	.attrs = fujitsu_pci_pmu_cpumask_attrs,
};

/*
 * Per PMU device attribute groups
 */
static const struct attribute_group *fujitsu_pci_pmu_attr_grps[] = {
	&fujitsu_pci_pmu_format_group,
	&fujitsu_pci_pmu_events_group,
	&fujitsu_pci_pmu_cpumask_attr_group,
	NULL,
};

/*
 * Probing functions and data.
 */

static int fujitsu_pci_pmu_online_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct pci_pmu *pcipmu = hlist_entry_safe(node, struct pci_pmu, node);

	/* If there is not a CPU/PMU association pick this CPU */
	if (cpumask_empty(&pcipmu->cpumask))
		cpumask_set_cpu(cpu, &pcipmu->cpumask);

	return 0;
}

static int fujitsu_pci_pmu_offline_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct pci_pmu *pcipmu = hlist_entry_safe(node, struct pci_pmu, node);
	unsigned int target;

	if (!cpumask_test_and_clear_cpu(cpu, &pcipmu->cpumask))
		return 0;
	target = cpumask_any_but(cpu_online_mask, cpu);
	if (target >= nr_cpu_ids)
		return 0;
	perf_pmu_migrate_context(&pcipmu->pmu, cpu, target);
	cpumask_set_cpu(target, &pcipmu->cpumask);
	return 0;
}

static int fujitsu_pci_pmu_probe(struct platform_device *pdev)
{
	struct pci_pmu *pcipmu;
	struct acpi_device *acpi_dev;
	struct resource *memrc;
	int ret;
	char *name;
	u64 uid;

	/* Initialize the PMU data structures */

	acpi_dev = ACPI_COMPANION(&pdev->dev);
	if (!acpi_dev)
		return -ENODEV;

	ret = acpi_dev_uid_to_integer(acpi_dev, &uid);
	if (ret) {
		dev_err(&pdev->dev, "unable to read ACPI uid\n");
		return ret;
	}

	pcipmu = devm_kzalloc(&pdev->dev, sizeof(*pcipmu), GFP_KERNEL);
	name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "pci_iod%llu_pci%llu",
			  (uid >> 4) & 0xF, uid & 0xF);
	if (!pcipmu || !name)
		return -ENOMEM;

	pcipmu->pmu = (struct pmu) {
		.parent		= &pdev->dev,
		.task_ctx_nr	= perf_invalid_context,

		.pmu_enable	= fujitsu_pci__pmu_enable,
		.pmu_disable	= fujitsu_pci__pmu_disable,
		.event_init	= fujitsu_pci__event_init,
		.add		= fujitsu_pci__event_add,
		.del		= fujitsu_pci__event_del,
		.start		= fujitsu_pci__event_start,
		.stop		= fujitsu_pci__event_stop,
		.read		= fujitsu_pci__event_read,

		.attr_groups	= fujitsu_pci_pmu_attr_grps,
		.capabilities	= PERF_PMU_CAP_NO_EXCLUDE,
	};

	pcipmu->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &memrc);
	if (IS_ERR(pcipmu->regs))
		return PTR_ERR(pcipmu->regs);

	fujitsu_pci__init(pcipmu);

	ret = platform_get_irq(pdev, 0);
	if (ret <= 0)
		return ret;

	ret = devm_request_irq(&pdev->dev, ret, fujitsu_pci__handle_irq, 0,
			       name, pcipmu);
	if (ret) {
		dev_err(&pdev->dev, "Request for IRQ failed for slice @%pa\n",
			&memrc->start);
		return ret;
	}

	/* Add this instance to the list used by the offline callback */
	ret = cpuhp_state_add_instance(CPUHP_AP_PERF_ARM_FUJITSU_PCI_ONLINE, &pcipmu->node);
	if (ret) {
		dev_err(&pdev->dev, "Error %d registering hotplug", ret);
		return ret;
	}

	ret = perf_pmu_register(&pcipmu->pmu, name, -1);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to register PCI PMU (%d)\n", ret);
		return ret;
	}

	dev_info(&pdev->dev, "Registered %s, type: %d\n", name, pcipmu->pmu.type);

	return 0;
}

static const struct acpi_device_id fujitsu_pci_pmu_acpi_match[] = {
	{ "FUJI200D", },
	{ }
};
MODULE_DEVICE_TABLE(acpi, fujitsu_pci_pmu_acpi_match);

static struct platform_driver fujitsu_pci_pmu_driver = {
	.driver = {
		.name = "fujitsu-pci-pmu",
		.acpi_match_table = ACPI_PTR(fujitsu_pci_pmu_acpi_match),
		.suppress_bind_attrs = true,
	},
	.probe = fujitsu_pci_pmu_probe,
};

static int __init register_fujitsu_pci_pmu_driver(void)
{
	int ret;

	/* Install a hook to update the reader CPU in case it goes offline */
	ret = cpuhp_setup_state_multi(CPUHP_AP_PERF_ARM_FUJITSU_PCI_ONLINE,
				      "perf/fujitsu/pci:online",
				      fujitsu_pci_pmu_online_cpu,
				      fujitsu_pci_pmu_offline_cpu);
	if (ret)
		return ret;

	return platform_driver_register(&fujitsu_pci_pmu_driver);
}
device_initcall(register_fujitsu_pci_pmu_driver);
