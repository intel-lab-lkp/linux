// SPDX-License-Identifier: GPL-2.0

/*
 * Exercise all namespace LSM hooks from a BPF LSM: unshare() new
 * namespaces (namespace_init), setns() into them (namespace_install) and
 * drop the last reference (namespace_free).
 */

#define _GNU_SOURCE
#include <test_progs.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/nsfs.h>
#include "task_local_storage_helpers.h"
#include "test_ns_lifecycle.skel.h"

struct ns_stat {
	__u32 ns_type;
	__u32 init_count;
	__u32 install_count;
	__u32 free_count;
};

static const struct {
	const char *path;
	int type;
} namespaces[] = {
	{ "/proc/self/ns/uts", CLONE_NEWUTS },
	{ "/proc/self/ns/ipc", CLONE_NEWIPC },
	{ "/proc/self/ns/net", CLONE_NEWNET },
};

#define NR_NS ARRAY_SIZE(namespaces)

static bool all_freed(int map_fd, const __u64 *ns_id)
{
	struct ns_stat stat;
	int i;

	for (i = 0; i < NR_NS; i++) {
		if (bpf_map_lookup_elem(map_fd, &ns_id[i], &stat) || !stat.free_count)
			return false;
	}
	return true;
}

void test_ns_lifecycle(void)
{
	int orig_fd[NR_NS] = { -1, -1, -1 }, new_fd[NR_NS] = { -1, -1, -1 };
	struct test_ns_lifecycle *skel = NULL;
	int pidfd, map_fd, err, i, tries;
	struct ns_stat stat;
	__u64 ns_id[NR_NS];
	struct stat st;

	for (i = 0; i < NR_NS; i++) {
		orig_fd[i] = open(namespaces[i].path, O_RDONLY | O_CLOEXEC);
		if (!ASSERT_OK_FD(orig_fd[i], "open orig ns"))
			goto close_fds;
	}

	pidfd = sys_pidfd_open(getpid(), 0);
	if (!ASSERT_OK_FD(pidfd, "pidfd_open"))
		goto close_fds;
	err = fstat(pidfd, &st);
	close(pidfd);
	if (!ASSERT_OK(err, "fstat pidfd"))
		goto close_fds;

	skel = test_ns_lifecycle__open_and_load();
	if (!ASSERT_OK_PTR(skel, "skel open_and_load"))
		goto close_fds;

	err = test_ns_lifecycle__attach(skel);
	if (!ASSERT_OK(err, "skel attach"))
		goto destroy;

	map_fd = bpf_map__fd(skel->maps.ns_lifecycle);
	skel->bss->monitored_pidfd_ino = st.st_ino;

	err = unshare(CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET);
	if (!ASSERT_OK(err, "unshare"))
		goto disarm;

	for (i = 0; i < NR_NS; i++) {
		new_fd[i] = open(namespaces[i].path, O_RDONLY | O_CLOEXEC);
		if (!ASSERT_OK_FD(new_fd[i], "open new ns"))
			goto restore;
		if (!ASSERT_OK(ioctl(new_fd[i], NS_GET_ID, &ns_id[i]), "NS_GET_ID"))
			goto restore;
	}

	/* Leave the new namespaces and install them again. */
	for (i = 0; i < NR_NS; i++) {
		if (!ASSERT_OK(setns(orig_fd[i], namespaces[i].type), "setns orig"))
			goto restore;
		if (!ASSERT_OK(setns(new_fd[i], namespaces[i].type), "setns new"))
			goto restore;
		if (!ASSERT_OK(setns(orig_fd[i], namespaces[i].type), "setns orig again"))
			goto restore;
	}

	skel->bss->monitored_pidfd_ino = 0;

	/* Drop the last references. Network namespaces are freed asynchronously. */
	for (i = 0; i < NR_NS; i++) {
		close(new_fd[i]);
		new_fd[i] = -1;
	}

	for (tries = 0; tries < 1000 && !all_freed(map_fd, ns_id); tries++) {
		kern_sync_rcu();
		usleep(10000);
	}

	for (i = 0; i < NR_NS; i++) {
		/* namespace_init must already have seen the id. */
		if (!ASSERT_OK(bpf_map_lookup_elem(map_fd, &ns_id[i], &stat), "lookup ns_stat"))
			continue;

		ASSERT_EQ(stat.ns_type, namespaces[i].type, "ns_type");
		ASSERT_EQ(stat.init_count, 1, "init_count");
		ASSERT_EQ(stat.install_count, 1, "install_count");
		ASSERT_EQ(stat.free_count, 1, "free_count");
	}
	goto destroy;

restore:
	for (i = 0; i < NR_NS; i++)
		setns(orig_fd[i], namespaces[i].type);
disarm:
	skel->bss->monitored_pidfd_ino = 0;
destroy:
	test_ns_lifecycle__destroy(skel);
close_fds:
	for (i = 0; i < NR_NS; i++) {
		if (new_fd[i] >= 0)
			close(new_fd[i]);
		if (orig_fd[i] >= 0)
			close(orig_fd[i]);
	}
}
