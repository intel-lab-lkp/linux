// SPDX-License-Identifier: GPL-2.0-or-later

#define __SANE_USERSPACE_TYPES__
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "helpers.h"

static int open_opath(const char *path, __u64 allowed_upgrades)
{
	struct open_how how = {
		.flags = O_PATH,
		.allowed_upgrades = allowed_upgrades,
	};
	int ret = raw_openat2(AT_FDCWD, path, &how, OPEN_HOW_SIZE_VER1);

	if (ret < 0)
		ksft_exit_fail_msg("open O_PATH: %s\n", strerror(errno));

	return ret;
}

static int reopen_empty(int dfd, __u64 flags, bool fatal)
{
	struct open_how how = {
		.flags = flags | OPENAT2_EMPTY_PATH,
	};
	int ret = raw_openat2(dfd, "", &how, OPEN_HOW_SIZE_VER1);

	if (ret < 0 && fatal)
		ksft_exit_fail_msg("open with OPENAT2_EMPTY_PATH: %s\n",
				   strerror(errno));
	return ret;
}

static int reopen_empty_opath(int dfd, __u64 allowed_upgrades)
{
	struct open_how how = {
		.flags = O_PATH | OPENAT2_EMPTY_PATH,
		.allowed_upgrades = allowed_upgrades,
	};
	int ret = raw_openat2(dfd, "", &how, OPEN_HOW_SIZE_VER1);

	if (ret < 0)
		ksft_exit_fail_msg("open O_PATH with OPENAT2_EMPTY_PATH: %s\n",
				   strerror(errno));
	return ret;
}

static int reopen_proc(int dfd, int flags, bool fatal)
{
	char path[64];
	snprintf(path, sizeof(path), "/proc/self/fd/%d", dfd);
	int ret = open(path, flags);

	if (ret < 0 && fatal)
		ksft_exit_fail_msg("open via procfs: %s\n", strerror(errno));

	return ret;
}

static void check_success(const char *desc, int ret)
{
	if (ret >= 0) {
		ksft_test_result_pass("%s\n", desc);
		close(ret);
	} else {
		ksft_test_result_fail("%s: expected success, got %s\n",
				      desc, strerror(errno));
	}
}

static void check_failure(const char *desc, int ret, int expected_errno)
{
	if (ret < 0 && errno == expected_errno) {
		ksft_test_result_pass("%s\n", desc);
	} else if (ret >= 0) {
		ksft_test_result_fail("%s: expected %s, got success\n",
				      desc, strerror(expected_errno));
		close(ret);
	} else {
		ksft_test_result_fail("%s: expected %s, got %s\n",
				      desc, strerror(expected_errno),
				      strerror(errno));
	}
}

static void check(const char *desc, int ret, int expected_errno)
{
	if (!expected_errno) {
		check_success(desc, ret);
	} else {
		check_failure(desc, ret, expected_errno);
	}
}

#define NUM_TESTS 42

