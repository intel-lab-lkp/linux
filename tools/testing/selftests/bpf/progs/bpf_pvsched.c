// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019 Facebook */

#include "vmlinux.h"
#include "bpf_tracing_net.h"
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_helpers.h>

char _license[] SEC("license") = "GPL";

SEC("struct_ops/pvsched_vcpu_reg")
int BPF_PROG(pvsched_vcpu_reg, struct pid *pid)
{
	bpf_printk("pvsched_vcpu_reg: pid: %p", pid);
	return 0;
}

SEC("struct_ops/pvsched_vcpu_unreg")
void BPF_PROG(pvsched_vcpu_unreg, struct pid *pid)
{
	bpf_printk("pvsched_vcpu_unreg: pid: %p", pid);
}

SEC("struct_ops/pvsched_vcpu_notify_event")
void BPF_PROG(pvsched_vcpu_notify_event, void *addr, struct pid *pid, __u32 event)
{
	bpf_printk("pvsched_vcpu_notify: pid: %p, event:%u", pid, event);
}

SEC(".struct_ops")
struct pvsched_vcpu_ops pvsched_ops = {
	.pvsched_vcpu_register		= (void *)pvsched_vcpu_reg,
	.pvsched_vcpu_unregister	= (void *)pvsched_vcpu_unreg,
	.pvsched_vcpu_notify_event	= (void *)pvsched_vcpu_notify_event,
	.events				= 0x6,
	.name				= "bpf_pvsched_ops",
};
