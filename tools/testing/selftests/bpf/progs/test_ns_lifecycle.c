// SPDX-License-Identifier: GPL-2.0

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

struct ns_stat {
	__u32 ns_type;
	__u32 init_count;
	__u32 install_count;
	__u32 free_count;
};

/* Unlike inode numbers namespace ids are never recycled. */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 16);
	__type(key, __u64);
	__type(value, struct ns_stat);
} ns_lifecycle SEC(".maps");

__u64 monitored_pidfd_ino = 0;

char _license[] SEC("license") = "GPL";

static bool is_monitored(void)
{
	struct task_struct *task;

	if (!monitored_pidfd_ino)
		return false;

	task = bpf_get_current_task_btf();
	return task->thread_pid->ino == monitored_pidfd_ino;
}

SEC("lsm/namespace_init")
int BPF_PROG(ns_init, struct ns_common *ns, int ret)
{
	struct ns_stat new = {}, *stat;
	__u64 ns_id;

	if (ret)
		return ret;

	if (!is_monitored())
		return 0;

	ns_id = ns->ns_id;
	new.ns_type = ns->ns_type;
	bpf_map_update_elem(&ns_lifecycle, &ns_id, &new, BPF_NOEXIST);
	stat = bpf_map_lookup_elem(&ns_lifecycle, &ns_id);
	if (stat)
		__sync_fetch_and_add(&stat->init_count, 1);
	return 0;
}

SEC("lsm/namespace_install")
int BPF_PROG(ns_install, const struct nsset *nsset, struct ns_common *ns, int ret)
{
	struct ns_stat *stat;
	__u64 ns_id;

	if (ret)
		return ret;

	if (!is_monitored())
		return 0;

	ns_id = ns->ns_id;
	stat = bpf_map_lookup_elem(&ns_lifecycle, &ns_id);
	if (stat)
		__sync_fetch_and_add(&stat->install_count, 1);
	return 0;
}

SEC("lsm/namespace_free")
int BPF_PROG(ns_free, struct ns_common *ns)
{
	struct ns_stat *stat;
	__u64 ns_id;

	ns_id = ns->ns_id;
	stat = bpf_map_lookup_elem(&ns_lifecycle, &ns_id);
	if (stat)
		__sync_fetch_and_add(&stat->free_count, 1);
	return 0;
}
