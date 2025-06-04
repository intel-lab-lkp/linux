// SPDX-License-Identifier: GPL-2.0
/*
 * Landlock tests - Pathwalk race conditions
 *
 * Copyright © 2025 Tingmao Wang <m@maowtm.org>
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <linux/fs.h>
#include <linux/mount.h>

#include "common.h"

#define NUM_SUBDIRS 1000
#define TEST_DIR TMP_DIR "/fs_race_test"
#define SUBDIR_NAME_FORMAT "s%dd1"
#define SUBDIR2_NAME_FORMAT "s%dd2"
#define SUBDIR3_NAME "d3"
#define TEST_FILE_NAME "file"
#define TEST_TIME 30
#define RANDOM_DELAY_AFTER_MOVE false

/* layout hierarchy:
 * tmp
 * └── fs_race_test
 *     ├── s0d1
 *     │   └── s0d2
 *     │       └── d3
 *     │           └── file
 *     |── s1d1
 *     │   └── s1d2
 *     └── ...
 */

FIXTURE(layout)
{
	int base_dir_fd;
	bool need_subdir_cleanup;
	int subdir_fds[NUM_SUBDIRS];
	int subdir2_fds[NUM_SUBDIRS];
	int subdir3_fd;
	int subdir3_at;
	int ruleset_fd;
};

static void create_subdirs(struct __test_metadata *const _metadata,
			   struct _test_data_layout *const self)
{
	int i, err;
	char subdir[20], subdir2[20];

	for (i = 0; i < NUM_SUBDIRS; i++) {
		snprintf(subdir, sizeof(subdir), SUBDIR_NAME_FORMAT, i);
		err = mkdirat(self->base_dir_fd, subdir, 0755);

		ASSERT_TRUE(err == 0 || errno == EEXIST)
		{
			TH_LOG("Failed to create " TEST_DIR "/%s: %s", subdir,
			       strerror(errno));
		}
		self->subdir_fds[i] = openat(self->base_dir_fd, subdir, O_PATH);
		ASSERT_NE(self->subdir_fds[i], -1)
		{
			TH_LOG("Failed to open " TEST_DIR "/%s: %s", subdir,
			       strerror(errno));
		}

		snprintf(subdir2, sizeof(subdir2), SUBDIR2_NAME_FORMAT, i);
		err = mkdirat(self->subdir_fds[i], subdir2, 0755);
		ASSERT_TRUE(err == 0 || errno == EEXIST)
		{
			TH_LOG("Failed to create " TEST_DIR "/%s/%s: %s",
			       subdir, subdir2, strerror(errno));
		}
		self->subdir2_fds[i] =
			openat(self->subdir_fds[i], subdir2, O_PATH);
		ASSERT_NE(self->subdir2_fds[i], -1)
		{
			TH_LOG("Failed to open " TEST_DIR "/%s/%s: %s", subdir,
			       subdir2, strerror(errno));
		}
	}

	self->subdir3_at = 0;
	err = mkdirat(self->subdir2_fds[self->subdir3_at], SUBDIR3_NAME, 0755);
	ASSERT_TRUE(err == 0)
	{
		TH_LOG("Failed to create " TEST_DIR "/" SUBDIR_NAME_FORMAT
		       "/" SUBDIR2_NAME_FORMAT "/" SUBDIR3_NAME ": %s",
		       self->subdir3_at, self->subdir3_at, strerror(errno));
	}
	self->subdir3_fd = openat(self->subdir2_fds[self->subdir3_at],
				  SUBDIR3_NAME, O_PATH);
	ASSERT_NE(self->subdir3_fd, -1)
	{
		TH_LOG("Failed to open " TEST_DIR "/" SUBDIR_NAME_FORMAT
		       "/" SUBDIR2_NAME_FORMAT "/" SUBDIR3_NAME ": %s",
		       self->subdir3_at, self->subdir3_at, strerror(errno));
	}

	self->need_subdir_cleanup = true;
}

static void cleanup_subdirs(struct __test_metadata *const _metadata,
			    struct _test_data_layout *const self)
{
	int i, err;
	char subdir[20], subdir2[20];

	if (!self->need_subdir_cleanup)
		return;

	self->need_subdir_cleanup = false;

