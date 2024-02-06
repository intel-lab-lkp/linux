// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2024 Meta Platforms, Inc. and affiliates. */
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "bpf_experimental.h"

struct {
	__uint(type, BPF_MAP_TYPE_ARENA);
	__uint(map_flags, BPF_F_MMAPABLE);
	__uint(max_entries, 1u << 24); /* max_entries * value_size == size of mmap() region */
	__ulong(map_extra, 2ull << 44); /* start of mmap() region */
	__type(key, __u64);
	__type(value, __u64);
} arena SEC(".maps");

#include "bpf_arena_alloc.h"
#include "bpf_arena_list.h"

struct elem {
	struct arena_list_node node;
	__u64 value;
};

struct arena_list_head __arena *list_head;
int list_sum;
int cnt;
bool skip = false;

SEC("syscall")
int arena_list_add(void *ctx)
{
#ifdef __BPF_FEATURE_ARENA_CAST
	__u64 i;

	list_head = bpf_alloc(sizeof(*list_head));

	bpf_for(i, 0, cnt) {
		struct elem __arena *n = bpf_alloc(sizeof(*n));

		n->value = i;
		list_add_head(&n->node, list_head);
	}
#else
	skip = true;
#endif
	return 0;
}

SEC("syscall")
int arena_list_del(void *ctx)
{
#ifdef __BPF_FEATURE_ARENA_CAST
	struct elem __arena *n;
	int sum = 0;

	list_for_each_entry(n, list_head, node) {
		sum += n->value;
		list_del(&n->node);
		bpf_free(n);
	}
	list_sum = sum;

	/* triple free will not crash the kernel */
	bpf_free(list_head);
	bpf_free(list_head);
	bpf_free(list_head);
#else
	skip = true;
#endif
	return 0;
}

char _license[] SEC("license") = "GPL";