int main(void)
{
	const char *path = "/tmp/upgrade_mask_test";
	int fd, src;

	ksft_print_header();

	if (!openat2_supported)
		ksft_exit_skip("openat2(2) not supported\n");

	/* Check allowed_upgrades support */
	{
		struct open_how how = { .flags = O_PATH,
					.allowed_upgrades = DENY_UPGRADES };
		fd = raw_openat2(AT_FDCWD, "/", &how, sizeof(how));
		if (fd < 0 && -fd == EINVAL)
			ksft_exit_skip("allowed_upgrades not supported by kernel\n");
		if (fd >= 0)
			close(fd);
	}

	ksft_set_plan(NUM_TESTS);

	fd = open(path, O_CREAT | O_WRONLY, 0644);
	if (fd < 0)
		ksft_exit_fail_msg("failed to create test file: %s\n",
				   strerror(errno));
	close(fd);

	/* test 1: DENY_UPGRADES (deny all) */
	src = open_opath(path, DENY_UPGRADES);
	check("deny_all: use empty_path to reopen O_RDONLY", reopen_empty(src, O_RDONLY, false), EACCES);
	check("deny_all: use empty_path to reopen O_WRONLY", reopen_empty(src, O_WRONLY, false), EACCES);
	check("deny_all: use empty_path to reopen O_RDWR",   reopen_empty(src, O_RDWR,   false), EACCES);
	check("deny_all: use procfs to reopen O_RDONLY",     reopen_proc(src, O_RDONLY,  false), EACCES);
	check("deny_all: use procfs to reopen O_WRONLY",     reopen_proc(src, O_WRONLY,  false), EACCES);
	check("deny_all: use procfs to reopen O_RDWR",       reopen_proc(src, O_RDWR,    false), EACCES);
	close(src);

	/* test 2: READ_UPGRADABLE */
	src = open_opath(path, READ_UPGRADABLE);
	check("read_only: use empty_path to reopen O_RDONLY", reopen_empty(src, O_RDONLY, false), 0);
	check("read_only: use empty_path to reopen O_WRONLY", reopen_empty(src, O_WRONLY, false), EACCES);
	check("read_only: use empty_path to reopen O_RDWR",   reopen_empty(src, O_RDWR,   false), EACCES);
	check("read_only: use procfs to reopen O_RDONLY",     reopen_proc(src, O_RDONLY,  false), 0);
	check("read_only: use procfs to reopen O_WRONLY",     reopen_proc(src, O_WRONLY,  false), EACCES);
	check("read_only: use procfs to reopen O_RDWR",       reopen_proc(src, O_RDWR,    false), EACCES);
	close(src);

	/* test 3: WRITE_UPGRADABLE */
	src = open_opath(path, WRITE_UPGRADABLE);
	check("write_only: use empty_path to reopen O_RDONLY", reopen_empty(src, O_RDONLY, false), EACCES);
	check("write_only: use empty_path to reopen O_WRONLY", reopen_empty(src, O_WRONLY, false), 0);
	check("write_only: use empty_path to reopen O_RDWR",   reopen_empty(src, O_RDWR,   false), EACCES);
	check("write_only: use procfs to reopen O_RDONLY",     reopen_proc(src, O_RDONLY,  false), EACCES);
	check("write_only: use procfs to reopen O_WRONLY",     reopen_proc(src, O_WRONLY,  false), 0);
	check("write_only: use procfs to reopen O_RDWR",       reopen_proc(src, O_RDWR,    false), EACCES);
	close(src);

	/* test 4: READ_UPGRADABLE | WRITE_UPGRADABLE */
	src = open_opath(path, READ_UPGRADABLE | WRITE_UPGRADABLE);
	check("allow_all: use empty_path to reopen O_RDONLY", reopen_empty(src, O_RDONLY, false), 0);
	check("allow_all: use empty_path to reopen O_WRONLY", reopen_empty(src, O_WRONLY, false), 0);
	check("allow_all: use empty_path to reopen O_RDWR",   reopen_empty(src, O_RDWR,   false), 0);
	check("allow_all: use procfs to reopen O_RDONLY",     reopen_proc(src, O_RDONLY,  false), 0);
	check("allow_all: use procfs to reopen O_WRONLY",     reopen_proc(src, O_WRONLY,  false), 0);
	check("allow_all: use procfs to reopen O_RDWR",       reopen_proc(src, O_RDWR,    false), 0);
	close(src);

	/* test 5: VER0 open_how (allowed_upgrades absent, defaults to unrestricted) */
	{
		struct open_how how = { .flags = O_PATH };
		src = raw_openat2(AT_FDCWD, path, &how, OPEN_HOW_SIZE_VER0);

		check("ver0: use empty_path to reopen O_RDONLY", reopen_empty(src, O_RDONLY, false), 0);
		check("ver0: use empty_path to reopen O_WRONLY", reopen_empty(src, O_WRONLY, false), 0);
		check("ver0: use empty_path to reopen O_RDWR",   reopen_empty(src, O_RDWR,   false), 0);
		check("ver0: use procfs to reopen O_RDONLY",     reopen_proc(src, O_RDONLY,  false), 0);
		check("ver0: use procfs to reopen O_WRONLY",     reopen_proc(src, O_WRONLY,  false), 0);
		check("ver0: use procfs to reopen O_RDWR",       reopen_proc(src, O_RDWR,    false), 0);
		close(src);
	}

	/* test 6: invalid allowed_upgrades bit rejected */
	{
		struct open_how how = { .flags = O_PATH, .allowed_upgrades = (1ULL << 63) };
		fd = raw_openat2(AT_FDCWD, path, &how, sizeof(how));
		check("invalid: unknown bit in allowed_upgrades rejected with EINVAL", fd, EINVAL);
	}

	/* test 7: transitivity through OPENAT2_EMPTY_PATH reopen */
	src = open_opath(path, READ_UPGRADABLE);
	{
		int mid = reopen_empty(src, O_RDONLY, true);
		close(src);
		check("transitive_empty: use procfs to reopen O_RDONLY", reopen_proc(mid, O_RDONLY, false), 0);
		check("transitive_empty: use procfs to reopen O_WRONLY", reopen_proc(mid, O_WRONLY, false), EACCES);
		check("transitive_empty: use procfs to reopen O_RDWR",   reopen_proc(mid, O_RDWR,   false), EACCES);
		close(mid);
	}

	/* test 8: transitivity through procfs reopen */
	src = open_opath(path, READ_UPGRADABLE);
	{
		int mid = reopen_proc(src, O_RDONLY, true);
		close(src);
		check("transitive_proc: use procfs to reopen O_RDONLY", reopen_empty(mid, O_RDONLY, false), 0);
		check("transitive_proc: use procfs to reopen O_WRONLY", reopen_empty(mid, O_WRONLY, false), EACCES);
		check("transitive_proc: use procfs to reopen O_RDWR",   reopen_empty(mid, O_RDWR,   false), EACCES);
		close(mid);
	}

	/* test 9: narrowing via intermediate O_PATH reopen */
	src = open_opath(path, READ_UPGRADABLE | WRITE_UPGRADABLE);
	{
		int mid = reopen_empty_opath(src, READ_UPGRADABLE);
		close(src);
		check("narrowing: use procfs to reopen O_RDONLY", reopen_proc(mid, O_RDONLY, false), 0);
		check("narrowing: use procfs to reopen O_WRONLY", reopen_proc(mid, O_WRONLY, false), EACCES);
		check("narrowing: use procfs to reopen O_RDWR",   reopen_proc(mid, O_RDWR,   false), EACCES);
		close(mid);
	}

	/* test 10: three-level chain */
	src = open_opath(path, READ_UPGRADABLE);
	{
		int mid = reopen_proc(src, O_RDONLY, true);
		close(src);
		int dst = reopen_proc(mid, O_RDONLY, true);
		close(mid);
		check("three_level: use procfs to reopen O_RDONLY", reopen_empty(dst, O_RDONLY, false), 0);
		check("three_level: use procfs to reopen O_WRONLY", reopen_empty(dst, O_WRONLY, false), EACCES);
		close(dst);
	}

	unlink(path);

	if (ksft_get_fail_cnt() + ksft_get_error_cnt() > 0)
		ksft_exit_fail();
	else
		ksft_exit_pass();
}
