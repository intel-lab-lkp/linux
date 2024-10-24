/*
 * Copyright (c) 2019 Alexey Dobriyan <adobriyan@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
/* Test that open(O_TMPFILE), linkat() doesn't screw accounting. */
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <unistd.h>

#include "../kselftest.h"

static int is_unshare(int flag)
{
	if (unshare(flag) == -1) {
		if (errno == ENOSYS || errno == EPERM) {
			ksft_test_result_fail("error: unshare, errno %d\n", errno);
			return -1; // Return -1 for failure
		}
		fprintf(stderr, "error: unshare, errno %d\n", errno);
		return -1;
	}

	return 0; // Return 0 for success
}

int main(void)
{
	int fd;

	// Setting up kselftest framework
	ksft_print_header();
	ksft_set_plan(1);

	// Check if test is run as root
	if (geteuid()) {
		ksft_test_result_skip("This test needs root to run!\n");
		return 1;
	}

	if (is_unshare(CLONE_NEWNS) == 0) {
		ksft_test_result_pass("unshare(): we have a new mount namespace.\n");
	} else {
		ksft_test_result_fail("unshare(): failed\n");
		return 1;
	}

	ksft_set_plan(2);

	if (mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) == -1) {
		ksft_test_result_fail("mount(): Root filesystem private mount: Fail %d\n", errno);
		return 1;
	} else {
		ksft_test_result_pass("mount(): Root filesystem private mount: Success\n");
	}

	ksft_set_plan(3);
	/* Our heroes: 1 root inode, 1 O_TMPFILE inode, 1 permanent inode. */
	if (mount(NULL, "/tmp", "tmpfs", 0, "nr_inodes=3") == -1) {
		ksft_test_result_fail("mount(): Mounting tmpfs on /tmp: Fail %d\n", errno);
		return 1;
	} else {
		ksft_test_result_pass("mount(): Mounting tmpfs on /tmp: Success\n");
	}

	ksft_set_plan(4);
	fd = openat(AT_FDCWD, "/tmp", O_WRONLY | O_TMPFILE, 0600);
	if (fd == -1) {
		ksft_test_result_fail("openat(): Open first temporary file: Fail %d\n", errno);
		return 1;
	} else {
		ksft_test_result_pass("openat(): Open first temporary file: Success\n");
	}

	ksft_set_plan(5);
	if (linkat(fd, "", AT_FDCWD, "/tmp/1", AT_EMPTY_PATH) == -1) {
		ksft_test_result_fail("linkat(): Linking the temporary file: Fail %d\n", errno);
		close(fd); // Ensure fd is closed on failure
		return 1;
	} else {
		ksft_test_result_pass("linkat(): Linking the temporary file: Success\n");
	}
	close(fd);

	ksft_set_plan(6);
	fd = openat(AT_FDCWD, "/tmp", O_WRONLY | O_TMPFILE, 0600);
	if (fd == -1) {
		ksft_test_result_fail("openat(): Opening the second temporary file: Fail %d\n", errno);
		return 1;
	} else {
		ksft_test_result_pass("openat(): Opening the second temporary file: Success\n");
	}

	ksft_exit_pass();
	return 0;
}
