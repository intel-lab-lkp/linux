// SPDX-License-Identifier: GPL-2.0
/*
 * HX ARM-NI-700 uncore PMU support
 *
 * This code is based on uncore PMUs arm_smmuv3_pmu and arm-cmn.
 *
 * One ni-700 can have many clock domains. Each of them has only one PMU.
 * Here one PMU corresponds to one 'struct ni_pmu' instance.
 *
 * PMU name will be ni_pmu_N_M, which N means different NI-700s and M means
 * different PMU in one NI-700. If only one NI-700 found in NI-700, name
 * will be ni_pmu_N.
 *
 * Node interface event name will be xxni_N_eventname, such as
 * asni_0_rdreq_any. There are many kinds of type of nodes in one clock
 * domain. Also means that there are many kinds of that in one PMU. So we
 * distinguish them by xxni string. Besides, maybe there are many nodes
 * have same type. So we have number N in event name.
 * By ni_pmu_0_0/asni_0_rdreq_any/, we can pinpoint accurate bus traffic.
 *
 * Example1: perf stat -a -e ni_pmu_0_0/asni_0_rdreq_any/,ni_pmu_0_0/cycles/
 * Example2: perf stat -a -e ni_pmu_0_0/asni,id=0,event=0x0/
 *
 * TODO: Secure or non-secure attribute in all event omitted now.
 *
 */

#define dev_fmt(fmt) "hx-ni-700 pmu: " fmt

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/cpuhotplug.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/perf_event.h>
#include <linux/platform_device.h>
#include <linux/smp.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/build_bug.h>
#include <linux/io-64-nonatomic-lo-hi.h>

/* number of counters in one ni pmu */
#define NI_PMU_COUNTER_NUM 8

/* node type values */
enum ni_node_type {
	NI_BASE = 0x0,
	NI_VD,
	NI_PD,
	NI_CD,
	NI_ASNI = 0x4,
	NI_AMNI,
	NI_PMU,
	NI_HSNI,
	NI_HMNI,
	NI_PMNI = 0x9,
};

/*
 * config:
 * 0-5    31      32-47      48-63
 * event  cycles  node_type  node_id
 *
 */
#define NI_EVENT_FORMAT_EVENT    GENMASK_ULL(5, 0)
#define NI_EVENT_FORMAT_CYCLES   BIT(31)
#define NI_EVENT_FORMAT_NODETYPE GENMASK_ULL(32 + NI_PMNI, 32)
#define NI_EVENT_FORMAT_ASNI     BIT(32 + NI_ASNI)
#define NI_EVENT_FORMAT_AMNI     BIT(32 + NI_AMNI)
#define NI_EVENT_FORMAT_HSNI     BIT(32 + NI_HSNI)
#define NI_EVENT_FORMAT_HMNI     BIT(32 + NI_HMNI)
#define NI_EVENT_FORMAT_PMNI     BIT(32 + NI_PMNI)
#define NI_EVENT_FORMAT_NODEID   GENMASK_ULL(63, 48)

#define NI_EVENT_FORMAT_NODE_TYPE GENMASK_ULL(63, 32)

#define ni_event_config_eventid(_config)   FIELD_GET(NI_EVENT_FORMAT_EVENT,       _config)
#define ni_event_config_cc(_config)        FIELD_GET(NI_EVENT_FORMAT_CYCLES,      _config)
#define _ni_event_config_nodetype(_config) FIELD_GET(NI_EVENT_FORMAT_NODETYPE,    _config)
#define ni_event_config_nodeid(_config)    FIELD_GET(NI_EVENT_FORMAT_NODEID,      _config)

#define NI_NODE_TYPE_MASK GENMASK(15, 0)
#define NI_NODE_ID_MASK   GENMASK(31, 16)

#define NI_PMU_PMCR_RST_CYC_CNTR BIT(2)
#define NI_PMU_PMCR_RST_EV_CNTR  BIT(1)
#define NI_PMU_PMCR_ENABLE       BIT(0)

static enum cpuhp_state hp_state;

static const char *const ni_node_name[] = {
	[NI_ASNI] = "asni",
	[NI_AMNI] = "amni",
	[NI_PMU]  = "pmu",
	[NI_HSNI] = "hsni",
	[NI_HMNI] = "hmni",
	[NI_PMNI] = "pmni",
};

/* one instance for one node */
struct ni_node {
	void __iomem *base;
	union {
		struct {
			u32 type:16;
			u32 id:16;
		};
		u32 node_type;
	};
};

/* xxx_reg_map only used to provide offset by using offsetof(). */
struct ni_node_reg_map {
	union {
		struct {
			u32 type:16;
			u32 id:16;
		};
		u32 node_type;
	};

	union {
		u32 child_num;
		u32 node_info;
	};

	union {
		struct {
			u32 secr_acc;
			u32 pmusela;
			u32 pmuselb;
		};
		DECLARE_FLEX_ARRAY(u32, child_offset);
	};
};

#define ni_node_offsetof(member)			\
	offsetof(struct ni_node_reg_map, member)

#define ni_node_pmuevsel(node, config)					\
	lo_hi_writeq(config, node->base + ni_node_offsetof(pmusela))

#define ni_node_read(base, member, readx)			\
	readx((void __iomem *)base + ni_node_offsetof(member))

#define ni_node_type(base)						\
	FIELD_GET(NI_NODE_TYPE_MASK, ni_node_read(base, node_type, readl_relaxed))

#define ni_node_id(base)						\
	FIELD_GET(NI_NODE_ID_MASK, ni_node_read(base, node_type, readl_relaxed))

#define ni_node_node_type(base)				\
	ni_node_read(base, node_type, readl_relaxed)

#define ni_child_number(base)						\
	(ni_node_type(base) < NI_ASNI ? ni_node_read(base, child_num, readl_relaxed) : 0)

