// SPDX-License-Identifier: GPL-2.0
/*
 * Filesystems that hit an error while frozen used to set SB_RDONLY and left
 * thaw_super() with the s_writers rwsems locked. Freeze, read a corrupted
 * directory, thaw and check that nothing blocks, that modifications fail with
 * EROFS and that a remount can go read-only but not back to read-write.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../kselftest_harness.h"
#include "../statmount/statmount.h"

/* ext2 and nilfs2 directory entry names, jfs dtree names must fit 11 chars */
#define MARKER		"sbrdonlyQ7Zk2Lp9"
#define MARKER_JFS	"sbrdonlyjfs"

enum layout { LAYOUT_EXT2, LAYOUT_NILFS2, LAYOUT_JFS };

FIXTURE(emergency_ro) {
	char dir[64];
	char img[96];
	char mnt[96];
	char dev[64];
	int loop_fd;
	bool mounted;
	bool wedged;	/* writers are blocked for good, leave it mounted */
};

FIXTURE_VARIANT(emergency_ro) {
	const char *fstype;
	const char *mkfs;
	const char *opts;
	off_t size;
	enum layout layout;
};

FIXTURE_VARIANT_ADD(emergency_ro, ext2) {
	.fstype = "ext2",
	.mkfs = "mkfs.ext2 -q -F -b 4096",
	.opts = "errors=remount-ro",
	.size = 64 << 20,
	.layout = LAYOUT_EXT2,
};

FIXTURE_VARIANT_ADD(emergency_ro, jfs) {
	.fstype = "jfs",
	.mkfs = "mkfs.jfs -q",
	.opts = "errors=remount-ro",
	.size = 256 << 20,
	.layout = LAYOUT_JFS,
};

FIXTURE_VARIANT_ADD(emergency_ro, nilfs2) {
	.fstype = "nilfs2",
	.mkfs = "mkfs.nilfs2 -q -f",
	.opts = "errors=remount-ro",
	.size = 512 << 20,
	.layout = LAYOUT_NILFS2,
};

static const char *at(const char *mnt, const char *rel)
{
	static char buf[4][PATH_MAX];
	static unsigned int next;
	char *p = buf[next++ % 4];

	snprintf(p, PATH_MAX, "%s/%s", mnt, rel);
	return p;
}

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

/*
 * With CONFIG_EXT4_USE_FOR_EXT2 ext4 serves "ext2" with its own emergency
 * state. Real ext2 doesn't register /sys/fs/ext4 or /proc/fs/ext4.
 */
static bool served_by_ext4(const char *dev)
{
	const char *name = strrchr(dev, '/') + 1;
	char path[128];

	snprintf(path, sizeof(path), "/sys/fs/ext4/%s", name);
	if (!access(path, F_OK))
		return true;
	snprintf(path, sizeof(path), "/proc/fs/ext4/%s", name);
	return !access(path, F_OK);
}

static bool have_tool(const char *mkfs)
{
	char cmd[128];
	int len = strcspn(mkfs, " ");

	snprintf(cmd, sizeof(cmd), "command -v %.*s >/dev/null 2>&1", len, mkfs);
	return system(cmd) == 0;
}

static int write_file(const char *path, const char *data)
{
	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	ssize_t len = strlen(data);

	if (fd < 0)
		return -1;
	if (write(fd, data, len) != len || fsync(fd)) {
		close(fd);
		return -1;
	}
	return close(fd);
}

