// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 SiFive
 *
 * Authors
 *	Zong Li <zong.li@sifive.com>
 */

#include <linux/auxiliary_bus.h>
#include <linux/io-64-nonatomic-hi-lo.h>
#include <linux/perf_event.h>

#include "../iommu/riscv/iommu.h"

/* 5.19 Performance monitoring counter overflow status (32bits) */
#define RISCV_IOMMU_REG_IOCOUNTOVF	0x0058
#define RISCV_IOMMU_IOCOUNTOVF_CY	BIT(0)
#define RISCV_IOMMU_IOCOUNTOVF_HPM	GENMASK_ULL(31, 1)

/* 5.20 Performance monitoring counter inhibits (32bits) */
#define RISCV_IOMMU_REG_IOCOUNTINH	0x005C
#define RISCV_IOMMU_IOCOUNTINH_CY	BIT(0)
#define RISCV_IOMMU_IOCOUNTINH_HPM	GENMASK(31, 0)

/* 5.21 Performance monitoring cycles counter (64bits) */
#define RISCV_IOMMU_REG_IOHPMCYCLES	0x0060
#define RISCV_IOMMU_IOHPMCYCLES_COUNTER	GENMASK_ULL(62, 0)
#define RISCV_IOMMU_IOHPMCYCLES_OF	BIT_ULL(63)
#define RISCV_IOMMU_REG_IOHPMCTR(_n)	(RISCV_IOMMU_REG_IOHPMCYCLES + ((_n) * 0x8))

/* 5.22 Performance monitoring event counters (31 * 64bits) */
#define RISCV_IOMMU_REG_IOHPMCTR_BASE	0x0068
#define RISCV_IOMMU_IOHPMCTR_COUNTER	GENMASK_ULL(63, 0)

/* 5.23 Performance monitoring event selectors (31 * 64bits) */
#define RISCV_IOMMU_REG_IOHPMEVT_BASE	0x0160
#define RISCV_IOMMU_REG_IOHPMEVT(_n)	(RISCV_IOMMU_REG_IOHPMEVT_BASE + ((_n) * 0x8))
#define RISCV_IOMMU_IOHPMEVT_EVENTID	GENMASK_ULL(14, 0)
#define RISCV_IOMMU_IOHPMEVT_DMASK	BIT_ULL(15)
#define RISCV_IOMMU_IOHPMEVT_PID_PSCID	GENMASK_ULL(35, 16)
#define RISCV_IOMMU_IOHPMEVT_DID_GSCID	GENMASK_ULL(59, 36)
#define RISCV_IOMMU_IOHPMEVT_PV_PSCV	BIT_ULL(60)
#define RISCV_IOMMU_IOHPMEVT_DV_GSCV	BIT_ULL(61)
#define RISCV_IOMMU_IOHPMEVT_IDT	BIT_ULL(62)
#define RISCV_IOMMU_IOHPMEVT_OF		BIT_ULL(63)
#define RISCV_IOMMU_IOHPMEVT_EVENT	GENMASK_ULL(62, 0)

/* The total number of counters is 31 event counters plus 1 cycle counter */
#define RISCV_IOMMU_HPM_COUNTER_NUM	32

static int cpuhp_state;

/**
 * enum riscv_iommu_hpmevent_id - Performance-monitoring event identifier
 *
 * @RISCV_IOMMU_HPMEVENT_CYCLE: Clock cycle counter
 * @RISCV_IOMMU_HPMEVENT_URQ: Untranslated requests
 * @RISCV_IOMMU_HPMEVENT_TRQ: Translated requests
 * @RISCV_IOMMU_HPMEVENT_ATS_RQ: ATS translation requests
 * @RISCV_IOMMU_HPMEVENT_TLB_MISS: TLB misses
 * @RISCV_IOMMU_HPMEVENT_DD_WALK: Device directory walks
 * @RISCV_IOMMU_HPMEVENT_PD_WALK: Process directory walks
 * @RISCV_IOMMU_HPMEVENT_S_VS_WALKS: First-stage page table walks
 * @RISCV_IOMMU_HPMEVENT_G_WALKS: Second-stage page table walks
 * @RISCV_IOMMU_HPMEVENT_MAX: Value to denote maximum Event IDs
 *
 * The specification does not define an event ID for counting the
 * number of clock cycles, meaning there is no associated 'iohpmevt0'.
 * Event ID 0 is an invalid event and does not overlap with any valid
 * event ID. Let's repurpose ID 0 as the cycle for perf, the cycle
 * event is not actually written into any register, it serves solely
 * as an identifier.
 */
