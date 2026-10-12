// SPDX-License-Identifier: GPL-2.0
/*
 * Remounting xfs after a shutdown.
 *
 * After FS_IOC_SHUTDOWN xfs refused to go read-only because flushing the
 * filesystem fails with EIO. Emergency remount (sysrq u) then WARNed in
 * reconfigure_super() for every shut down xfs, a panic with panic_on_warn.
 * Now going read-only works, also when the remount's own flush shuts xfs
 * down, going back to read-write fails with EIO and remounts that only
 * change options work.
 *
 * sysrq u itself can't be tested here, it remounts every filesystem in the
 * system read-only. The plain remount read-only goes through the same
 * xfs_remount_ro() path minus SB_FORCE.
 */
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/falloc.h>
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

/* mkfs.xfs refuses filesystems smaller than 300MiB */
#define IMG_SIZE	(512 << 20)

#ifndef FS_IOC_SHUTDOWN
#define FS_IOC_SHUTDOWN _IOR('X', 125, __u32)
#endif
#ifndef FS_SHUTDOWN_FLAGS_DEFAULT
#define FS_SHUTDOWN_FLAGS_DEFAULT 0x0
#endif
#ifndef FS_SHUTDOWN_FLAGS_LOGFLUSH
#define FS_SHUTDOWN_FLAGS_LOGFLUSH 0x1
#endif
#ifndef FS_SHUTDOWN_FLAGS_NOLOGFLUSH
#define FS_SHUTDOWN_FLAGS_NOLOGFLUSH 0x2
#endif

FIXTURE(xfs_shutdown) {
	char dir[64];
	char img[96];
	char mnt[96];
	char dev[32];
	int loop;
	int loop_fd;
	bool tmpfs;
	bool mounted;
};

FIXTURE_VARIANT(xfs_shutdown) {
	/* FS_IOC_SHUTDOWN flags, -1 lets the remount's own flush shut it down */
	int shutdown_flags;
};

FIXTURE_VARIANT_ADD(xfs_shutdown, shutdown_default) {
	.shutdown_flags = FS_SHUTDOWN_FLAGS_DEFAULT,
};

FIXTURE_VARIANT_ADD(xfs_shutdown, shutdown_logflush) {
	.shutdown_flags = FS_SHUTDOWN_FLAGS_LOGFLUSH,
};

FIXTURE_VARIANT_ADD(xfs_shutdown, shutdown_nologflush) {
	.shutdown_flags = FS_SHUTDOWN_FLAGS_NOLOGFLUSH,
};

FIXTURE_VARIANT_ADD(xfs_shutdown, shutdown_by_remount) {
	.shutdown_flags = -1,
};

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

static uint32_t get_be32(const unsigned char *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));
	return be32toh(v);
}

static uint64_t get_be64(const unsigned char *p)
{
	uint64_t v;

	memcpy(&v, p, sizeof(v));
	return be64toh(v);
}

/*
 * Make the next log write fail. The image lives on our own tmpfs. Punch the
 * internal log out of it and shrink the tmpfs to what is in use, then writing
 * the log needs a new page and the loop device fails it with ENOSPC. xfs
 * shuts down on the first log write error.
 */
static int fail_log_writes(FIXTURE_DATA(xfs_shutdown) *self)
{
	uint32_t blocksize, agblocks, logblocks;
	uint64_t logstart, agno, agbno;
	unsigned char sb[512];
	struct statvfs sv;
	char opts[64];
	int fd, ret = -1;

	fd = open(self->img, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	/* struct xfs_dsb */
	if (pread(fd, sb, sizeof(sb), 0) != sizeof(sb) || memcmp(sb, "XFSB", 4))
		goto out;
	blocksize = get_be32(sb + 4);
	logstart = get_be64(sb + 48);
	agblocks = get_be32(sb + 84);
	logblocks = get_be32(sb + 96);
	if (!logstart)
		goto out;
	/* sb_agblklog */
	agno = logstart >> sb[124];
	agbno = logstart & ((1ULL << sb[124]) - 1);
	if (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
		      (agno * agblocks + agbno) * blocksize,
		      (uint64_t)logblocks * blocksize))
		goto out;

	/* the loop device may still allocate pages in between */
	for (int tries = 0; tries < 10; tries++) {
		if (statvfs(self->dir, &sv))
			break;
		snprintf(opts, sizeof(opts), "size=%llu",
			 (unsigned long long)(sv.f_blocks - sv.f_bfree) * sv.f_frsize);
		if (!mount(NULL, self->dir, NULL, MS_REMOUNT, opts)) {
			ret = 0;
			break;
		}
	}
out:
	close(fd);
	return ret;
}