static uint16_t le16(const unsigned char *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const unsigned char *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }
static uint64_t le64(const unsigned char *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

/*
 * Corrupt the entry of the marker file in directory d, found by searching the
 * device for its name. ext2 and nilfs2: rec_len := 0 (2 bytes before the
 * name_len byte). jfs: the entry sits in the dtree root inside d's dinode at
 * offset 224 (32 byte slots, the name at +6), freecnt := -128. Returns the
 * number of corrupted entries.
 */
static int corrupt(int fd, enum layout layout, ino_t dir_ino, ino_t mark_ino)
{
	const size_t chunk = 1 << 22, overlap = 4096;
	unsigned char *buf, pat[2 * sizeof(MARKER)];
	size_t patlen = 0;
	off_t size, off;
	int hits = 0;

	if (layout == LAYOUT_JFS) {
		for (const char *m = MARKER_JFS; *m; m++) {
			pat[patlen++] = *m;
			pat[patlen++] = 0;	/* UCS-2 little endian */
		}
	} else {
		patlen = strlen(MARKER);
		memcpy(pat, MARKER, patlen);
	}

	/* drop cached blocks, nilfs2 writes its logs around the page cache */
	if (ioctl(fd, BLKFLSBUF, 0))
		return -1;
	buf = malloc(chunk + overlap);
	if (!buf)
		return -1;
	size = lseek(fd, 0, SEEK_END);
	for (off = 0; off < size; off += chunk) {
		off_t base = off >= (off_t)overlap ? off - overlap : 0;
		ssize_t n = pread(fd, buf, chunk + (off - base), base);

		if (n <= 0)
			break;
		for (size_t i = off - base; i + patlen <= (size_t)n; i++) {
			unsigned char *p = buf + i, zero[2] = { 0, 0 };
			off_t pos = base + i;

			if (memcmp(p, pat, patlen))
				continue;

			if (layout == LAYOUT_EXT2) {
				if (i < 8 || le32(p - 8) != mark_ino || p[-2] != patlen)
					continue;
				if (pwrite(fd, zero, 2, pos - 4) != 2)
					goto out;
				hits++;
			} else if (layout == LAYOUT_NILFS2) {
				if (i < 12 || le64(p - 12) != mark_ino || p[-2] != patlen)
					continue;
				if (pwrite(fd, zero, 2, pos - 4) != 2)
					goto out;
				hits++;
			} else {
				off_t entry = pos - 6, dino, slot;
				unsigned char d[512], bad = 0x80;

				if (i < 6 || le32(p - 6) != mark_ino ||
				    p[-1] != strlen(MARKER_JFS))
					continue;
				slot = ((entry - 224) % 512) / 32;
				if ((entry - 224) % 32 || slot < 1 || slot > 8)
					continue;
				dino = entry - 224 - 32 * slot;
				if (pread(fd, d, sizeof(d), dino) != sizeof(d))
					goto out;
				/* di_fileset is FILESYSTEM_I, di_number is d */
				if (le32(d + 4) != 16 || le32(d + 8) != dir_ino)
					continue;
				if (pwrite(fd, &bad, 1, dino + 224 + 18) != 1)
					goto out;
				hits++;
			}
		}
	}
out:
	free(buf);
	if (fsync(fd))
		return -1;
	return hits;
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

/* statmount() has to agree with the superblock options in mountinfo. */
static bool statmount_rdonly(int fd)
{
	struct statmount sm;

	if (statmount(0, 0, fd, STATMOUNT_SB_BASIC, &sm, sizeof(sm),
		      STATMOUNT_BY_FD))
		return false;
	return sm.sb_flags & MS_RDONLY;
}

/* Run fn() in a child, it must neither block on s_writers nor fail. */
static int run_child(int (*fn)(const char *, int), const char *mnt, int prefd)
{
	int status, fd;
	pid_t pid;

	pid = fork();
	if (pid < 0)
		return -1;
	if (!pid) {
		/* a child stuck in sb_start_write() mustn't hold the runner's pipe */
		fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
		}
		_exit(fn(mnt, prefd));
	}

	for (int i = 0; i < 100; i++) {
		if (waitpid(pid, &status, WNOHANG) == pid)
			return WIFEXITED(status) ? WEXITSTATUS(status) : 255;
		usleep(100000);
	}
	/* stuck in sb_start_write(), cannot be killed */
	return -ETIMEDOUT;
}

/* Every modification fails with EROFS, returns the number of mismatches. */
static int modify_all(const char *mnt, int prefd)
{
	int bad = 0, fd;

	fd = open(at(mnt, "w/new"), O_CREAT | O_WRONLY, 0644);
	bad += !(fd < 0 && errno == EROFS);
	bad += !(pwrite(prefd, "x", 1, 0) < 0 && errno == EROFS);
	bad += !(chmod(at(mnt, "f"), 0600) && errno == EROFS);
	bad += !(mkdir(at(mnt, "w/dir"), 0755) && errno == EROFS);
	bad += !(rename(at(mnt, "f"), at(mnt, "w/f")) && errno == EROFS);
	bad += !(unlink(at(mnt, "f")) && errno == EROFS);
	bad += !(truncate(at(mnt, "f"), 0) && errno == EROFS);
	fd = open(at(mnt, "f"), O_WRONLY);
	bad += !(fd < 0 && errno == EROFS);
	return bad;
}

FIXTURE_SETUP(emergency_ro)
{
	struct loop_config cfg = { .info.lo_flags = LO_FLAGS_AUTOCLEAR };
	int ctl, nr, img_fd;
	char cmd[256];

	self->loop_fd = -1;
	self->mounted = false;
	self->wedged = false;
	self->dir[0] = '\0';

	if (geteuid())
		SKIP(return, "needs root");
	if (!have_fs(variant->fstype))
		SKIP(return, "no %s support", variant->fstype);
	if (!have_tool(variant->mkfs))
		SKIP(return, "no %.*s", (int)strcspn(variant->mkfs, " "), variant->mkfs);
	ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0)
		SKIP(return, "no loop devices");

	strcpy(self->dir, "/tmp/sb_rdonly_XXXXXX");
	ASSERT_NE(mkdtemp(self->dir), NULL);
	snprintf(self->img, sizeof(self->img), "%s/img", self->dir);
	snprintf(self->mnt, sizeof(self->mnt), "%s/mnt", self->dir);
	ASSERT_EQ(mkdir(self->mnt, 0755), 0);

	img_fd = open(self->img, O_CREAT | O_RDWR, 0600);
	ASSERT_GE(img_fd, 0);
	ASSERT_EQ(ftruncate(img_fd, variant->size), 0);
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

	snprintf(cmd, sizeof(cmd), "%s %s >/dev/null 2>&1", variant->mkfs, self->dev);
	ASSERT_EQ(system(cmd), 0);
}

