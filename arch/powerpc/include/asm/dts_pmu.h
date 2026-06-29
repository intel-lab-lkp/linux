#ifndef _ASM_DTS_PMU_H
#define _ASM_DTS_PMU_H

#include <linux/types.h>

#define MAX_MMCR   5
#define MAX_DTS_EVENTS 32
#define MAX_PMU_COUNTERS 6

extern u32 mmcr_regs_sprs[MAX_MMCR];
extern int mmcr_count;
#endif
