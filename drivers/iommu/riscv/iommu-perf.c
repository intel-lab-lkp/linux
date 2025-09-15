// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU hpm implementations
 * Copyright(c) 2025 Beijing Institute of Open Source Chip (BOSC)
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/perf_event.h>
#include <linux/iommu.h>
#include "iommu.h"
#include "iommu-perf.h"

#define EVENT_CYCLES              0
#define EVENT_UNTRANSLATED_REQ    1
#define EVENT_TRANSLATED_REQUESTS 2
#define EVENT_ATS_TRANSLATION_REQ 3
#define EVENT_TLB_MISS            4
#define EVENT_DDT_WALK            5
#define EVENT_PDT_WALK            6
#define EVENT_STAGE1_PT_WALK      7
#define EVENT_STAGE2_PT_WALK      8

RISCV_IOMMU_PMU_EXT_EVENT_ATTR(translated_req,      "config=0x001")
RISCV_IOMMU_PMU_EXT_EVENT_ATTR(untranslated_req,    "config=0x002")
RISCV_IOMMU_PMU_EXT_EVENT_ATTR(ats_translation_req, "config=0x003")
RISCV_IOMMU_PMU_EXT_EVENT_ATTR(tlb_miss,            "config=0x004")
RISCV_IOMMU_PMU_EXT_EVENT_ATTR(ddt_walk,            "config=0x005")
RISCV_IOMMU_PMU_EXT_EVENT_ATTR(pdt_walk,            "config=0x006")
RISCV_IOMMU_PMU_EXT_EVENT_ATTR(stage1_pt_walk,      "config=0x007")
RISCV_IOMMU_PMU_EXT_EVENT_ATTR(stage2_pt_walk,      "config=0x008")

static const struct attribute_group *riscv_iommu_pmu_attr_update_dummy[] = {
	&translated_req,
	&untranslated_req,
	&ats_translation_req,
	&tlb_miss,
	&ddt_walk,
	&pdt_walk,
	&stage1_pt_walk,
	&stage2_pt_walk,
	NULL
};

static struct riscv_iommu_pmu_event_map event_map[] = {
	{ "dummy", riscv_iommu_pmu_attr_update_dummy },
	{ NULL,    NULL}
};

PMU_EVENT_ATTR_STRING(pv_pscv,   format_pv_pscv_attr,   "config1:0");
PMU_EVENT_ATTR_STRING(dv_gscv,   format_dv_gscv_attr,   "config1:1");
PMU_EVENT_ATTR_STRING(idt,       format_idt_attr,       "config1:2");
PMU_EVENT_ATTR_STRING(pid_pscid, format_pid_pscid_attr, "config1:20-39");
PMU_EVENT_ATTR_STRING(did_gscid, format_did_gscid_attr, "config1:40-63");

static struct attribute *formats_attrs[] = {
	&format_pv_pscv_attr.attr.attr,
	&format_dv_gscv_attr.attr.attr,
	&format_idt_attr.attr.attr,
	&format_pid_pscid_attr.attr.attr,
	&format_did_gscid_attr.attr.attr,
	NULL
};

static struct attribute_group riscv_iommu_pmu_format_attr_group = {
	.name = "format",
	.attrs = formats_attrs,
};

PMU_EVENT_ATTR_STRING(cycles, event_cycles_attr, "config=0x0");

static struct attribute *events_attrs[] = {
	&event_cycles_attr.attr.attr,
	NULL
};

static struct attribute_group riscv_iommu_pmu_events_attr_group = {
	.name = "events",
	.attrs = events_attrs,
};

static const struct attribute_group *riscv_iommu_pmu_attr_groups[] = {
	&riscv_iommu_pmu_format_attr_group,
	&riscv_iommu_pmu_events_attr_group,
	NULL,
};

static void __riscv_iommu_pmu_unregister(struct riscv_iommu_pmu *iommu_pmu);
static void riscv_iommu_pmu_read(struct perf_event *event);

