// SPDX-License-Identifier: GPL-2.0
/*
 * Simple filesystem to test FUSE symlink cache
 *
 * This is a simple FUSE filesystem that contains two objects: a file named
 * 'file' and a symlink to that file named 'link'.  Whenever the ->readlink() is
 * executed to resolve 'link' a counter will be incremented.  A ->read() to any
 * filesystem object will return the value in this counter.
 *
 * A '--cache' argument will allow to enable symlink caching (disabled by
 * default).  This means that, if caching is enabled, resolving a symlink will
 * only call into user-space the first time.
 */

#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <assert.h>

#define FILE	"file"
#define LINK	"link"

static struct options {
	int cache_symlinks;
} options;

static const struct fuse_opt option_spec[] = {
	{ "--cache", offsetof(struct options, cache_symlinks), 1 },
	FUSE_OPT_END
};

static int readlink_counter = 0;

static void *symlink_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	if (options.cache_symlinks)
		fuse_set_feature_flag(conn, FUSE_CAP_CACHE_SYMLINKS);

	return NULL;
}

static int symlink_getattr(const char *path, struct stat *stbuf,
			   struct fuse_file_info *fi)
{
	int res = 0;

	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else if (strcmp(path + 1, FILE) == 0) {
		char data[64];

		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = sprintf(data, "%d\n", readlink_counter);
	} else if (strcmp(path + 1, LINK) == 0) {
		stbuf->st_mode = S_IFLNK | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = strlen(FILE);
	} else
		res = -ENOENT;

	return res;
}

static int symlink_readlink(const char *path, char *buf, size_t size)
{
	if (strcmp(path + 1, LINK) != 0)
		return -ENOENT;

	memcpy(buf, FILE, strlen(FILE));
	readlink_counter++;

	return 0;
}

static int symlink_read(const char *path, char *buf, size_t sz, off_t off,
			struct fuse_file_info *fi)
{
	char data[64];
	int len;

	len = sprintf(data, "%d\n", readlink_counter);
	memcpy(buf, data, len);

	return len;
}

static const struct fuse_operations symlink_oper = {
	.init           = symlink_init,
	.getattr	= symlink_getattr,
	.readlink	= symlink_readlink,
	.read		= symlink_read,
};

int main(int argc, char *argv[])
{
	int ret;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	options.cache_symlinks = 0;
	if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1)
		return 1;

	ret = fuse_main(args.argc, args.argv, &symlink_oper, NULL);
	fuse_opt_free_args(&args);

	return ret;
}
