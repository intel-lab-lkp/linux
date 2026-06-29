#ifndef _ASM_DTS_PMU_H
#define _ASM_DTS_PMU_H

#include <linux/types.h>

#define MAX_FIELDS 17
#define MAX_MMCR   5
#define MAX_DTS_EVENTS 32
#define MAX_PMU_COUNTERS 6
#define DTS_COND_ALWAYS      0
#define DTS_COND_MARKED      1
#define DTS_COND_THRESHOLD   2
#define DTS_COND_L1          3
#define DTS_COND_BHRB        4
#define DTS_COND_CACHE       5
#define DTS_COND_RADIX       6

struct dts_field_map {
	u32 bits_start, bits_end;
	u32 target_field_base, target_field_shift;
	u32 pgm_start, mmcr;
	bool use_target_field_shift, is_pmc;
	char name[32];
};

extern struct dts_field_map field_maps[MAX_FIELDS];
extern int field_count;

extern u32 mmcr_regs_sprs[MAX_MMCR];
extern int mmcr_count;
extern struct dts_constraint_map *constraint_maps;
extern int constraint_map_count;

int compute_mmcr_dts(u64 event[], int n_ev,
			unsigned int hwc[], struct mmcr_regs *mmcr,
			struct perf_event *pevents[], u32 flags);

struct dts_constraint_field {
	u64 event_mask, constraint_mask;
	u32 event_shift, constraint_shift, condition;
};

struct dts_constraint_map {
	struct dts_constraint_field field;
};

struct restricted_counter {
	u32 pmc;
	u64 event;
};

struct dts_threshold_constraints {
	bool supported;
	struct dts_constraint_field thresh_sel, thresh_cmp, thresh_ctl;
};

struct dts_nc_constraints {
	u64 mask;
	u32 shift, increment;
};

struct dts_pmu_constraints {
	u32 max_counter;

	struct restricted_counter restricted[8];
	int num_restricted;

	bool require_cache_selector_zero;
	u32 cache_selector_mask;

	bool require_pmc_for_ebb, bhrb_requires_ebb;

	struct dts_constraint_field sample, ebb, bhrb, l1_qualifier,
				    fab_match, radix_scope, cache_group,
				    cache_pmc4, l2l3_group;

	struct dts_threshold_constraints threshold;
	struct dts_nc_constraints nc;
};

extern struct dts_pmu_constraints dts_constraints;
#endif