static void riscv_iommu_pmu_hpmevt_set(iohpmevt_t *hpmevt, unsigned long event_id,
				       int pv_pscv, int dv_gscv, int idt,
				       int pid_pscid, int did_gscid, int of)
{
	hpmevt->val = 0;
	hpmevt->eventID = event_id;
	hpmevt->IDT = idt;
	hpmevt->OF = of;
	hpmevt->PID_PSCID = pid_pscid;
	hpmevt->DID_GSCID = did_gscid;
	hpmevt->PV_PSCV = pv_pscv;
	hpmevt->DV_GSCV = dv_gscv;
}

static struct riscv_iommu_perf_event *
get_riscv_iommu_perf_event(struct riscv_iommu_pmu *iommu_pmu,
			   struct perf_event *event,
			   int pv_pscv, int dv_gscv, int idt,
			   int pid_pscid, int did_gscid,
			   int *idx)
{
	int i, nr;
	struct riscv_iommu_device *iommu = iommu_pmu->iommu;
	struct riscv_iommu_perf_event *iommu_event;

	for (i = 0; i < RISCV_IOMMU_IOHPMCTR_CNT; i++) {
		iommu_event = iommu->events[i];
		if (iommu_event == NULL)
			continue;

		if (iommu_event->perf_event == event) {
			nr = i;
			goto update;
		}
	}

	nr = find_first_zero_bit(&iommu->iohpmctr_bitmap,
				 RISCV_IOMMU_IOHPMCTR_CNT);
	if (nr >= RISCV_IOMMU_IOHPMCTR_CNT)
		return NULL;
again:
	if (test_and_set_bit(nr, &iommu_pmu->iommu->iohpmctr_bitmap))
		goto again;

	iommu_event = kzalloc(sizeof(struct riscv_iommu_perf_event), GFP_KERNEL);
	if (!iommu_event)
		return NULL;
update:
	iommu_event->perf_event = event;
	iommu_event->pv_pscv = pv_pscv;
	iommu_event->dv_gscv = dv_gscv;
	iommu_event->idt = idt;
	iommu_event->pid_pscid = pid_pscid;
	iommu_event->did_gscid = did_gscid;
	iommu->events[nr] = iommu_event;

	*idx = nr;

	return iommu_event;
}

static int riscv_iommu_pmu_event_add(struct riscv_iommu_pmu *iommu_pmu,
				     struct perf_event *event)
{
	int nr = -1, of;
	unsigned long event_id = event->attr.config;
	riscv_iommu_pmu_cfg1_t config1;
	struct hw_perf_event *hwc = &event->hw;
	struct riscv_iommu_device *iommu = iommu_pmu->iommu;

	config1.val = event->attr.config1;

	if (event_id >= RISCV_IOMMU_IOHPMCTR_CNT)
		return -EINVAL;

	if (iommu->hpm_irq)
		of = 0;
	else
		of = 1;

	if (event_id == EVENT_CYCLES) {
		unsigned long val;

		val = riscv_iommu_readq(iommu_pmu->iommu, RISCV_IOMMU_REG_IOHPMCYCLES);
		if (of)
			val &= ~RISCV_IOMMU_IOHPMCYCLES_OF;
		else
			val |= RISCV_IOMMU_IOHPMCYCLES_OF;
		riscv_iommu_writeq(iommu_pmu->iommu, RISCV_IOMMU_REG_IOHPMCYCLES, val);

		hwc->idx = 0;
		iommu->events[0]->perf_event = event;
	} else {
		struct riscv_iommu_perf_event *iommu_perf_event;

		iommu_perf_event = get_riscv_iommu_perf_event(iommu_pmu, event,
							      config1.pv_pscv, config1.dv_gscv,
							      config1.idt, config1.pid_pscid,
							      config1.did_gscid, &nr);
		if (!iommu_perf_event)
			return -ENOSPC;

		riscv_iommu_pmu_hpmevt_set(&iommu_pmu->iommu->iohpmevt[nr], event_id,
					   iommu_perf_event->pv_pscv, iommu_perf_event->dv_gscv,
					   iommu_perf_event->idt, iommu_perf_event->pid_pscid,
					   iommu_perf_event->did_gscid, of);
		riscv_iommu_writeq(iommu_pmu->iommu, RISCV_IOMMU_REG_IOHPMEVT(nr),
				   iommu_pmu->iommu->iohpmevt[nr].val);

		hwc->idx = nr;
	}

