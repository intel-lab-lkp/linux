// SPDX-License-Identifier: GPL-2.0
/*
 * Test: a request refused for an inode the client still holds a reference to
 *
 * FUSE_OPEN, FUSE_GETATTR, FUSE_SETATTR, FUSE_READLINK and FUSE_STATFS carry a
 * nodeid rather than a path.  The client only sends them for an inode it has
 * already looked up and holds a reference to, and a server owes the client that
 * inode until it is sent FUSE_FORGET.  A server that lets the inode go early,
 * as one backing a shared directory does when the name is renamed over, answers
 * with ENOENT.
 *
 * On an unfixed kernel that ENOENT is passed out unchanged.  The path walk has
 * no reason to doubt it and the caller is told a file is missing when it never
 * stopped existing.  Callers that read a missing file as an empty one act on
 * the emptiness.
 *
 * Fixed (fs/fuse/file.c and fs/fuse/dir.c): ENOENT becomes ESTALE, which
 * describes the handle rather than the name.  filename_lookup() and
 * do_filp_open() already retry with LOOKUP_REVAL on ESTALE, so the name is
 * resolved again and the inode it refers to now is used.  A name that has
 * genuinely gone away fails the retried lookup, so ENOENT still reaches a
 * caller that deserves it.
 *
 * Only requests reachable through a path walk are covered, because the retry
 * is what makes ESTALE useful and the walk is what performs it.  An operation
 * on a descriptor already open has no equivalent recovery.
 *
 * Test outline:
 *  1. Mount a minimal FUSE fs holding one file.
 *  2. The server refuses the first request of the kind under test and allows
 *     every one after it, standing in for a server that released the inode and
 *     has since resolved the name again.
 *  3. openat() the file.
 *     Buggy:  ENOENT reaches the caller, one open was asked for.  FAIL.
 *     Fixed:  the walk retries, the second open is allowed, the descriptor is
 *             returned, two opens were asked for.  PASS.
 *  4. stat(), chmod(), readlink() and statfs() by name, which are the same
 *     recovery through FUSE_GETATTR, FUSE_SETATTR, FUSE_READLINK and
 *     FUSE_STATFS.  Each is reached through a path walk, which is what makes
 *     the retry available.
 *  5. Open a name the server does not have at all.
 *     Both:   ENOENT, because the lookup fails rather than the open, and a
 *             file that is absent must still look absent.
 */

#define _GNU_SOURCE
#define FUSE_USE_VERSION 34

#include <errno.h>
#include <fcntl.h>
#include <fuse_lowlevel.h>
#include <linux/limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "../../kselftest_harness.h"

#define FILE_NAME	"held"
#define LINK_NAME	"held-link"
#define ABSENT_NAME	"no-such-file"
#define FILE_INO	2
#define LINK_INO	3
#define CONTENTS	"present\n"
#define LINK_TARGET	FILE_NAME

/*
 * Which request the server refuses, and how many times.  Shared with the
 * daemon thread; one test runs at a time, so plain ints.
 *
 * Every one of these names an inode by nodeid rather than by name, so a
 * refusal of any of them is describing a handle rather than a missing file.
 * FUSE_LOOKUP is deliberately absent: it carries a name, so its ENOENT is an
 * answer rather than a fault, and absent_name_still_reports_absent covers it.
 */
enum refuse_what {
	REFUSE_NOTHING,
	REFUSE_OPEN,
	REFUSE_GETATTR,
	REFUSE_SETATTR,
	REFUSE_READLINK,
	REFUSE_STATFS,
};

static struct {
	enum refuse_what what;
	int refusals_left;
	int opens_seen;
	int getattrs_seen;
	int setattrs_seen;
	int readlinks_seen;
	int statfss_seen;
} g_ds;

/* True once, for the request under test, and then never again. */
static bool refuse_now(enum refuse_what what)
{
	if (g_ds.what != what || g_ds.refusals_left <= 0)
		return false;
	g_ds.refusals_left--;
	return true;
}

static void fill_attr(fuse_ino_t ino, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_ino = ino;
	/*
	 * Owned by whoever runs the test, so that chmod() is a request the
	 * kernel will carry through to the server rather than refuse itself.
	 */
	st->st_uid = getuid();
	st->st_gid = getgid();
	if (ino == FUSE_ROOT_ID) {
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
	} else if (ino == LINK_INO) {
		st->st_mode = S_IFLNK | 0777;
		st->st_nlink = 1;
		st->st_size = sizeof(LINK_TARGET) - 1;
	} else {
		st->st_mode = S_IFREG | 0644;
		st->st_nlink = 1;
		st->st_size = sizeof(CONTENTS) - 1;
	}
}

