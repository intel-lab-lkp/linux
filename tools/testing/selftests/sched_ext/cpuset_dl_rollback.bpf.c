/* SPDX-License-Identifier: GPL-2.0 */
/*
 * A sched_ext scheduler used to trigger attach rollback after cpuset has
 * already accepted the migration.
 *
 * Reject moving SCHED_DEADLINE tasks between cgroups from cgroup_prep_move(),
 * which makes the cpu controller fail after cpuset has already succeeded.
 */

#include <scx/common.bpf.h>

#define SCHED_DEADLINE 6

char _license[] SEC("license") = "GPL";

s32 BPF_STRUCT_OPS(cpuset_dl_rollback_cgroup_prep_move, struct task_struct *p,
		   struct cgroup *from, struct cgroup *to)
{
	if (p->policy == SCHED_DEADLINE)
		return -EAGAIN;

	return 0;
}
SEC(".struct_ops.link")
struct sched_ext_ops cpuset_dl_rollback_ops = {
	.cgroup_prep_move	= (void *)cpuset_dl_rollback_cgroup_prep_move,
	.name			= "cpuset_dl_rollback",
};