#define ni_child_pointer(periphbase, base, idx)				\
	((void __iomem *)periphbase + ni_node_read(base, child_offset[idx], readl_relaxed))

struct ni_pmu;
struct ni_pmu_reg_map {
	u32 node_type;                   /* offset: 0x000 */
	u32 secr_acc;                    /* offset: 0x004 */
	struct {
		u32 counter;
		u32 reserved;
	} pmevcntr[8];                   /* offset: 0x008 */
	u8  reserved_1[0xF8 - 0x48];     /* offset: 0x048 */
	u32 pmccntr_lower;               /* offset: 0x0F8 */
	u32 pmccntr_upper;               /* offset: 0x0FC */
	u8  reserved_2[0x400 - 0x100];   /* offset: 0x100 */
	u32 pmevtyper[8];                /* offset: 0x400 */
	u8  reserved_3[0x610 - 0x420];   /* offset: 0x420 */
	u32 pmssr;                       /* offset: 0x610 */
	u32 pmovssr;                     /* offset: 0x614 */
	u32 pmccntsr_lower;              /* offset: 0x618 */
	u32 pmccntsr_upper;              /* offset: 0x61C */
	u32 pmevcntsr[8];                /* offset: 0x620 */
	u8  reserved_4[0x6F0 - 0x640];   /* offset: 0x640 */
	u32 pmsscr;                      /* offset: 0x6F0 */
	u8  reserved_5[0xC00 - 0x6F4];   /* offset: 0x6F4 */
	u32 pmcntenset;                  /* offset: 0xC00 */
	u8  reserved_6[0xC20 - 0xC04];   /* offset: 0xC04 */
	u32 pmcntenclr;                  /* offset: 0xC20 */
	u8  reserved_7[0xC40 - 0xC24];   /* offset: 0xC24 */
	u32 pmintenset;                  /* offset: 0xC40 */
	u8  reserved_8[0xC60 - 0xC44];   /* offset: 0xC44 */
	u32 pmintenclr;                  /* offset: 0xC60 */
	u8  reserved_9[0xC80 - 0xC64];   /* offset: 0xC64 */
	u32 pmovsclr;                    /* offset: 0xC80 */
	u8  reserved_10[0xCC0 - 0xC84];  /* offset: 0xC84 */
	u32 pmovsset;                    /* offset: 0xCC0 */
	u8  reserved_11[0xD80 - 0xCC4];  /* offset: 0xCC4 */
	u32 pmcccgr;                     /* offset: 0xD80 */
	u8  reserved_12[0xE00 - 0xD84];  /* offset: 0xD84 */
	u32 pmcfgr;                      /* offset: 0xE00 */
	u32 pmcr;                        /* offset: 0xE04 */
};

/* Not read or write registers directly. */
#define ni_pmu_offset(ni_pmu, member)					\
	((void __iomem *)ni_pmu->pmu_node.base + offsetof(struct ni_pmu_reg_map, member))

#define ni_pmu_interrupt_enable(ni_pmu, en_bit)		\
	writel(en_bit, ni_pmu_offset(ni_pmu, pmintenset))

#define ni_pmu_interrupt_disable(ni_pmu, en_bit)		\
	writel(en_bit, ni_pmu_offset(ni_pmu, pmintenclr))

#define ni_pmu_counter_enable(ni_pmu, en_bit)		\
	writel(en_bit, ni_pmu_offset(ni_pmu, pmcntenset))

#define ni_pmu_counter_disable(ni_pmu, en_bit)		\
	writel(en_bit, ni_pmu_offset(ni_pmu, pmcntenclr))

#define ni_pmu_pmevtyper_sel_node(ni_pmu, ev_typer, cnt_idx)		\
	writel(ev_typer, ni_pmu_offset(ni_pmu, pmevtyper[cnt_idx]))

struct global_ni {
	void __iomem *base;
	struct hlist_node node;
	struct device *dev;
	union {
		unsigned int pmu_num;
		unsigned int cd_num;
	};
	unsigned int on_cpu;
	int irq_num;
	struct ni_pmu *ni_pmus[];
};

struct ni_pmu {
	struct ni_node pmu_node;
	struct perf_event *events[NI_PMU_COUNTER_NUM + 1];
	struct pmu pmu;
	struct device *dev;
	unsigned int irq;
	struct global_ni *ni;
	int ev_src_num;
	struct ni_node ev_src_nodes[];
};

#define to_ni_pmu(_pmu) container_of(_pmu, struct ni_pmu, pmu)

#define NI_PMU_CC_EN_BIT 31
#define is_cc_event(hwc) (!(hwc->config))
#define cnt_init_val(hwc) (is_cc_event(hwc) ? (0x1ULL << 63) : (0x1ULL << 31))
#define cnt_en_bit(hwc) (is_cc_event(hwc) ? BIT(NI_PMU_CC_EN_BIT) : BIT(hwc->idx))

struct ni_event_desc {
	u64 eventid;
	const char *name;
};

struct ni_event_attr {
	struct device_attribute attr;
	struct ni_event_desc *ev_desc;
	struct ni_node *node;
};

#define to_ni_event_attr(p) \
	container_of(p, struct ni_event_attr, attr)

#define NI_EVENT_DESC(_eventid, _name)			\
	(&((struct ni_event_desc[]) {			\
			{ .name = __stringify(_name),	\
			  .eventid = _eventid,}		\
		})[0])

struct ni_format_attr {
	struct device_attribute attr;
	u64 field;
};

