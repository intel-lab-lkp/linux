// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Platform Performance Measuring Unit (PPMU) core file
 *
 * Copyright (c) 2024-25 Samsung Electronics Co., Ltd.
 *
 * Authors: Vivek Yadav <vivek.2311@samsung.com>
 *          Ravi Patel <ravi.patel@samsung.com>
 */

#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/perf_event.h>
#include "samsung_ppmu.h"

/*
 * PMU format attributes
 */
ssize_t samsung_ppmu_format_sysfs_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *eattr;

	eattr = container_of(attr, struct dev_ext_attribute, attr);

	return sysfs_emit(buf, "%s\n", (char *)eattr->var);
}
EXPORT_SYMBOL_GPL(samsung_ppmu_format_sysfs_show);

/*
 * PMU event attributes
 */
ssize_t samsung_ppmu_event_sysfs_show(struct device *dev,
				      struct device_attribute *attr, char *page)
{
	struct dev_ext_attribute *eattr;

	eattr = container_of(attr, struct dev_ext_attribute, attr);

	return sysfs_emit(page, "config=0x%lx\n", (unsigned long)eattr->var);
}
EXPORT_SYMBOL_GPL(samsung_ppmu_event_sysfs_show);

/*
 * sysfs cpumask attributes. For PPMU, we only have a single CPU to show
 */
ssize_t samsung_ppmu_cpumask_sysfs_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	struct samsung_ppmu *samsung_ppmu = to_samsung_ppmu(dev_get_drvdata(dev));

	return sysfs_emit(buf, "%d\n", samsung_ppmu->on_cpu);
}
EXPORT_SYMBOL_GPL(samsung_ppmu_cpumask_sysfs_show);

ssize_t samsung_ppmu_identifier_attr_show(struct device *dev,
					  struct device_attribute *attr,
					  char *page)
{
	struct samsung_ppmu *samsung_ppmu = to_samsung_ppmu(dev_get_drvdata(dev));

	return sysfs_emit(page, "0x%08x\n", samsung_ppmu->identifier);
}
EXPORT_SYMBOL_GPL(samsung_ppmu_identifier_attr_show);

static irqreturn_t samsung_ppmu_isr(int irq, void *data)
{
	struct samsung_ppmu *samsung_ppmu = data;
	unsigned long overflown;
	int idx;

	overflown = samsung_ppmu->ops->get_int_status(samsung_ppmu);
	if (!overflown)
		return IRQ_NONE;

	/*
	 * Find the counter index which overflowed if the bit was set
	 * and handle it.
	 */
	for_each_set_bit(idx, &overflown, samsung_ppmu->num_counters)
		samsung_ppmu->ops->clear_int_status(samsung_ppmu, idx);

	return IRQ_HANDLED;
}

int samsung_ppmu_init_irq(struct samsung_ppmu *samsung_ppmu,
			  struct platform_device *pdev)
{
	int irq0, irq1, ret, irq_count;

	irq0 = platform_get_irq(pdev, 0);
	if (irq0 < 0) {
		dev_err(&pdev->dev, "Failed to get IRQ 0\n");
		return irq0;
	}

	ret = devm_request_irq(&pdev->dev, irq0, samsung_ppmu_isr,
			       IRQF_NOBALANCING | IRQF_NO_THREAD | IRQF_SHARED,
			       dev_name(&pdev->dev), samsung_ppmu);
	if (ret) {
		dev_err(&pdev->dev,
			"Fail to request IRQ: %d ret: %d.\n", irq0, ret);
		return ret;
	}

	samsung_ppmu->irq0 = irq0;

