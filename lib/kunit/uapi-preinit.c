// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit Userspace environment setup.
 *
 * Copyright (C) 2025, Linutronix GmbH.
 * Author: Thomas Weißschuh <thomas.weissschuh@linutronix.de>
 *
 * This is *userspace* code.
 */

#include <sys/mount.h>
#include <sys/stat.h>

#include "../../tools/testing/selftests/kselftest.h"

static int setup_api_mount(const char *target, const char *fstype)
{
	int ret;

	ret = mkdir(target, 0755);
	if (ret && errno != EEXIST)
		return -errno;

	ret = mount("none", target, fstype, 0, NULL);
	if (ret && errno != EBUSY)
		return -errno;

	return 0;
}

static void exit_failure(const char *stage, int err)
{
	/* If preinit fails synthesize a failed test report. */
	ksft_print_header();
	ksft_set_plan(1);
	ksft_test_result_fail("Failed during test setup: %s: %s\n", stage, strerror(-err));
	ksft_finished();
}

int main(int argc, char **argv, char **envp)
{
	int ret;

	ret = setup_api_mount("/proc", "proc");
	if (ret)
		exit_failure("mount /proc", ret);

	ret = setup_api_mount("/sys", "sysfs");
	if (ret)
		exit_failure("mount /sys", ret);

	if (IS_ENABLED(CONFIG_DEVTMPFS)) {
		ret = setup_api_mount("/dev", "devtmpfs");
		if (ret)
			exit_failure("mount /dev", ret);
	}

	ret = execve(argv[0], argv, envp);
	if (ret)
		exit_failure("execve", ret);

	return 0;
}