static struct ni_event_desc *ni_asni_event_descs[] = {
	NI_EVENT_DESC(0x00, rdreq_any),
	NI_EVENT_DESC(0x01, rdreq_dev_arcache),
	NI_EVENT_DESC(0x02, rdreq_rns),
	NI_EVENT_DESC(0x03, rdreq_ro),
	NI_EVENT_DESC(0x04, req_cache_clr),
	NI_EVENT_DESC(0x05, rdreq_beat_any),
	NI_EVENT_DESC(0x06, rdreq_handshake_rlast),
	NI_EVENT_DESC(0x07, wtreq_any),
	NI_EVENT_DESC(0x08, wtreq_dev),
	NI_EVENT_DESC(0x09, wtreq_wns),
	NI_EVENT_DESC(0x0a, wtreq_wlu),
	NI_EVENT_DESC(0x0b, wtreq_wu),
	NI_EVENT_DESC(0x0c, wtreq_atomic),
	NI_EVENT_DESC(0x0d, wtreq_beat_any),
	NI_EVENT_DESC(0x0e, rdreq_stall),
	NI_EVENT_DESC(0x0f, rddata_stall),
	NI_EVENT_DESC(0x10, wtreq_stall),
	NI_EVENT_DESC(0x11, wtdata_stall),
	NI_EVENT_DESC(0x12, wtresp_stall),
	NI_EVENT_DESC(0x13, wtreq_cst),
	NI_EVENT_DESC(0x14, wtchann_nopersist),
	NI_EVENT_DESC(0x15, wtchann_persist),
	NI_EVENT_DESC(0x16, rdreq_nzero_mem_ops),
	NI_EVENT_DESC(0x17, wtreq_nzero_mem_ops),
	NI_EVENT_DESC(0x20, req_stall_cc_ot_limit),
	NI_EVENT_DESC(0x21, req_stall_cc_tspec_limit),
	NI_EVENT_DESC(0x22, req_stall_arbit),
	NI_EVENT_DESC(0x23, req_stall_rd_tracker),
	NI_EVENT_DESC(0x24, req_stall_wt_tracker),
	NI_EVENT_DESC(0x25, aw_stall_wdatafifo_full),
	NI_EVENT_DESC(0x26, ar_stall_reorderbuf_full),
	NI_EVENT_DESC(0x27, aw_cdas_stall),
	NI_EVENT_DESC(0x28, ar_cdas_stall),
	NI_EVENT_DESC(0x29, atomic_rd_stall),
	NI_EVENT_DESC(0x2a, wtchann_wtreq_stall),
	NI_EVENT_DESC(0x2b, rdchann_rdreq_stall),
	NI_EVENT_DESC(0x2c, aw_stall_ot),
	NI_EVENT_DESC(0x2d, ar_stall_ot),
	NI_EVENT_DESC(0x2e, aw_stall_tspec),
	NI_EVENT_DESC(0x2f, ar_stall_tspec),
	NI_EVENT_DESC(0x30, lwmd_arbit_stall_wchann),
	NI_EVENT_DESC(0x31, lwmd_arbit_stall_rchann),
};

static struct ni_event_desc *ni_amni_event_descs[] = {
	NI_EVENT_DESC(0x00, rdreq_any),
	NI_EVENT_DESC(0x01, rdreq_dev_arcache),
	NI_EVENT_DESC(0x02, rdreq_rns),
	NI_EVENT_DESC(0x03, rdreq_ro),
	NI_EVENT_DESC(0x04, req_cache_clr),
	NI_EVENT_DESC(0x05, rdreq_beat_any),
	NI_EVENT_DESC(0x06, rdreq_handshake_rlast),
	NI_EVENT_DESC(0x07, wtreq_any),
	NI_EVENT_DESC(0x08, wtreq_dev),
	NI_EVENT_DESC(0x09, wtreq_wns),
	NI_EVENT_DESC(0x0a, wtreq_wlu),
	NI_EVENT_DESC(0x0b, wtreq_wu),
	NI_EVENT_DESC(0x0c, wtreq_atomic),
	NI_EVENT_DESC(0x0d, wtreq_beat_any),
	NI_EVENT_DESC(0x0e, rdreq_stall),
	NI_EVENT_DESC(0x0f, rddata_stall),
	NI_EVENT_DESC(0x10, wtreq_stall),
	NI_EVENT_DESC(0x11, wtdata_stall),
	NI_EVENT_DESC(0x12, wtresp_stall),
	NI_EVENT_DESC(0x13, wtreq_cst),
	NI_EVENT_DESC(0x14, wtchann_nopersist),
	NI_EVENT_DESC(0x15, wtchann_persist),
	NI_EVENT_DESC(0x16, rdreq_nzero_mem_ops),
	NI_EVENT_DESC(0x17, wtreq_nzero_mem_ops),
	NI_EVENT_DESC(0x20, req_stall_rd_tracker),
	NI_EVENT_DESC(0x21, req_stall_wt_tracker),
	NI_EVENT_DESC(0x22, wtchann_b_resp),
	NI_EVENT_DESC(0x23, rdchann_rd_resp),
	NI_EVENT_DESC(0x24, lwmd_arbit_stall_wchann),
	NI_EVENT_DESC(0x25, lwmd_arbit_stall_rchann),
};

