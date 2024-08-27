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
/* main prog */
__xlated("4: r1 = *(u64 *)(r1 +0)")
__xlated("5: call pc+1")
__xlated("6: exit")
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
__xlated("8: w0 = w6")
__xlated("9: w0 *= 2")
__xlated("10: exit")
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
/* save __u64 *ctx to stack */
__xlated("4: *(u64 *)(r10 -8) = r1")
/* main prog */
__xlated("5: r1 = *(u64 *)(r1 +0)")
__xlated("6: call pc+")
/* epilogue */
__xlated("7: r1 = *(u64 *)(r10 -8)")
__xlated("8: r1 = *(u64 *)(r1 +0)")
__xlated("9: r6 = *(u32 *)(r1 +0)")
__xlated("10: w6 += 10000")
__xlated("11: *(u32 *)(r1 +0) = r6")
__xlated("12: w0 = w6")
__xlated("13: w0 *= 2")
__xlated("14: exit")
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
__retval(1001) /* PROLOGUE_A [1000] + SUBPROG_A [1] */
int syscall_prologue_subprog(void *ctx)
{
	struct st_ops_args args = {};

	return bpf_kfunc_st_ops_test_prologue(&args);
}

SEC("syscall")
__retval(20002) /* (SUBPROG_A [1] + EPILOGUE_A [10000]) * 2 */
int syscall_epilogue_subprog(void *ctx)
{
	struct st_ops_args args = {};

	return bpf_kfunc_st_ops_test_epilogue(&args);
}

SEC("syscall")
__retval(22002) /* (PROLOGUE_A [1000] + SUBPROG_A [1] + EPILOGUE_A [10000]) * 2 */
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
