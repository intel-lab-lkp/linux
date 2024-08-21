// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2024 Meta Platforms, Inc. and affiliates. */

#include <vmlinux.h>
#include <bpf/bpf_tracing.h>
#include "bpf_misc.h"
#include "../bpf_testmod/bpf_testmod.h"
#include "../bpf_testmod/bpf_testmod_kfunc.h"

char _license[] SEC("license") = "GPL";

static __noinline __used int subprog(struct st_ops_args *args)
{
	args->a += 1;
	return args->a;
}

__success
/* prologue */
__xlated("0: r6 = *(u64 *)(r1 +0)")
__xlated("1: r7 = *(u32 *)(r6 +0)")
__xlated("2: w7 += 1000")
__xlated("3: *(u32 *)(r6 +0) = r7")
__xlated("4: r7 = r1")
__xlated("5: r1 = r6")
__xlated("6: call kernel-function")
__xlated("7: r1 = r6")
__xlated("8: call kernel-function")
__xlated("9: r1 = r7")
/* main prog */
__xlated("10: r1 = *(u64 *)(r1 +0)")
__xlated("11: call pc+1")
__xlated("12: exit")
SEC("struct_ops/test_prologue_subprog")
__naked int test_prologue_subprog(void)
{
	asm volatile (
	"r1 = *(u64 *)(r1 +0);"
	"call subprog;"
	"exit;"
	::: __clobber_all);
}

__success
/* save __u64 *ctx to stack */
__xlated("0: *(u64 *)(r10 -8) = r1")
/* main prog */
__xlated("1: r1 = *(u64 *)(r1 +0)")
__xlated("2: call pc+")
/* epilogue */
__xlated("3: r1 = *(u64 *)(r10 -8)")
__xlated("4: r1 = *(u64 *)(r1 +0)")
__xlated("5: r6 = *(u32 *)(r1 +0)")
__xlated("6: w6 += 10000")
__xlated("7: *(u32 *)(r1 +0) = r6")
__xlated("8: r6 = r1")
__xlated("9: call kernel-function")
__xlated("10: r1 = r6")
__xlated("11: call kernel-function")
__xlated("12: w0 *= 2")
__xlated("13: exit")
SEC("struct_ops/test_epilogue_subprog")
__naked int test_epilogue_subprog(void)
{
	asm volatile (
	"r1 = *(u64 *)(r1 +0);"
	"call subprog;"
	"exit;"
	::: __clobber_all);
}

__success
/* prologue */
__xlated("0: r6 = *(u64 *)(r1 +0)")
__xlated("1: r7 = *(u32 *)(r6 +0)")
__xlated("2: w7 += 1000")
__xlated("3: *(u32 *)(r6 +0) = r7")
__xlated("4: r7 = r1")
__xlated("5: r1 = r6")
__xlated("6: call kernel-function")
__xlated("7: r1 = r6")
__xlated("8: call kernel-function")
__xlated("9: r1 = r7")
/* save __u64 *ctx to stack */
__xlated("10: *(u64 *)(r10 -8) = r1")
/* main prog */
__xlated("11: r1 = *(u64 *)(r1 +0)")
__xlated("12: call pc+")
/* epilogue */
__xlated("13: r1 = *(u64 *)(r10 -8)")
__xlated("14: r1 = *(u64 *)(r1 +0)")
__xlated("15: r6 = *(u32 *)(r1 +0)")
__xlated("16: w6 += 10000")
__xlated("17: *(u32 *)(r1 +0) = r6")
__xlated("18: r6 = r1")
__xlated("19: call kernel-function")
__xlated("20: r1 = r6")
__xlated("21: call kernel-function")
__xlated("22: w0 *= 2")
__xlated("23: exit")
SEC("struct_ops/test_pro_epilogue_subprog")
__naked int test_pro_epilogue_subprog(void)
{
	asm volatile (
	"r1 = *(u64 *)(r1 +0);"
	"call subprog;"
	"exit;"
	::: __clobber_all);
}

SEC("syscall")
__retval(1111) /* PROLOGUE_A [1110] + SUBPROG_A [1] */
int syscall_prologue_subprog(void *ctx)
{
	struct st_ops_args args = {};

	return bpf_kfunc_st_ops_test_prologue(&args);
}

SEC("syscall")
__retval(20222) /* (SUBPROG_A [1] + EPILOGUE_A [10110]) * 2 */
int syscall_epilogue_subprog(void *ctx)
{
	struct st_ops_args args = {};

	return bpf_kfunc_st_ops_test_epilogue(&args);
}

SEC("syscall")
__retval(22442) /* (PROLOGUE_A [1110] + SUBPROG_A [1] + EPILOGUE_A [10110]) * 2 */
int syscall_pro_epilogue_subprog(void *ctx)
{
	struct st_ops_args args = {};

	return bpf_kfunc_st_ops_test_pro_epilogue(&args);
}

SEC(".struct_ops.link")
struct bpf_testmod_st_ops pro_epilogue_subprog = {
	.test_prologue = (void *)test_prologue_subprog,
	.test_epilogue = (void *)test_epilogue_subprog,
	.test_pro_epilogue = (void *)test_pro_epilogue_subprog,
};