static struct ni_event_desc *ni_hsni_event_descs[] = {
	NI_EVENT_DESC(0x00, rdreq_any),
	NI_EVENT_DESC(0x01, rdreq_dev),
	NI_EVENT_DESC(0x02, rdreq_noshare),
	NI_EVENT_DESC(0x03, rdreq_share),
	NI_EVENT_DESC(0x04, rdreq_share_nonormal),
	NI_EVENT_DESC(0x05, rdreq_beat_any),
	NI_EVENT_DESC(0x07, wtreq_any),
	NI_EVENT_DESC(0x08, wtreq_dev),
	NI_EVENT_DESC(0x09, wtreq_noshare),
	NI_EVENT_DESC(0x0a, wtreq_all),
	NI_EVENT_DESC(0x0b, wtreq_share),
	NI_EVENT_DESC(0x0c, wtreq_share_nonormal),
	NI_EVENT_DESC(0x0d, wtreq_beat_any),
	NI_EVENT_DESC(0x0f, rddata_stall),
	NI_EVENT_DESC(0x11, wtdata_stall),
	NI_EVENT_DESC(0x20, req_stall_cc_ot_limit),
	NI_EVENT_DESC(0x21, req_stall_cc_tspec_limit),
	NI_EVENT_DESC(0x22, rdreq_stall_cc_ely_wtresp),
	NI_EVENT_DESC(0x24, req_stall_nzero_wtcnt),
	NI_EVENT_DESC(0x25, w_stall_wdatafifo_full),
	NI_EVENT_DESC(0x2a, wtreq_stall_lack_gt),
	NI_EVENT_DESC(0x2b, rdreq_stall_lack_gt),
};

static struct ni_event_desc *ni_hmni_event_descs[] = {
	NI_EVENT_DESC(0x00, rdreq_any),
	NI_EVENT_DESC(0x01, rdreq_dev),
	NI_EVENT_DESC(0x02, rdreq_noshare),
	NI_EVENT_DESC(0x03, rdreq_share),
	NI_EVENT_DESC(0x04, rdreq_share_nonormal),
	NI_EVENT_DESC(0x05, rdreq_beat_any),
	NI_EVENT_DESC(0x07, wtreq_any),
	NI_EVENT_DESC(0x08, wtreq_dev),
	NI_EVENT_DESC(0x09, wtreq_noshare),
	NI_EVENT_DESC(0x0a, wtreq_all),
	NI_EVENT_DESC(0x0b, wtreq_share),
	NI_EVENT_DESC(0x0c, wtreq_share_nonormal),
	NI_EVENT_DESC(0x0d, wtreq_beat_any),
	NI_EVENT_DESC(0x0e, rd_addr_phase_stall),
	NI_EVENT_DESC(0x0f, rd_data_phase_stall),
	NI_EVENT_DESC(0x10, wt_addr_phase_stall),
	NI_EVENT_DESC(0x11, wt_data_phase_stall),
	NI_EVENT_DESC(0x22, wtresp_stall_lack_gt),
	NI_EVENT_DESC(0x23, rdresp_stall_lack_gt),
};

static struct ni_event_desc *ni_pmni_event_descs[] = {
	NI_EVENT_DESC(0x00, rdreq_any),
	NI_EVENT_DESC(0x01, rdreq_dev_arcache),
	NI_EVENT_DESC(0x02, rdreq_noshared),
	NI_EVENT_DESC(0x05, rd_prdata_any),
	NI_EVENT_DESC(0x07, wtreq_any),
	NI_EVENT_DESC(0x08, wtreq_dev),
	NI_EVENT_DESC(0x09, wtreq_noshared),
	NI_EVENT_DESC(0x0d, wtdata_beat_any),
	NI_EVENT_DESC(0x0e, rdreq_stall),
	NI_EVENT_DESC(0x0f, rddata_stall),
	NI_EVENT_DESC(0x10, wtreq_stall),
	NI_EVENT_DESC(0x11, wtdata_stall),
	NI_EVENT_DESC(0x22, wtresp_stall_lack_gt),
	NI_EVENT_DESC(0x23, rdresp_stall_lack_gt),
};

static int ni_ev_desc_array_size(enum ni_node_type type,
				 struct ni_event_desc ***descs)
{
	switch (type) {
	case NI_ASNI:
		if (descs)
			*descs = ni_asni_event_descs;
		return ARRAY_SIZE(ni_asni_event_descs);
	case NI_AMNI:
		if (descs)
			*descs = ni_amni_event_descs;
		return ARRAY_SIZE(ni_amni_event_descs);
	case NI_HSNI:
		if (descs)
			*descs = ni_hsni_event_descs;
		return ARRAY_SIZE(ni_hsni_event_descs);
	case NI_HMNI:
		if (descs)
			*descs = ni_hmni_event_descs;
		return ARRAY_SIZE(ni_hmni_event_descs);
	case NI_PMNI:
		if (descs)
			*descs = ni_pmni_event_descs;
		return ARRAY_SIZE(ni_pmni_event_descs);
	default:
		return 0;
	}
}

static ssize_t ni_event_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct ni_event_attr *eattr;

	eattr = to_ni_event_attr(attr);

	if (eattr->ev_desc)
		return sysfs_emit(buf,
				  "%s,id=0x%x,event=0x%llx\n",
				  ni_node_name[eattr->node->type],
				  eattr->node->id,
				  eattr->ev_desc->eventid);

	return sysfs_emit(buf, "cycles\n");
}

static ssize_t ni_format_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct ni_format_attr *fmt = container_of(attr, struct ni_format_attr, attr);
	int lo = __ffs64(fmt->field), hi = fls64(fmt->field);

	if (lo == hi)
		return sysfs_emit(buf, "config:%d\n", lo);

	return sysfs_emit(buf, "config:%d-%d\n", lo, hi);
}


#define NI_FORMAT_ATTR(_name, _fld)					\
	(&((struct ni_format_attr[]) {{					\
				.attr = __ATTR(_name, 0444, ni_format_show, NULL), \
				.field = _fld,				\
			}})[0].attr.attr)

static struct attribute *ni_format_attrs[] = {
	NI_FORMAT_ATTR(event, NI_EVENT_FORMAT_EVENT),
	NI_FORMAT_ATTR(cycles, NI_EVENT_FORMAT_CYCLES),
	NI_FORMAT_ATTR(asni, NI_EVENT_FORMAT_ASNI),
	NI_FORMAT_ATTR(amni, NI_EVENT_FORMAT_AMNI),
	NI_FORMAT_ATTR(hsni, NI_EVENT_FORMAT_HSNI),
	NI_FORMAT_ATTR(hmni, NI_EVENT_FORMAT_HMNI),
	NI_FORMAT_ATTR(pmni, NI_EVENT_FORMAT_PMNI),
	NI_FORMAT_ATTR(id, NI_EVENT_FORMAT_NODEID),
	NULL
};