	if (self->subdir3_fd != -1) {
		err = unlinkat(self->subdir3_fd, TEST_FILE_NAME, 0);
		ASSERT_TRUE(err == 0 || errno == ENOENT)
		{
			TH_LOG("Failed to remove " TEST_DIR
			       "/" SUBDIR_NAME_FORMAT "/" SUBDIR2_NAME_FORMAT
			       "/" SUBDIR3_NAME "/" TEST_FILE_NAME ": %s",
			       self->subdir3_at, self->subdir3_at,
			       strerror(errno));
		}
		close(self->subdir3_fd);
		self->subdir3_fd = -1;

		err = unlinkat(self->subdir2_fds[self->subdir3_at],
			       SUBDIR3_NAME, AT_REMOVEDIR);
		ASSERT_TRUE(err == 0 || errno == ENOENT)
		{
			TH_LOG("Failed to remove " TEST_DIR
			       "/" SUBDIR_NAME_FORMAT "/" SUBDIR2_NAME_FORMAT
			       "/" SUBDIR3_NAME ": %s",
			       self->subdir3_at, self->subdir3_at,
			       strerror(errno));
		}
		self->subdir3_at = -1;
	}

	for (i = 0; i < NUM_SUBDIRS; i++) {
		if (self->subdir2_fds[i] != -1) {
			close(self->subdir2_fds[i]);
			self->subdir2_fds[i] = -1;

			snprintf(subdir2, sizeof(subdir2), SUBDIR2_NAME_FORMAT,
				 i);
			err = unlinkat(self->subdir_fds[i], subdir2,
				       AT_REMOVEDIR);
			ASSERT_TRUE(err == 0 || errno == ENOENT)
			{
				TH_LOG("Failed to remove " TEST_DIR
				       "/" SUBDIR_NAME_FORMAT "/%s: %s",
				       i, subdir2, strerror(errno));
			}
		}

		if (self->subdir_fds[i] == -1)
			continue;

		close(self->subdir_fds[i]);
		self->subdir_fds[i] = -1;

		snprintf(subdir, sizeof(subdir), SUBDIR_NAME_FORMAT, i);
		err = unlinkat(self->base_dir_fd, subdir, AT_REMOVEDIR);
		ASSERT_TRUE(err == 0 || errno == ENOENT)
		{
			TH_LOG("Failed to remove " TEST_DIR "/%s: %s", subdir,
			       strerror(errno));
		}
	}
}

static void create_test_dir(struct __test_metadata *const _metadata,
			    struct _test_data_layout *const self)
{
	int err;

	err = mkdir(TMP_DIR, 0755);
	ASSERT_TRUE(err == 0 || errno == EEXIST)
	{
		TH_LOG("Failed to create ./" TMP_DIR ": %s", strerror(errno));
		return;
	}

	err = mkdir(TEST_DIR, 0755);
	ASSERT_TRUE(err == 0 || errno == EEXIST)
	{
		TH_LOG("Failed to create " TEST_DIR ": %s", strerror(errno));
		return;
	}

	self->base_dir_fd = open(TEST_DIR, O_PATH);
	ASSERT_NE(self->base_dir_fd, -1)
	{
		TH_LOG("Failed to open " TEST_DIR ": %s", strerror(errno));
		return;
	}
}

static void cleanup_test_dir(struct __test_metadata *const _metadata,
			     struct _test_data_layout *const self)
{
	int err;

	close(self->base_dir_fd);
	err = rmdir(TEST_DIR);
	ASSERT_EQ(0, err)
	{
		TH_LOG("Failed to remove " TEST_DIR ": %s", strerror(errno));
	}
	err = rmdir(TMP_DIR);
	ASSERT_EQ(0, err)
	{
		TH_LOG("Failed to remove ./" TMP_DIR ": %s", strerror(errno));
	}
}

static void create_test_file(struct __test_metadata *const _metadata,
			     struct _test_data_layout *const self)
{
	int dfd;
	int fd;

	ASSERT_NE(-1, self->subdir3_at);
	dfd = self->subdir3_fd;
	ASSERT_NE(-1, dfd);

	fd = openat(dfd, TEST_FILE_NAME, O_CREAT | O_RDWR, 0644);
	ASSERT_NE(-1, fd)
	{
		TH_LOG("Failed to create " TEST_DIR "/" SUBDIR_NAME_FORMAT
		       "/" SUBDIR2_NAME_FORMAT "/" SUBDIR3_NAME
		       "/" TEST_FILE_NAME ": %s",
		       self->subdir3_at, self->subdir3_at, strerror(errno));
		return;
	}
	close(fd);
}

