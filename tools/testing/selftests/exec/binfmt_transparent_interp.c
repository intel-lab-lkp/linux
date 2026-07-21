// SPDX-License-Identifier: GPL-2.0
/*
 * Asserting interpreter for the transparent binfmt_misc mode. It runs in
 * place of the dispatched binary and verifies the identity the kernel
 * constructed: the aux vector contract, the exe link, argv, cmdline, comm
 * and the write denial on the binary. BINFMT_TEST_BINARY names the binary;
 * the harness execs it with the arguments "argone argtwo". Prints
 * TRANSPARENT_OK and exits 0 when every check holds.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef AT_FLAGS_TRANSPARENT_INTERP
#define AT_FLAGS_TRANSPARENT_INTERP (1 << 1)
#endif

#define TASK_COMM_LEN 16

static int fail;

static void ok(int cond, const char *what)
{
	if (!cond) {
		fprintf(stderr, "TRANSPARENT_FAIL: %s (errno %d)\n", what, errno);
		fail = 1;
	}
}

int main(int argc, char **argv)
{
	const char *binary = getenv("BINFMT_TEST_BINARY");
	const char *argv0 = getenv("BINFMT_TEST_ARGV0");
	char exe[PATH_MAX], real[PATH_MAX], buf[PATH_MAX];
	char expect[PATH_MAX + 32];
	unsigned long execfd;
	struct stat stb, stfd;
	const char *want[3];
	const char *base;
	size_t expect_len, i;
	ssize_t n;
	int fd;

	if (!binary) {
		fprintf(stderr, "TRANSPARENT_FAIL: BINFMT_TEST_BINARY unset\n");
		return 1;
	}
	/* Distinct from the binary path, so a classic argv splice is caught. */
	want[0] = argv0 ? argv0 : binary;
	want[1] = "argone";
	want[2] = "argtwo";

	/* The aux vector announces the transparent contract. */
	ok(getauxval(AT_FLAGS) & AT_FLAGS_TRANSPARENT_INTERP,
	   "AT_FLAGS lacks AT_FLAGS_TRANSPARENT_INTERP");

	/* AT_EXECFD refers to the very file that was executed. */
	execfd = getauxval(AT_EXECFD);
	ok(execfd > 2, "no AT_EXECFD");
	ok(!stat(binary, &stb), "cannot stat the binary");
	ok(!fstat((int)execfd, &stfd), "cannot fstat AT_EXECFD");
	ok(stb.st_dev == stfd.st_dev && stb.st_ino == stfd.st_ino,
	   "AT_EXECFD is not the binary");

	/* The exe link names the binary, not this interpreter. */
	n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	ok(n > 0, "cannot read /proc/self/exe");
	if (n > 0)
		exe[n] = '\0';
	ok(n > 0 && realpath(binary, real) && !strcmp(exe, real),
	   "/proc/self/exe is not the binary");

	/* argv arrived unspliced. */
	ok(argc == (int)ARRAY_SIZE(want), "argv was rewritten");
	for (i = 0; i < ARRAY_SIZE(want) && i < (size_t)argc; i++)
		ok(!strcmp(argv[i], want[i]), "argv was rewritten");

	/* And so did the kernel's copy of it: the same strings, NUL separated. */
	for (i = 0, expect_len = 0; i < ARRAY_SIZE(want); i++) {
		size_t len = strlen(want[i]) + 1;

		memcpy(expect + expect_len, want[i], len);
		expect_len += len;
	}
	fd = open("/proc/self/cmdline", O_RDONLY);
	n = fd >= 0 ? read(fd, buf, sizeof(buf)) : -1;
	if (fd >= 0)
		close(fd);
	ok(n == (ssize_t)expect_len && !memcmp(buf, expect, expect_len),
	   "/proc/self/cmdline was rewritten");

	/* comm is the binary's basename. */
	base = strrchr(binary, '/');
	base = base ? base + 1 : binary;
	snprintf(expect, TASK_COMM_LEN, "%s", base);
	fd = open("/proc/self/comm", O_RDONLY);
	n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
	if (fd >= 0)
		close(fd);
	if (n > 0 && buf[n - 1] == '\n')
		n--;
	if (n >= 0)
		buf[n] = '\0';
	ok(n > 0 && !strcmp(buf, expect), "comm is not the binary's basename");

	/* The binary is write-denied while it runs, like a direct exec. */
	errno = 0;
	fd = open(binary, O_WRONLY);
	ok(fd < 0 && errno == ETXTBSY, "binary is writable while running");
	if (fd >= 0)
		close(fd);
	errno = 0;
	fd = open("/proc/self/exe", O_WRONLY);
	ok(fd < 0 && errno == ETXTBSY, "exe link is writable while running");
	if (fd >= 0)
		close(fd);

	if (!fail)
		printf("TRANSPARENT_OK\n");
	return fail;
}
