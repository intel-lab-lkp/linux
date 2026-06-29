#ifndef _ASM_DTS_PMU_H
#define _ASM_DTS_PMU_H

#include <linux/types.h>

#define MAX_FIELDS 17
#define MAX_MMCR   5
#define MAX_DTS_EVENTS 32
#define MAX_PMU_COUNTERS 6

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

int compute_mmcr_dts(u64 event[], int n_ev,
			unsigned int hwc[], struct mmcr_regs *mmcr,
			struct perf_event *pevents[], u32 flags);

#endif