static const struct attribute_group ni_format_attrs_group = {
	.name = "format",
	.attrs = ni_format_attrs,
};

static ssize_t ni_cpumask_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct ni_pmu *ni_pmu = to_ni_pmu(dev_get_drvdata(dev));

	return cpumap_print_to_pagebuf(true, buf, cpumask_of(ni_pmu->ni->on_cpu));
}

static struct device_attribute ni_cpumask_attr =
		__ATTR(cpumask, 0444, ni_cpumask_show, NULL);

static struct attribute *ni_addition_attrs[] = {
	&ni_cpumask_attr.attr,
	NULL,
};

static const struct attribute_group ni_addition_attrs_group = {
	.attrs = ni_addition_attrs,
};

static u64 ni_cntr_get_and_init_optionally(struct perf_event *event, bool init)
{
	u64 old_val, new_val;
	struct ni_pmu *ni_pmu = to_ni_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	if (!is_cc_event(hwc))
		old_val = readl(ni_pmu_offset(ni_pmu, pmevcntr[hwc->idx].counter));
	else
		old_val = lo_hi_readq(ni_pmu_offset(ni_pmu, pmccntr_lower));

	if (!init)
		return old_val;

	new_val = cnt_init_val(hwc);
	if (!is_cc_event(hwc))
		writel(new_val, ni_pmu_offset(ni_pmu, pmevcntr[hwc->idx].counter));
	else
		lo_hi_writeq(new_val, ni_pmu_offset(ni_pmu, pmccntr_lower));

	return old_val;
}

static void ni_pmu_event_update(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	u64 delta, prev, now;

	do {
		prev = local64_read(&event->hw.prev_count);
		now = ni_cntr_get_and_init_optionally(event, false);
	} while (local64_cmpxchg(&event->hw.prev_count, prev, now) != prev);

	delta = now - prev;

	if (!is_cc_event(hwc))
		delta &= 0xFFFFFFFFULL;

	local64_add(delta, &event->count);
}

static void ni_pmu_set_period(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;

	ni_cntr_get_and_init_optionally(event, true);

	local64_set(&event->hw.prev_count, cnt_init_val(hwc));
}

static void ni_pmu_enable(struct pmu *pmu)
{
	struct ni_pmu *ni_pmu = to_ni_pmu(pmu);

	writel(NI_PMU_PMCR_ENABLE, ni_pmu_offset(ni_pmu, pmcr));
}

static inline void ni_pmu_disable(struct pmu *pmu)
{
	struct ni_pmu *ni_pmu = to_ni_pmu(pmu);

	writel(0, ni_pmu_offset(ni_pmu, pmcr));
}

static struct ni_node *ni_pmu_find_ev_src(struct ni_pmu *ni_pmu, u32 node_type)
{
	int idx;

	for (idx = 0; idx < ni_pmu->ev_src_num; idx++)
		if (ni_pmu->ev_src_nodes[idx].node_type == node_type)
			return &ni_pmu->ev_src_nodes[idx];

	return NULL;
}

static bool is_event_supported(u64 eventid, enum ni_node_type type)
{
	int num;
	int idx;
	struct ni_event_desc **descs;

	num = ni_ev_desc_array_size(type, &descs);

	for (idx = 0; idx < num; idx++)
		if (eventid == descs[idx]->eventid)
			break;

	return idx == num ? false : true;
}

static enum ni_node_type ni_event_config_nodetype(u64 config)
{
	unsigned long nodetype = _ni_event_config_nodetype(config);
	unsigned long lo = __ffs(nodetype), hi = __fls(nodetype);

	if (!nodetype || lo != hi)
		return 0;

	return lo;
}

static int ni_pmu_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct ni_pmu *ni_pmu = to_ni_pmu(event->pmu);
	u64 config;
	u32 id, type, node_type;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	if (is_sampling_event(event))
		return -EINVAL;

	event->cpu = ni_pmu->ni->on_cpu;

	config = event->attr.config;

	hwc->config_base = config;
	hwc->config = ni_event_config_eventid(config);

	if (ni_event_config_cc(config)) {
		if (hwc->config)
			return -EINVAL;
		else
			return 0; /* cycle event */
	}

	type = ni_event_config_nodetype(config);
	if (!type)
		return -EINVAL;

	id = ni_event_config_nodeid(config);
	node_type = type << 16 | id;

	if (!ni_pmu_find_ev_src(ni_pmu, node_type))
		return -EINVAL;

	if (!is_event_supported(hwc->config, type))
		return -EINVAL;

	return 0;
}

static void ni_pmu_event_start(struct perf_event *event, int flags)
{
	struct ni_pmu *ni_pmu = to_ni_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	u32 id, type, node_type, ev_typer;

	hwc->state = 0;

	ni_pmu_set_period(event);

	type = ni_event_config_nodetype(hwc->config_base);
	id = ni_event_config_nodeid(hwc->config_base);

	node_type = type << 16 | id;
	ev_typer = type << 8 | id;

	if (!is_cc_event(hwc)) {
		ni_node_pmuevsel(ni_pmu_find_ev_src(ni_pmu, node_type), hwc->config);
		ni_pmu_pmevtyper_sel_node(ni_pmu, ev_typer, hwc->idx);
	}

	ni_pmu_counter_enable(ni_pmu, cnt_en_bit(hwc));
}