	return 0;
}

static int riscv_iommu_pmu_hpmevt_idx_get(struct riscv_iommu_pmu *iommu_pmu, int event_id)
{
	int i;
	iohpmevt_t *iohpmevt;

	for (i = 0; i < RISCV_IOMMU_IOHPMCTR_CNT; i++) {
		iohpmevt = &iommu_pmu->iommu->iohpmevt[i];
		if (iohpmevt->eventID == event_id)
			return iohpmevt - iommu_pmu->iommu->iohpmevt;
	}

	return -1;
}

static int riscv_iommu_event_del(struct riscv_iommu_pmu *iommu_pmu,
				 struct perf_event *event)
{
	unsigned long config = event->attr.config;
	struct riscv_iommu_device *iommu = iommu_pmu->iommu;

	if (config >= RISCV_IOMMU_IOHPMCTR_CNT)
		return -EINVAL;

	if (config == EVENT_CYCLES) {
		iommu->events[0] = NULL;
	} else {
		int nr;

		nr = riscv_iommu_pmu_hpmevt_idx_get(iommu_pmu, config);
		if (-1 == nr)
			return -1;
		riscv_iommu_pmu_hpmevt_set(&iommu_pmu->iommu->iohpmevt[nr], 0,
					   0, 0, 0, 0, 0, 0);
		clear_bit(nr, &iommu_pmu->iommu->iohpmctr_bitmap);
		riscv_iommu_writeq(iommu_pmu->iommu, RISCV_IOMMU_REG_IOHPMEVT(nr),
				   0);
		kfree(iommu->events[nr]);
		iommu->events[nr] = NULL;
	}

	return 0;
}

static int riscv_iommu_pmu_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;

	if (event->attr.sample_period)
		return -EINVAL;

	if (event->cpu < 0)
		return -EINVAL;

	hwc->config = event->attr.config;

	return 0;
}

static void riscv_iommu_pmu_enable(struct pmu *pmu)
{
}

static void riscv_iommu_pmu_disable(struct pmu *pmu)
{
}

static void riscv_iommu_pmu_start(struct perf_event *event, int flags)
{
	struct riscv_iommu_pmu *iommu_pmu = riscv_iommu_event_to_pmu(event);
	struct hw_perf_event *hwc = &event->hw;
	unsigned long count;

	hwc->state = 0;

	if (hwc->idx == EVENT_CYCLES)
		count = riscv_iommu_readq(iommu_pmu->iommu, RISCV_IOMMU_REG_IOHPMCYCLES);
	else
		count = riscv_iommu_readq(iommu_pmu->iommu, RISCV_IOMMU_REG_IOHPMCTR(hwc->idx));

	local64_set((&hwc->prev_count), count);

	perf_event_update_userpage(event);
}

static void riscv_iommu_pmu_stop(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	if (!(hwc->state & PERF_HES_STOPPED)) {
		riscv_iommu_pmu_read(event);
		hwc->state |= PERF_HES_STOPPED | PERF_HES_UPTODATE;
	}
}

static int riscv_iommu_pmu_add(struct perf_event *event, int flags)
{
	struct riscv_iommu_pmu *iommu_pmu = riscv_iommu_event_to_pmu(event);
	struct hw_perf_event *hwc = &event->hw;

	riscv_iommu_pmu_event_add(iommu_pmu, event);

	hwc->state = PERF_HES_UPTODATE | PERF_HES_STOPPED;

	if (flags & PERF_EF_START)
		riscv_iommu_pmu_start(event, 0);

	return 0;
}

