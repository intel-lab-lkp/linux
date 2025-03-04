// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2025 Meta Platforms, Inc. and affiliates. */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

char _license[] SEC("license") = "GPL";

SEC("freplace/kprobe_prog")
int new_kprobe_prog(struct pt_regs *ctx)
{
	return 1;
}
