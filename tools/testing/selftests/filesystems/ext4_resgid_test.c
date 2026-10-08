// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#define TEST_UID 1000
#define FILL_GID 1001
#define RESGID 456
#define TEST_BLOCK_SIZE 4096
#define MAX_BLOCKS 32768

struct worker_result {
	off_t offset;
	int error;
	int setup_error;
};

static void worker(int fd, gid_t gid, bool fill, off_t offset, int pipefd)
{
	struct worker_result result = {
		.offset = offset,
	};
	int i;

	if (setgroups(0, NULL) || setresgid(gid, gid, gid) ||
	    setresuid(TEST_UID, TEST_UID, TEST_UID)) {
		result.setup_error = errno;
		goto report;
	}

	if (fill) {
		for (i = 0; i < MAX_BLOCKS; i++) {
			if (fallocate(fd, 0, result.offset, TEST_BLOCK_SIZE)) {
				result.error = errno;
				goto report;
			}
			result.offset += TEST_BLOCK_SIZE;
		}
		result.error = EFBIG;
	} else if (fallocate(fd, 0, result.offset, TEST_BLOCK_SIZE)) {
		result.error = errno;
	}

report:
	if (write(pipefd, &result, sizeof(result)) != sizeof(result))
		_exit(1);
	_exit(0);
}

static int run_worker(int fd, gid_t gid, bool fill, off_t offset,
		      struct worker_result *result)
{
	int pipefd[2], status;
	pid_t pid, waited;
	ssize_t n;

	if (pipe(pipefd))
		return -errno;
	pid = fork();
	if (pid < 0) {
		int error = errno;

		close(pipefd[0]);
		close(pipefd[1]);
		return -error;
	}
	if (!pid) {
		close(pipefd[0]);
		worker(fd, gid, fill, offset, pipefd[1]);
	}

	close(pipefd[1]);
	do {
		n = read(pipefd[0], result, sizeof(*result));
	} while (n < 0 && errno == EINTR);
	close(pipefd[0]);
	do {
		waited = waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited != pid)
		return -errno;
	if (n != sizeof(*result) || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		return -EIO;

	return 0;
}

int main(int argc, char **argv)
{
	char mountpoint[] = "/tmp/ext4-resgid.XXXXXX";
	char filename[sizeof(mountpoint) + sizeof("/fill")];
	struct worker_result fill_result, group_result;
	bool mounted = false, skipped = false;
	int fd = -1, ret, failed = 0;
	char message[160] = "reserved GID can allocate reserved ext4 blocks";

	ksft_print_header();
	ksft_set_plan(1);

	if (geteuid()) {
		ksft_test_result_skip("must be run as root\n");
		ksft_finished();
	}
	if (argc != 2) {
		ksft_test_result_fail("usage: %s block-device\n", argv[0]);
		ksft_finished();
	}
	if (unshare(CLONE_NEWNS)) {
		if (errno == EPERM || errno == EINVAL) {
			ksft_test_result_skip("private mount namespace is unavailable\n");
			ksft_finished();
		}
		ksft_exit_fail_perror("creating mount namespace");
	}
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)) {
		if (errno == EPERM || errno == EACCES) {
			ksft_test_result_skip("cannot make mounts private\n");
			ksft_finished();
		}
		ksft_exit_fail_perror("making mounts private");
	}
	if (!mkdtemp(mountpoint))
		ksft_exit_fail_perror("creating mountpoint");
	if (mount(argv[1], mountpoint, "ext4", 0, NULL)) {
		int error = errno;

		rmdir(mountpoint);
		errno = error;
		ksft_exit_fail_perror("mounting ext4 test image");
	}
	mounted = true;

	snprintf(filename, sizeof(filename), "%s/fill", mountpoint);
	fd = open(filename, O_CREAT | O_EXCL | O_RDWR, 0666);
	if (fd < 0) {
		snprintf(message, sizeof(message), "creating test file: %s",
			 strerror(errno));
		failed = 1;
		goto out;
	}
	if (fchown(fd, TEST_UID, FILL_GID) || fchmod(fd, 0666)) {
		snprintf(message, sizeof(message), "setting test-file owner: %s",
			 strerror(errno));
		failed = 1;
		goto out;
	}

	ret = run_worker(fd, FILL_GID, true, 0, &fill_result);
	if (ret) {
		snprintf(message, sizeof(message), "filling filesystem: %s",
			 strerror(-ret));
		failed = 1;
		goto out;
	}
	if (fill_result.setup_error) {
		if (fill_result.setup_error == EPERM) {
			snprintf(message, sizeof(message),
				 "cannot drop worker privileges: %s",
				 strerror(fill_result.setup_error));
			skipped = true;
			goto out;
		}
		snprintf(message, sizeof(message), "dropping worker privileges: %s",
			 strerror(fill_result.setup_error));
		failed = 1;
		goto out;
	}
	if (fill_result.error != ENOSPC) {
		snprintf(message, sizeof(message),
			 "unprivileged fill stopped with %s, expected ENOSPC",
			 strerror(fill_result.error));
		failed = 1;
		goto out;
	}

	ret = run_worker(fd, RESGID, false, fill_result.offset, &group_result);
	if (ret) {
		snprintf(message, sizeof(message), "testing reserved GID: %s",
			 strerror(-ret));
		failed = 1;
		goto out;
	}
	if (group_result.setup_error) {
		snprintf(message, sizeof(message), "setting reserved GID: %s",
			 strerror(group_result.setup_error));
		failed = 1;
		goto out;
	}
	if (group_result.error) {
		snprintf(message, sizeof(message),
			 "reserved-GID allocation failed: %s",
			 strerror(group_result.error));
		failed = 1;
	}

out:
	if (fd >= 0 && close(fd)) {
		snprintf(message, sizeof(message), "closing test file: %s",
			 strerror(errno));
		failed = 1;
	}
	if (unlink(filename) && errno != ENOENT) {
		snprintf(message, sizeof(message), "removing test file: %s",
			 strerror(errno));
		failed = 1;
	}
	if (mounted && umount(mountpoint)) {
		snprintf(message, sizeof(message), "unmounting test image: %s",
			 strerror(errno));
		failed = 1;
	}
	if (rmdir(mountpoint)) {
		snprintf(message, sizeof(message), "removing mountpoint: %s",
			 strerror(errno));
		failed = 1;
	}

	if (failed)
		ksft_test_result_fail("%s\n", message);
	else if (skipped)
		ksft_test_result_skip("%s\n", message);
	else
		ksft_test_result_pass("%s\n", message);
	ksft_finished();
}
