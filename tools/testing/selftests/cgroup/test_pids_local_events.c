// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cgroup_util.h"
#include "kselftest.h"

static int test_pids_events_notify(const char *root)
{
	union {
		struct inotify_event align;
		char bytes[sizeof(struct inotify_event) + NAME_MAX + 1];
	} event_buf;
	struct pollfd pfd;
	char *cgroup = NULL, *events_path = NULL;
	const char *reason = "pids.events did not notify after a rejected fork";
	bool moved = false;
	int inotify_fd = -1, ret = KSFT_FAIL, watch;
	ssize_t n;
	pid_t child;

	cgroup = cg_name(root, "pids_local_events_notify");
	if (!cgroup) {
		reason = "failed to allocate cgroup path";
		goto cleanup;
	}
	if (cg_create(cgroup)) {
		reason = "failed to create test cgroup";
		goto cleanup;
	}
	if (cg_write(cgroup, "pids.max", "1")) {
		reason = "failed to set pids.max";
		goto cleanup;
	}

	if (asprintf(&events_path, "%s/pids.events", cgroup) < 0) {
		reason = "failed to allocate pids.events path";
		goto cleanup;
	}
	inotify_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if (inotify_fd < 0) {
		reason = "inotify_init1 failed";
		goto cleanup;
	}
	watch = inotify_add_watch(inotify_fd, events_path, IN_MODIFY);
	if (watch < 0) {
		reason = "inotify_add_watch on pids.events failed";
		goto cleanup;
	}

	if (cg_enter_current(cgroup)) {
		reason = "failed to move test into cgroup";
		goto cleanup;
	}
	moved = true;

	child = fork();
	if (child == 0)
		_exit(0);
	if (child > 0) {
		waitpid(child, NULL, 0);
		reason = "fork succeeded despite pids.max";
		goto cleanup;
	}
	if (errno != EAGAIN) {
		reason = "fork failed for a reason other than pids.max";
		goto cleanup;
	}

	if (cg_read_key_long(cgroup, "pids.events", "max ") != 1) {
		reason = "pids.events did not count the rejected fork";
		goto cleanup;
	}

	pfd.fd = inotify_fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 1000) <= 0) {
		reason = "timed out waiting for pids.events notification";
		goto cleanup;
	}
	n = read(inotify_fd, event_buf.bytes, sizeof(event_buf.bytes));
	if (n < sizeof(struct inotify_event) ||
	    !(event_buf.align.mask & IN_MODIFY)) {
		reason = "received no pids.events modify notification";
		goto cleanup;
	}
	if (cg_read_key_long(cgroup, "pids.events", "max ") != 1) {
		reason = "pids.events did not count the rejected fork";
		goto cleanup;
	}

	ret = KSFT_PASS;

cleanup:
	if (inotify_fd >= 0)
		close(inotify_fd);
	if (moved)
		cg_enter_current(root);
	if (cgroup)
		cg_destroy(cgroup);
	free(events_path);
	free(cgroup);
	if (ret != KSFT_PASS)
		ksft_print_msg("%s\n", reason);
	return ret;
}

int main(void)
{
	char root[PATH_MAX];
	int ret;

	ksft_print_header();
	if (cg_find_unified_root(root, sizeof(root), NULL))
		ksft_exit_skip("cgroup v2 isn't mounted\n");
	if (proc_mount_contains("pids_localevents") <= 0)
		ksft_exit_skip("mount cgroup2 with pids_localevents to run this test\n");
	if (cg_read_strstr(root, "cgroup.controllers", "pids"))
		ksft_exit_skip("pids controller isn't available\n");
	if (cg_read_strstr(root, "cgroup.subtree_control", "pids") &&
	    cg_write(root, "cgroup.subtree_control", "+pids"))
		ksft_exit_skip("failed to enable pids controller\n");

	ksft_set_plan(1);
	ret = test_pids_events_notify(root);
	ksft_test_result_report(ret, "pids.events notifies on a rejected fork\n");
	ksft_finished();
}