enum riscv_iommu_hpmevent_id {
	RISCV_IOMMU_HPMEVENT_CYCLE	= 0,
	RISCV_IOMMU_HPMEVENT_URQ        = 1,
	RISCV_IOMMU_HPMEVENT_TRQ        = 2,
	RISCV_IOMMU_HPMEVENT_ATS_RQ     = 3,
	RISCV_IOMMU_HPMEVENT_TLB_MISS   = 4,
	RISCV_IOMMU_HPMEVENT_DD_WALK    = 5,
	RISCV_IOMMU_HPMEVENT_PD_WALK    = 6,
	RISCV_IOMMU_HPMEVENT_S_VS_WALKS = 7,
	RISCV_IOMMU_HPMEVENT_G_WALKS    = 8,
	RISCV_IOMMU_HPMEVENT_MAX        = 9
};

struct riscv_iommu_pmu {
	struct pmu pmu;
	struct hlist_node node;
	void __iomem *reg;
	unsigned int on_cpu;
	int num_counters;
	u64 cycle_cntr_mask;
	u64 event_cntr_mask;
	struct perf_event *events[RISCV_IOMMU_HPM_COUNTER_NUM];
	DECLARE_BITMAP(used_counters, RISCV_IOMMU_HPM_COUNTER_NUM);
	u32 hi_prev[RISCV_IOMMU_HPM_COUNTER_NUM];
	u32 lo_prev[RISCV_IOMMU_HPM_COUNTER_NUM];
};

#define to_riscv_iommu_pmu(p) (container_of(p, struct riscv_iommu_pmu, pmu))

#define RISCV_IOMMU_PMU_ATTR_EXTRACTOR(_name, _mask)			\
	static inline u32 get_##_name(struct perf_event *event)		\
	{								\
		return FIELD_GET(_mask, event->attr.config);		\
	}								\

RISCV_IOMMU_PMU_ATTR_EXTRACTOR(event, RISCV_IOMMU_IOHPMEVT_EVENTID);
RISCV_IOMMU_PMU_ATTR_EXTRACTOR(partial_matching, RISCV_IOMMU_IOHPMEVT_DMASK);
RISCV_IOMMU_PMU_ATTR_EXTRACTOR(pid_pscid, RISCV_IOMMU_IOHPMEVT_PID_PSCID);
RISCV_IOMMU_PMU_ATTR_EXTRACTOR(did_gscid, RISCV_IOMMU_IOHPMEVT_DID_GSCID);
RISCV_IOMMU_PMU_ATTR_EXTRACTOR(filter_pid_pscid, RISCV_IOMMU_IOHPMEVT_PV_PSCV);
RISCV_IOMMU_PMU_ATTR_EXTRACTOR(filter_did_gscid, RISCV_IOMMU_IOHPMEVT_DV_GSCV);
RISCV_IOMMU_PMU_ATTR_EXTRACTOR(filter_id_type, RISCV_IOMMU_IOHPMEVT_IDT);

/* Formats */
PMU_FORMAT_ATTR(event,            "config:0-14");
PMU_FORMAT_ATTR(partial_matching, "config:15");
PMU_FORMAT_ATTR(pid_pscid,        "config:16-35");
PMU_FORMAT_ATTR(did_gscid,        "config:36-59");
PMU_FORMAT_ATTR(filter_pid_pscid, "config:60");
PMU_FORMAT_ATTR(filter_did_gscid, "config:61");
PMU_FORMAT_ATTR(filter_id_type,   "config:62");

static struct attribute *riscv_iommu_pmu_formats[] = {
	&format_attr_event.attr,
	&format_attr_partial_matching.attr,
	&format_attr_pid_pscid.attr,
	&format_attr_did_gscid.attr,
	&format_attr_filter_pid_pscid.attr,
	&format_attr_filter_did_gscid.attr,
	&format_attr_filter_id_type.attr,
	NULL,
};

static const struct attribute_group riscv_iommu_pmu_format_group = {
	.name = "format",
	.attrs = riscv_iommu_pmu_formats,
};

