#ifndef __SELFTEST_FUSE_COMMON_H__
#define __SELFTEST_FUSE_COMMON_H__

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FUSE_USE_VERSION 31
#include <fuse_lowlevel.h>

#define MAX_ERR_MSG 256

#define MOUNTPOINT_TEMPLATE "/tmp/fuse_test_XXXXXX"
#define MOUNTPOINT_SZ 64

int fs_setup(struct fuse_session **se, char *mountpoint,
	     const struct fuse_lowlevel_ops *fs_ops,
	     pthread_t *thread, char *err);
void fs_teardown(struct fuse_session *se, pthread_t thread, char *mountpoint);

#endif /* __SELFTEST_FUSE_COMMON_H__ */