static void riscv_iommu_pmu_del(struct perf_event *event, int flags)
{
	struct riscv_iommu_pmu *iommu_pmu = riscv_iommu_event_to_pmu(event);

	riscv_iommu_pmu_stop(event, PERF_EF_UPDATE);

	riscv_iommu_event_del(iommu_pmu, event);
	event->hw.idx = -1;

	perf_event_update_userpage(event);
}

static void riscv_iommu_pmu_read(struct perf_event *event)
{
	struct riscv_iommu_pmu *iommu_pmu = riscv_iommu_event_to_pmu(event);
	struct hw_perf_event *hwc = &event->hw;
	unsigned long prev_count, new_count, delta;

again:
	prev_count = local64_read(&hwc->prev_count);
	if (hwc->idx == EVENT_CYCLES)
		new_count = riscv_iommu_readq(iommu_pmu->iommu, RISCV_IOMMU_REG_IOHPMCYCLES);
	else
		new_count = riscv_iommu_readq(iommu_pmu->iommu, RISCV_IOMMU_REG_IOHPMCTR(hwc->idx));

	if (local64_xchg(&hwc->prev_count, new_count) != prev_count)
		goto again;
	delta = new_count - prev_count;

	local64_add(delta, &event->count);
}

int riscv_iommu_pmu_alloc(struct riscv_iommu_device *iommu)
{
	struct riscv_iommu_pmu *iommu_pmu;
	struct riscv_iommu_perf_event *iommu_event;
	int ret = 0;

	if (iommu->pmu)
		return -EEXIST;

	iommu_pmu = kzalloc(sizeof(struct riscv_iommu_pmu), GFP_KERNEL);
	if (!iommu_pmu)
		return -ENOMEM;

	iommu_pmu->iommu = iommu;
	iommu->pmu = iommu_pmu;

	set_bit(0, &iommu_pmu->iommu->iohpmctr_bitmap);
	iommu_event = kzalloc(sizeof(struct riscv_iommu_perf_event), GFP_KERNEL);
	if (!iommu_event) {
		ret = -ENOMEM;
		goto free_pmu;
	}
	iommu->events[0] = iommu_event;

	return 0;

free_pmu:
	kfree(iommu_pmu);
	return ret;
}

static void riscv_iommu_pmu_do_overflow(struct riscv_iommu_device *iommu)
{
	int idx;
	struct riscv_iommu_perf_event *iommu_event;

	for_each_set_bit(idx, &iommu->iohpmctr_bitmap,
			 RISCV_IOMMU_IOHPMCTR_CNT) {
		iohpmevt_t hpmevt;
		unsigned int val;

		if (idx == 0)
			continue;
		iommu_event = iommu->events[idx];
		if (!iommu_event)
			continue;
		hpmevt.val = riscv_iommu_readq(iommu, RISCV_IOMMU_REG_IOHPMEVT(idx));
		if (hpmevt.OF) {
			riscv_iommu_pmu_read(iommu_event->perf_event);

			hpmevt.OF = 0;
			riscv_iommu_writeq(iommu, RISCV_IOMMU_REG_IOHPMEVT(idx), hpmevt.val);

			val = riscv_iommu_readl(iommu, RISCV_IOMMU_REG_IPSR);
			val &= ~RISCV_IOMMU_IPSR_PMIP;
			riscv_iommu_writel(iommu, RISCV_IOMMU_REG_IPSR, val);
		}
	}
}

static irqreturn_t riscv_iommu_pmu_irq_handler(int irq, void *data)
{
	struct riscv_iommu_device *iommu = (struct riscv_iommu_device *)data;

	riscv_iommu_pmu_do_overflow(iommu);

	return IRQ_HANDLED;
}

