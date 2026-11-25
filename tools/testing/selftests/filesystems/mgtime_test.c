// SPDX-License-Identifier: GPL-2.0
/*
 * tmpfs uses multigrain timestamps: once the timestamps of a directory
 * have been queried, the next change to the directory must show a new
 * ctime and mtime, even within the same coarse clock tick.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <linux/magic.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "kselftest_harness.h"

/*
 * Each try changes the directory right before querying it, so nearly
 * every try lands in the same coarse clock tick as that change. A single
 * one that shows no new timestamp fails the test, so a hundred tries
 * leave no realistic chance for a broken kernel to pass.
 */
#define NR_TRIES 100

enum dir_op {
	OP_CREATE,
	OP_MKDIR,
	OP_LINK,
	OP_UNLINK,
	OP_RMDIR,
	OP_RENAME,
	OP_RENAME_OVER_FILE,
	OP_RENAME_OVER_DIR,
	OP_RENAME_EXCHANGE,
};

FIXTURE(mgtime) {
	int root;
};

FIXTURE_VARIANT(mgtime) {
	enum dir_op op;
	/* Take the renamed entry from another directory. */
	bool other_dir;
};

FIXTURE_VARIANT_ADD(mgtime, create) {
	.op = OP_CREATE,
};

FIXTURE_VARIANT_ADD(mgtime, mkdir) {
	.op = OP_MKDIR,
};

FIXTURE_VARIANT_ADD(mgtime, link) {
	.op = OP_LINK,
};

FIXTURE_VARIANT_ADD(mgtime, unlink) {
	.op = OP_UNLINK,
};

FIXTURE_VARIANT_ADD(mgtime, rmdir) {
	.op = OP_RMDIR,
};

FIXTURE_VARIANT_ADD(mgtime, rename_other_dir) {
	.op = OP_RENAME,
	.other_dir = true,
};

FIXTURE_VARIANT_ADD(mgtime, rename_over_file) {
	.op = OP_RENAME_OVER_FILE,
};

FIXTURE_VARIANT_ADD(mgtime, rename_over_file_other_dir) {
	.op = OP_RENAME_OVER_FILE,
	.other_dir = true,
};

FIXTURE_VARIANT_ADD(mgtime, rename_over_dir_other_dir) {
	.op = OP_RENAME_OVER_DIR,
	.other_dir = true,
};

FIXTURE_VARIANT_ADD(mgtime, rename_exchange_other_dir) {
	.op = OP_RENAME_EXCHANGE,
	.other_dir = true,
};

FIXTURE_SETUP(mgtime)
{
	struct statfs sfs;
	int fsfd;

	fsfd = fsopen("tmpfs", FSOPEN_CLOEXEC);
	if (fsfd < 0 && errno == EPERM)
		SKIP(return, "mounting tmpfs requires CAP_SYS_ADMIN");
	ASSERT_GE(fsfd, 0);
	ASSERT_EQ(fsconfig(fsfd, FSCONFIG_CMD_CREATE, NULL, NULL, 0), 0);
	self->root = fsmount(fsfd, FSMOUNT_CLOEXEC, 0);
	ASSERT_GE(self->root, 0);
	ASSERT_EQ(close(fsfd), 0);

	/* Without CONFIG_SHMEM tmpfs is ramfs, which has no multigrain timestamps. */
	ASSERT_EQ(fstatfs(self->root, &sfs), 0);
	if (sfs.f_type != TMPFS_MAGIC)
		SKIP(return, "tmpfs is ramfs without CONFIG_SHMEM");
}

FIXTURE_TEARDOWN(mgtime)
{
	close(self->root);
}

