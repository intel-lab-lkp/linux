// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

#include "bpf_record_action.h"

int output_formats[__OUTPUT_FORMATS_MAX_NUM] = { 0 };
int output_format_num = 0;
int enabled = 0;

#define MAX_CPUS  1024

/* bpf-output associated map */
struct __sample_data__ {
	__uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
	__type(key, int);
	__type(value, __u32);
	__uint(max_entries, MAX_CPUS);
} __sample_data__ SEC(".maps");

struct sample_data_tmp {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__type(key, int);
	__type(value, struct __output_data_payload);
	__uint(max_entries, 1);
} sample_data_tmp SEC(".maps");

static inline struct __output_data_payload *sample_data_payload(void)
{
	int key = 0;

	return bpf_map_lookup_elem(&sample_data_tmp, &key);
}

static inline int output_cpu(__u8 *data, int size)
{
	__u32 *cpu = (__u32 *)data;

	if (size < sizeof(__u32))
		return -1;

	*cpu = bpf_get_smp_processor_id();
	return sizeof(__u32);
}

static inline int output_pid(__u8 *data, int size)
{
	__u32 *pid = (__u32 *)data;

	if (size < sizeof(__u32))
		return -1;

	*pid = bpf_get_current_pid_tgid() >> 32;
	return sizeof(__u32);
}

SEC("xxx")
int sample_output(u64 *ctx)
{
	struct __output_data_payload *sample;
	__u8 *data;
	int i;
	int total = 0;
	int ret = 0;

	if (!enabled)
		return 0;

	sample = sample_data_payload();
	if (!sample)
		return 0;

	data = sample->__data;
	for (i = 0; i < output_format_num && i < __OUTPUT_FORMATS_MAX_NUM; i++) {
		switch (output_formats[i]) {
		case __OUTPUT_FORMAT_TYPE_CPU:
			ret = output_cpu(data + total, __OUTPUT_DATA_MAX_SIZE - total);
			break;
		case __OUTPUT_FORMAT_TYPE_PID:
			ret = output_pid(data + total, __OUTPUT_DATA_MAX_SIZE - total);
			break;
		default:
			ret = -1;
			break;
		}

		if (ret < 0)
			return 0;

		total += ret;
		if (total > __OUTPUT_DATA_MAX_SIZE)
			return 0;
	}

	sample->__size = total;
	bpf_perf_event_output(ctx, &__sample_data__, BPF_F_CURRENT_CPU,
			      sample, sizeof(__u32) + total);
	return 0;
}

char _license[] SEC("license") = "GPL";
