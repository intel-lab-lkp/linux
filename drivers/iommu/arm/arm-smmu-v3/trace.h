/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ARM SMMUv3 trace support
 *
 * Copyright (c) 2026 OpenCloudOS / openEuler
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM arm_smmu_v3

#if !defined(_TRACE_ARM_SMMU_V3_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_ARM_SMMU_V3_H

#include <linux/tracepoint.h>

#include "arm-smmu-v3.h"

TRACE_EVENT(smmu_evtq_event,

	TP_PROTO(struct arm_smmu_device *smmu, u64 *evt),

	TP_ARGS(smmu, evt),

	TP_STRUCT__entry(
		__string(iommu, dev_name(smmu->dev))
		__field(u64, evt0)
		__field(u64, evt1)
		__field(u64, evt2)
		__field(u64, evt3)
	),

	TP_fast_assign(
		__assign_str(iommu);
		__entry->evt0 = evt[0];
		__entry->evt1 = evt[1];
		__entry->evt2 = evt[2];
		__entry->evt3 = evt[3];
	),

	TP_printk("%s evt: 0x%016llx 0x%016llx 0x%016llx 0x%016llx",
		__get_str(iommu),
		__entry->evt0, __entry->evt1,
		__entry->evt2, __entry->evt3)
);

#endif /* _TRACE_ARM_SMMU_V3_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH ../../drivers/iommu/arm/arm-smmu-v3/
#define TRACE_INCLUDE_FILE trace
#include <trace/define_trace.h>
