// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2024 Meta Platforms, Inc. and affiliates. */

#include <vmlinux.h>
#include <bpf/bpf_tracing.h>
#include "bpf_misc.h"

/* r1 with off = 0 is checked, which marks new id for r0 with off=8 */
SEC("tp_btf/bpf_testmod_test_raw_tp_null")
__failure
__msg("2: (b7) r2 = 0                        ; R2_w=0")
__msg("3: (07) r0 += 8                       ; R0_w=trusted_ptr_or_null_sk_buff(id=1,off=8)")
__msg("4: (15) if r1 == 0x0 goto pc+2        ; R1_w=trusted_ptr_sk_buff()")
__msg("5: (bf) r2 = r0                       ; R0_w=trusted_ptr_or_null_sk_buff(id=2,off=8)")
int BPF_PROG(test_raw_tp_null_check_zero_off, struct sk_buff *skb)
{
	asm volatile (
		"r1 = *(u64 *)(r1 +0);			\
		 r0 = r1;				\
		 r2 = 0;				\
		 r0 += 8;				\
		 if r1 == 0 goto jmp;			\
		 r2 = r0;				\
		 *(u64 *)(r2 +0) = r2;			\
		 jmp:					"
		::
		: __clobber_all
	);
	return 0;
}

/* r2 with offset is checked, which marks r1 with off=0 as non-NULL */
SEC("tp_btf/bpf_testmod_test_raw_tp_null")
__failure
__msg("3: (07) r2 += 8                       ; R2_w=trusted_ptr_or_null_sk_buff(id=1,off=8)")
__msg("4: (15) if r2 == 0x0 goto pc+2        ; R2_w=trusted_ptr_or_null_sk_buff(id=2,off=8)")
__msg("5: (bf) r1 = r1                       ; R1_w=trusted_ptr_sk_buff()")
int BPF_PROG(test_raw_tp_null_copy_check_with_off, struct sk_buff *skb)
{
	asm volatile (
		"r1 = *(u64 *)(r1 +0);			\
		 r2 = r1;				\
		 r3 = 0;				\
		 r2 += 8;				\
		 if r2 == 0 goto jmp2;			\
		 r1 = r1;				\
		 *(u64 *)(r3 +0) = r3;			\
		 jmp2:					"
		::
		: __clobber_all
	);
	return 0;
}

/* Ensure id's are incremented everytime things are checked.. */
SEC("tp_btf/bpf_testmod_test_raw_tp_null")
__failure
__msg("2: (07) r0 += 8                       ; R0_w=trusted_ptr_or_null_sk_buff(id=1,off=8)")
__msg("3: (15) if r0 == 0x0 goto pc+4        ; R0_w=trusted_ptr_or_null_sk_buff(id=2,off=8)")
__msg("4: (15) if r0 == 0x0 goto pc+3        ; R0_w=trusted_ptr_or_null_sk_buff(id=4,off=8)")
__msg("5: (15) if r0 == 0x0 goto pc+2        ; R0_w=trusted_ptr_or_null_sk_buff(id=6,off=8)")
__msg("6: (bf) r2 = r0                       ; R0_w=trusted_ptr_or_null_sk_buff(id=6,off=8)")
int BPF_PROG(test_raw_tp_check_with_off, struct sk_buff *skb)
{
	asm volatile (
		"r1 = *(u64 *)(r1 +0);			\
		 r0 = r1;				\
		 r0 += 8;				\
		 if r0 == 0 goto jmp3;			\
		 if r0 == 0 goto jmp3;			\
		 if r0 == 0 goto jmp3;			\
		 r2 = r0;				\
		 *(u64 *)(r2 +0) = r2;			\
		 jmp3:					"
		::
		: __clobber_all
	);
	return 0;
}

char _license[] SEC("license") = "GPL";