static void ni_pmu_event_stop(struct perf_event *event, int flags)
{
	struct ni_pmu *ni_pmu = to_ni_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	if (hwc->state & PERF_HES_STOPPED)
		return;

	ni_pmu_counter_disable(ni_pmu, cnt_en_bit(hwc));

	ni_pmu_event_update(event);

	hwc->state |= PERF_HES_STOPPED | PERF_HES_UPTODATE;
}

static void ni_pmu_event_read(struct perf_event *event)
{
	ni_pmu_event_update(event);
}

static int ni_pmu_event_add(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	struct ni_pmu *ni_pmu = to_ni_pmu(event->pmu);
	int idx;

	idx = 0;
	if (is_cc_event(hwc) && ni_pmu->events[NI_PMU_COUNTER_NUM])
		return -EAGAIN; /* The cycle counter is in use. */

	idx = 0;
	if (is_cc_event(hwc))
		idx = NI_PMU_COUNTER_NUM;
	else
		while ((idx < NI_PMU_COUNTER_NUM) && ni_pmu->events[idx])
			idx++;

	if (!is_cc_event(hwc) && idx == NI_PMU_COUNTER_NUM)
		return -EAGAIN; /* All general counter is in use. */

	hwc->idx = idx;

	hwc->config = hwc->config << idx * 8; /* including is_cc */
	hwc->state = PERF_HES_STOPPED | PERF_HES_UPTODATE;

	local64_set(&event->hw.prev_count, 0);

	ni_pmu->events[idx] = event;

	ni_pmu_interrupt_enable(ni_pmu, cnt_en_bit(hwc));

	if (flags & PERF_EF_START)
		ni_pmu_event_start(event, flags);

	return 0;
}

static void ni_pmu_event_del(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	struct ni_pmu *ni_pmu = to_ni_pmu(event->pmu);

	ni_pmu_event_stop(event, flags);
	ni_pmu_interrupt_disable(ni_pmu, cnt_en_bit(hwc));
	ni_pmu->events[hwc->idx] = NULL;
}

static irqreturn_t _ni_pmu_handle_irq(struct ni_pmu *ni_pmu)
{
	unsigned long ovsr;
	int idx;
	struct perf_event *event;

	ovsr = readl(ni_pmu_offset(ni_pmu, pmovsclr));
	if (!ovsr)
		return IRQ_NONE;

	writel(ovsr, ni_pmu_offset(ni_pmu, pmovsclr));

	for_each_set_bit(idx, (unsigned long *)&ovsr, 32) {
		if (idx >= NI_PMU_COUNTER_NUM)
			idx = NI_PMU_COUNTER_NUM;

		event = ni_pmu->events[idx];
		if (WARN_ON_ONCE(!event))
			continue;

		ni_pmu_event_update(event);
		ni_pmu_set_period(event);
		if (idx == NI_PMU_COUNTER_NUM)
			break;
	}

	return IRQ_HANDLED;
}

static irqreturn_t ni_pmu_handle_irq(int irq_num, void *data)
{
	struct ni_pmu *ni_pmu = data;
	int idx, ret = IRQ_NONE;

	if (ni_pmu->ni->irq_num != 1)
		return _ni_pmu_handle_irq(ni_pmu);

	for (idx = 0; idx < ni_pmu->ni->pmu_num; idx++)
		ret |= _ni_pmu_handle_irq(ni_pmu->ni->ni_pmus[idx]);

	return ret;
}

static int ni_pmu_offline_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct global_ni *ni;
	unsigned int target;
	int idx;


	ni = hlist_entry_safe(node, struct global_ni, node);
	if (cpu != ni->on_cpu)
		return 0;


	target = cpumask_any_but(cpu_online_mask, cpu);
	if (target >= nr_cpu_ids)
		return 0;


	for (idx = 0; idx < ni->pmu_num; idx++) {
		perf_pmu_migrate_context(&ni->ni_pmus[idx]->pmu, cpu, target);
		WARN_ON(irq_set_affinity(ni->ni_pmus[idx]->irq, cpumask_of(target)));
	}

	ni->on_cpu = target;

	return 0;
}

static u32 ni_child_number_total(void __iomem *periphbase,
				 void __iomem *from, enum ni_node_type type)
{
	enum ni_node_type node_type;
	int total, idx;
	void __iomem *child_base;

	node_type = ni_node_type(from);

	if (node_type == type)
		return 1;

	if (node_type >= NI_ASNI)
		return 0;

	total = 0;
	for (idx = 0; idx < ni_child_number(from); idx++) {
		child_base = ni_child_pointer(periphbase, from, idx);
		total += ni_child_number_total(periphbase, child_base, type);
	}

	return total;
}

static void ni_pmu_reset(struct ni_pmu *ni_pmu)
{
	ni_pmu_disable(&ni_pmu->pmu);

#define clear_reg(name) \
	writel(readl(ni_pmu_offset(ni_pmu, name)), ni_pmu_offset(ni_pmu, name))

	clear_reg(pmcntenclr);
	clear_reg(pmintenclr);
	clear_reg(pmovsclr);

	writel_relaxed(NI_PMU_PMCR_RST_CYC_CNTR & NI_PMU_PMCR_RST_EV_CNTR,
		       ni_pmu_offset(ni_pmu, pmcr));
}

static int ni_pmu_irq_setup(struct ni_pmu *ni_pmu, int irq_idx)
{
	int err;
	unsigned long flags = IRQF_NOBALANCING | IRQF_SHARED | IRQF_NO_THREAD;

	ni_pmu->irq = platform_get_irq(to_platform_device(ni_pmu->dev), irq_idx);
	if (ni_pmu->irq < 0)
		return ni_pmu->irq;

	err = devm_request_irq(ni_pmu->dev, ni_pmu->irq, ni_pmu_handle_irq,
			       flags, dev_name(ni_pmu->dev), ni_pmu);
	if (err)
		return err;

	err = irq_set_affinity(ni_pmu->irq, cpumask_of(ni_pmu->ni->on_cpu));
	if (err)
		return err;

	return 0;
}

