// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

#include "../kselftest.h"
#include "helpers.h"

#ifndef O_CRED_ALLOW
#define O_CRED_ALLOW 0x2000000
#endif

#ifndef OA2_CRED_INHERIT
#define OA2_CRED_INHERIT (1UL << 28)
#endif

enum { FD_NORM, FD_NCA, FD_DIR, FD_DCA, FD_MAX };

int main(int argc, char *argv[], char *env[])
{
	struct open_how how1 = {
				.flags = O_RDONLY,
				.resolve = RESOLVE_BENEATH,
			       };
	struct open_how how2 = {
				.flags = O_RDONLY | OA2_CRED_INHERIT,
				.resolve = RESOLVE_BENEATH,
			       };
	int size = sizeof(struct open_how);
	int i;
	int fd;
	int err;
	int fds[FD_MAX];
#define NFD(n) ((n) + 3)
#define FD_OK(n) (NFD(n) == fds[n])

	if (!openat2_supported) {
		ksft_print_msg("openat2(2) unsupported\n");
		return 0;
	}

	ksft_set_plan(14);

	fds[FD_NORM] = open("/proc/self/maps", O_RDONLY);
	fds[FD_NCA] = open("/proc/self/maps", O_RDONLY | O_CRED_ALLOW);
	fds[FD_DIR] = open("/proc/self", O_RDONLY | O_DIRECTORY);
	fds[FD_DCA] = open("/proc/self", O_RDONLY | O_DIRECTORY | O_CRED_ALLOW);
	ksft_test_result(FD_OK(FD_NORM), "file open\n");
	ksft_test_result(FD_OK(FD_NCA), "file open with O_CRED_ALLOW\n");
	ksft_test_result(FD_OK(FD_DIR), "directory open\n");
	ksft_test_result(FD_OK(FD_DCA), "directory open with O_CRED_ALLOW\n");

	err = fchdir(fds[FD_DIR]);
	if (err) {
		ksft_perror("fchdir() failed");
		ksft_exit_fail_msg("fchdir\n");
		return 1;
	}
	fd = syscall(SYS_openat2, AT_FDCWD, "maps", &how1, size);
	ksft_test_result(fd != -1, "AT_FDCWD success\n");
	close(fd);
	/* OA2_CRED_INHERIT fails with AT_FDCWD */
	fd = syscall(SYS_openat2, AT_FDCWD, "maps", &how2, size);
	ksft_test_result(fd == -1 && errno == EINVAL, "AT_FDCWD EINVAL\n");

	fd = syscall(SYS_openat2, fds[FD_NORM], "maps", &how1, size);
	ksft_test_result(fd == -1 && errno == ENOTDIR, "regilar file ENOTDIR\n");
	/* No O_CRED_ALLOW -> EPERM */
	fd = syscall(SYS_openat2, fds[FD_NORM], "maps", &how2, size);
	ksft_test_result(fd == -1 && errno == EPERM, "regilar file EPERM\n");

	fd = syscall(SYS_openat2, fds[FD_NCA], "maps", &how1, size);
	ksft_test_result(fd == -1 && errno == ENOTDIR, "regilar file ENOTDIR\n");
	fd = syscall(SYS_openat2, fds[FD_NCA], "maps", &how2, size);
	ksft_test_result(fd == -1 && errno == ENOTDIR, "regilar file ENOTDIR\n");

	fd = syscall(SYS_openat2, fds[FD_DIR], "maps", &how1, size);
	ksft_test_result(fd != -1, "dir fd success\n");
	close(fd);
	/* No O_CRED_ALLOW -> EPERM */
	fd = syscall(SYS_openat2, fds[FD_DIR], "maps", &how2, size);
	ksft_test_result(fd == -1 && errno == EPERM, "dir fd EPERM\n");

	fd = syscall(SYS_openat2, fds[FD_DCA], "maps", &how1, size);
	ksft_test_result(fd != -1, "dir O_CRED_ALLOW fd success\n");
	close(fd);
	fd = syscall(SYS_openat2, fds[FD_DCA], "maps", &how2, size);
	ksft_test_result(fd != -1, "dir O_CRED_ALLOW fd O_CRED_INHERIT success\n");
	close(fd);

	for (i = 0; i < FD_MAX; i++)
		close(fds[i]);
	ksft_finished();
	return 0;
}
