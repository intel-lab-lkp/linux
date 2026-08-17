// SPDX-License-Identifier: GPL-2.0
/*
 * Simple filesystem to test FUSE symlink cache
 *
 * This is a simple FUSE filesystem that contains two objects: a file named
 * 'file' and a symlink to that file named 'link'.  If symlink caching is
 * disabled (i.e. FUSE_CAP_CACHE_SYMLINKS is reset during FUSE_INIT), whenever
 * the ->readlink() is executed to resolve 'link' a counter will be incremented.
 *
 * If symlink caching is enabled (i.e. FUSE_CAP_CACHE_SYMLINKS is set during
 * FUSE_INIT), resolving a symlink will only call into user-space the first
 * time.
 */

#define FUSE_USE_VERSION 31

#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <pthread.h>
#include <fuse_lowlevel.h>

#include "kselftest_harness.h"

#define FILENAME "file"
#define FILE_INO 42

#define LINKNAME "link"
#define LINK_INO 43

#define TIMEOUT	86400.0f

#define SYMLINK_MOUNTPOINT "/tmp/symlink_cache_test_XXXXXX"

struct test_state {
	pthread_mutex_t lock;
	bool cache;
	int readlink_counter;
} test_state = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

static void fs_init(void *userdata, struct fuse_conn_info *conn)
{
	pthread_mutex_lock(&test_state.lock);
	if (test_state.cache)
		fuse_set_feature_flag(conn, FUSE_CAP_CACHE_SYMLINKS);
	else
		fuse_unset_feature_flag(conn, FUSE_CAP_CACHE_SYMLINKS);
	pthread_mutex_unlock(&test_state.lock);
}

static void fs_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct fuse_entry_param e = {};

	if (parent != FUSE_ROOT_ID ||
	    (!strcmp(name, FILENAME) && !(strcmp(name, LINKNAME))))
		fuse_reply_err(req, ENOENT);
	else {
		if (!strcmp(name, FILENAME)) {
			e.ino = FILE_INO;
			e.attr.st_mode = S_IFREG | 0444;
			e.attr.st_nlink = 2;
		} else if (!strcmp(name, LINKNAME)) {
			e.ino = LINK_INO;
			e.attr.st_mode = S_IFLNK | 0444;
			e.attr.st_nlink = 1;
			e.attr.st_size = strlen(FILENAME);
		} else {
			e.ino = FUSE_ROOT_ID;
			e.attr.st_mode = S_IFDIR | 0755;
			e.attr.st_nlink = 2;
		}
		e.attr_timeout = TIMEOUT;
		e.entry_timeout = TIMEOUT;
		fuse_reply_entry(req, &e);
	}
}

static void fs_readlink(fuse_req_t req, fuse_ino_t ino)
{
	char buf[PATH_MAX];
	size_t sz = strlen(FILENAME);

	if (ino != LINK_INO) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	memcpy(buf, FILENAME, sz);
	buf[sz] = '\0';
	pthread_mutex_lock(&test_state.lock);
	test_state.readlink_counter++;
	pthread_mutex_unlock(&test_state.lock);

	fuse_reply_readlink(req, buf);
}

static const struct fuse_lowlevel_ops symlink_ops = {
	.init           = fs_init,
	.lookup		= fs_lookup,
	.readlink	= fs_readlink,
};

static void *run_daemon(void *arg)
{
	struct fuse_session *se = (struct fuse_session *)arg;

	fuse_session_loop(se);

	return NULL;
}

FIXTURE(symlink_cache)
{
	struct fuse_session *se;
	char mountpoint[PATH_MAX];
	pthread_t thread;
};
FIXTURE_VARIANT(symlink_cache)
{
	const bool cache;
};
FIXTURE_VARIANT_ADD(symlink_cache, symlinks_nocache)
{
	/* Variant with symlink cache disabled */
	.cache = false,
};
FIXTURE_VARIANT_ADD(symlink_cache, symlinks_cache)
{
	/* Variant with symlink cache enabled */
	.cache = true,
};

FIXTURE_SETUP(symlink_cache)
{
	char *fuse_argv[] = { "fuse_symlink_cache_test", NULL };
	struct fuse_args args = FUSE_ARGS_INIT(1, fuse_argv);

	pthread_mutex_lock(&test_state.lock);
	test_state.readlink_counter = 0;
	test_state.cache = variant->cache;
	pthread_mutex_unlock(&test_state.lock);

	strcpy(self->mountpoint, SYMLINK_MOUNTPOINT);
	if (!mkdtemp(self->mountpoint))
		SKIP(return, "mkdtemp: %s", strerror(errno));

	self->se = fuse_session_new(&args, &symlink_ops,
				    sizeof(symlink_ops), NULL);
	if (!self->se) {
		rmdir(self->mountpoint);
		SKIP(return, "Failed to created FUSE session");
	}
	if (fuse_session_mount(self->se, self->mountpoint)) {
		fuse_session_destroy(self->se);
		rmdir(self->mountpoint);
		SKIP(return, "Failed to mount FUSE session");
	}
	if (pthread_create(&self->thread, NULL, run_daemon, self->se)) {
		fuse_session_unmount(self->se);
		fuse_session_destroy(self->se);
		rmdir(self->mountpoint);
		SKIP(return, "pthread_create: %s", strerror(errno));
	}

	fuse_opt_free_args(&args);
}

FIXTURE_TEARDOWN(symlink_cache)
{
	fuse_session_exit(self->se);
	fuse_session_unmount(self->se);
	pthread_join(self->thread, NULL);
	fuse_session_destroy(self->se);
	rmdir(self->mountpoint);
}

TEST_F(symlink_cache, test_symlink_cache)
{
	char pathname[PATH_MAX];
	char buf[PATH_MAX];
	ssize_t sz;
	int counter;
	int i;

	sprintf(pathname, "%s/%s", self->mountpoint, LINKNAME);
	for (i = 0; i < 100; i++) {
		sz = readlink(pathname, buf, PATH_MAX);
		ASSERT_NE(sz, -1);
	}
	pthread_mutex_lock(&test_state.lock);
	counter = test_state.readlink_counter;
	pthread_mutex_unlock(&test_state.lock);

	if (variant->cache) {
		ASSERT_EQ(counter, 1);
	} else {
		ASSERT_EQ(counter, 100);
	}
}

TEST_HARNESS_MAIN