static void t_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct fuse_entry_param e;
	fuse_ino_t ino;

	if (parent != FUSE_ROOT_ID)
		ino = 0;
	else if (!strcmp(name, FILE_NAME))
		ino = FILE_INO;
	else if (!strcmp(name, LINK_NAME))
		ino = LINK_INO;
	else
		ino = 0;

	if (!ino) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	memset(&e, 0, sizeof(e));
	e.ino = ino;
	e.attr_timeout = 0;
	e.entry_timeout = 0;
	fill_attr(ino, &e.attr);
	fuse_reply_entry(req, &e);
}

static void t_getattr(fuse_req_t req, fuse_ino_t ino,
		      struct fuse_file_info *fi)
{
	struct stat st;

	(void)fi;
	/* The root is left alone; refusing it would break the mount itself. */
	if (ino == FILE_INO) {
		g_ds.getattrs_seen++;
		if (refuse_now(REFUSE_GETATTR)) {
			fuse_reply_err(req, ENOENT);
			return;
		}
	}
	fill_attr(ino, &st);
	fuse_reply_attr(req, &st, 0);
}

static void t_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	if (ino != FILE_INO) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	g_ds.opens_seen++;
	if (refuse_now(REFUSE_OPEN)) {
		/*
		 * The inode is gone as far as this server is concerned, even
		 * though the client is holding a reference to it and asked by
		 * nodeid rather than by name.
		 */
		fuse_reply_err(req, ENOENT);
		return;
	}
	fuse_reply_open(req, fi);
}

static void t_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
		   struct fuse_file_info *fi)
{
	size_t len = sizeof(CONTENTS) - 1;

	(void)fi;
	if (ino != FILE_INO) {
		fuse_reply_err(req, ENOENT);
		return;
	}
	if ((size_t)off >= len) {
		fuse_reply_buf(req, NULL, 0);
		return;
	}
	if (off + size > len)
		size = len - off;
	fuse_reply_buf(req, CONTENTS + off, size);
}

static void t_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
		      int to_set, struct fuse_file_info *fi)
{
	struct stat st;

	(void)attr;
	(void)to_set;
	(void)fi;
	if (ino == FILE_INO) {
		g_ds.setattrs_seen++;
		if (refuse_now(REFUSE_SETATTR)) {
			fuse_reply_err(req, ENOENT);
			return;
		}
	}
	fill_attr(ino, &st);
	fuse_reply_attr(req, &st, 0);
}

static void t_readlink(fuse_req_t req, fuse_ino_t ino)
{
	if (ino != LINK_INO) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	g_ds.readlinks_seen++;
	if (refuse_now(REFUSE_READLINK)) {
		fuse_reply_err(req, ENOENT);
		return;
	}
	fuse_reply_readlink(req, LINK_TARGET);
}

static void t_statfs(fuse_req_t req, fuse_ino_t ino)
{
	struct statvfs sfs;

	(void)ino;
	g_ds.statfss_seen++;
	if (refuse_now(REFUSE_STATFS)) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	memset(&sfs, 0, sizeof(sfs));
	sfs.f_bsize = 512;
	sfs.f_frsize = 512;
	sfs.f_namemax = NAME_MAX;
	fuse_reply_statfs(req, &sfs);
}

static const struct fuse_lowlevel_ops fs_ops = {
	.lookup		= t_lookup,
	.getattr	= t_getattr,
	.setattr	= t_setattr,
	.readlink	= t_readlink,
	.statfs		= t_statfs,
	.open		= t_open,
	.read		= t_read,
};

static void *run_daemon(void *arg)
{
	fuse_session_loop((struct fuse_session *)arg);
	return NULL;
}

/* ---- kselftest harness --------------------------------------------------- */

FIXTURE(open_estale) {
	struct fuse_session *se;
	char                 mountpoint[PATH_MAX];
	char                 file_path[PATH_MAX];
	char                 link_path[PATH_MAX];
	char                 absent_path[PATH_MAX];
	pthread_t            thread;
};