static int ni_event_attr_init(struct device *dev,
			      struct ni_event_attr *eattr,
			       struct ni_node *node,
			       struct ni_event_desc *desc)
{
	const char *name;

	sysfs_attr_init(&eattr->attr.attr);

	eattr->ev_desc = desc;
	eattr->node = node;

	if (desc && node)
		name = devm_kasprintf(dev,
			      GFP_KERNEL,
			      "%s_%d_%s",
			      ni_node_name[node->type],
			      node->id,
			      desc->name);
	else if (!desc && !node)
		name = "cycles";
	else {
		return -EINVAL;
	}

	if (!name)
		return -ENOMEM;

	eattr->attr = (struct device_attribute){
		.attr = {
			.name = name,
			.mode = VERIFY_OCTAL_PERMISSIONS(0444)
		},
		.show	= ni_event_show,
		.store	= NULL,
	};

	return 0;
}

static int ni_pmu_init_attr_groups(struct ni_pmu *ni_pmu)
{
	int idx, ev_idx, ev_num, ret, ev_num_tmp;
	struct ni_node *node;
	struct ni_event_desc **descs;
	struct attribute **eattrs;
	struct ni_event_attr *ni_eattrs;
	struct device *dev;
	struct attribute_group *eattr_group;
	const struct attribute_group **attr_groups;
	const struct attribute_group *ni_attr_groups_template[4];

	dev = ni_pmu->dev;

	eattr_group = devm_kzalloc(dev, sizeof(*eattr_group), GFP_KERNEL);

	ev_num = 0;
	for (idx = 0; idx < ni_pmu->ev_src_num; idx++) {
		node = &ni_pmu->ev_src_nodes[idx];

		ev_num += ni_ev_desc_array_size(node->type, NULL);
	}

	ev_num++;

	eattrs = devm_kmalloc(dev, sizeof(eattrs[0]) * (ev_num + 1), GFP_KERNEL);
	if (!eattrs)
		return -ENOMEM;

	ni_eattrs = devm_kzalloc(dev, sizeof(ni_eattrs[0]) * ev_num, GFP_KERNEL);
	if (!ni_eattrs)
		return -ENOMEM;

	ev_num = 0;
	ret = ni_event_attr_init(dev, &ni_eattrs[ev_num++], NULL, NULL);
	if (ret)
		return ret;

	for (idx = 0; idx < ni_pmu->ev_src_num; idx++) {
		node = &ni_pmu->ev_src_nodes[idx];

		ev_num_tmp = ni_ev_desc_array_size(node->type, &descs);
		for (ev_idx = 0; ev_idx < ev_num_tmp; ev_idx++) {
			struct ni_event_desc *desc;

			desc = descs[ev_idx];

			ret = ni_event_attr_init(dev, &ni_eattrs[ev_num++], node, desc);
			if (ret)
				return ret;
		}
	}

	for (idx = 0; idx < ev_num; idx++)
		eattrs[idx] = &ni_eattrs[idx].attr.attr;

	eattrs[idx] = NULL;

	eattr_group->name = "events";
	eattr_group->attrs = eattrs;

	ni_attr_groups_template[0] = eattr_group;
	ni_attr_groups_template[1] = &ni_format_attrs_group;
	ni_attr_groups_template[2] = &ni_addition_attrs_group;
	ni_attr_groups_template[3] = NULL;

	attr_groups = devm_kmemdup(dev,
				   ni_attr_groups_template,
				   sizeof(ni_attr_groups_template),
				   GFP_KERNEL);
	if (!attr_groups)
		return -ENOMEM;

	ni_pmu->pmu.attr_groups = attr_groups;

	return 0;
}

DEFINE_FREE(cd_arrays, void __iomem **, if (_T) kfree(_T))

static int ni_discovery(struct global_ni *ni)
{
	u32 vd_idx, pd_idx, cd_idx, nd_idx, num_idx = 0;
	void __iomem *vd, *pd, *cd, *nd;
	int num;
	struct ni_pmu *ni_pmu;
	struct ni_node node;
	void __iomem *pbase;
	struct device *dev = ni->dev;

	void  __iomem **cd_arrays __free(cd_arrays) = NULL;

	pbase = ni->base;

	cd_arrays = kmalloc_array(ni->cd_num, sizeof(typeof(cd)), GFP_KERNEL);

	/* Step1: Get all clock domains. */
	for (vd_idx = 0; vd_idx < ni_child_number(ni->base); vd_idx++) {
		vd = ni_child_pointer(pbase, ni->base, vd_idx);

		for (pd_idx = 0; pd_idx < ni_child_number(vd); pd_idx++) {
			pd = ni_child_pointer(pbase, vd, pd_idx);

			dev_dbg(dev, "The %dth power domain has %d clock domain",
				pd_idx,
				ni_child_number(pd));

			for (cd_idx = 0; cd_idx < ni_child_number(pd); cd_idx++) {
				cd_arrays[num_idx++] =
					ni_child_pointer(pbase, pd, cd_idx);
			}
		}
	}

	/* Step2: Traverse all clock domains. */
	for (cd_idx = 0; cd_idx < ni->cd_num; cd_idx++) {
		cd = cd_arrays[cd_idx];

		num = ni_child_number(cd);
		dev_dbg(dev, "The %dth clock domain has %d child nodes:", cd_idx, num);

		/* Omit pmu node */
		ni_pmu = devm_kzalloc(dev, struct_size(ni_pmu, ev_src_nodes, num - 1),
				      GFP_KERNEL);
		ni_pmu->ev_src_num = num - 1;

		if (!ni_pmu)
			return -ENOMEM;

		num_idx = 0;
		for (nd_idx = 0; nd_idx < num; nd_idx++) {
			nd = ni_child_pointer(pbase, cd, nd_idx);

			node.base = nd;
			node.node_type = ni_node_node_type(nd);

			if (unlikely(ni_node_type(nd) == NI_PMU))
				ni_pmu->pmu_node = node;
			else
				ni_pmu->ev_src_nodes[num_idx++] = node;
			dev_dbg(dev, "  name: %s   id: %d", ni_node_name[node.type], node.id);
		}

		ni_pmu->dev = dev;
		ni_pmu->ni = ni;
		ni->ni_pmus[cd_idx] = ni_pmu;
	}

	return 0;
}

