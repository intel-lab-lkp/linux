// SPDX-License-Identifier: GPL-2.0

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include "bpf_misc.h"

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1);
	__type(key, long long);
	__type(value, struct test_val);
} map_hash_48b SEC(".maps");

SEC("socket")
__description("invalid map type for tail call")
__failure __msg("expected prog array map for tail call")
__failure_unpriv
__naked void invalid_map_for_tail_call(void)
{
	asm volatile ("			\
	r2 = %[map_hash_48b] ll;	\
	r3 = 0;				\
	call %[bpf_tail_call];		\
	exit;				\
"	:
	: __imm(bpf_tail_call),
	  __imm_addr(map_hash_48b),
	: __clobber_all);
}

char _license[] SEC("license") = "GPL";