FIXTURE_SETUP(open_estale)
{
	char *fuse_argv[] = { "fuse_estale_test", NULL };
	struct fuse_args args = FUSE_ARGS_INIT(1, fuse_argv);

	memset(&g_ds, 0, sizeof(g_ds));
	g_ds.what = REFUSE_NOTHING;
	g_ds.refusals_left = 1;

	strcpy(self->mountpoint, "/tmp/open_estale_test_XXXXXX");
	if (!mkdtemp(self->mountpoint))
		SKIP(return, "mkdtemp: %s", strerror(errno));

	snprintf(self->file_path, sizeof(self->file_path),
		 "%s/" FILE_NAME, self->mountpoint);
	snprintf(self->link_path, sizeof(self->link_path),
		 "%s/" LINK_NAME, self->mountpoint);
	snprintf(self->absent_path, sizeof(self->absent_path),
		 "%s/" ABSENT_NAME, self->mountpoint);

	self->se = fuse_session_new(&args, &fs_ops, sizeof(fs_ops), NULL);
	if (!self->se) {
		rmdir(self->mountpoint);
		SKIP(return, "fuse_session_new failed");
	}

	if (fuse_session_mount(self->se, self->mountpoint)) {
		fuse_session_destroy(self->se);
		rmdir(self->mountpoint);
		SKIP(return, "fuse_session_mount failed (no fusermount3 or no privileges)");
	}

	if (pthread_create(&self->thread, NULL, run_daemon, self->se)) {
		fuse_session_unmount(self->se);
		fuse_session_destroy(self->se);
		rmdir(self->mountpoint);
		SKIP(return, "pthread_create: %s", strerror(errno));
	}

	fuse_opt_free_args(&args);
}

FIXTURE_TEARDOWN(open_estale)
{
	fuse_session_exit(self->se);
	fuse_session_unmount(self->se);
	pthread_join(self->thread, NULL);
	fuse_session_destroy(self->se);
	rmdir(self->mountpoint);
}

TEST_F(open_estale, refused_open_is_retried)
{
	int fd;

	g_ds.what = REFUSE_OPEN;

	fd = open(self->file_path, O_RDONLY);

	/*
	 * The refusal describes a handle the server should have honoured, so
	 * the walk is entitled to resolve the name again and open what it
	 * refers to now. Reporting the file missing instead ends the walk.
	 */
	ASSERT_GE(fd, 0) {
		TH_LOG("open failed with %s after %d open request(s)",
		       strerror(errno), g_ds.opens_seen);
	}
	EXPECT_EQ(2, g_ds.opens_seen);
	close(fd);
}

TEST_F(open_estale, refused_getattr_on_path_is_retried)
{
	struct stat st;

	g_ds.what = REFUSE_GETATTR;

	/*
	 * Reached by name, so the walk can resolve it again and ask a second
	 * time, the same recovery the open gets.
	 */
	ASSERT_EQ(0, stat(self->file_path, &st)) {
		TH_LOG("stat failed with %s after %d getattr request(s)",
		       strerror(errno), g_ds.getattrs_seen);
	}
	EXPECT_GT(g_ds.getattrs_seen, 1);
}

TEST_F(open_estale, refused_setattr_on_path_is_retried)
{
	g_ds.what = REFUSE_SETATTR;

	/*
	 * chmod() reaches the inode by name, so the same retry applies: the
	 * refusal describes a handle and the walk may resolve the name again.
	 */
	ASSERT_EQ(0, chmod(self->file_path, 0600)) {
		TH_LOG("chmod failed with %s after %d setattr request(s)",
		       strerror(errno), g_ds.setattrs_seen);
	}
	EXPECT_GT(g_ds.setattrs_seen, 1);
}

TEST_F(open_estale, refused_readlink_on_path_is_retried)
{
	char buf[PATH_MAX];
	ssize_t n;

	g_ds.what = REFUSE_READLINK;

	n = readlink(self->link_path, buf, sizeof(buf) - 1);
	ASSERT_GE(n, 0) {
		TH_LOG("readlink failed with %s after %d readlink request(s)",
		       strerror(errno), g_ds.readlinks_seen);
	}
	buf[n] = '\0';
	EXPECT_STREQ(LINK_TARGET, buf);
	EXPECT_GT(g_ds.readlinks_seen, 1);
}

TEST_F(open_estale, refused_statfs_on_path_is_retried)
{
	struct statfs sfs;

	g_ds.what = REFUSE_STATFS;

	/*
	 * statfs() describes the mount rather than the file, but it is still
	 * reached through a path walk, so a refusal that names a handle is
	 * retried the same way.
	 */
	ASSERT_EQ(0, statfs(self->file_path, &sfs)) {
		TH_LOG("statfs failed with %s after %d statfs request(s)",
		       strerror(errno), g_ds.statfss_seen);
	}
	EXPECT_GT(g_ds.statfss_seen, 1);
}

TEST_F(open_estale, absent_name_still_reports_absent)
{
	int fd;

	/*
	 * Here it is the lookup that fails rather than the open, so nothing is
	 * being described as stale and the caller must still be told the name
	 * is not there.
	 */
	fd = open(self->absent_path, O_RDONLY);
	ASSERT_LT(fd, 0);
	EXPECT_EQ(ENOENT, errno);
}

TEST_HARNESS_MAIN