struct shared_region {
	bool stop;
};

static void move_subdir3_and_rmdir(struct __test_metadata *const _metadata,
				   struct _test_data_layout *const self, int to)
{
	int from, to_fd, err;
	char pathbuf1[255], pathbuf2[255], pathbuf3[255], pathbuf4[255];

	ASSERT_NE(to, self->subdir3_at);

	from = self->subdir3_at;
	ASSERT_NE(-1, from);
	to_fd = self->subdir2_fds[to];
	ASSERT_NE(-1, to_fd);

	snprintf(pathbuf1, sizeof(pathbuf1), SUBDIR_NAME_FORMAT, from);
	snprintf(pathbuf2, sizeof(pathbuf2),
		 SUBDIR_NAME_FORMAT "/" SUBDIR2_NAME_FORMAT, from, from);
	snprintf(pathbuf3, sizeof(pathbuf3),
		 SUBDIR_NAME_FORMAT "/" SUBDIR2_NAME_FORMAT "/" SUBDIR3_NAME,
		 from, from);
	snprintf(pathbuf4, sizeof(pathbuf4),
		 SUBDIR_NAME_FORMAT "/" SUBDIR2_NAME_FORMAT "/" SUBDIR3_NAME,
		 to, to);

	close(self->subdir2_fds[from]);
	close(self->subdir_fds[from]);

	/*
	 * rename and the 2 following unlinkat must be executed as close as
	 * possible
	 */

	err = renameat(self->base_dir_fd, pathbuf3, self->base_dir_fd,
		       pathbuf4);
	ASSERT_EQ(0, err)
	{
		TH_LOG("Failed to move " SUBDIR3_NAME
		       " from " SUBDIR_NAME_FORMAT "/" SUBDIR2_NAME_FORMAT
		       " to " SUBDIR_NAME_FORMAT "/" SUBDIR2_NAME_FORMAT ": %s",
		       from, from, to, to, strerror(errno));
	}

	err = unlinkat(self->base_dir_fd, pathbuf2, AT_REMOVEDIR);
	ASSERT_NE(-1, err)
	{
		TH_LOG("Failed to remove %s: %s", pathbuf2, strerror(errno));
	}

	err = unlinkat(self->base_dir_fd, pathbuf1, AT_REMOVEDIR);
	ASSERT_NE(-1, err)
	{
		TH_LOG("Failed to remove " TEST_DIR "/%s: %s", pathbuf1,
		       strerror(errno));
	}

	self->subdir_fds[from] = -1;
	self->subdir2_fds[from] = -1;
	self->subdir3_at = to;
}

static void create_ruleset(struct __test_metadata *const _metadata,
			   struct _test_data_layout *const self)
{
	struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_fs = LANDLOCK_ACCESS_FS_READ_FILE |
				     LANDLOCK_ACCESS_FS_READ_DIR |
				     LANDLOCK_ACCESS_FS_WRITE_FILE |
				     LANDLOCK_ACCESS_FS_REMOVE_FILE |
				     LANDLOCK_ACCESS_FS_MAKE_REG |
				     LANDLOCK_ACCESS_FS_MAKE_DIR |
				     LANDLOCK_ACCESS_FS_REMOVE_DIR |
				     LANDLOCK_ACCESS_FS_REFER,
		.handled_access_net = 0,
		.scoped = 0,
	};
	struct landlock_path_beneath_attr rule_attr = {
		.parent_fd = -1,
		.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_READ_DIR,
	};
	int ruleset_fd, err, dfd;

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_GE(ruleset_fd, 0)
	{
		TH_LOG("Failed to create ruleset: %s", strerror(errno));
	}

	for (int i = 0; i < NUM_SUBDIRS; i++) {
		/* We want the rule to be on s*d1 */
		dfd = self->subdir_fds[i];
		ASSERT_NE(-1, dfd);
		rule_attr.parent_fd = dfd;
		err = landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
					&rule_attr, 0);
		ASSERT_EQ(0, err)
		{
			TH_LOG("Failed to add rule for " TEST_DIR
			       "/" SUBDIR_NAME_FORMAT ": %s",
			       i, strerror(errno));
		}
	}

	self->ruleset_fd = ruleset_fd;
}

