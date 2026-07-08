// SPDX-License-Identifier: GPL-2.0

/*
 * Simple filesystem to test FUSE ACLs cache
 *
 * This is a simple FUSE filesystem that contains a single object (a file named
 * 'file') and which allows to set the 'system.posix_acl_access' ACL on that
 * object.  Whenever this ACL is read from the FUSE filesystem, a counter is
 * incremented.  And value for this counter can be obtained from reading from
 * this file.
 *
 * When ACLs are being cached (the '--cache' argument was used to mount this
 * filesystem), this counter will only be incremented the first time the ACL is
 * read, as the kernel won't be calling into user-space again until that cache
 * is invalidated.
 */
#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 12)

#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/xattr.h>

#define FILE	"file"
#define TIMEOUT	86400.0f

static struct options {
	int cache_acls;
} options;

static const struct fuse_opt option_spec[] = {
	{ "--cache", offsetof(struct options, cache_acls), 1 },
	FUSE_OPT_END
};

static int getxattr_counter = 0;

static void acl_init(void *userdata, struct fuse_conn_info *conn)
{
	if (options.cache_acls)
		fuse_set_feature_flag(conn, FUSE_CAP_POSIX_ACL);
	else
		fuse_unset_feature_flag(conn, FUSE_CAP_POSIX_ACL);
}

static int acl_stat(fuse_ino_t ino, struct stat *stbuf)
{
	char data[64];

	stbuf->st_ino = ino;
	switch (ino) {
	case 1:
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		break;
	case 42:
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = sprintf(data, "%d\n", getxattr_counter);
		break;
	default:
		return -1;
	}

	stbuf->st_uid = getuid();
	stbuf->st_gid = getgid();
	stbuf->st_atime = stbuf->st_mtime = stbuf->st_ctime = time(NULL);

	return 0;
}

static void acl_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct fuse_entry_param e;

	memset(&e, 0, sizeof(e));

	if (parent != 1 || strcmp(name, FILE) != 0)
		fuse_reply_err(req, ENOENT);
	else {
		e.ino = 42;
		e.attr_timeout = TIMEOUT;
		e.entry_timeout = TIMEOUT;
		acl_stat(e.ino, &e.attr);
		fuse_reply_entry(req, &e);
	}
}

static void acl_getattr(fuse_req_t req, fuse_ino_t ino,
			struct fuse_file_info *fi)
{
	struct stat attr;

	memset(&attr, 0, sizeof(struct stat));
	if (acl_stat(ino, &attr) == -1)
		fuse_reply_err(req, ENOENT);
	else
		fuse_reply_attr(req, &attr, TIMEOUT);
}

static void acl_read(fuse_req_t req, fuse_ino_t ino, size_t size,
		     off_t off, struct fuse_file_info *fi)
{
	char data[64];
	int len;

	len = sprintf(data, "%d\n", getxattr_counter);
	if (off < len)
		fuse_reply_buf(req, data + off, MIN(len - off, size));
	else
		fuse_reply_buf(req, NULL, 0);
}

char *xattr_value = NULL;
size_t xattr_value_sz = 0;

static void acl_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
			 const char *value, size_t size, int flags)
{
	int ret = 0;

	if (ino != 42)
		ret = ENOENT;
	else if (!strcmp(name, "system.posix_acl_access")) {
		if (xattr_value)
			free(xattr_value);
		xattr_value = malloc(size);
		memcpy(xattr_value, value, size);
		xattr_value_sz = size;
	} else
		ret = ENOTSUP;

	fuse_reply_err(req, ret);
}

static void acl_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
			 size_t size)
{
	if (ino != 42)
		fuse_reply_err(req, ENOENT);
	else if (!xattr_value || strcmp(name, "system.posix_acl_access"))
		fuse_reply_err(req, ENODATA);
	else if (size) {
		fuse_reply_buf(req, xattr_value, xattr_value_sz);
		getxattr_counter++;
	} else
		fuse_reply_xattr(req, xattr_value_sz);
}

static const struct fuse_lowlevel_ops acl_op = {
	.init           = acl_init,
	.lookup		= acl_lookup,
	.getattr	= acl_getattr,
	.getxattr	= acl_getxattr,
	.setxattr	= acl_setxattr,
	.read		= acl_read,
};

int main(int argc, char *argv[])
{
	struct fuse_session *se;
	struct fuse_loop_config *config;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse_cmdline_opts opts;
	int ret = -1;

	options.cache_acls = 0;

	if (fuse_parse_cmdline(&args, &opts) != 0)
		return ret;

	if (opts.mountpoint == NULL) {
		printf("usage: %s [options] <mountpoint>\n", argv[0]);
		goto out_args;
	}

	if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1)
		goto out_args;

	se = fuse_session_new(&args, &acl_op, sizeof(acl_op), NULL);
	if (!se)
		goto out_args;
	if (fuse_set_signal_handlers(se))
		goto out_session;
	if (fuse_session_mount(se, opts.mountpoint))
		goto out_signal;

	fuse_daemonize(opts.foreground);
	if (opts.singlethread) {
		ret = fuse_session_loop(se);
	} else {
		config = fuse_loop_cfg_create();
		fuse_loop_cfg_set_clone_fd(config, opts.clone_fd);
		fuse_loop_cfg_set_max_threads(config, opts.max_threads);
		ret = fuse_session_loop_mt(se, config);
		fuse_loop_cfg_destroy(config);
		config = NULL;
	}
	fuse_session_unmount(se);

out_signal:
	fuse_remove_signal_handlers(se);
out_session:
	fuse_session_destroy(se);
out_args:
	free(opts.mountpoint);
	fuse_opt_free_args(&args);

	return ret;
}
