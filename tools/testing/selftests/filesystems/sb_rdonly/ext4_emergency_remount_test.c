// SPDX-License-Identifier: GPL-2.0
/*
 * Remounting an ext4 in its emergency state.
 *
 * After FS_IOC_SHUTDOWN or an error with errors=remount-ro ext4 refused every
 * change of the read-only state. Refusing the remount read-only made
 * emergency remount (sysrq u) WARN in reconfigure_super() for every such
 * ext4, a panic with panic_on_warn. Now going read-only works and writes
 * nothing, going back to read-write still fails with EROFS and remounts that
 * only change options work.
 *
 * sysrq u itself can't be tested here, it remounts every filesystem in the
 * system read-only. The plain remount read-only goes through the same
 * __ext4_remount() path minus SB_FORCE.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "../../kselftest_harness.h"

#define IMG_SIZE	(128 << 20)

#ifndef FS_IOC_SHUTDOWN
#define FS_IOC_SHUTDOWN _IOR('X', 125, __u32)
#endif
#ifndef FS_SHUTDOWN_FLAGS_DEFAULT
#define FS_SHUTDOWN_FLAGS_DEFAULT 0x0
#endif
#ifndef FS_SHUTDOWN_FLAGS_NOLOGFLUSH
#define FS_SHUTDOWN_FLAGS_NOLOGFLUSH 0x2
#endif

enum how { HOW_SHUTDOWN, HOW_ERROR };

FIXTURE(ext4_emergency) {
	char dir[64];
	char img[96];
	char mnt[96];
	char dev[32];
	int loop;
	int loop_fd;
	bool mounted;
};

FIXTURE_VARIANT(ext4_emergency) {
	const char *mkfs;
	enum how how;
	__u32 shutdown_flags;
	const char *state;	/* the state option shown in the mount options */
};

FIXTURE_VARIANT_ADD(ext4_emergency, shutdown) {
	.mkfs = "mkfs.ext4 -q -F -b 4096",
	.how = HOW_SHUTDOWN,
	.shutdown_flags = FS_SHUTDOWN_FLAGS_NOLOGFLUSH,
	.state = "shutdown",
};

/* the journal stays live, going read-only must not write it */
FIXTURE_VARIANT_ADD(ext4_emergency, shutdown_default) {
	.mkfs = "mkfs.ext4 -q -F -b 4096",
	.how = HOW_SHUTDOWN,
	.shutdown_flags = FS_SHUTDOWN_FLAGS_DEFAULT,
	.state = "shutdown",
};

FIXTURE_VARIANT_ADD(ext4_emergency, error) {
	.mkfs = "mkfs.ext4 -q -F -b 4096",
	.how = HOW_ERROR,
	.state = "emergency_ro",
};

FIXTURE_VARIANT_ADD(ext4_emergency, error_nojournal) {
	.mkfs = "mkfs.ext4 -q -F -b 4096 -O ^has_journal",
	.how = HOW_ERROR,
	.state = "emergency_ro",
};

static bool mount_option(const char *mnt, const char *opt)
{
	char line[4096], needle[128];
	bool found = false;
	FILE *f;

	f = fopen("/proc/self/mountinfo", "r");
	if (!f)
		return false;
	snprintf(needle, sizeof(needle), " %s ", mnt);
	while (fgets(line, sizeof(line), f)) {
		char *super;

		if (!strstr(line, needle))
			continue;
		/* the superblock options follow the separator */
		super = strstr(line, " - ");
		if (super && strstr(super, opt))
			found = true;
	}
	fclose(f);
	return found;
}

/* The superblock options start with its own "ro" or "rw". */
static bool super_rdonly(const char *mnt)
{
	char line[4096], needle[128], fstype[64], source[256], opts[2048];
	bool ro = false;
	FILE *f;

	f = fopen("/proc/self/mountinfo", "r");
	if (!f)
		return false;
	snprintf(needle, sizeof(needle), " %s ", mnt);
	while (fgets(line, sizeof(line), f)) {
		char *super;

		if (!strstr(line, needle))
			continue;
		super = strstr(line, " - ");
		if (super && sscanf(super, " - %63s %255s %2047s",
				    fstype, source, opts) == 3)
			ro = !strncmp(opts, "ro", 2) && (opts[2] == ',' || !opts[2]);
	}
	fclose(f);
	return ro;
}