	irq_count = of_property_count_elems_of_size(pdev->dev.of_node, "interrupts", sizeof(u32));
	if (irq_count > 1) {
		irq1 = platform_get_irq(pdev, 1);
		if (irq1 < 0) {
			dev_err(&pdev->dev, "Failed to get IRQ 0\n");
			return irq1;
		}

		ret = devm_request_irq(&pdev->dev, irq1, samsung_ppmu_isr,
				       IRQF_NOBALANCING | IRQF_NO_THREAD | IRQF_SHARED,
				       dev_name(&pdev->dev), samsung_ppmu);
		if (ret) {
			dev_err(&pdev->dev,
				"Fail to request IRQ: %d ret: %d.\n", irq1, ret);
			return ret;
		}
		samsung_ppmu->irq1 = irq1;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(samsung_ppmu_init_irq);

int samsung_ppmu_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct samsung_ppmu *samsung_ppmu;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/*
	 * We do not support sampling as the counters are all
	 * shared by all CPU cores in a CPU die. Also we
	 * do not support attach to a task(per-process mode)
	 */
	if (is_sampling_event(event) || event->attach_state & PERF_ATTACH_TASK)
		return -EOPNOTSUPP;

	/* PPMU counters not specific to any CPU, so cannot support per-task */
	if (event->cpu < 0)
		return -EINVAL;

	/* Check if events in group does not exceed the available counters */
	samsung_ppmu = to_samsung_ppmu(event->pmu);
	if (event->attr.config > samsung_ppmu->check_event)
		return -EINVAL;

	/*
	 * We don't assign an index until we actually place the event onto
	 * hardware. Use -1 to signify that we haven't decided where to put it
	 * yet.
	 */
	hwc->idx = -1;
	hwc->config_base = event->attr.config;

	/* Enforce to use the same CPU for all events in this PMU */
	event->cpu = samsung_ppmu->on_cpu;

	return 0;
}
EXPORT_SYMBOL_GPL(samsung_ppmu_event_init);

/*
 * Set the counter to count the event that we're interested in,
 * and enable interrupt and counter.
 */
static void samsung_ppmu_enable_event(struct perf_event *event)
{
	struct samsung_ppmu *samsung_ppmu = to_samsung_ppmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	samsung_ppmu->ops->write_evtype(samsung_ppmu, hwc->idx,
					SAMSUNG_PPMU_GET_EVENTID(event));

	samsung_ppmu->ops->enable_counter(samsung_ppmu, hwc);
}

/*
 * Disable counter and interrupt.
 */
static void samsung_ppmu_disable_event(struct perf_event *event)
{
	struct samsung_ppmu *samsung_ppmu = to_samsung_ppmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	samsung_ppmu->ops->disable_counter(samsung_ppmu, hwc);
}

void samsung_ppmu_event_update(struct perf_event *event)
{
	struct samsung_ppmu *samsung_ppmu = to_samsung_ppmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	u64 delta, prev_raw_count, new_raw_count;

	/* read previous counter value */
	prev_raw_count = samsung_ppmu->prev_counter[hwc->idx];

	/* Read the count from the counter register */
	new_raw_count = samsung_ppmu->ops->read_counter(samsung_ppmu, hwc);

	/* compute the delta */
	delta = new_raw_count - prev_raw_count;

	local64_add(delta, &event->count);
}
EXPORT_SYMBOL_GPL(samsung_ppmu_event_update);

void samsung_ppmu_start(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	if (WARN_ON_ONCE(!(hwc->state & PERF_HES_STOPPED)))
		return;

	WARN_ON_ONCE(!(hwc->state & PERF_HES_UPTODATE));
	hwc->state = 0;

	samsung_ppmu_enable_event(event);
	perf_event_update_userpage(event);
}
EXPORT_SYMBOL_GPL(samsung_ppmu_start);

void samsung_ppmu_stop(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	samsung_ppmu_disable_event(event);
	WARN_ON_ONCE(hwc->state & PERF_HES_STOPPED);
	hwc->state |= PERF_HES_STOPPED;

	if (hwc->state & PERF_HES_UPTODATE)
		return;

	/* Read hardware counter and update the perf counter statistics */
	samsung_ppmu_event_update(event);
	hwc->state |= PERF_HES_UPTODATE;
}
EXPORT_SYMBOL_GPL(samsung_ppmu_stop);

int samsung_ppmu_add(struct perf_event *event, int flags)
{
	struct samsung_ppmu *samsung_ppmu = to_samsung_ppmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	int idx;

	hwc->state = PERF_HES_STOPPED | PERF_HES_UPTODATE;

	/* Get an available counter index for counting */
	idx = samsung_ppmu->ops->get_event_idx(event);
	if (idx < 0)
		return idx;

	event->hw.idx = idx;
	samsung_ppmu->pmu_events.hw_events[idx] = event;

	if (flags & PERF_EF_START)
		samsung_ppmu_start(event, PERF_EF_RELOAD);

	return 0;
}
EXPORT_SYMBOL_GPL(samsung_ppmu_add);

void samsung_ppmu_del(struct perf_event *event, int flags)
{
	struct samsung_ppmu *samsung_ppmu = to_samsung_ppmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	samsung_ppmu_stop(event, PERF_EF_UPDATE);

	samsung_ppmu->prev_counter[hwc->idx] = 0;

	/* Clear the event bit */
	clear_bit(hwc->idx, samsung_ppmu->pmu_events.used_mask);
	perf_event_update_userpage(event);
	samsung_ppmu->pmu_events.hw_events[hwc->idx] = NULL;
}
EXPORT_SYMBOL_GPL(samsung_ppmu_del);

void samsung_ppmu_read(struct perf_event *event)
{
	/* Read hardware counter and update the perf counter statistics */
	samsung_ppmu_event_update(event);
}
EXPORT_SYMBOL_GPL(samsung_ppmu_read);

void samsung_ppmu_enable(struct pmu *pmu)
{
	struct samsung_ppmu *samsung_ppmu = to_samsung_ppmu(pmu);
	bool enabled = !bitmap_empty(samsung_ppmu->pmu_events.used_mask,
				     samsung_ppmu->num_counters);

	if (!enabled)
		return;

	samsung_ppmu->ops->start_counters(samsung_ppmu);
}
EXPORT_SYMBOL_GPL(samsung_ppmu_enable);

void samsung_ppmu_disable(struct pmu *pmu)
{
	struct samsung_ppmu *samsung_ppmu = to_samsung_ppmu(pmu);

	samsung_ppmu->ops->stop_counters(samsung_ppmu);
}
EXPORT_SYMBOL_GPL(samsung_ppmu_disable);

void samsung_ppmu_init(struct samsung_ppmu *s_ppmu, struct module *module)
{
	struct pmu *pmu = &s_ppmu->pmu;

	pmu->module		= module;
	pmu->task_ctx_nr	= perf_invalid_context;
	pmu->event_init		= samsung_ppmu_event_init;
	pmu->pmu_enable		= samsung_ppmu_enable;
	pmu->pmu_disable	= samsung_ppmu_disable;
	pmu->add		= samsung_ppmu_add;
	pmu->del		= samsung_ppmu_del;
	pmu->start		= samsung_ppmu_start;
	pmu->stop		= samsung_ppmu_stop;
	pmu->read		= samsung_ppmu_read;
	pmu->attr_groups	= s_ppmu->pmu_events.attr_groups;
	pmu->capabilities	= PERF_PMU_CAP_NO_EXCLUDE;
}
EXPORT_SYMBOL_GPL(samsung_ppmu_init);

MODULE_ALIAS("perf:samsung-ppmu-core");
MODULE_DESCRIPTION("Samsung Platform Performance Measuring Unit (PPMU) driver");
MODULE_AUTHOR("Vivek Yadav <vivek.2311@samsung.com>");
MODULE_AUTHOR("Ravi Patel <ravi.patel@samsung.com>");
MODULE_LICENSE("GPL");