/* Create a file without taking its status. */
static int create_file(int dfd, const char *name)
{
	int fd;

	fd = openat(dfd, name, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	return close(fd);
}

static int remove_all(int root)
{
	static const char *const files[] = {
		"dir/victim", "dir/new", "dir/link", "src/new",
	};
	static const char *const dirs[] = {
		"dir/victim", "dir/new", "src/new", "dir", "src",
	};

	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
		if (unlinkat(root, files[i], 0) && errno != ENOENT &&
		    errno != EISDIR)
			return -1;
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		if (unlinkat(root, dirs[i], AT_REMOVEDIR) && errno != ENOENT &&
		    errno != ENOTDIR)
			return -1;
	return 0;
}

/* Build what the operation needs, changing "dir" last. */
static int prepare(int root, enum dir_op op, bool other_dir)
{
	const char *from = other_dir ? "src/new" : "dir/new";

	if (mkdirat(root, "src", 0755) || mkdirat(root, "dir", 0755))
		return -1;

	switch (op) {
	case OP_CREATE:
	case OP_MKDIR:
	case OP_LINK:
	case OP_UNLINK:
		return create_file(root, "dir/victim");
	case OP_RMDIR:
		return mkdirat(root, "dir/victim", 0755);
	case OP_RENAME:
	case OP_RENAME_OVER_FILE:
	case OP_RENAME_EXCHANGE:
		if (create_file(root, from))
			return -1;
		return create_file(root, "dir/victim");
	case OP_RENAME_OVER_DIR:
		if (mkdirat(root, from, 0755))
			return -1;
		return mkdirat(root, "dir/victim", 0755);
	}
	return -1;
}

static int change(int root, enum dir_op op, bool other_dir)
{
	const char *from = other_dir ? "src/new" : "dir/new";

	switch (op) {
	case OP_CREATE:
		return create_file(root, "dir/new");
	case OP_MKDIR:
		return mkdirat(root, "dir/new", 0755);
	case OP_LINK:
		return linkat(root, "dir/victim", root, "dir/link", 0);
	case OP_UNLINK:
		return unlinkat(root, "dir/victim", 0);
	case OP_RMDIR:
		return unlinkat(root, "dir/victim", AT_REMOVEDIR);
	case OP_RENAME:
		return renameat(root, from, root, "dir/new");
	case OP_RENAME_OVER_FILE:
	case OP_RENAME_OVER_DIR:
		return renameat(root, from, root, "dir/victim");
	case OP_RENAME_EXCHANGE:
		return renameat2(root, from, root, "dir/victim", RENAME_EXCHANGE);
	}
	return -1;
}

static bool same_time(const struct statx_timestamp *a,
		      const struct statx_timestamp *b)
{
	return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

TEST_F(mgtime, parent_times_change)
{
	for (int i = 0; i < NR_TRIES; i++) {
		struct statx before, after;
		int dfd;

		ASSERT_EQ(prepare(self->root, variant->op, variant->other_dir), 0);
		dfd = openat(self->root, "dir", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		ASSERT_GE(dfd, 0);

		/* Querying the timestamps asks for a visible next change. */
		ASSERT_EQ(statx(dfd, "", AT_EMPTY_PATH,
				STATX_CTIME | STATX_MTIME, &before), 0);
		ASSERT_EQ(change(self->root, variant->op, variant->other_dir), 0);
		ASSERT_EQ(statx(dfd, "", AT_EMPTY_PATH,
				STATX_CTIME | STATX_MTIME, &after), 0);

		ASSERT_FALSE(same_time(&before.stx_ctime, &after.stx_ctime))
			TH_LOG("try %d: ctime stayed at %lld.%09u", i,
			       (long long)before.stx_ctime.tv_sec,
			       before.stx_ctime.tv_nsec);
		ASSERT_FALSE(same_time(&before.stx_mtime, &after.stx_mtime))
			TH_LOG("try %d: mtime stayed at %lld.%09u", i,
			       (long long)before.stx_mtime.tv_sec,
			       before.stx_mtime.tv_nsec);

		ASSERT_EQ(close(dfd), 0);
		ASSERT_EQ(remove_all(self->root), 0);
	}
}

TEST_HARNESS_MAIN
