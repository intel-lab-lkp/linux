// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for tasks whose fs->root and/or fs->pwd is the NULL path.
 * See "fs: support tasks with a null root or cwd".
 *
 * Each test runs against this task's own fs_struct. We unshare it first
 * so we only ever touch a private copy, never the shared one, and we
 * restore the original root/cwd afterwards.
 */
#include <kunit/test.h>
#include <linux/fs_struct.h>
#include <linux/namei.h>
#include <linux/path.h>

/* The NULL path: { .mnt = NULL, .dentry = NULL }. */
static const struct path null_path;

struct null_fs_ctx {
	struct path saved_root;
	struct path saved_pwd;
	struct path anchor;	/* a real directory, standing in for a dirfd */
};

static int null_fs_setup(struct null_fs_ctx *ctx)
{
	int err;

	err = unshare_fs_struct();
	if (err)
		return err;
	err = kern_path("/", LOOKUP_DIRECTORY, &ctx->anchor);
	if (err)
		return err;
	get_fs_root(current->fs, &ctx->saved_root);
	get_fs_pwd(current->fs, &ctx->saved_pwd);
	return 0;
}

static void null_fs_teardown(struct null_fs_ctx *ctx)
{
	set_fs_root(current->fs, &ctx->saved_root);
	set_fs_pwd(current->fs, &ctx->saved_pwd);
	path_put(&ctx->saved_root);
	path_put(&ctx->saved_pwd);
	path_put(&ctx->anchor);
}

/* Resolve @name, drop any reference it returns, and yield the errno. */
static int try_kern_path(const char *name)
{
	struct path out;
	int err = kern_path(name, 0, &out);

	if (!err)
		path_put(&out);
	return err;
}

static int try_fd_relative(struct null_fs_ctx *ctx, const char *name)
{
	struct path out;
	int err = vfs_path_lookup(ctx->anchor.dentry, ctx->anchor.mnt,
				  name, 0, &out);

	if (!err)
		path_put(&out);
	return err;
}

/* No root: absolute paths fail, but ".." climbs and the cwd still works. */
static void null_root_test(struct kunit *test)
{
	struct null_fs_ctx ctx;

	KUNIT_ASSERT_EQ(test, null_fs_setup(&ctx), 0);
	set_fs_root(current->fs, &null_path);

	/* A leading '/' has nothing to anchor to. */
	KUNIT_EXPECT_EQ(test, try_kern_path("/"), -ENOENT);

	/* ".." is unbounded rather than refused (it would have been
	 * -ENOENT before this feature). It starts from the still-present
	 * cwd and runs out of parents at the mount root.
	 */
	KUNIT_EXPECT_EQ(test, try_kern_path(".."), 0);

	/* The cwd is untouched: AT_FDCWD-relative lookups still resolve. */
	KUNIT_EXPECT_EQ(test, try_kern_path("."), 0);

	/* A dirfd-anchored lookup never consults fs->root. */
	KUNIT_EXPECT_EQ(test, try_fd_relative(&ctx, "."), 0);

	null_fs_teardown(&ctx);
}

/* No cwd: AT_FDCWD-relative paths fail, but absolute and dirfds work. */
static void null_cwd_test(struct kunit *test)
{
	struct null_fs_ctx ctx;

	KUNIT_ASSERT_EQ(test, null_fs_setup(&ctx), 0);
	set_fs_pwd(current->fs, &null_path);

	/* Relative-to-cwd lookups have no starting point. */
	KUNIT_EXPECT_EQ(test, try_kern_path("."), -ENOENT);
	KUNIT_EXPECT_EQ(test, try_kern_path("foo"), -ENOENT);

	/* The root is untouched: absolute lookups still resolve. */
	KUNIT_EXPECT_EQ(test, try_kern_path("/"), 0);

	/* A dirfd-anchored lookup never consults fs->pwd. */
	KUNIT_EXPECT_EQ(test, try_fd_relative(&ctx, "."), 0);

	null_fs_teardown(&ctx);
}

/* Neither root nor cwd: only descriptor-relative lookups remain. */
static void null_root_and_cwd_test(struct kunit *test)
{
	struct null_fs_ctx ctx;

	KUNIT_ASSERT_EQ(test, null_fs_setup(&ctx), 0);
	set_fs_root(current->fs, &null_path);
	set_fs_pwd(current->fs, &null_path);

	KUNIT_EXPECT_EQ(test, try_kern_path("/"), -ENOENT);
	KUNIT_EXPECT_EQ(test, try_kern_path("."), -ENOENT);

	/* The held descriptor still names files. */
	KUNIT_EXPECT_EQ(test, try_fd_relative(&ctx, "."), 0);

	null_fs_teardown(&ctx);
}

static struct kunit_case null_root_cwd_test_cases[] = {
	KUNIT_CASE(null_root_test),
	KUNIT_CASE(null_cwd_test),
	KUNIT_CASE(null_root_and_cwd_test),
	{},
};

static struct kunit_suite null_root_cwd_test_suite = {
	.name = "null_root_cwd",
	.test_cases = null_root_cwd_test_cases,
};

kunit_test_suite(null_root_cwd_test_suite);
