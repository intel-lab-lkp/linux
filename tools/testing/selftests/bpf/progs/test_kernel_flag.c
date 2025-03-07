// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2025 Microsoft Corporation
 *
 * Author: Blaise Boscaccy <bboscaccy@linux.microsoft.com>
 */

#include "vmlinux.h"
#include <errno.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char _license[] SEC("license") = "GPL";

__u32 monitored_pid;

SEC("lsm.s/bpf")
int BPF_PROG(bpf, int cmd, union bpf_attr *attr, unsigned int size, bool kernel)
{
	__u32 pid;

	pid = bpf_get_current_pid_tgid() >> 32;

	if (pid != monitored_pid)
		return 0;

	if (kernel)
		return -EINVAL;
	else
		return 0;
}
