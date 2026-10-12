// SPDX-License-Identifier: GPL-2.0
/*
 * gfs2 used to set SB_RDONLY when it withdrew. A withdraw while the
 * filesystem is frozen then made thaw_super() skip the release of the
 * s_writers rwsems: every later writer blocks for good and umount warns in
 * rcu_sync_dtor(). gfs2 could not even be thawed, it can't get the freeze
 * glock back once withdrawn. Now gfs2 keeps the withdraw to itself
 * (SDF_WITHDRAWN), shows "withdrawn" in the mount options and a withdrawn
 * filesystem can be thawed.
 *
 * Freeze a lock_nolock gfs2, withdraw it through sysfs, thaw it and check
 * that writers fail instead of blocking. Once withdrawn every path walk
 * into gfs2 fails with EIO, so all of it goes through file descriptors
 * opened before the withdraw.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <unistd.h>

#include "../../kselftest_harness.h"

FIXTURE(gfs2) {
	char dir[64];
	char img[96];
	char mnt[96];
	char dev[64];
	char table[32];
	int loop_fd;
	bool mounted;
	bool wedged;	/* frozen or writers blocked for good, leave it mounted */
};

static bool have_fs(const char *fstype)
{
	char line[128], cmd[128];
	bool found = false;
	FILE *f;

	for (int tries = 0; tries < 2 && !found; tries++) {
		f = fopen("/proc/filesystems", "r");
		if (!f)
			return false;
		while (fgets(line, sizeof(line), f)) {
			char *name = strrchr(line, '\t');

			name = name ? name + 1 : line;
			name[strcspn(name, "\n")] = '\0';
			if (!strcmp(name, fstype))
				found = true;
		}
		fclose(f);
		if (!found) {
			snprintf(cmd, sizeof(cmd), "modprobe -q %s", fstype);
			if (system(cmd))
				break;
		}
	}
	return found;
}

static bool mount_option(FIXTURE_DATA(gfs2) *self, const char *opt)
{
	char line[4096], needle[128];
	bool found = false;
	FILE *f;

	f = fopen("/proc/self/mountinfo", "r");
	if (!f)
		return false;
	snprintf(needle, sizeof(needle), " %s ", self->mnt);
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

/* Find the gfs2 sysfs directory of the mounted filesystem by its device. */
static int find_sysfs(FIXTURE_DATA(gfs2) *self, char *dir, size_t len)
{
	char path[PATH_MAX], want[32], have[32];
	struct dirent *d;
	struct stat st;
	int ret = -1;
	DIR *sys;
	FILE *f;

	if (stat(self->mnt, &st))
		return -1;
	snprintf(want, sizeof(want), "%u:%u\n", major(st.st_dev), minor(st.st_dev));
	sys = opendir("/sys/fs/gfs2");
	if (!sys)
		return -1;
	while ((d = readdir(sys))) {
		if (d->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/fs/gfs2/%s/id", d->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(have, sizeof(have), f) && !strcmp(have, want)) {
			snprintf(dir, len, "/sys/fs/gfs2/%s", d->d_name);
			ret = 0;
		}
		fclose(f);
		if (!ret)
			break;
	}
	closedir(sys);
	return ret;
}

static int withdraw(FIXTURE_DATA(gfs2) *self)
{
	char dir[PATH_MAX], path[PATH_MAX + 16];
	int fd, ret;

	if (find_sysfs(self, dir, sizeof(dir)))
		return -1;
	snprintf(path, sizeof(path), "%s/withdraw", dir);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ret = write(fd, "1", 1) == 1 ? 0 : -1;
	close(fd);
	/* do_withdraw() runs from a work item and waits up to 5s for the log */
	sleep(8);
	return ret;
}

/* Writes through fd must fail, not block on s_writers. */
static int modify_fd(int fd)
{
	int bad = 0;

	bad += !(pwrite(fd, "x", 1, 0) < 0);
	bad += !(fchmod(fd, 0600) < 0);
	bad += !(ftruncate(fd, 0) < 0);
	return bad;
}

/* Run modify_fd() in a child: 0, the number of mismatches or -ETIMEDOUT. */
static int run_child(int fd)
{
	int status, null;
	pid_t pid;

	pid = fork();
	if (pid < 0)
		return -1;
	if (!pid) {
		/* a child stuck in sb_start_write() mustn't hold the runner's pipe */
		null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
		}
		_exit(modify_fd(fd));
	}

	for (int i = 0; i < 100; i++) {
		if (waitpid(pid, &status, WNOHANG) == pid)
			return WIFEXITED(status) ? WEXITSTATUS(status) : 255;
		usleep(100000);
	}
	/* stuck in sb_start_write(), cannot be killed */
	return -ETIMEDOUT;
}