static int ni_pmu_probe(struct platform_device *pdev)
{
	int ret, cd_num, idx, irq_num, irq_idx;
	void __iomem *periphbase;
	struct global_ni *ni;
	struct device *dev = &pdev->dev;
	char *name;
	static int id;
	struct ni_pmu *ni_pmu;

	periphbase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(periphbase))
		return dev_err_probe(dev, PTR_ERR(periphbase), "Couldn't get ioremap\n");

	cd_num = ni_child_number_total(periphbase, periphbase, NI_CD);

	/* Each clock domain contains one PMU. So cd_num == pmu_num. */
	ni = devm_kzalloc(dev,
			  struct_size(ni, ni_pmus, cd_num),
			  GFP_KERNEL);
	if (!ni)
		return -ENOMEM;

	ni->cd_num = cd_num;
	ni->base = periphbase;
	ni->dev = dev;
	ni->on_cpu = raw_smp_processor_id();
	platform_set_drvdata(pdev, ni);

	ret = ni_discovery(ni);
	if (ret)
		return ret;

	irq_num = platform_irq_count(pdev);
	/* Support that one NI with one irq or one clock domain with one irq. */
	if (irq_num < 0 || (irq_num != 1 && irq_num != ni->cd_num)) {
		dev_err(dev, "Error in irq number: %d.", irq_num);
		return -EINVAL;
	}

	if (irq_num != cd_num) {
		dev_warn(dev, "Only one IRQ found for all PMU.");
		ret = ni_pmu_irq_setup(ni->ni_pmus[0], 0);
		if (ret)
			return ret;
	}

	ni->irq_num = irq_num;

	for (idx = 0, irq_idx = 0; idx < ni->pmu_num; idx++) {
		ni_pmu = ni->ni_pmus[idx];
		ret = ni_pmu_init_attr_groups(ni_pmu);
		if (ret)
			return ret;

		if (irq_num == cd_num) {
			ret = ni_pmu_irq_setup(ni_pmu, irq_idx++);
			if (ret)
				return ret;
		}

		ni_pmu_reset(ni_pmu);

		ni_pmu->pmu = (struct pmu) {
			.module		= THIS_MODULE,
			.task_ctx_nr    = perf_invalid_context,
			.pmu_enable	= ni_pmu_enable,
			.pmu_disable	= ni_pmu_disable,
			.event_init	= ni_pmu_event_init,
			.add		= ni_pmu_event_add,
			.del		= ni_pmu_event_del,
			.start		= ni_pmu_event_start,
			.stop		= ni_pmu_event_stop,
			.read		= ni_pmu_event_read,
			.attr_groups    = ni_pmu->pmu.attr_groups,
			.capabilities	= PERF_PMU_CAP_NO_EXCLUDE,
		};

		ret = of_alias_get_id(pdev->dev.of_node, "ni-pmu");
		if (ret >= 0)
			id = ret;

		if (cd_num > 1)
			name = devm_kasprintf(dev, GFP_KERNEL, "ni_pmu_%d_%d", id++, idx);
		else
			name = devm_kasprintf(dev, GFP_KERNEL, "ni_pmu_%d", id++);

		ret = perf_pmu_register(&ni_pmu->pmu, name, -1);
		if (ret) {
			dev_err(dev, "Error %d_%d registering PMU", id - 1, idx);
			return ret;
		}
	}

	ret = cpuhp_state_add_instance_nocalls(hp_state,
					       &ni->node);
	if (ret)
		return ret;

	return 0;
}

static int ni_pmu_remove(struct platform_device *pdev)
{
	struct global_ni *ni = platform_get_drvdata(pdev);
	int idx;

	for (idx = 0; idx < ni->pmu_num; idx++)
		perf_pmu_unregister(&ni->ni_pmus[idx]->pmu);

	cpuhp_remove_multi_state(hp_state);

	return 0;
}

static const struct of_device_id ni_pmu_of_match[] = {
	{ .compatible = "hexin,c2000-arm-ni" },
	{},
};

static struct platform_driver ni_pmu_driver = {
	.driver = {
		.name = "ni-pmu",
		.of_match_table = ni_pmu_of_match,
	},
	.remove = ni_pmu_remove,
	.probe = ni_pmu_probe,
};

static int __init ni_pmu_init(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN,
				      "perf/arm/ni:online",
				      NULL,
				      ni_pmu_offline_cpu);
	if (ret < 0)
		return ret;

	hp_state = ret;

	ret = platform_driver_register(&ni_pmu_driver);
	if (ret)
		cpuhp_remove_multi_state(hp_state);

	return ret;
}

static void __exit ni_pmu_exit(void)
{
	cpuhp_remove_multi_state(hp_state);
	platform_driver_unregister(&ni_pmu_driver);
}

module_init(ni_pmu_init);
module_exit(ni_pmu_exit);

MODULE_AUTHOR("Jialong Yang <jialong.yang@shingroup.cn>");
MODULE_DESCRIPTION("PMU driver for ARM NI-700 Performance Monitors Unit");
MODULE_LICENSE("GPL");