/* Sectors written to the loop device so far, -1 on error. */
static long long written(int loop)
{
	unsigned long long v[7];
	char path[64];
	FILE *f;
	int n;

	snprintf(path, sizeof(path), "/sys/block/loop%d/stat", loop);
	f = fopen(path, "r");
	if (!f)
		return -1;
	n = fscanf(f, "%llu %llu %llu %llu %llu %llu %llu",
		   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
	fclose(f);
	return n == 7 ? (long long)v[6] : -1;
}

static int enter_emergency(FIXTURE_DATA(ext4_emergency) *self,
			   const FIXTURE_VARIANT(ext4_emergency) *variant)
{
	char path[128];
	int fd, ret;

	if (variant->how == HOW_SHUTDOWN) {
		__u32 flags = variant->shutdown_flags;

		fd = open(self->mnt, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (fd < 0)
			return -1;
		ret = ioctl(fd, FS_IOC_SHUTDOWN, &flags);
		close(fd);
		return ret;
	}

	/* ext4_error() through the test knob, errors=remount-ro reacts */
	snprintf(path, sizeof(path), "/sys/fs/ext4/loop%d/trigger_fs_error", self->loop);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ret = write(fd, "selftest", 8) == 8 ? 0 : -1;
	close(fd);
	return ret;
}

static void ext4_emergency_cleanup(FIXTURE_DATA(ext4_emergency) *self)
{
	if (self->mounted)
		umount2(self->mnt, MNT_DETACH);
	if (self->loop_fd >= 0)
		close(self->loop_fd);	/* LO_FLAGS_AUTOCLEAR detaches it */
	unlink(self->img);
	rmdir(self->mnt);
	rmdir(self->dir);
}

FIXTURE_SETUP(ext4_emergency)
{
	struct loop_config cfg = { .info.lo_flags = LO_FLAGS_AUTOCLEAR };
	char cmd[256], path[128];
	int ctl, fd, ret;

	self->loop = -1;
	self->loop_fd = -1;
	self->mounted = false;

	if (geteuid() != 0)
		SKIP(return, "test requires CAP_SYS_ADMIN");
	if (access("/dev/loop-control", R_OK | W_OK))
		SKIP(return, "test requires loop devices");

	ASSERT_EQ(unshare(CLONE_NEWNS), 0);
	ASSERT_EQ(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL), 0);

	snprintf(self->dir, sizeof(self->dir), "/tmp/ext4_emergency.XXXXXX");
	ASSERT_NE(mkdtemp(self->dir), NULL);
	snprintf(self->img, sizeof(self->img), "%s/img", self->dir);
	snprintf(self->mnt, sizeof(self->mnt), "%s/mnt", self->dir);
	ASSERT_EQ(mkdir(self->mnt, 0700), 0);

	fd = open(self->img, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(ftruncate(fd, IMG_SIZE), 0);
	snprintf(cmd, sizeof(cmd), "%s %s >/dev/null 2>&1", variant->mkfs, self->img);
	if (system(cmd)) {
		close(fd);
		ext4_emergency_cleanup(self);
		SKIP(return, "mkfs.ext4 failed or is not available");
	}

	ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	ASSERT_GE(ctl, 0);
	cfg.fd = fd;
	/* another user can grab the free device first */
	for (int tries = 0;; tries++) {
		self->loop = ioctl(ctl, LOOP_CTL_GET_FREE);
		ASSERT_GE(self->loop, 0);
		snprintf(self->dev, sizeof(self->dev), "/dev/loop%d", self->loop);
		self->loop_fd = open(self->dev, O_RDWR | O_CLOEXEC);
		ASSERT_GE(self->loop_fd, 0);
		if (!ioctl(self->loop_fd, LOOP_CONFIGURE, &cfg))
			break;
		ASSERT_EQ(errno, EBUSY);
		ASSERT_LT(tries, 10);
		close(self->loop_fd);
		self->loop_fd = -1;
	}
	close(ctl);
	close(fd);

	ret = mount(self->dev, self->mnt, "ext4", 0, "errors=remount-ro");
	if (ret && errno == ENODEV) {
		ext4_emergency_cleanup(self);
		SKIP(return, "kernel lacks ext4");
	}
	ASSERT_EQ(ret, 0);
	self->mounted = true;

	snprintf(path, sizeof(path), "%s/f", self->mnt);
	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, "data\n", 5), 5);
	ASSERT_EQ(fsync(fd), 0);
	close(fd);
}

FIXTURE_TEARDOWN(ext4_emergency)
{
	ext4_emergency_cleanup(self);
}

TEST_F_TIMEOUT(ext4_emergency, remount, 60)
{
	long long w0, w1;
	struct statvfs sv;
	int ret;

	ASSERT_EQ(enter_emergency(self, variant), 0);
	ASSERT_TRUE(mount_option(self->mnt, variant->state));

	/* nothing below may write, get whatever is still dirty out first */
	sync();
	w0 = written(self->loop);
	ASSERT_GE(w0, 0);

	/* only options change */
	ASSERT_EQ(mount(NULL, self->mnt, NULL, MS_REMOUNT, "errors=remount-ro"), 0);
	ASSERT_EQ(statvfs(self->mnt, &sv), 0);
	ASSERT_FALSE(!!(sv.f_flag & ST_RDONLY));

	/* going read-only works */
	ret = mount(NULL, self->mnt, NULL, MS_REMOUNT | MS_RDONLY, NULL);
	ASSERT_EQ(ret, 0) TH_LOG("remount read-only: %s", strerror(errno));
	ASSERT_TRUE(super_rdonly(self->mnt));
	ASSERT_TRUE(mount_option(self->mnt, variant->state));

	/* without writing anything */
	w1 = written(self->loop);
	ASSERT_EQ(w1, w0) TH_LOG("%lld sectors written by the remounts", w1 - w0);

	/* going back to read-write doesn't */
	ret = mount(NULL, self->mnt, NULL, MS_REMOUNT, NULL);
	ASSERT_EQ(ret, -1);
	ASSERT_EQ(errno, EROFS);
	ASSERT_TRUE(super_rdonly(self->mnt));

	ASSERT_EQ(umount(self->mnt), 0);
	self->mounted = false;
}

TEST_HARNESS_MAIN