static int child_restrict_self(int ruleset_fd)
{
	int err, n;
	char errstr[512];

	err = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	if (err != 0) {
		err = errno;
		n = snprintf(errstr, sizeof(errstr),
			     "child process prctl(PR_SET_NO_NEW_PRIVS): %s\n",
			     strerror(err));
		write(STDERR_FILENO, errstr, n + 1);
		return err;
	}

	err = landlock_restrict_self(ruleset_fd, 0);
	if (err != 0) {
		err = errno;
		n = snprintf(errstr, sizeof(errstr),
			     "child process landlock_restrict_self: %s\n",
			     strerror(err));
		write(STDERR_FILENO, errstr, n + 1);
		return err;
	}

	return 0;
}

static int child_process(int subdir3_fd, int ruleset_fd,
			 volatile bool *stop_sign)
{
	int err;

	err = child_restrict_self(ruleset_fd);
	if (err != 0)
		return err;

	while (!*stop_sign) {
		err = openat(subdir3_fd, TEST_FILE_NAME, O_RDONLY);
		char errstr[512];
		int n;

		if (err < 0) {
			err = errno;
			n = snprintf(errstr, sizeof(errstr),
				     "openat(%d -> " SUBDIR3_NAME
				     ", " TEST_FILE_NAME "): %s\n",
				     subdir3_fd, strerror(err));
			write(STDERR_FILENO, errstr, n + 1);
			return err;
		}
		close(err);
	}
	return 0;
}

static void do_test(struct __test_metadata *const _metadata,
		    struct _test_data_layout *const self)
{
	struct shared_region *shr;
	int child_pid, status, err;

	create_test_file(_metadata, self);

	ASSERT_LE(sizeof(struct shared_region), 4096);
	shr = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(shr, MAP_FAILED)
	{
		TH_LOG("Failed to create shared memory region with mmap: %s",
		       strerror(errno));
		return;
	}

	*(volatile bool *)(&shr->stop) = false;

	child_pid = fork();
	if (child_pid == 0) {
		for (int i = 0; i < NUM_SUBDIRS; i++) {
			if (self->subdir_fds[i] != -1)
				close(self->subdir_fds[i]);
			if (self->subdir2_fds[i] != -1)
				close(self->subdir2_fds[i]);
		}
		close(self->base_dir_fd);
		exit(child_process(self->subdir3_fd, self->ruleset_fd,
				    &shr->stop));
		return;
	}

	ASSERT_NE(-1, child_pid)
	{
		TH_LOG("Failed to fork child process: %s", strerror(errno));
	}

	close(self->ruleset_fd);
	self->ruleset_fd = -1;

	for (int i = 1; i < NUM_SUBDIRS; i++) {
		move_subdir3_and_rmdir(_metadata, self, i);
		if (RANDOM_DELAY_AFTER_MOVE) {
			struct timespec ts = { .tv_sec = 0,
					       .tv_nsec = rand() % 400001 };
			nanosleep(&ts, NULL);
		}
	}

	*(volatile bool *)(&shr->stop) = true;
	err = waitpid(child_pid, &status, 0);
	ASSERT_NE(-1, err)
	{
		TH_LOG("Failed to wait for child process: %s", strerror(errno));
	}
	ASSERT_EQ(child_pid, err);
	status = WEXITSTATUS(status);
	ASSERT_EQ(0, status)
	{
		TH_LOG("Child process terminated with exit code %d", status);
	}
}

FIXTURE_SETUP(layout)
{
	create_test_dir(_metadata, self);
	self->subdir3_at = -1;
	self->subdir3_fd = -1;
	self->ruleset_fd = -1;
	for (int i = 0; i < NUM_SUBDIRS; i++) {
		self->subdir_fds[i] = -1;
		self->subdir2_fds[i] = -1;
	}
};

FIXTURE_TEARDOWN(layout)
{
	cleanup_test_dir(_metadata, self);
}

TEST_F_TIMEOUT(layout, pathwalk_race_test, TEST_TIME + 10)
{
	int start_time = time(NULL);

	while (time(NULL) - start_time < TEST_TIME) {
		create_subdirs(_metadata, self);
		create_ruleset(_metadata, self);
		do_test(_metadata, self);
		cleanup_subdirs(_metadata, self);
	}
}

TEST_HARNESS_MAIN