/* Events */
static ssize_t riscv_iommu_pmu_event_show(struct device *dev,
					  struct device_attribute *attr,
					  char *page)
{
	struct perf_pmu_events_attr *pmu_attr;

	pmu_attr = container_of(attr, struct perf_pmu_events_attr, attr);

	return sysfs_emit(page, "event=0x%02llx\n", pmu_attr->id);
}

#define RISCV_IOMMU_PMU_EVENT_ATTR(name, id)			\
	PMU_EVENT_ATTR_ID(name, riscv_iommu_pmu_event_show, id)

static struct attribute *riscv_iommu_pmu_events[] = {
	RISCV_IOMMU_PMU_EVENT_ATTR(cycle, RISCV_IOMMU_HPMEVENT_CYCLE),
	RISCV_IOMMU_PMU_EVENT_ATTR(untranslated_req, RISCV_IOMMU_HPMEVENT_URQ),
	RISCV_IOMMU_PMU_EVENT_ATTR(translated_req, RISCV_IOMMU_HPMEVENT_TRQ),
	RISCV_IOMMU_PMU_EVENT_ATTR(ats_trans_req, RISCV_IOMMU_HPMEVENT_ATS_RQ),
	RISCV_IOMMU_PMU_EVENT_ATTR(tlb_miss, RISCV_IOMMU_HPMEVENT_TLB_MISS),
	RISCV_IOMMU_PMU_EVENT_ATTR(ddt_walks, RISCV_IOMMU_HPMEVENT_DD_WALK),
	RISCV_IOMMU_PMU_EVENT_ATTR(pdt_walks, RISCV_IOMMU_HPMEVENT_PD_WALK),
	RISCV_IOMMU_PMU_EVENT_ATTR(s_vs_pt_walks, RISCV_IOMMU_HPMEVENT_S_VS_WALKS),
	RISCV_IOMMU_PMU_EVENT_ATTR(g_pt_walks, RISCV_IOMMU_HPMEVENT_G_WALKS),
	NULL,
};

static const struct attribute_group riscv_iommu_pmu_events_group = {
	.name = "events",
	.attrs = riscv_iommu_pmu_events,
};

/* cpumask */
static ssize_t riscv_iommu_cpumask_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct riscv_iommu_pmu *pmu = to_riscv_iommu_pmu(dev_get_drvdata(dev));

	return cpumap_print_to_pagebuf(true, buf, cpumask_of(pmu->on_cpu));
}

static struct device_attribute riscv_iommu_cpumask_attr =
	__ATTR(cpumask, 0444, riscv_iommu_cpumask_show, NULL);

static struct attribute *riscv_iommu_cpumask_attrs[] = {
	&riscv_iommu_cpumask_attr.attr,
	NULL
};

static const struct attribute_group riscv_iommu_pmu_cpumask_group = {
	.attrs = riscv_iommu_cpumask_attrs,
};

static const struct attribute_group *riscv_iommu_pmu_attr_grps[] = {
	&riscv_iommu_pmu_cpumask_group,
	&riscv_iommu_pmu_format_group,
	&riscv_iommu_pmu_events_group,
	NULL,
};

/* PMU Operations */
static void riscv_iommu_pmu_set_counter(struct riscv_iommu_pmu *pmu, u32 idx,
					u64 value)
{
	u64 counter_mask = idx ? pmu->event_cntr_mask : pmu->cycle_cntr_mask;

	writeq(value & counter_mask, pmu->reg + RISCV_IOMMU_REG_IOHPMCTR(idx));
}

/*
 * As stated in the RISC-V IOMMU Specification, Chapter 6:
 * Whether an 8 byte access to an IOMMU register is single-copy atomic
 * is UNSPECIFIED, and such an access may appear, internally to the
 * IOMMU, as if two separate 4 byte accesses - first to the high half
 * and second to the low half - were performed
 *
 * To make sure the driver works correctly on different hardware,
 * the software will always use two 4-byte access for the counter.
 *
 * This function implements the hi-lo-hi pattern to detect and handle
 * wraparound during the read operation:
 *   1. Read high half (hi)
 *   2. Read low half (lo)
 *   3. Check if low half wrapped or high half changed:
 *      - If lo <= lo_prev: possible wraparound occurred
 *      - If hi != hi_prev: high half changed during read
 *   4. If wraparound detected, re-read high half and assume low half is 0
 *   5. Update previous values for next read
 */
