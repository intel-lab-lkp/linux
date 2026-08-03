// SPDX-License-Identifier: GPL-2.0
/* Exercise re-entry into call_srcu() from BPF; see progs/rcu_reentry.c. */
#define _GNU_SOURCE
#include <sched.h>
#include <sys/syscall.h>
#include <test_progs.h>
#include "rcu_reentry.skel.h"

static int sys_pidfd_open(pid_t pid, unsigned int flags)
{
	return syscall(__NR_pidfd_open, pid, flags);
}

void test_rcu_reentry(void)
{
	struct rcu_reentry *skel;
	int err, pidfd = -1, map_fd;
	__u64 val = 1;
	cpu_set_t set;

	skel = rcu_reentry__open_and_load();
	if (!ASSERT_OK_PTR(skel, "skel_open_and_load"))
		return;

	err = rcu_reentry__attach(skel);
	if (!ASSERT_OK(err, "skel_attach"))
		goto out;

	/* Keep the re-entry on a single CPU. */
	CPU_ZERO(&set);
	CPU_SET(0, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		perror("sched_setaffinity");

	pidfd = sys_pidfd_open(getpid(), 0);
	if (!ASSERT_GE(pidfd, 0, "pidfd_open"))
		goto out;
	map_fd = bpf_map__fd(skel->maps.task_stg);
	err = bpf_map_update_elem(map_fd, &pidfd, &val, BPF_NOEXIST);
	if (!ASSERT_OK(err, "boot_create"))
		goto out;

	/* Arm the handler for this thread, then trigger call_rcu_tasks_trace(). */
	skel->bss->target_pid = syscall(__NR_gettid);
	err = bpf_map_delete_elem(map_fd, &pidfd);
	ASSERT_OK(err, "boot_delete");

	/* Only Tree SRCU enqueues via rcu_segcblist_enqueue(); skip elsewhere. */
	if (!skel->bss->hits) {
		test__skip();
		goto out;
	}
	ASSERT_EQ(skel->bss->reentered, 1, "reentry_deferred");
out:
	if (pidfd >= 0)
		close(pidfd);
	rcu_reentry__destroy(skel);
}