static irqreturn_t riscv_iommu_pmu_ipsr(int irq, void *data)
{
	struct riscv_iommu_device *iommu = (struct riscv_iommu_device *)data;

	if (riscv_iommu_readl(iommu, RISCV_IOMMU_REG_IPSR) & RISCV_IOMMU_IPSR_PMIP)
		return IRQ_WAKE_THREAD;

	return IRQ_NONE;
}

static int riscv_iommu_pmu_vec(struct riscv_iommu_device *iommu)
{
	return FIELD_GET(RISCV_IOMMU_ICVEC_PMIV, iommu->icvec);
}

static int riscv_iommu_pmu_set_irq(struct riscv_iommu_device *iommu)
{
	int irq, ret;

	if (!iommu)
		return -EINVAL;
	irq = iommu->irqs[riscv_iommu_pmu_vec(iommu)];
	if (!irq)
		return -EEXIST;
	iommu->hpm_irq = irq;

	ret = request_threaded_irq(irq, riscv_iommu_pmu_ipsr,
				   riscv_iommu_pmu_irq_handler,
				   IRQF_ONESHOT, "rv-iommu-pmu-irq", iommu);
	if (ret) {
		iommu->hpm_irq = 0;
		return ret;
	}

	return 0;
}

static const struct attribute_group **
riscv_iommu_get_ext_attr(struct riscv_iommu_pmu *iommu_pmu)
{
	struct riscv_iommu_pmu_event_map *map = event_map;
	const char *str;
	struct device *dev = iommu_pmu->iommu->dev;

	if (of_property_read_string(dev->of_node, "pmu-name", &str))
		return NULL;

	while (map->compatible) {
		if (!strcmp(map->compatible, str))
			return map->attr_group;
	}

	return NULL;
}

static int __riscv_iommu_pmu_register(struct riscv_iommu_pmu *iommu_pmu,
				      const char *name)
{
	int ret;

	if (!iommu_pmu)
		return -EINVAL;

	iommu_pmu->pmu.attr_groups = riscv_iommu_pmu_attr_groups;
	iommu_pmu->pmu.attr_update = riscv_iommu_get_ext_attr(iommu_pmu);
	iommu_pmu->pmu.task_ctx_nr = perf_invalid_context;
	iommu_pmu->pmu.event_init  = riscv_iommu_pmu_event_init;
	iommu_pmu->pmu.pmu_enable  = riscv_iommu_pmu_enable;
	iommu_pmu->pmu.pmu_disable = riscv_iommu_pmu_disable;
	iommu_pmu->pmu.add         = riscv_iommu_pmu_add;
	iommu_pmu->pmu.del         = riscv_iommu_pmu_del;
	iommu_pmu->pmu.start       = riscv_iommu_pmu_start;
	iommu_pmu->pmu.stop        = riscv_iommu_pmu_stop;
	iommu_pmu->pmu.read        = riscv_iommu_pmu_read;

	ret = perf_pmu_register(&iommu_pmu->pmu, name, -1);
	if (ret)
		return ret;

	return 0;
}

static void __riscv_iommu_pmu_unregister(struct riscv_iommu_pmu *iommu_pmu)
{
	if (!iommu_pmu)
		return;

	perf_pmu_unregister(&iommu_pmu->pmu);

	kfree(iommu_pmu->iommu->events[0]);
	kfree(iommu_pmu);
}

int riscv_iommu_pmu_register(struct riscv_iommu_device *iommu)
{
	int ret;

	ret = __riscv_iommu_pmu_register(iommu->pmu, "riscv-iommu-pmu");
	if (ret)
		goto err;

	ret = riscv_iommu_pmu_set_irq(iommu);
	if (ret)
		goto unregister;

	return 0;

unregister:
	riscv_iommu_pmu_unregister(iommu);
err:
	return ret;
}

void riscv_iommu_pmu_unregister(struct riscv_iommu_device *iommu)
{
	__riscv_iommu_pmu_unregister(iommu->pmu);

	iommu->pmu = NULL;
}