static u64 riscv_iommu_pmu_get_counter(struct riscv_iommu_pmu *pmu, u32 idx)
{
	void __iomem *addr = pmu->reg + RISCV_IOMMU_REG_IOHPMCTR(idx);
	u64 value, counter_mask = idx ? pmu->event_cntr_mask : pmu->cycle_cntr_mask;
	u32 hi, lo;

	hi = readl(addr + 4);
	lo = readl(addr);

	if (lo <= pmu->lo_prev[idx] || hi != pmu->hi_prev[idx]) {
		u32 hi_tmp = readl(addr + 4);

		/*
		 * If hi changes, then hi+1:0 must have happened while
		 * the code was running so it is a safe return value
		 */
		if (hi_tmp != hi) {
			hi = hi_tmp;
			lo = 0;
		}
		pmu->lo_prev[idx] = ~0u;
		pmu->hi_prev[idx] = hi;
	}
	pmu->lo_prev[idx] = lo;

	value = (((u64)hi << 32) | lo) & counter_mask;

	/* The bit 63 of cycle counter (i.e., idx == 0) is OF bit */
	return idx ? value : (value & ~RISCV_IOMMU_IOHPMCYCLES_OF);
}

static bool is_cycle_event(u64 event)
{
	return event == RISCV_IOMMU_HPMEVENT_CYCLE;
}

static void riscv_iommu_pmu_set_event(struct riscv_iommu_pmu *pmu, u32 idx,
				      u64 value)
{
	/* There is no associtated IOHPMEVT0 for IOHPMCYCLES */
	if (is_cycle_event(value))
		return;

	/* Event counter start from idx 1 */
	writeq(FIELD_GET(RISCV_IOMMU_IOHPMEVT_EVENT, value),
	       pmu->reg + RISCV_IOMMU_REG_IOHPMEVT(idx - 1));
}

static void riscv_iommu_pmu_enable_counter(struct riscv_iommu_pmu *pmu, u32 idx)
{
	void __iomem *addr = pmu->reg + RISCV_IOMMU_REG_IOCOUNTINH;
	u32 value = readl(addr);

	writel(value & ~BIT(idx), addr);
}

static void riscv_iommu_pmu_disable_counter(struct riscv_iommu_pmu *pmu, u32 idx)
{
	void __iomem *addr = pmu->reg + RISCV_IOMMU_REG_IOCOUNTINH;
	u32 value = readl(addr);

	writel(value | BIT(idx), addr);
}

static void riscv_iommu_pmu_start_all(struct riscv_iommu_pmu *pmu)
{
	void __iomem *addr = pmu->reg + RISCV_IOMMU_REG_IOCOUNTINH;
	u32 used_cntr = 0;

	/* The performance-monitoring counter inhibits is a 32-bit WARL register */
	bitmap_to_arr32(&used_cntr, pmu->used_counters, pmu->num_counters);

	writel(~used_cntr, addr);
}

static void riscv_iommu_pmu_stop_all(struct riscv_iommu_pmu *pmu)
{
	writel(GENMASK_ULL(pmu->num_counters - 1, 0),
	       pmu->reg + RISCV_IOMMU_REG_IOCOUNTINH);
}

/* PMU APIs */
static void riscv_iommu_pmu_set_period(struct perf_event *event)
{
	struct riscv_iommu_pmu *pmu = to_riscv_iommu_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	u64 counter_mask = hwc->idx ? pmu->event_cntr_mask : pmu->cycle_cntr_mask;
	u64 period;

	/*
	 * Limit the maximum period to prevent the counter value
	 * from overtaking the one we are about to program.
	 * In effect we are reducing max_period to account for
	 * interrupt latency (and we are being very conservative).
	 */
	period = counter_mask >> 1;
	riscv_iommu_pmu_set_counter(pmu, hwc->idx, period);
	local64_set(&hwc->prev_count, period);
}