static void xfs_shutdown_cleanup(FIXTURE_DATA(xfs_shutdown) *self)
{
	if (self->mounted)
		umount2(self->mnt, MNT_DETACH);
	if (self->loop_fd >= 0)
		close(self->loop_fd);	/* LO_FLAGS_AUTOCLEAR detaches it */
	unlink(self->img);
	rmdir(self->mnt);
	if (self->tmpfs)
		umount2(self->dir, MNT_DETACH);
	rmdir(self->dir);
}

FIXTURE_SETUP(xfs_shutdown)
{
	struct loop_config cfg = { .info.lo_flags = LO_FLAGS_AUTOCLEAR };
	char cmd[256], path[128];
	int ctl, fd, ret;

	self->loop = -1;
	self->loop_fd = -1;
	self->tmpfs = false;
	self->mounted = false;

	if (geteuid() != 0)
		SKIP(return, "test requires CAP_SYS_ADMIN");
	if (access("/dev/loop-control", R_OK | W_OK))
		SKIP(return, "test requires loop devices");

	ASSERT_EQ(unshare(CLONE_NEWNS), 0);
	ASSERT_EQ(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL), 0);

	snprintf(self->dir, sizeof(self->dir), "/tmp/xfs_shutdown.XXXXXX");
	ASSERT_NE(mkdtemp(self->dir), NULL);
	/* fail_log_writes() needs the image on a tmpfs of our own */
	ASSERT_EQ(mount("tmpfs", self->dir, "tmpfs", 0, "size=1g,mode=0700"), 0);
	self->tmpfs = true;
	snprintf(self->img, sizeof(self->img), "%s/img", self->dir);
	snprintf(self->mnt, sizeof(self->mnt), "%s/mnt", self->dir);
	ASSERT_EQ(mkdir(self->mnt, 0700), 0);

	fd = open(self->img, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(ftruncate(fd, IMG_SIZE), 0);
	snprintf(cmd, sizeof(cmd), "mkfs.xfs -q -f %s >/dev/null 2>&1", self->img);
	if (system(cmd)) {
		close(fd);
		xfs_shutdown_cleanup(self);
		SKIP(return, "mkfs.xfs failed or is not available");
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

	ret = mount(self->dev, self->mnt, "xfs", 0, NULL);
	if (ret && errno == ENODEV) {
		xfs_shutdown_cleanup(self);
		SKIP(return, "kernel lacks xfs");
	}
	ASSERT_EQ(ret, 0);
	self->mounted = true;

	snprintf(path, sizeof(path), "%s/f", self->mnt);
	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, "data\n", 5), 5);
	ASSERT_EQ(fsync(fd), 0);
	/* leave something dirty for the shutdown */
	ASSERT_EQ(write(fd, "more\n", 5), 5);
	close(fd);
}

FIXTURE_TEARDOWN(xfs_shutdown)
{
	xfs_shutdown_cleanup(self);
}

TEST_F_TIMEOUT(xfs_shutdown, remount, 60)
{
	struct statvfs sv;
	int fd, ret;

	if (variant->shutdown_flags < 0) {
		ASSERT_EQ(fail_log_writes(self), 0);
	} else {
		__u32 flags = variant->shutdown_flags;

		fd = open(self->mnt, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		ASSERT_GE(fd, 0);
		ASSERT_EQ(ioctl(fd, FS_IOC_SHUTDOWN, &flags), 0);
		close(fd);
	}

	/* only options change */
	ASSERT_EQ(mount(NULL, self->mnt, NULL, MS_REMOUNT, NULL), 0);
	ASSERT_EQ(statvfs(self->mnt, &sv), 0);
	ASSERT_FALSE(!!(sv.f_flag & ST_RDONLY));

	/* going read-only works */
	ret = mount(NULL, self->mnt, NULL, MS_REMOUNT | MS_RDONLY, NULL);
	ASSERT_EQ(ret, 0) TH_LOG("remount read-only: %s", strerror(errno));
	ASSERT_TRUE(super_rdonly(self->mnt));

	/* going back to read-write doesn't, also not after the remount's shutdown */
	ret = mount(NULL, self->mnt, NULL, MS_REMOUNT, NULL);
	ASSERT_EQ(ret, -1);
	ASSERT_EQ(errno, EIO);
	ASSERT_TRUE(super_rdonly(self->mnt));

	ASSERT_EQ(umount(self->mnt), 0);
	self->mounted = false;
}

TEST_HARNESS_MAIN
