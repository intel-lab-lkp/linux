// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <linux/capability.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_UID 1000
#define POLICY_GID 2000
#define ALLOWED_GID 2001
#define FORBIDDEN_GID 2002

#define GID_POLICY_FILE \
	"/sys/kernel/security/safesetid/gid_allowlist_policy"
#define GID_POLICY "2000:2001\n"

enum test_operation {
	OP_SETRESGID,
	OP_SETGROUPS,
};

static int install_policy(void)
{
	FILE *policy;
	int ret = 0;

	policy = fopen(GID_POLICY_FILE, "w");
	if (!policy) {
		perror("fopen gid policy");
		return -1;
	}
	if (fputs(GID_POLICY, policy) == EOF) {
		perror("write gid policy");
		ret = -1;
	}
	if (fclose(policy)) {
		perror("close gid policy");
		ret = -1;
	}
	return ret;
}

static int retain_only_setgid(void)
{
	cap_value_t cap = CAP_SETGID;
	cap_t caps;
	int ret = -1;

	caps = cap_get_proc();
	if (!caps) {
		perror("cap_get_proc");
		return -1;
	}
	if (cap_clear(caps) ||
	    cap_set_flag(caps, CAP_PERMITTED, 1, &cap, CAP_SET) ||
	    cap_set_flag(caps, CAP_EFFECTIVE, 1, &cap, CAP_SET) ||
	    cap_set_proc(caps)) {
		perror("retain CAP_SETGID");
		goto out;
	}
	ret = 0;
out:
	cap_free(caps);
	return ret;
}

static int check_child_credentials(void)
{
	struct __user_cap_header_struct header = {
		.version = _LINUX_CAPABILITY_VERSION_3,
	};
	struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3] = {};
	uint64_t effective, permitted, inheritable;
	uid_t ruid, euid, suid;
	gid_t rgid, egid, sgid;
	gid_t groups[2];
	uint64_t expected = 1ULL << CAP_SETGID;
	int i, ngroups;

	if (getresuid(&ruid, &euid, &suid) ||
	    getresgid(&rgid, &egid, &sgid)) {
		perror("getresuid/getresgid");
		return -1;
	}
	if (ruid != TEST_UID || euid != TEST_UID || suid != TEST_UID ||
	    rgid != POLICY_GID || egid != POLICY_GID || sgid != POLICY_GID) {
		fprintf(stderr, "unexpected child IDs\n");
		return -1;
	}

	ngroups = getgroups(2, groups);
	if (ngroups != 1 || groups[0] != POLICY_GID) {
		fprintf(stderr, "unexpected initial supplementary groups\n");
		return -1;
	}
	if (syscall(SYS_capget, &header, data)) {
		perror("capget");
		return -1;
	}
	effective = data[0].effective | ((uint64_t)data[1].effective << 32);
	permitted = data[0].permitted | ((uint64_t)data[1].permitted << 32);
	inheritable = data[0].inheritable | ((uint64_t)data[1].inheritable << 32);
	if (effective != expected || permitted != expected || inheritable) {
		fprintf(stderr, "child does not hold only CAP_SETGID\n");
		return -1;
	}
	for (i = 0; i <= CAP_LAST_CAP; i++) {
		if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, i, 0, 0) != 0) {
			fprintf(stderr, "unexpected ambient capability %d\n", i);
			return -1;
		}
	}
	return 0;
}

static int prepare_child(void)
{
	gid_t groups[] = { POLICY_GID };

	if (setgroups(1, groups)) {
		perror("initial setgroups");
		return -1;
	}
	if (setresgid(POLICY_GID, POLICY_GID, POLICY_GID)) {
		perror("setresgid policy source");
		return -1;
	}
	if (prctl(PR_SET_KEEPCAPS, 1L)) {
		perror("PR_SET_KEEPCAPS");
		return -1;
	}
	if (setresuid(TEST_UID, TEST_UID, TEST_UID)) {
		perror("setresuid test user");
		return -1;
	}
	if (retain_only_setgid())
		return -1;
	return check_child_credentials();
}

static void run_child(enum test_operation operation, gid_t target)
{
	gid_t groups[] = { target };
	int ret;

	if (prepare_child())
		_exit(2);

	if (operation == OP_SETRESGID)
		ret = setresgid(target, target, target);
	else
		ret = setgroups(1, groups);

	if (ret) {
		fprintf(stderr, "%s(%u) returned %s\n",
			operation == OP_SETRESGID ? "setresgid" : "setgroups",
			target, strerror(errno));
		_exit(3);
	}
	_exit(0);
}

static int run_case(const char *name, enum test_operation operation,
		    gid_t target, bool expect_kill)
{
	struct pollfd pollfd = {};
	int status;
	int pidfd;
	pid_t child;

	child = fork();
	if (child < 0) {
		perror("fork");
		return -1;
	}
	if (!child)
		run_child(operation, target);

	pidfd = syscall(SYS_pidfd_open, child, 0);
	if (pidfd < 0) {
		perror("pidfd_open");
		kill(child, SIGKILL);
		waitpid(child, &status, 0);
		return -1;
	}
	pollfd.fd = pidfd;
	pollfd.events = POLLIN;
	if (poll(&pollfd, 1, 5000) != 1) {
		kill(child, SIGKILL);
		waitpid(child, &status, 0);
		close(pidfd);
		fprintf(stderr, "not ok - %s (timeout)\n", name);
		return -1;
	}
	close(pidfd);
	if (waitpid(child, &status, 0) < 0) {
		perror("waitpid");
		return -1;
	}

	if (expect_kill) {
		if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) {
			printf("ok - %s\n", name);
			return 0;
		}
		fprintf(stderr, "not ok - %s (forbidden transition survived)\n",
			name);
		return -1;
	}

	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		printf("ok - %s\n", name);
		return 0;
	}
	fprintf(stderr, "not ok - %s (allowed transition failed)\n", name);
	return -1;
}

int main(void)
{
	int failures = 0;

	if (getuid()) {
		fprintf(stderr, "must be run as root\n");
		return 4;
	}
	if (install_policy())
		return 1;

	failures += run_case("allowed GID transition", OP_SETRESGID,
			     ALLOWED_GID, false) != 0;
	failures += run_case("forbidden GID transition", OP_SETRESGID,
			     FORBIDDEN_GID, true) != 0;
	failures += run_case("allowed supplementary group", OP_SETGROUPS,
			     ALLOWED_GID, false) != 0;
	failures += run_case("forbidden supplementary group", OP_SETGROUPS,
			     FORBIDDEN_GID, true) != 0;

	return failures ? 1 : 0;
}