FIXTURE_TEARDOWN(emergency_ro)
{
	if (self->mounted) {
		if (self->wedged)
			TH_LOG("leaving %s mounted, its writers are blocked", self->mnt);
		else if (umount2(self->mnt, MNT_DETACH))
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

static const char *marker(const FIXTURE_VARIANT(emergency_ro) *variant)
{
	return variant->layout == LAYOUT_JFS ? "d/" MARKER_JFS : "d/" MARKER;
}

/* Populate, corrupt the marker entry offline and mount again. */
static bool prepare(struct __test_metadata *_metadata,
		    FIXTURE_DATA(emergency_ro) *self,
		    const FIXTURE_VARIANT(emergency_ro) *variant)
{
	const char *opts = variant->opts;
	const char *mark = marker(variant);
	struct stat st;
	ino_t dir_ino, mark_ino;
	int ret;

	ASSERT_EQ(mount(self->dev, self->mnt, variant->fstype, 0, opts), 0);
	self->mounted = true;
	if (variant->layout == LAYOUT_EXT2 && served_by_ext4(self->dev))
		return false;
	ASSERT_EQ(mkdir(at(self->mnt, "d"), 0755), 0);
	ASSERT_EQ(mkdir(at(self->mnt, "w"), 0755), 0);
	ASSERT_EQ(write_file(at(self->mnt, mark), "marker\n"), 0);
	ASSERT_EQ(write_file(at(self->mnt, "f"), "data\n"), 0);
	sync();
	/* nilfs2 only checksums the last log at mount, keep d's block out of it */
	ASSERT_EQ(write_file(at(self->mnt, "z"), "later\n"), 0);
	sync();
	ASSERT_EQ(stat(at(self->mnt, "d"), &st), 0);
	dir_ino = st.st_ino;
	ASSERT_EQ(stat(at(self->mnt, mark), &st), 0);
	mark_ino = st.st_ino;
	ASSERT_EQ(umount(self->mnt), 0);
	self->mounted = false;

	ret = corrupt(self->loop_fd, variant->layout, dir_ino, mark_ino);
	ASSERT_GE(ret, 1) {
		TH_LOG("marker entry not found on %s", self->dev);
	}

	ASSERT_EQ(mount(self->dev, self->mnt, variant->fstype, 0, opts), 0);
	self->mounted = true;
	return true;
}

TEST_F_TIMEOUT(emergency_ro, freeze_error_thaw, 120)
{
	const char *opts = variant->opts;
	const char *mark = marker(variant);
	struct statvfs sv;
	struct stat st;
	int prefd, rootfd, ret;

	if (!prepare(_metadata, self, variant))
		SKIP(return, "ext2 is served by ext4 (CONFIG_EXT4_USE_FOR_EXT2)");

	prefd = open(at(self->mnt, "f"), O_RDWR);
	ASSERT_GE(prefd, 0);
	rootfd = open(self->mnt, O_RDONLY | O_DIRECTORY);
	ASSERT_GE(rootfd, 0);
	ASSERT_FALSE(statmount_rdonly(rootfd));

	ASSERT_EQ(ioctl(rootfd, FIFREEZE, 0), 0);
	/* reads still go to the device and find the corruption */
	ret = stat(at(self->mnt, mark), &st);
	EXPECT_EQ(ret, -1);
	EXPECT_EQ(errno, EIO);
	ASSERT_EQ(ioctl(rootfd, FITHAW, 0), 0);

	/* the error made it read-only for the VFS, SB_RDONLY stays clear */
	ASSERT_EQ(statvfs(self->mnt, &sv), 0);
	ASSERT_TRUE(!!(sv.f_flag & ST_RDONLY));
	ASSERT_TRUE(super_rdonly(self->mnt));
	ASSERT_TRUE(statmount_rdonly(rootfd));

	ret = run_child(modify_all, self->mnt, prefd);
	if (ret == -ETIMEDOUT)
		self->wedged = true;
	ASSERT_EQ(ret, 0) {
		TH_LOG("%s", ret == -ETIMEDOUT ? "a writer is blocked after thaw" :
			"a modification did not fail with EROFS");
	}

	/* freezing still works and doesn't leave writers blocked */
	ASSERT_EQ(ioctl(rootfd, FIFREEZE, 0), 0);
	ASSERT_EQ(ioctl(rootfd, FITHAW, 0), 0);
	ret = run_child(modify_all, self->mnt, prefd);
	if (ret == -ETIMEDOUT)
		self->wedged = true;
	ASSERT_EQ(ret, 0);

	/* remount keeps refusing, read-only works, read-write doesn't */
	close(prefd);
	ASSERT_EQ(mount(NULL, self->mnt, NULL, MS_REMOUNT, opts), 0);
	EXPECT_TRUE(super_rdonly(self->mnt));
	ret = open(at(self->mnt, "w/new2"), O_CREAT | O_WRONLY, 0644);
	EXPECT_EQ(ret, -1);
	EXPECT_EQ(errno, EROFS);
	ASSERT_EQ(mount(NULL, self->mnt, NULL, MS_REMOUNT | MS_RDONLY, opts), 0);
	EXPECT_TRUE(super_rdonly(self->mnt));
	ret = mount(NULL, self->mnt, NULL, MS_REMOUNT, opts);
	EXPECT_EQ(ret, -1);
	EXPECT_EQ(errno, EROFS);

	close(rootfd);
	ASSERT_EQ(umount(self->mnt), 0);
	self->mounted = false;
}

/* Allocate blocks without syncing them, then hit the error. */
static int write_then_error(struct __test_metadata *_metadata,
			    FIXTURE_DATA(emergency_ro) *self,
			    const FIXTURE_VARIANT(emergency_ro) *variant)
{
	static char buf[64 << 10];
	struct stat st;
	int fd;

	memset(buf, 'x', sizeof(buf));
	fd = open(at(self->mnt, "g"), O_CREAT | O_RDWR, 0644);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, buf, sizeof(buf)), (ssize_t)sizeof(buf));
	EXPECT_EQ(stat(at(self->mnt, marker(variant)), &st), -1);
	ASSERT_TRUE(super_rdonly(self->mnt));
	return fd;
}