static int riscv_iommu_pmu_event_init(struct perf_event *event)
{
	struct riscv_iommu_pmu *pmu = to_riscv_iommu_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	struct perf_event *sibling;
	int total_event_counters = pmu->num_counters - 1;
	int counters = 0;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	if (hwc->sample_period)
		return -EOPNOTSUPP;

	if (event->cpu < 0)
		return -EOPNOTSUPP;

	event->cpu = pmu->on_cpu;

	hwc->idx = -1;
	hwc->config = event->attr.config;

	if (event->group_leader == event)
		return 0;

	if (is_cycle_event(get_event(event->group_leader)))
		if (++counters > total_event_counters)
			return -EINVAL;

	for_each_sibling_event(sibling, event->group_leader) {
		if (is_cycle_event(get_event(sibling)))
			continue;

		if (sibling->pmu != event->pmu && !is_software_event(sibling))
			return -EINVAL;

		if (++counters > total_event_counters)
			return -EINVAL;
	}

	return 0;
}

static void riscv_iommu_pmu_update(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct riscv_iommu_pmu *pmu = to_riscv_iommu_pmu(event->pmu);
	u64 delta, prev, now;
	u32 idx = hwc->idx;
	u64 counter_mask = idx ? pmu->event_cntr_mask : pmu->cycle_cntr_mask;

	do {
		prev = local64_read(&hwc->prev_count);
		now = riscv_iommu_pmu_get_counter(pmu, idx);
	} while (local64_cmpxchg(&hwc->prev_count, prev, now) != prev);

	delta = (now - prev) & counter_mask;
	local64_add(delta, &event->count);
}

static void riscv_iommu_pmu_start(struct perf_event *event, int flags)
{
	struct riscv_iommu_pmu *pmu = to_riscv_iommu_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	if (WARN_ON_ONCE(!(event->hw.state & PERF_HES_STOPPED)))
		return;

	if (flags & PERF_EF_RELOAD)
		WARN_ON_ONCE(!(event->hw.state & PERF_HES_UPTODATE));

	hwc->state = 0;
	riscv_iommu_pmu_set_period(event);
	riscv_iommu_pmu_set_event(pmu, hwc->idx, hwc->config);
	riscv_iommu_pmu_enable_counter(pmu, hwc->idx);

	perf_event_update_userpage(event);
}

static void riscv_iommu_pmu_stop(struct perf_event *event, int flags)
{
	struct riscv_iommu_pmu *pmu = to_riscv_iommu_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx;

	if (hwc->state & PERF_HES_STOPPED)
		return;

	riscv_iommu_pmu_disable_counter(pmu, idx);

	if ((flags & PERF_EF_UPDATE) && !(hwc->state & PERF_HES_UPTODATE))
		riscv_iommu_pmu_update(event);

	hwc->state |= PERF_HES_STOPPED | PERF_HES_UPTODATE;
}

static int riscv_iommu_pmu_add(struct perf_event *event, int flags)
{
	struct riscv_iommu_pmu *pmu = to_riscv_iommu_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	unsigned int num_counters = pmu->num_counters;
	int idx;

	/* Reserve index zero for iohpmcycles */
	if (is_cycle_event(get_event(event)))
		idx = 0;
	else
		idx = find_next_zero_bit(pmu->used_counters, num_counters, 1);

	/* All event counters or cycle counter are in use */
	if (idx == num_counters || pmu->events[idx])
		return -EAGAIN;

	set_bit(idx, pmu->used_counters);

	pmu->events[idx] = event;
	hwc->idx = idx;
	hwc->state = PERF_HES_STOPPED | PERF_HES_UPTODATE;
	local64_set(&hwc->prev_count, 0);

	if (flags & PERF_EF_START)
		riscv_iommu_pmu_start(event, flags);

	/* Propagate changes to the userspace mapping. */
	perf_event_update_userpage(event);

	return 0;
}

static void riscv_iommu_pmu_read(struct perf_event *event)
{
	riscv_iommu_pmu_update(event);
}

static void riscv_iommu_pmu_del(struct perf_event *event, int flags)
{
	struct riscv_iommu_pmu *pmu = to_riscv_iommu_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx;

	riscv_iommu_pmu_stop(event, PERF_EF_UPDATE);
	pmu->events[idx] = NULL;
	clear_bit(idx, pmu->used_counters);

	perf_event_update_userpage(event);
}

