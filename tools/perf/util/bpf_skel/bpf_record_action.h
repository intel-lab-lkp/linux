/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_UTIL_BPF_SKEL_BPF_RECORD_ACTION_H_
#define __PERF_UTIL_BPF_SKEL_BPF_RECORD_ACTION_H_

#define __TASK_COMM_MAX_SIZE 16

#define __OUTPUT_FORMATS_MAX_NUM 8

enum __output_format_type {
	__OUTPUT_FORMAT_TYPE_CPU,
	__OUTPUT_FORMAT_TYPE_PID,
	__OUTPUT_FORMAT_TYPE_MAX,
};

#define __OUTPUT_DATA_MAX_SIZE 256
struct __output_data_payload {
	__u32 __size;
	__u8 __data[__OUTPUT_DATA_MAX_SIZE];
};

#endif /* __PERF_UTIL_BPF_SKEL_BPF_RECORD_ACTION_H_ */