/* jfs BUG()ed on uncommitted tlocks when umount followed a remount,ro */
TEST_F_TIMEOUT(emergency_ro, error_remount_ro, 120)
{
	if (!prepare(_metadata, self, variant))
		SKIP(return, "ext2 is served by ext4 (CONFIG_EXT4_USE_FOR_EXT2)");

	close(write_then_error(_metadata, self, variant));
	ASSERT_EQ(mount(NULL, self->mnt, NULL, MS_REMOUNT | MS_RDONLY,
			variant->opts), 0);
	EXPECT_TRUE(super_rdonly(self->mnt));
	ASSERT_EQ(umount(self->mnt), 0);
	self->mounted = false;
}

/* fsync() mustn't claim an inode is on disk that writeback refused */
TEST_F_TIMEOUT(emergency_ro, error_fsync, 120)
{
	int fd, ret;

	if (!prepare(_metadata, self, variant))
		SKIP(return, "ext2 is served by ext4 (CONFIG_EXT4_USE_FOR_EXT2)");

	fd = write_then_error(_metadata, self, variant);
	sync();
	ret = fsync(fd);
	EXPECT_EQ(ret, -1);
	EXPECT_EQ(errno, EROFS);
	close(fd);
	ASSERT_EQ(umount(self->mnt), 0);
	self->mounted = false;
}

TEST_HARNESS_MAIN
