// SPDX-License-Identifier: GPL-2.0

#include "fuse_common.h"

static void *run_daemon(void *arg)
{
	fuse_session_loop((struct fuse_session *)arg);
	return NULL;
}

int fs_setup(struct fuse_session **se, char *mountpoint,
	     const struct fuse_lowlevel_ops *fs_ops,
	     pthread_t *thread, char *err)
{
	char *fuse_argv[] = { "fuse_test", NULL };
	struct fuse_args args = FUSE_ARGS_INIT(1, fuse_argv);

	strcpy(mountpoint, MOUNTPOINT_TEMPLATE);
	if (!mkdtemp(mountpoint)) {
		snprintf(err, MAX_ERR_MSG, "mkdtemp: %s", strerror(errno));
		return -1;
	}

	*se = fuse_session_new(&args, fs_ops, sizeof(*fs_ops), NULL);
	if (!*se) {
		rmdir(mountpoint);
		snprintf(err, MAX_ERR_MSG, "fuse_session_new failed");
		return -1;
	}

	if (fuse_session_mount(*se, mountpoint)) {
		fuse_session_destroy(*se);
		rmdir(mountpoint);
		snprintf(err, MAX_ERR_MSG, "fuse_session_mount failed "
			"(missing fusermount3 or insufficient privileges)");
		return -1;
	}

	if (pthread_create(thread, NULL, run_daemon, *se)) {
		fuse_session_unmount(*se);
		fuse_session_destroy(*se);
		rmdir(mountpoint);
		snprintf(err, MAX_ERR_MSG, "pthread_create: %s", strerror(errno));
		return -1;
	}

	fuse_opt_free_args(&args);

	return 0;
}

void fs_teardown(struct fuse_session *se, pthread_t thread, char *mountpoint)
{
	fuse_session_exit(se);
	fuse_session_unmount(se);
	pthread_join(thread, NULL);
	fuse_session_destroy(se);
	rmdir(mountpoint);
}