static int riscv_iommu_pmu_online_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct riscv_iommu_pmu *iommu_pmu;

	iommu_pmu = hlist_entry_safe(node, struct riscv_iommu_pmu, node);

	if (iommu_pmu->on_cpu == -1)
		iommu_pmu->on_cpu = cpu;

	return 0;
}

static int riscv_iommu_pmu_offline_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct riscv_iommu_pmu *iommu_pmu;
	unsigned int target_cpu;

	iommu_pmu = hlist_entry_safe(node, struct riscv_iommu_pmu, node);

	if (cpu != iommu_pmu->on_cpu)
		return 0;

	iommu_pmu->on_cpu = -1;

	target_cpu = cpumask_any_but(cpu_online_mask, cpu);
	if (target_cpu >= nr_cpu_ids)
		return 0;

	perf_pmu_migrate_context(&iommu_pmu->pmu, cpu, target_cpu);
	iommu_pmu->on_cpu = target_cpu;

	return 0;
}

static irqreturn_t riscv_iommu_pmu_handle_irq(struct riscv_iommu_pmu *pmu)
{
	u32 ovf = readl(pmu->reg + RISCV_IOMMU_REG_IOCOUNTOVF);
	int idx;

	if (!ovf)
		return IRQ_NONE;

	riscv_iommu_pmu_stop_all(pmu);

	for_each_set_bit(idx, (unsigned long *)&ovf, pmu->num_counters) {
		struct perf_event *event = pmu->events[idx];

		if (WARN_ON_ONCE(!event))
			continue;

		riscv_iommu_pmu_update(event);
		riscv_iommu_pmu_set_period(event);
	}

	riscv_iommu_pmu_start_all(pmu);

	return IRQ_HANDLED;
}

static irqreturn_t riscv_iommu_pmu_irq_handler(int irq, void *dev_id)
{
	struct riscv_iommu_pmu *pmu = (struct riscv_iommu_pmu *)dev_id;
	irqreturn_t ret;

	/* Check whether this interrupt is for PMU */
	if (!(readl_relaxed(pmu->reg + RISCV_IOMMU_REG_IPSR) & RISCV_IOMMU_IPSR_PMIP))
		return IRQ_NONE;

	/* Process PMU IRQ */
	ret = riscv_iommu_pmu_handle_irq(pmu);

	/* Clear performance monitoring interrupt pending bit */
	writel_relaxed(RISCV_IOMMU_IPSR_PMIP, pmu->reg + RISCV_IOMMU_REG_IPSR);

	return ret;
}

static unsigned int riscv_iommu_pmu_get_irq_num(struct riscv_iommu_device *iommu)
{
	/* Reuse ICVEC.CIV mask for all interrupt vectors mapping */
	int vec = (iommu->icvec >> (RISCV_IOMMU_IPSR_PMIP * 4)) & RISCV_IOMMU_ICVEC_CIV;

	return iommu->irqs[vec];
}

static int riscv_iommu_pmu_request_irq(struct riscv_iommu_device *iommu,
				       struct riscv_iommu_pmu *pmu)
{
	unsigned int irq = riscv_iommu_pmu_get_irq_num(iommu);

	/*
	 * Set the IRQF_ONESHOT flag because this IRQ can be shared with
	 * other threaded IRQs by other queues.
	 */
	return devm_request_irq(iommu->dev, irq, riscv_iommu_pmu_irq_handler,
				IRQF_ONESHOT | IRQF_SHARED, dev_name(iommu->dev), pmu);
}

static void riscv_iommu_pmu_free_irq(struct riscv_iommu_device *iommu,
				     struct riscv_iommu_pmu *pmu)
{
	unsigned int irq = riscv_iommu_pmu_get_irq_num(iommu);

	free_irq(irq, pmu);
}

