// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#define __SANE_USERSPACE_TYPES__ // Use ll64

#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>

#include "../../kselftest_harness.h"
#include "log.h"
#include "wrappers.h"

FIXTURE(set_layers_via_fds) {
};

FIXTURE_SETUP(set_layers_via_fds)
{
	ASSERT_EQ(mkdir("/set_layers_via_fds", 0755), 0);
}

FIXTURE_TEARDOWN(set_layers_via_fds)
{
	umount2("/set_layers_via_fds", 0);
	ASSERT_EQ(rmdir("/set_layers_via_fds"), 0);
}

TEST_F(set_layers_via_fds, set_layers_via_fds)
{
	int fd_context, fd_tmpfs, fd_overlay;
	int layer_fds[5] = { -EBADF, -EBADF, -EBADF, -EBADF, -EBADF };
	bool layers_found[5] = { false, false, false, false, false };
	size_t len = 0;
	char *line = NULL;
	FILE *f_mountinfo;

	ASSERT_EQ(unshare(CLONE_NEWNS), 0);
	ASSERT_EQ(sys_mount(NULL, "/", NULL, MS_SLAVE | MS_REC, NULL), 0);

	fd_context = sys_fsopen("tmpfs", 0);
	ASSERT_GE(fd_context, 0);

	ASSERT_EQ(sys_fsconfig(fd_context, FSCONFIG_CMD_CREATE, NULL, NULL, 0), 0);
	fd_tmpfs = sys_fsmount(fd_context, 0, 0);
	ASSERT_GE(fd_tmpfs, 0);
	ASSERT_EQ(close(fd_context), 0);

	ASSERT_EQ(mkdirat(fd_tmpfs, "w", 0755), 0);
	ASSERT_EQ(mkdirat(fd_tmpfs, "u", 0755), 0);
	ASSERT_EQ(mkdirat(fd_tmpfs, "l1", 0755), 0);
	ASSERT_EQ(mkdirat(fd_tmpfs, "l2", 0755), 0);
	ASSERT_EQ(mkdirat(fd_tmpfs, "l3", 0755), 0);

	layer_fds[0] = openat(fd_tmpfs, "w", O_DIRECTORY);
	ASSERT_GE(layer_fds[0], 0);

	layer_fds[1] = openat(fd_tmpfs, "u", O_DIRECTORY);
	ASSERT_GE(layer_fds[1], 0);

	layer_fds[2] = openat(fd_tmpfs, "l1", O_DIRECTORY);
	ASSERT_GE(layer_fds[2], 0);

	layer_fds[3] = openat(fd_tmpfs, "l2", O_DIRECTORY);
	ASSERT_GE(layer_fds[3], 0);

	layer_fds[4] = openat(fd_tmpfs, "l3", O_DIRECTORY);
	ASSERT_GE(layer_fds[4], 0);

	ASSERT_EQ(sys_move_mount(fd_tmpfs, "", -EBADF, "/tmp", MOVE_MOUNT_F_EMPTY_PATH), 0);
	ASSERT_EQ(close(fd_tmpfs), 0);

	fd_context = sys_fsopen("overlay", 0);
	ASSERT_GE(fd_context, 0);

	ASSERT_NE(sys_fsconfig(fd_context, FSCONFIG_SET_FD, "lowerdir_fd", NULL, layer_fds[2]), 0);

	ASSERT_EQ(sys_fsconfig(fd_context, FSCONFIG_SET_FD, "workdir_fd", NULL, layer_fds[0]), 0);
	ASSERT_EQ(sys_fsconfig(fd_context, FSCONFIG_SET_FD, "upperdir_fd", NULL, layer_fds[1]), 0);
	ASSERT_EQ(sys_fsconfig(fd_context, FSCONFIG_SET_FD, "lowerdir_fd+", NULL, layer_fds[2]), 0);
	ASSERT_EQ(sys_fsconfig(fd_context, FSCONFIG_SET_FD, "lowerdir_fd+", NULL, layer_fds[3]), 0);
	ASSERT_EQ(sys_fsconfig(fd_context, FSCONFIG_SET_FD, "lowerdir_fd+", NULL, layer_fds[4]), 0);

	ASSERT_EQ(sys_fsconfig(fd_context, FSCONFIG_CMD_CREATE, NULL, NULL, 0), 0);

	fd_overlay = sys_fsmount(fd_context, 0, 0);
	ASSERT_GE(fd_overlay, 0);

	ASSERT_EQ(sys_move_mount(fd_overlay, "", -EBADF, "/set_layers_via_fds", MOVE_MOUNT_F_EMPTY_PATH), 0);

	f_mountinfo = fopen("/proc/self/mountinfo", "r");
	ASSERT_NE(f_mountinfo, NULL);

	while (getline(&line, &len, f_mountinfo) != -1) {
		char *haystack = line;

		if (strstr(haystack, "workdir=/tmp/w"))
			layers_found[0] = true;
		if (strstr(haystack, "upperdir=/tmp/u"))
			layers_found[1] = true;
		if (strstr(haystack, "lowerdir+=/tmp/l1"))
			layers_found[2] = true;
		if (strstr(haystack, "lowerdir+=/tmp/l2"))
			layers_found[3] = true;
		if (strstr(haystack, "lowerdir+=/tmp/l3"))
			layers_found[4] = true;
	}
	free(line);

	for (int i = 0; i < 5; i++) {
		ASSERT_EQ(layers_found[i], true);
		ASSERT_EQ(close(layer_fds[i]), 0);
	}

	ASSERT_EQ(close(fd_context), 0);
	ASSERT_EQ(close(fd_overlay), 0);
	ASSERT_EQ(fclose(f_mountinfo), 0);
}

TEST_HARNESS_MAIN
