/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CXL Hotness Monitoring Unit Support
 */

#ifndef INCLUDE__PERF_CXL_HMU_H__
#define INCLUDE__PERF_CXL_HMU_H__

#define CXL_HMU_PMU_NAME		"cxl_hmu"
#define CXL_HMU_AUXTRACE_PRIV_SIZE	sizeof(u64)

struct auxtrace_record *chmu_recording_init(int *err,
					       struct perf_pmu *cxl_hmu_pmu);

int cxl_hmu_process_auxtrace_info(union perf_event *event,
				   struct perf_session *session);

#endif