static int riscv_iommu_pmu_probe(struct auxiliary_device *auxdev,
				 const struct auxiliary_device_id *id)
{
	struct  riscv_iommu_device *iommu_dev = dev_get_platdata(&auxdev->dev);
	struct riscv_iommu_pmu *iommu_pmu;
	void __iomem *addr;
	char *name;
	int ret;

	iommu_pmu = devm_kzalloc(&auxdev->dev, sizeof(*iommu_pmu), GFP_KERNEL);
	if (!iommu_pmu)
		return -ENOMEM;

	iommu_pmu->reg = iommu_dev->reg;

	/* Counter number and width are hardware-implemented. Detect them by write 1s */
	addr = iommu_pmu->reg + RISCV_IOMMU_REG_IOCOUNTINH;
	writel(RISCV_IOMMU_IOCOUNTINH_HPM, addr);
	iommu_pmu->num_counters = hweight32(readl(addr));

	addr = iommu_pmu->reg + RISCV_IOMMU_REG_IOHPMCYCLES;
	writeq(RISCV_IOMMU_IOHPMCYCLES_COUNTER, addr);
	iommu_pmu->cycle_cntr_mask = readq(addr);

	/* Assume the width of all event counters are the same */
	addr = iommu_pmu->reg + RISCV_IOMMU_REG_IOHPMCTR_BASE;
	writeq(RISCV_IOMMU_IOHPMCTR_COUNTER, addr);
	iommu_pmu->event_cntr_mask = readq(addr);

	iommu_pmu->pmu = (struct pmu) {
		.module		= THIS_MODULE,
		.parent		= &auxdev->dev,
		.task_ctx_nr	= perf_invalid_context,
		.event_init	= riscv_iommu_pmu_event_init,
		.add		= riscv_iommu_pmu_add,
		.del		= riscv_iommu_pmu_del,
		.start		= riscv_iommu_pmu_start,
		.stop		= riscv_iommu_pmu_stop,
		.read		= riscv_iommu_pmu_read,
		.attr_groups	= riscv_iommu_pmu_attr_grps,
		.capabilities	= PERF_PMU_CAP_NO_EXCLUDE,
	};

	auxiliary_set_drvdata(auxdev, iommu_pmu);

	name = devm_kasprintf(&auxdev->dev, GFP_KERNEL,
			      "riscv_iommu_pmu_%s", dev_name(iommu_dev->dev));
	if (!name) {
		dev_err(&auxdev->dev, "Failed to create name riscv_iommu_pmu_%s\n",
			dev_name(iommu_dev->dev));
		return -ENOMEM;
	}

	/* Bind all events to the same cpu context to avoid race enabling */
	iommu_pmu->on_cpu = raw_smp_processor_id();

	ret = cpuhp_state_add_instance_nocalls(cpuhp_state, &iommu_pmu->node);
	if (ret) {
		dev_err(&auxdev->dev, "Failed to register hotplug %s: %d\n", name, ret);
		return ret;
	}

	ret = riscv_iommu_pmu_request_irq(iommu_dev, iommu_pmu);
	if (ret) {
		dev_err(&auxdev->dev, "Failed to request irq %s: %d\n", name, ret);
		goto err_cpuhp_remove;
	}

	ret = perf_pmu_register(&iommu_pmu->pmu, name, -1);
	if (ret) {
		dev_err(&auxdev->dev, "Failed to registe %s: %d\n", name, ret);
		goto err_free_irq;
	}

	dev_info(&auxdev->dev, "%s: Registered with %d counters\n",
		 name, iommu_pmu->num_counters);

	return 0;

err_free_irq:
	riscv_iommu_pmu_free_irq(iommu_dev, iommu_pmu);
err_cpuhp_remove:
	cpuhp_state_remove_instance_nocalls(cpuhp_state, &iommu_pmu->node);
	return ret;
}

static const struct auxiliary_device_id riscv_iommu_pmu_id_table[] = {
	{ .name = "iommu.pmu" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, riscv_iommu_pmu_id_table);

static struct auxiliary_driver iommu_pmu_driver = {
	.probe		= riscv_iommu_pmu_probe,
	.id_table	= riscv_iommu_pmu_id_table,
};

static int __init riscv_iommu_pmu_init(void)
{
	int ret;

	cpuhp_state = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN,
					      "perf/riscv/iommu:online",
					      riscv_iommu_pmu_online_cpu,
					      riscv_iommu_pmu_offline_cpu);
	if (cpuhp_state < 0)
		return cpuhp_state;

	ret = auxiliary_driver_register(&iommu_pmu_driver);
	if (ret)
		cpuhp_remove_multi_state(cpuhp_state);

	return ret;
}
module_init(riscv_iommu_pmu_init);

MODULE_DESCRIPTION("RISC-V IOMMU PMU");
MODULE_LICENSE("GPL");