FIXTURE_SETUP(gfs2)
{
	struct loop_config cfg = { .info.lo_flags = LO_FLAGS_AUTOCLEAR };
	int ctl, nr, img_fd;
	char cmd[384];

	self->loop_fd = -1;
	self->mounted = false;
	self->wedged = false;
	self->dir[0] = '\0';

	if (geteuid())
		SKIP(return, "needs root");
	if (!have_fs("gfs2"))
		SKIP(return, "no gfs2 support");
	if (system("command -v mkfs.gfs2 >/dev/null 2>&1"))
		SKIP(return, "no mkfs.gfs2");
	ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0)
		SKIP(return, "no loop devices");

	strcpy(self->dir, "/tmp/sb_rdonly_XXXXXX");
	ASSERT_NE(mkdtemp(self->dir), NULL);
	snprintf(self->img, sizeof(self->img), "%s/img", self->dir);
	snprintf(self->mnt, sizeof(self->mnt), "%s/mnt", self->dir);
	/* the lock table names the sysfs directory, keep it unique */
	snprintf(self->table, sizeof(self->table), "sbrdonly:t%d", getpid() % 100000);
	ASSERT_EQ(mkdir(self->mnt, 0755), 0);

	img_fd = open(self->img, O_CREAT | O_RDWR, 0600);
	ASSERT_GE(img_fd, 0);
	ASSERT_EQ(ftruncate(img_fd, 256 << 20), 0);
	cfg.fd = img_fd;
	/* another user can grab the free device first */
	for (int tries = 0;; tries++) {
		nr = ioctl(ctl, LOOP_CTL_GET_FREE);
		ASSERT_GE(nr, 0);
		snprintf(self->dev, sizeof(self->dev), "/dev/loop%d", nr);
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
	close(img_fd);

	snprintf(cmd, sizeof(cmd),
		 "mkfs.gfs2 -O -p lock_nolock -j 1 -t %s %s >/dev/null 2>&1",
		 self->table, self->dev);
	ASSERT_EQ(system(cmd), 0);
}

FIXTURE_TEARDOWN(gfs2)
{
	if (self->mounted) {
		if (self->wedged)
			TH_LOG("leaving %s mounted, it is frozen or its writers are blocked",
			       self->mnt);
		else if (umount(self->mnt))
			TH_LOG("umount %s: %s", self->mnt, strerror(errno));
		else
			self->mounted = false;
	}
	if (self->loop_fd >= 0)
		close(self->loop_fd);	/* LO_FLAGS_AUTOCLEAR detaches it */
	if (!self->dir[0])
		return;
	unlink(self->img);
	if (!self->mounted) {
		rmdir(self->mnt);
		rmdir(self->dir);
	}
}

TEST_F_TIMEOUT(gfs2, withdraw_while_frozen, 120)
{
	struct statvfs sv;
	char path[PATH_MAX];
	int fd, prefd, rootfd, ret;

	ASSERT_EQ(mount(self->dev, self->mnt, "gfs2", 0, NULL), 0);
	self->mounted = true;
	snprintf(path, sizeof(path), "%s/f", self->mnt);
	fd = open(path, O_CREAT | O_WRONLY, 0644);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, "data\n", 5), 5);
	ASSERT_EQ(fsync(fd), 0);
	close(fd);
	prefd = open(path, O_RDWR);
	ASSERT_GE(prefd, 0);
	rootfd = open(self->mnt, O_RDONLY | O_DIRECTORY);
	ASSERT_GE(rootfd, 0);

	ASSERT_EQ(ioctl(rootfd, FIFREEZE, 0), 0);
	ret = withdraw(self);
	if (ret)
		ioctl(rootfd, FITHAW, 0);
	ASSERT_EQ(ret, 0);
	EXPECT_TRUE(mount_option(self, "withdrawn"));

	ret = ioctl(rootfd, FITHAW, 0);
	if (ret)
		self->wedged = true;
	ASSERT_EQ(ret, 0) {
		TH_LOG("FITHAW: %s, a withdrawn gfs2 stays frozen", strerror(errno));
	}
	ASSERT_EQ(fstatvfs(rootfd, &sv), 0);
	ASSERT_FALSE(!!(sv.f_flag & ST_RDONLY)) {
		TH_LOG("the withdraw set SB_RDONLY on the frozen filesystem");
	}

	ret = run_child(prefd);
	if (ret == -ETIMEDOUT)
		self->wedged = true;
	ASSERT_EQ(ret, 0) {
		TH_LOG("%s", ret == -ETIMEDOUT ? "a writer is blocked after thaw" :
			"a modification did not fail");
	}

	/* freezing a withdrawn filesystem fails and leaves it thawed */
	EXPECT_NE(ioctl(rootfd, FIFREEZE, 0), 0);
	ret = run_child(prefd);
	if (ret == -ETIMEDOUT)
		self->wedged = true;
	ASSERT_EQ(ret, 0);

	close(prefd);
	close(rootfd);
	ASSERT_EQ(umount(self->mnt), 0);
	self->mounted = false;
}

TEST_HARNESS_MAIN
