// SPDX-License-Identifier: GPL-2.0
/*
 * A mount that another namespace's rmdir detached, or that went down with
 * the detached tree it was in when the tree's last fd was closed, keeps
 * its submounts connected, and a connected submount is put by its
 * parent's final mntput(). A submount whose filesystem keeps a file open
 * on the parent then holds the parent's count above zero for good:
 * nothing in userspace refers to either mount any more and nothing can
 * release them. A loop device is the simplest such filesystem, its
 * backing file sits on the parent.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/fuse.h>
#include <linux/keyctl.h>
#include <linux/major.h>
#include <linux/raid/md_u.h>
#include <linux/raid/md_p.h>
#include <linux/loop.h>
#include <linux/magic.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>

#include "../wrappers.h"
#include "../../kselftest_harness.h"

#define IMAGE_SIZE	(1440 * 1024)
#define SECTOR		512

/* A blank FAT12 floppy image: boot sector, two FATs, an empty root directory. */
static int write_fat12(int fd)
{
	struct stat st;
	unsigned char sector[SECTOR] = {
		0xeb, 0x3c, 0x90, 'M', 'S', 'W', 'I', 'N', '4', '.', '1',
		[11] = 0x00, 0x02,	/* bytes per sector: 512 */
		[13] = 1,		/* sectors per cluster */
		[14] = 1, 0,		/* reserved sectors */
		[16] = 2,		/* FATs */
		[17] = 0xe0, 0x00,	/* root directory entries: 224 */
		[19] = 0x40, 0x0b,	/* total sectors: 2880 */
		[21] = 0xf0,		/* media descriptor */
		[22] = 9, 0,		/* sectors per FAT */
		[24] = 18, 0,		/* sectors per track */
		[26] = 2, 0,		/* heads */
		[38] = 0x29,		/* extended boot signature */
		[39] = 0x12, 0x34, 0x56, 0x78,
		[43] = 'N', 'O', ' ', 'N', 'A', 'M', 'E', ' ', ' ', ' ', ' ',
		[54] = 'F', 'A', 'T', '1', '2', ' ', ' ', ' ',
		[510] = 0x55, 0xaa,
	};
	unsigned char fat[SECTOR] = { 0xf0, 0xff, 0xff };

	if (pwrite(fd, sector, SECTOR, 0) != SECTOR)
		return -1;
	/* the first FAT and the second one, one sector each is enough */
	if (pwrite(fd, fat, SECTOR, 1 * SECTOR) != SECTOR ||
	    pwrite(fd, fat, SECTOR, 10 * SECTOR) != SECTOR)
		return -1;
	if (fstat(fd, &st) || S_ISBLK(st.st_mode))
		return 0;
	return ftruncate(fd, IMAGE_SIZE);
}

/*
 * A blank FAT12 image with 4 KiB sectors and @sectors of them, for a device
 * with 4 KiB logical blocks (zram) or for a bigger image than a floppy.
 */
static int write_fat12_4k(int fd, unsigned int sectors)
{
	unsigned int fat_sectors = (sectors * 3 / 2 + 4095) / 4096;
	unsigned char sector[4096] = {
		0xeb, 0x3c, 0x90, 'M', 'S', 'W', 'I', 'N', '4', '.', '1',
		[11] = 0x00, 0x10,	/* bytes per sector: 4096 */
		[13] = 1,		/* sectors per cluster */
		[14] = 1, 0,		/* reserved sectors */
		[16] = 2,		/* FATs */
		[17] = 128, 0,		/* root directory entries: one sector */
		[19] = sectors & 0xff, sectors >> 8,
		[21] = 0xf8,		/* media descriptor */
		[22] = fat_sectors, 0,
		[24] = 63, 0,		/* sectors per track */
		[26] = 255, 0,		/* heads */
		[38] = 0x29,		/* extended boot signature */
		[39] = 0x12, 0x34, 0x56, 0x78,
		[43] = 'N', 'O', ' ', 'N', 'A', 'M', 'E', ' ', ' ', ' ', ' ',
		[54] = 'F', 'A', 'T', '1', '2', ' ', ' ', ' ',
		[510] = 0x55, 0xaa,
	};
	unsigned char fat[4096] = { 0xf8, 0xff, 0xff };
	struct stat st;

	if (pwrite(fd, sector, sizeof(sector), 0) != sizeof(sector))
		return -1;
	if (pwrite(fd, fat, sizeof(fat), 1 * 4096) != sizeof(fat) ||
	    pwrite(fd, fat, sizeof(fat), (1 + fat_sectors) * 4096) != sizeof(fat))
		return -1;
	if (fstat(fd, &st) || S_ISBLK(st.st_mode))
		return 0;
	return ftruncate(fd, (off_t)sectors * 4096);
}

#define MINIX_BLOCK	1024
#define MINIX_BLOCKS	4096			/* a 4 MiB image */
#define MINIX_INODES	512
#define MINIX_ITABLE	(MINIX_INODES * 32 / MINIX_BLOCK)
#define MINIX_FIRSTDATA	(2 + 1 + 1 + MINIX_ITABLE)	/* boot, super, imap, zmap, inodes */

/*
 * A blank minix v1 image, for the holders that need a FIFO or a device
 * node on the dying mount, which vfat can't hold. Superblock in block 1,
 * one block each for the inode and zone bitmaps, the inode table, and
 * the root directory in the first data zone.
 */
static int write_minix(int fd)
{
	struct {
		__u16 s_ninodes, s_nzones, s_imap_blocks, s_zmap_blocks;
		__u16 s_firstdatazone, s_log_zone_size;
		__u32 s_max_size;
		__u16 s_magic, s_state;
	} sb = {
		.s_ninodes = MINIX_INODES,
		.s_nzones = MINIX_BLOCKS,
		.s_imap_blocks = 1,
		.s_zmap_blocks = 1,
		.s_firstdatazone = MINIX_FIRSTDATA,
		.s_max_size = (7 + 512 + 512 * 512) * MINIX_BLOCK,
		.s_magic = MINIX_SUPER_MAGIC,
		.s_state = 1,		/* MINIX_VALID_FS */
	};
	struct {
		__u16 i_mode, i_uid;
		__u32 i_size, i_time;
		__u8 i_gid, i_nlinks;
		__u16 i_zone[9];
	} root = {
		.i_mode = S_IFDIR | 0755,
		.i_size = 2 * 16,
		.i_nlinks = 2,
		.i_zone = { MINIX_FIRSTDATA },
	};
	unsigned char imap[MINIX_BLOCK], zmap[MINIX_BLOCK], dir[MINIX_BLOCK] = {};
	int i;

	/* bit 0 is reserved in both maps, the root inode and its zone are in use */
	memset(imap, 0xff, sizeof(imap));
	for (i = 2; i <= MINIX_INODES; i++)
		imap[i / 8] &= ~(1 << (i % 8));
	memset(zmap, 0xff, sizeof(zmap));
	for (i = 2; i <= MINIX_BLOCKS - MINIX_FIRSTDATA; i++)
		zmap[i / 8] &= ~(1 << (i % 8));
	dir[0] = 1;
	dir[2] = '.';
	dir[16] = 1;
	dir[18] = '.';
	dir[19] = '.';

	if (pwrite(fd, &sb, sizeof(sb), 1 * MINIX_BLOCK) != sizeof(sb) ||
	    pwrite(fd, imap, sizeof(imap), 2 * MINIX_BLOCK) != sizeof(imap) ||
	    pwrite(fd, zmap, sizeof(zmap), 3 * MINIX_BLOCK) != sizeof(zmap) ||
	    pwrite(fd, &root, sizeof(root), 4 * MINIX_BLOCK) != sizeof(root) ||
	    pwrite(fd, dir, sizeof(dir), MINIX_FIRSTDATA * MINIX_BLOCK) != sizeof(dir))
		return -1;
	return ftruncate(fd, (off_t)MINIX_BLOCKS * MINIX_BLOCK);
}

static int read_sysfs(const char *path, char *buf, size_t size)
{
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, size - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return 0;
}

/* Does any mount namespace in the system show a mount of @dev? */
static bool mounted_anywhere(const char *dev)
{
	char path[PATH_MAX], line[4096];
	struct dirent *de;
	bool found = false;
	DIR *proc;
	FILE *f;

	proc = opendir("/proc");
	if (!proc)
		return false;
	while (!found && (de = readdir(proc))) {
		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/mountinfo", de->d_name);
		f = fopen(path, "re");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			if (strstr(line, dev)) {
				found = true;
				break;
			}
		}
		fclose(f);
	}
	closedir(proc);
	return found;
}

/* Wait up to @ms milliseconds for the loop device to give up its backing file. */
static bool loop_released(const char *sysfs, int ms)
{
	char buf[PATH_MAX];

	for (; ms > 0; ms -= 100) {
		if (read_sysfs(sysfs, buf, sizeof(buf)) < 0)
			return true;
		usleep(100000);
	}
	return read_sysfs(sysfs, buf, sizeof(buf)) < 0;
}

FIXTURE(loop_cycle) {
	char dev[32];		/* the loop device the child set up */
	char sysfs[64];		/* its backing_file attribute */
};

FIXTURE_SETUP(loop_cycle)
{
	if (geteuid() != 0)
		SKIP(return, "test requires CAP_SYS_ADMIN");
	if (access("/dev/loop-control", R_OK | W_OK))
		SKIP(return, "test requires loop devices");

	ASSERT_EQ(unshare(CLONE_NEWNS), 0);
	ASSERT_EQ(mount("", "/", NULL, MS_REC | MS_PRIVATE, NULL), 0);

	rmdir("/mnt_dir");
	ASSERT_EQ(mkdir("/mnt_dir", 0755), 0);
	ASSERT_EQ(mount("tmpfs", "/mnt_dir", "tmpfs", 0, NULL), 0);
	self->dev[0] = '\0';
}

FIXTURE_TEARDOWN(loop_cycle)
{
	umount2("/mnt_dir", MNT_DETACH);
	rmdir("/mnt_dir");
}

/* Child exit codes. */
enum {
	CHILD_OK,
	CHILD_NS,		/* could not set up the namespace or the tmpfs */
	CHILD_IMAGE,		/* could not write the image */
	CHILD_LOOP,		/* could not set up the loop device */
	CHILD_MOUNT,		/* could not mount it (vfat and msdos both refused) */
	CHILD_PIPE,		/* the parent went away */
	CHILD_HOLDER,		/* could not set the holder up below the mount */
	CHILD_SKIP,		/* the kernel lacks what the holder needs */
};

/* Bind a free loop device to the open image @ifd; the device number. */
static int loop_bind(int ifd)
{
	int cfd, lfd, n;
	char dev[32];

	cfd = open("/dev/loop-control", O_RDWR);
	if (cfd < 0)
		return -1;
	n = ioctl(cfd, LOOP_CTL_GET_FREE);
	close(cfd);
	if (n < 0)
		return -1;
	snprintf(dev, sizeof(dev), "/dev/loop%d", n);
	lfd = open(dev, O_RDWR);
	if (lfd < 0)
		return -1;
	if (ioctl(lfd, LOOP_SET_FD, ifd))
		n = -1;
	close(lfd);
	return n;
}

/* Write an image to @img, bind a loop device to it and mount that at @mp; the device number. */
static int loop_mount(const char *img, const char *mp)
{
	char dev[32];
	int ifd, n;

	ifd = open(img, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (ifd < 0 || write_fat12(ifd))
		return -CHILD_IMAGE;
	n = loop_bind(ifd);
	close(ifd);	/* the loop device holds the file from now on */
	if (n < 0)
		return -CHILD_LOOP;
	snprintf(dev, sizeof(dev), "/dev/loop%d", n);
	if (mkdir(mp, 0755))
		return -CHILD_MOUNT;
	if (mount(dev, mp, "vfat", 0, NULL) && mount(dev, mp, "msdos", 0, NULL))
		return -CHILD_MOUNT;
	return n;
}

/*
 * Two tmpfs mounts, each carrying the image of the loop mount below the
 * other: the loop mount below /mnt_dir/vol has its image on /mnt_dir/vol2
 * and the other way round.
 */
static int crossed_child(int to_parent, int from_parent)
{
	int n[2];
	char c;

	if (unshare(CLONE_NEWNS) || mount("", "/", NULL, MS_REC | MS_PRIVATE, NULL))
		return CHILD_NS;
	if (mkdir("/mnt_dir/vol", 0755) || mount("tmpfs", "/mnt_dir/vol", "tmpfs", 0, NULL) ||
	    mkdir("/mnt_dir/vol2", 0755) || mount("tmpfs", "/mnt_dir/vol2", "tmpfs", 0, NULL))
		return CHILD_NS;
	n[0] = loop_mount("/mnt_dir/vol2/img", "/mnt_dir/vol/mnt");
	if (n[0] < 0)
		return -n[0];
	n[1] = loop_mount("/mnt_dir/vol/img", "/mnt_dir/vol2/mnt");
	if (n[1] < 0)
		return -n[1];
	if (write(to_parent, n, sizeof(n)) != sizeof(n))
		return CHILD_PIPE;
	if (read(from_parent, &c, 1) != 1)
		return CHILD_PIPE;
	return CHILD_OK;
}

/*
 * In its own mount namespace the child mounts a tmpfs on /mnt_dir/vol,
 * puts a filesystem image on it, binds a loop device to the image and
 * mounts that loop device below. The loop device's backing file is a
 * reference on the mount the image is on, held by the loop device, held
 * by the mounted filesystem, held by the mount below.
 */
static int loop_child(int to_parent, int from_parent)
{
	char c;
	int n;

	if (unshare(CLONE_NEWNS) || mount("", "/", NULL, MS_REC | MS_PRIVATE, NULL))
		return CHILD_NS;
	if (mkdir("/mnt_dir/vol", 0755) || mount("tmpfs", "/mnt_dir/vol", "tmpfs", 0, NULL))
		return CHILD_NS;
	n = loop_mount("/mnt_dir/vol/img", "/mnt_dir/vol/mnt");
	if (n < 0)
		return -n;

	if (write(to_parent, &n, sizeof(n)) != sizeof(n))
		return CHILD_PIPE;
	/* keep the namespace alive while the parent removes the directory */
	if (read(from_parent, &c, 1) != 1)
		return CHILD_PIPE;
	return CHILD_OK;
}

/*
 * An exclusive open of the device fails while a filesystem holds it, and
 * the only filesystem that ever did is the one mounted below the dead
 * mount. Give a release in flight a moment. Then the device must clear
 * right away rather than only be marked for autoclear.
 */
static void assert_loop_released(struct __test_metadata *_metadata,
				 FIXTURE_DATA(loop_cycle) *self)
{
	int lfd;

	for (int i = 0; i < 20; i++) {
		lfd = open(self->dev, O_RDONLY | O_EXCL);
		if (lfd >= 0)
			break;
		usleep(100000);
	}
	EXPECT_GE(lfd, 0)
		TH_LOG("%s is still held by the loop mount below the dead mount: nothing refers to either mount and nothing can release them",
		       self->dev);
	if (lfd >= 0)
		close(lfd);

	lfd = open(self->dev, O_RDWR);
	ASSERT_GE(lfd, 0);
	ASSERT_EQ(ioctl(lfd, LOOP_CLR_FD), 0);
	close(lfd);
	ASSERT_TRUE(loop_released(self->sysfs, 5000))
		TH_LOG("%s kept its backing file after LOOP_CLR_FD: the filesystem on it is still mounted somewhere nobody can reach",
		       self->dev);
}

/*
 * rmdir of /mnt_dir/vol from here, where it is not a mountpoint, detaches
 * the child's tmpfs with the loop mount connected below it. Once the child
 * is gone nothing refers to either mount. The loop device must then be
 * free to give up its backing file, which only happens when the mounted
 * filesystem below the detached tmpfs has been released.
 */
TEST_F(loop_cycle, detached_loop_mount_released)
{
	int to_parent[2], to_child[2];
	char buf[PATH_MAX];
	int status, n = -1;
	pid_t pid;

	ASSERT_EQ(pipe(to_parent), 0);
	ASSERT_EQ(pipe(to_child), 0);

	pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		close(to_parent[0]);
		close(to_child[1]);
		_exit(loop_child(to_parent[1], to_child[0]));
	}
	close(to_parent[1]);
	close(to_child[0]);

	if (read(to_parent[0], &n, sizeof(n)) != sizeof(n)) {
		waitpid(pid, &status, 0);
		if (WIFEXITED(status) && WEXITSTATUS(status) == CHILD_MOUNT)
			SKIP(return, "test requires a FAT filesystem");
		ASSERT_EQ(status, 0);
	}
	snprintf(self->dev, sizeof(self->dev), "/dev/loop%d", n);
	snprintf(self->sysfs, sizeof(self->sysfs), "/sys/block/loop%d/loop/backing_file", n);
	ASSERT_EQ(read_sysfs(self->sysfs, buf, sizeof(buf)), 0);
	ASSERT_NE(strstr(buf, "/mnt_dir/vol/img"), NULL);

	/* not a mountpoint in this namespace, so the directory can go */
	ASSERT_EQ(rmdir("/mnt_dir/vol"), 0);

	/* the child leaves: its namespace and every reference it held are gone */
	ASSERT_EQ(write(to_child[1], "", 1), 1);
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	ASSERT_EQ(status, 0);
	close(to_parent[0]);
	close(to_child[1]);

	/* nothing can reach the two mounts any more */
	ASSERT_EQ(access("/mnt_dir/vol", F_OK), -1);
	ASSERT_FALSE(mounted_anywhere(self->dev));

	assert_loop_released(_metadata, self);
}

/*
 * The same two mounts in a detached tree: a clone of /mnt_dir/vol from
 * open_tree(), the image opened through the clone so that the loop device
 * holds the clone, and the loop mount moved below the clone. The last
 * close of the tree's fd dissolves the tree with the loop mount left
 * connected below the dead clone, and nothing refers to either afterwards.
 */
TEST_F(loop_cycle, dissolved_tree_loop_mount_released)
{
	int tfd, ifd, fsfd, mfd, n;
	char buf[PATH_MAX];

	fsfd = sys_fsopen("vfat", 0);
	if (fsfd < 0)
		fsfd = sys_fsopen("msdos", 0);
	if (fsfd < 0)
		SKIP(return, "test requires a FAT filesystem");

	ASSERT_EQ(mkdir("/mnt_dir/vol", 0755), 0);
	ASSERT_EQ(mount("tmpfs", "/mnt_dir/vol", "tmpfs", 0, NULL), 0);
	tfd = sys_open_tree(AT_FDCWD, "/mnt_dir/vol", OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC);
	ASSERT_GE(tfd, 0);

	/* the image, opened through the clone: the loop device holds the clone */
	ifd = openat(tfd, "img", O_RDWR | O_CREAT | O_EXCL, 0600);
	ASSERT_GE(ifd, 0);
	ASSERT_EQ(write_fat12(ifd), 0);
	n = loop_bind(ifd);
	close(ifd);
	ASSERT_GE(n, 0);
	snprintf(self->dev, sizeof(self->dev), "/dev/loop%d", n);
	snprintf(self->sysfs, sizeof(self->sysfs), "/sys/block/loop%d/loop/backing_file", n);

	/* the loop mount, moved below the clone */
	ASSERT_EQ(mkdirat(tfd, "mnt", 0755), 0);
	ASSERT_EQ(sys_fsconfig(fsfd, FSCONFIG_SET_STRING, "source", self->dev, 0), 0);
	ASSERT_EQ(sys_fsconfig(fsfd, FSCONFIG_CMD_CREATE, NULL, NULL, 0), 0);
	mfd = sys_fsmount(fsfd, 0, 0);
	ASSERT_GE(mfd, 0);
	close(fsfd);
	ASSERT_EQ(sys_move_mount(mfd, "", tfd, "mnt", MOVE_MOUNT_F_EMPTY_PATH), 0);
	close(mfd);

	ASSERT_EQ(read_sysfs(self->sysfs, buf, sizeof(buf)), 0);
	ASSERT_NE(strstr(buf, "img"), NULL);

	/* the last fd of the tree: both mounts die, the loop mount connected */
	close(tfd);

	/* nothing can reach the two mounts any more */
	ASSERT_FALSE(mounted_anywhere(self->dev));

	assert_loop_released(_metadata, self);
}

/*
 * The cycle in two steps: rmdir of /mnt_dir/vol leaves the loop mount
 * below it connected while its image's mount, /mnt_dir/vol2, is alive;
 * then rmdir of /mnt_dir/vol2 takes that one with the loop mount whose
 * image is on the dead /mnt_dir/vol. Each dead mount now owns a loop
 * mount whose filesystem pins the other.
 */
TEST_F(loop_cycle, crossed_images_released)
{
	int to_parent[2], to_child[2];
	char sysfs[2][64], dev[2][32];
	int status, n[2] = { -1, -1 };
	pid_t pid;

	ASSERT_EQ(pipe(to_parent), 0);
	ASSERT_EQ(pipe(to_child), 0);

	pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		close(to_parent[0]);
		close(to_child[1]);
		_exit(crossed_child(to_parent[1], to_child[0]));
	}
	close(to_parent[1]);
	close(to_child[0]);

	if (read(to_parent[0], n, sizeof(n)) != sizeof(n)) {
		waitpid(pid, &status, 0);
		if (WIFEXITED(status) && WEXITSTATUS(status) == CHILD_MOUNT)
			SKIP(return, "test requires a FAT filesystem");
		ASSERT_EQ(status, 0);
	}
	for (int i = 0; i < 2; i++) {
		snprintf(dev[i], sizeof(dev[i]), "/dev/loop%d", n[i]);
		snprintf(sysfs[i], sizeof(sysfs[i]), "/sys/block/loop%d/loop/backing_file", n[i]);
	}

	/* step one: the mount with the first loop mount below it goes */
	ASSERT_EQ(rmdir("/mnt_dir/vol"), 0);

	/* step two: the other one, with the loop mount whose image is on the first */
	ASSERT_EQ(rmdir("/mnt_dir/vol2"), 0);

	/* the child leaves: its namespace and every reference it held are gone */
	ASSERT_EQ(write(to_child[1], "", 1), 1);
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	ASSERT_EQ(status, 0);
	close(to_parent[0]);
	close(to_child[1]);

	/* nothing can reach the four mounts any more */
	for (int i = 0; i < 2; i++) {
		ASSERT_FALSE(mounted_anywhere(dev[i]));
		strcpy(self->dev, dev[i]);
		strcpy(self->sysfs, sysfs[i]);
		assert_loop_released(_metadata, self);
	}
}

/*
 * The holders. Each keeps a file or a path on a mount P for as long as
 * its own filesystem or device lives, and each has that filesystem or
 * device mounted at C below P. Once another namespace's rmdir has
 * detached P with C connected below it and the child is gone, P is owned
 * by nobody, C by P, and P is kept by whatever the holder still holds.
 *
 * P is a loop mount so that its death can be observed: the loop device
 * gives its backing file up when P's superblock goes. The image sits on
 * a tmpfs next to P, not above it, so the loop device's own reference is
 * not part of the picture.
 */
enum holder {
	HOLDER_AUTOFS,		/* a FIFO on P as the daemon's pipe */
	HOLDER_ZRAM,		/* a device node on P as the writeback device */
	HOLDER_ECRYPTFS,	/* a directory on P as the lower directory */
	HOLDER_BINFMT_MISC,	/* an executable on P as an 'F' interpreter */
	HOLDER_FUSE,		/* a file on P as a passthrough backing file */
	HOLDER_ZLOOP,		/* a directory on P for the zone files */
	HOLDER_GADGET,		/* a file on P as a mass storage LUN, over dummy_hcd */
	HOLDER_MD,		/* a file on P as an array's bitmap file */
};

#define HOLDER_IMG	"/mnt_dir/img/p.img"
#define HOLDER_MNT	"/mnt_dir/p"
#define HOLDER_BELOW	"/mnt_dir/p/c"

static int write_file(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, s, strlen(s));
	close(fd);
	return n == (ssize_t)strlen(s) ? 0 : -1;
}

/* Bind a free loop device to @img; the device number. */
static int loop_attach(const char *img)
{
	int ifd, n;

	ifd = open(img, O_RDWR);
	if (ifd < 0)
		return -1;
	n = loop_bind(ifd);
	close(ifd);
	return n;
}

static int holder_autofs(void)
{
	char opts[64];
	int pfd;

	if (mkfifo(HOLDER_MNT "/pipe", 0600))
		return CHILD_HOLDER;
	pfd = open(HOLDER_MNT "/pipe", O_RDWR);
	if (pfd < 0)
		return CHILD_HOLDER;
	snprintf(opts, sizeof(opts), "fd=%d,minproto=5,maxproto=5", pfd);
	if (mount("autofs", HOLDER_BELOW, "autofs", 0, opts))
		return errno == ENODEV ? CHILD_SKIP : CHILD_HOLDER;
	close(pfd);	/* the mount keeps its own */
	return CHILD_OK;
}

/*
 * zram's writeback device has to be a block device node, and that node has
 * to be on P. Point it at a second loop device.
 */
static int holder_zram(void)
{
	char buf[64];
	int fd, n;

	if (access("/sys/block/zram0/backing_dev", W_OK))
		return CHILD_SKIP;
	/* somebody else's device, leave it alone */
	if (read_sysfs("/sys/block/zram0/initstate", buf, sizeof(buf)) || buf[0] != '0')
		return CHILD_SKIP;
	fd = open("/mnt_dir/img/wb.img", O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0 || ftruncate(fd, IMAGE_SIZE))
		return CHILD_HOLDER;
	close(fd);
	n = loop_attach("/mnt_dir/img/wb.img");
	if (n < 0)
		return CHILD_HOLDER;
	if (mknod(HOLDER_MNT "/wbdev", S_IFBLK | 0600, makedev(7, n)))
		return CHILD_HOLDER;
	if (write_file("/sys/block/zram0/backing_dev", HOLDER_MNT "/wbdev"))
		return CHILD_HOLDER;
	snprintf(buf, sizeof(buf), "%d", 4 * 1024 * 1024);
	if (write_file("/sys/block/zram0/disksize", buf))
		return CHILD_HOLDER;
	fd = open("/dev/zram0", O_RDWR);
	if (fd < 0 || write_fat12_4k(fd, 1024))	/* zram has 4 KiB blocks */
		return CHILD_HOLDER;
	close(fd);
	if (mount("/dev/zram0", HOLDER_BELOW, "vfat", 0, NULL) &&
	    mount("/dev/zram0", HOLDER_BELOW, "msdos", 0, NULL))
		return CHILD_HOLDER;
	return CHILD_OK;
}

/* The kernel's auth token layout, which userspace has to match byte for byte. */
struct ecryptfs_auth_tok {
	__u16 version;
	__u16 token_type;
	__u32 flags;
	struct {
		__u32 flags, encrypted_key_size, decrypted_key_size;
		__u8 encrypted_key[512], decrypted_key[64];
	} session_key;
	__u8 reserved[32];
	struct {
		__u32 password_bytes;
		__s32 hash_algo;
		__u32 hash_iterations, session_key_encryption_key_bytes, flags;
		__u8 session_key_encryption_key[64];
		__u8 signature[17];
		__u8 salt[8];
	} password;
} __attribute__((packed));

#define ECRYPTFS_SIG	"0123456789abcdef"

static int holder_ecryptfs(void)
{
	struct ecryptfs_auth_tok tok = {
		.version = 0x0004,
		.token_type = 0,		/* ECRYPTFS_PASSWORD */
		.password.session_key_encryption_key_bytes = 16,
		.password.flags = 0x02,		/* ECRYPTFS_SESSION_KEY_ENCRYPTION_KEY_SET */
		.password.signature = ECRYPTFS_SIG,
	};

	if (syscall(__NR_add_key, "user", ECRYPTFS_SIG, &tok, sizeof(tok),
		    KEY_SPEC_SESSION_KEYRING) < 0)
		return CHILD_HOLDER;
	if (mkdir(HOLDER_MNT "/lower", 0755))
		return CHILD_HOLDER;
	if (mount(HOLDER_MNT "/lower", HOLDER_BELOW, "ecryptfs", 0,
		  "ecryptfs_sig=" ECRYPTFS_SIG ",ecryptfs_cipher=aes,ecryptfs_key_bytes=16"))
		return errno == ENODEV ? CHILD_SKIP : CHILD_HOLDER;
	return CHILD_OK;
}

/*
 * binfmt_misc instances are per user namespace, so the mount below P is
 * made from a new one, which gets a copy of P.
 */
static int holder_binfmt_misc(void)
{
	char buf[4096];
	int in, out;
	ssize_t n;

	in = open("/proc/self/exe", O_RDONLY);
	out = open(HOLDER_MNT "/interp", O_WRONLY | O_CREAT | O_EXCL, 0755);
	if (in < 0 || out < 0)
		return CHILD_HOLDER;
	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, n) != n)
			return CHILD_HOLDER;
	close(in);
	close(out);

	if (unshare(CLONE_NEWUSER | CLONE_NEWNS))
		return errno == EINVAL ? CHILD_SKIP : CHILD_HOLDER;
	if (write_file("/proc/self/setgroups", "deny") ||
	    write_file("/proc/self/uid_map", "0 0 1") ||
	    write_file("/proc/self/gid_map", "0 0 1"))
		return CHILD_HOLDER;
	if (mount("binfmt_misc", HOLDER_BELOW, "binfmt_misc", 0, NULL))
		return errno == ENODEV ? CHILD_SKIP : CHILD_HOLDER;
	if (write_file(HOLDER_BELOW "/register", ":cycle:E::cyc::" HOLDER_MNT "/interp:F"))
		return CHILD_HOLDER;
	return CHILD_OK;
}

/*
 * A fuse server that only ever answers FUSE_INIT, with passthrough on, and
 * then registers a file on P as a backing file. The registration alone
 * makes the fuse superblock hold the file.
 */
static int holder_fuse(void)
{
	struct fuse_backing_map map = {};
	struct fuse_in_header *ih;
	struct fuse_init_out init = {
		.major = FUSE_KERNEL_VERSION,
		.minor = FUSE_KERNEL_MINOR_VERSION,
		.flags = FUSE_INIT_EXT,
		.flags2 = FUSE_PASSTHROUGH >> 32,
		.max_write = 4096,
		.max_stack_depth = 1,
	};
	struct fuse_out_header oh = { .len = sizeof(oh) + sizeof(init) };
	struct iovec iov[2] = { { &oh, sizeof(oh) }, { &init, sizeof(init) } };
	char opts[64], buf[8192];
	int ffd, bfd;
	ssize_t n;

	bfd = open(HOLDER_MNT "/backing", O_RDWR | O_CREAT | O_EXCL, 0600);
	if (bfd < 0)
		return CHILD_HOLDER;
	ffd = open("/dev/fuse", O_RDWR);
	if (ffd < 0)
		return CHILD_SKIP;
	snprintf(opts, sizeof(opts), "fd=%d,rootmode=40000,user_id=0,group_id=0", ffd);
	if (mount("fuse", HOLDER_BELOW, "fuse", 0, opts))
		return errno == ENODEV ? CHILD_SKIP : CHILD_HOLDER;

	n = read(ffd, buf, sizeof(buf));
	ih = (void *)buf;
	if (n < (ssize_t)sizeof(*ih) || ih->opcode != FUSE_INIT)
		return CHILD_HOLDER;
	oh.unique = ih->unique;
	if (writev(ffd, iov, 2) != (ssize_t)oh.len)
		return CHILD_HOLDER;

	map.fd = bfd;
	if (ioctl(ffd, FUSE_DEV_IOC_BACKING_OPEN, &map) < 0)
		return errno == EPERM ? CHILD_SKIP : CHILD_HOLDER;
	close(bfd);	/* the connection keeps its own */
	return CHILD_OK;	/* ffd stays open until the child exits */
}

/* Wait for a device node the kernel is about to create. */
static int open_when_there(const char *dev, int flags, int ms)
{
	int fd;

	for (; ms > 0; ms -= 100) {
		fd = open(dev, flags);
		if (fd >= 0)
			return fd;
		usleep(100000);
	}
	return -1;
}

/*
 * zloop keeps every zone file open. One conventional zone is enough for a
 * FAT image, the sequential one stays empty.
 */
static int holder_zloop(void)
{
	int fd;

	if (access("/dev/zloop-control", W_OK))
		return CHILD_SKIP;
	if (mkdir(HOLDER_MNT "/zl", 0755) || mkdir(HOLDER_MNT "/zl/0", 0755))
		return CHILD_HOLDER;
	if (write_file("/dev/zloop-control",
		       "add id=0,capacity_mb=8,zone_size_mb=4,conv_zones=1,base_dir=" HOLDER_MNT "/zl"))
		return CHILD_HOLDER;
	fd = open_when_there("/dev/zloop0", O_RDWR, 5000);
	if (fd < 0 || write_fat12_4k(fd, 1024))	/* 4 KiB blocks, like P */
		return CHILD_HOLDER;
	close(fd);
	if (mount("/dev/zloop0", HOLDER_BELOW, "vfat", 0, NULL) &&
	    mount("/dev/zloop0", HOLDER_BELOW, "msdos", 0, NULL))
		return CHILD_HOLDER;
	return CHILD_OK;
}

#define GADGET	"/sys/kernel/config/usb_gadget/g1"

/* The disk usb-storage created for the gadget, by the LUN's inquiry string. */
static int find_gadget_disk(char *dev, size_t len, int ms)
{
	char path[PATH_MAX], model[64];
	struct dirent *de;
	DIR *d;

	for (; ms > 0; ms -= 100, usleep(100000)) {
		d = opendir("/sys/block");
		if (!d)
			return -1;
		while ((de = readdir(d))) {
			if (strncmp(de->d_name, "sd", 2))
				continue;
			snprintf(path, sizeof(path), "/sys/block/%s/device/model", de->d_name);
			if (read_sysfs(path, model, sizeof(model)) ||
			    strncmp(model, "File-Stor Gadget", 16))
				continue;
			snprintf(dev, len, "/dev/%s", de->d_name);
			closedir(d);
			return 0;
		}
		closedir(d);
	}
	return -1;
}

/*
 * A mass storage gadget bound to the dummy UDC, so that this kernel is
 * also the USB host that sees the LUN as a SCSI disk. sd locks the
 * medium on open, which is what keeps the LUN's file from being ejected.
 */
#define GADGET_STEP(x) do {						\
	if (x) {							\
		fprintf(stderr, "gadget: %s failed: %s\n", #x, strerror(errno)); \
		return CHILD_HOLDER;					\
	}								\
} while (0)

static int holder_gadget(void)
{
	char dev[PATH_MAX];
	int fd;

	if (access("/sys/kernel/config", F_OK) ||
	    (mount("configfs", "/sys/kernel/config", "configfs", 0, NULL) && errno != EBUSY))
		return CHILD_SKIP;
	if (access("/sys/kernel/config/usb_gadget", F_OK) ||
	    access("/sys/class/udc/dummy_udc.0", F_OK))
		return CHILD_SKIP;

	fd = open(HOLDER_MNT "/lun.img", O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0 || write_fat12(fd))
		return CHILD_HOLDER;
	close(fd);

	GADGET_STEP(mkdir(GADGET, 0755));
	GADGET_STEP(write_file(GADGET "/idVendor", "0x1d6b"));
	GADGET_STEP(write_file(GADGET "/idProduct", "0x0104"));
	GADGET_STEP(mkdir(GADGET "/strings/0x409", 0755));
	GADGET_STEP(write_file(GADGET "/strings/0x409/serialnumber", "1"));
	GADGET_STEP(write_file(GADGET "/strings/0x409/manufacturer", "kselftest"));
	GADGET_STEP(write_file(GADGET "/strings/0x409/product", "cycle"));
	GADGET_STEP(mkdir(GADGET "/configs/c.1", 0755));
	GADGET_STEP(mkdir(GADGET "/configs/c.1/strings/0x409", 0755));
	GADGET_STEP(write_file(GADGET "/configs/c.1/strings/0x409/configuration", "c"));
	GADGET_STEP(mkdir(GADGET "/functions/mass_storage.0", 0755));
	GADGET_STEP(write_file(GADGET "/functions/mass_storage.0/lun.0/removable", "1"));
	GADGET_STEP(write_file(GADGET "/functions/mass_storage.0/lun.0/file", HOLDER_MNT "/lun.img"));
	GADGET_STEP(symlink(GADGET "/functions/mass_storage.0", GADGET "/configs/c.1/mass_storage.0"));
	GADGET_STEP(write_file(GADGET "/UDC", "dummy_udc.0"));

	/* usb-storage waits a second before it scans the device */
	GADGET_STEP(find_gadget_disk(dev, sizeof(dev), 15000));
	fd = open_when_there(dev, O_RDONLY, 5000);
	GADGET_STEP(fd < 0);
	close(fd);
	if (mount(dev, HOLDER_BELOW, "vfat", 0, NULL) &&
	    mount(dev, HOLDER_BELOW, "msdos", 0, NULL))
		return CHILD_HOLDER;
	return CHILD_OK;
}

#define BITMAP_MAGIC	0x6d746962

/*
 * A RAID1 of one loop device, not persistent, with its bitmap in a file
 * on P. The bitmap file needs a superblock the kernel accepts; sync_size,
 * uuid and events are not looked at for a non-persistent array.
 */
static int holder_md(void)
{
	struct {
		__u32 magic, version;
		__u8 uuid[16];
		__u64 events, events_cleared, sync_size;
		__u32 state, chunksize, daemon_sleep, write_behind;
	} bsb = {
		.magic = BITMAP_MAGIC,
		.version = 4,
		.chunksize = 64 * 1024,
		.daemon_sleep = 5,
	};
	mdu_array_info_t info = {
		.level = 1,
		.raid_disks = 1,
		.size = 8 * 1024,		/* KiB */
		.not_persistent = 1,
	};
	mdu_disk_info_t disk = {
		.major = 7,
		.state = (1 << MD_DISK_ACTIVE) | (1 << MD_DISK_SYNC),
	};
	int fd, mdfd, bfd, n;
	char buf[4096];

	fd = open("/mnt_dir/img/md.img", O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0 || ftruncate(fd, 8 * 1024 * 1024))
		return CHILD_HOLDER;
	close(fd);
	n = loop_attach("/mnt_dir/img/md.img");
	if (n < 0)
		return CHILD_HOLDER;
	disk.minor = n;

	bfd = open(HOLDER_MNT "/bitmap", O_RDWR | O_CREAT | O_EXCL, 0600);
	if (bfd < 0 || write(bfd, &bsb, sizeof(bsb)) != sizeof(bsb) || ftruncate(bfd, 4096))
		return CHILD_HOLDER;

	if (read_sysfs("/proc/mdstat", buf, sizeof(buf)) || !strstr(buf, "[raid1]")) {
		fprintf(stderr, "md: no raid1 personality: %s\n", buf);
		return CHILD_SKIP;
	}
	if (access("/dev/md0", F_OK) && mknod("/dev/md0", S_IFBLK | 0600, makedev(9, 0)))
		return CHILD_HOLDER;
	mdfd = open("/dev/md0", O_RDWR);
	if (mdfd < 0)
		return CHILD_SKIP;
	/* the bitmap ops are only installed once a bitmap type is chosen */
	write_file("/sys/block/md0/md/bitmap_type", "bitmap");
	/* EBUSY: somebody else's array, leave it alone */
	if (ioctl(mdfd, SET_ARRAY_INFO, &info))
		return errno == EBUSY ? CHILD_SKIP : CHILD_HOLDER;
	if (ioctl(mdfd, ADD_NEW_DISK, &disk))
		return CHILD_HOLDER;
	/* attach the bitmap file before the array runs, the way mdadm does */
	if (ioctl(mdfd, SET_BITMAP_FILE, bfd)) {
		fprintf(stderr, "md: SET_BITMAP_FILE: %s\n", strerror(errno));
		return errno == EINVAL ? CHILD_SKIP : CHILD_HOLDER;
	}
	close(bfd);	/* the array keeps its own */
	if (ioctl(mdfd, RUN_ARRAY, NULL)) {
		fprintf(stderr, "md: RUN_ARRAY: %s\n", strerror(errno));
		return CHILD_HOLDER;
	}
	if (write_fat12(mdfd))
		return CHILD_HOLDER;
	close(mdfd);
	if (mount("/dev/md0", HOLDER_BELOW, "vfat", 0, NULL) &&
	    mount("/dev/md0", HOLDER_BELOW, "msdos", 0, NULL))
		return CHILD_HOLDER;
	return CHILD_OK;
}

/*
 * In its own mount namespace the child puts the image on a tmpfs next to
 * P, mounts P from a loop device, and sets the holder up with its own
 * filesystem or device mounted at C below P.
 */
static int holder_child(int to_parent, int from_parent, enum holder holder)
{
	bool fifo = holder == HOLDER_AUTOFS || holder == HOLDER_ZRAM;
	/* room for a 4 MiB zone file or a floppy image on P */
	bool big = holder == HOLDER_ZLOOP || holder == HOLDER_GADGET;
	const char *type = fifo ? "minix" : "vfat";
	char dev[32], c;
	int ifd, n, ret;

	if (unshare(CLONE_NEWNS) || mount("", "/", NULL, MS_REC | MS_PRIVATE, NULL))
		return CHILD_NS;
	if (mkdir("/mnt_dir/img", 0755) || mount("tmpfs", "/mnt_dir/img", "tmpfs", 0, NULL))
		return CHILD_NS;

	ifd = open(HOLDER_IMG, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (ifd < 0 || (fifo ? write_minix(ifd) :
			big ? write_fat12_4k(ifd, 3072) : write_fat12(ifd)))
		return CHILD_IMAGE;
	close(ifd);
	n = loop_attach(HOLDER_IMG);
	if (n < 0)
		return CHILD_LOOP;
	snprintf(dev, sizeof(dev), "/dev/loop%d", n);
	if (mkdir(HOLDER_MNT, 0755))
		return CHILD_MOUNT;
	if (mount(dev, HOLDER_MNT, type, 0, NULL) &&
	    (fifo || mount(dev, HOLDER_MNT, "msdos", 0, NULL)))
		return fifo && errno == ENODEV ? CHILD_SKIP : CHILD_MOUNT;
	if (mkdir(HOLDER_BELOW, 0755))
		return CHILD_MOUNT;

	switch (holder) {
	case HOLDER_AUTOFS:
		ret = holder_autofs();
		break;
	case HOLDER_ZRAM:
		ret = holder_zram();
		break;
	case HOLDER_ECRYPTFS:
		ret = holder_ecryptfs();
		break;
	case HOLDER_BINFMT_MISC:
		ret = holder_binfmt_misc();
		break;
	case HOLDER_FUSE:
		ret = holder_fuse();
		break;
	case HOLDER_ZLOOP:
		ret = holder_zloop();
		break;
	case HOLDER_GADGET:
		ret = holder_gadget();
		break;
	case HOLDER_MD:
		ret = holder_md();
		break;
	default:
		ret = CHILD_HOLDER;
	}
	if (ret != CHILD_OK)
		return ret;

	if (write(to_parent, &n, sizeof(n)) != sizeof(n))
		return CHILD_PIPE;
	if (read(from_parent, &c, 1) != 1)
		return CHILD_PIPE;
	return CHILD_OK;
}

/*
 * A device keeps its file for as long as it is configured, so once nothing
 * below the dead mount is left the device has to be told to let go. With
 * the cycle unbroken C's filesystem still holds the device and every one
 * of these refuses.
 */
static int holder_let_go_once(enum holder holder)
{
	int fd, ret;

	switch (holder) {
	case HOLDER_ZRAM:
		return write_file("/sys/block/zram0/reset", "1");
	case HOLDER_ZLOOP:
		return write_file("/dev/zloop-control", "remove id=0");
	case HOLDER_GADGET:
		if (mount("configfs", "/sys/kernel/config", "configfs", 0, NULL) && errno != EBUSY)
			return -1;
		/* a zero-length write is a no-op for configfs; a newline ejects */
		return write_file(GADGET "/functions/mass_storage.0/lun.0/file", "\n");
	case HOLDER_MD:
		fd = open("/dev/md0", O_RDONLY);
		if (fd < 0)
			return -1;
		ret = ioctl(fd, STOP_ARRAY);
		close(fd);
		return ret;
	default:
		return 0;
	}
}

/*
 * The release of C's filesystem may still be in flight when the child is
 * gone, and a device that is still held refuses to let go, so try for a
 * while. With the cycle unbroken it refuses for good.
 */
static void holder_let_go(enum holder holder)
{
	for (int i = 0; i < 50; i++) {
		if (!holder_let_go_once(holder))
			return;
		usleep(100000);
	}
}

/*
 * rmdir of /mnt_dir/p from here, where it is a plain directory, detaches
 * P in the child's namespace with C connected below it. Once the child is
 * gone the holder's file on P is the only thing left that refers to P,
 * and it is dropped only when C's filesystem dies, which waits for P.
 * The loop device backing P tells whether that resolved.
 */
static void holder_cycle(struct __test_metadata *_metadata,
			 FIXTURE_DATA(loop_cycle) *self, enum holder holder)
{
	int to_parent[2], from_parent[2], status, n;
	pid_t pid;

	ASSERT_EQ(pipe(to_parent), 0);
	ASSERT_EQ(pipe(from_parent), 0);
	pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		close(to_parent[0]);
		close(from_parent[1]);
		_exit(holder_child(to_parent[1], from_parent[0], holder));
	}
	close(to_parent[1]);
	close(from_parent[0]);

	if (read(to_parent[0], &n, sizeof(n)) != sizeof(n)) {
		waitpid(pid, &status, 0);
		if (WIFEXITED(status) && WEXITSTATUS(status) == CHILD_SKIP)
			SKIP(return, "the kernel lacks what this holder needs");
		ASSERT_TRUE(false)
			TH_LOG("child failed to set the holder up: exit status %d",
			       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	}
	snprintf(self->dev, sizeof(self->dev), "/dev/loop%d", n);
	snprintf(self->sysfs, sizeof(self->sysfs), "/sys/block/loop%d/loop/backing_file", n);

	ASSERT_EQ(rmdir(HOLDER_MNT), 0);
	ASSERT_EQ(write(from_parent[1], "x", 1), 1);
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	ASSERT_EQ(WEXITSTATUS(status), CHILD_OK);

	holder_let_go(holder);
	assert_loop_released(_metadata, self);
	if (holder == HOLDER_GADGET)
		write_file(GADGET "/UDC", "\n");
}

TEST_F(loop_cycle, autofs_pipe_on_dead_mount_released)
{
	holder_cycle(_metadata, self, HOLDER_AUTOFS);
}

TEST_F(loop_cycle, zram_writeback_node_on_dead_mount_released)
{
	holder_cycle(_metadata, self, HOLDER_ZRAM);
}

TEST_F(loop_cycle, ecryptfs_lower_on_dead_mount_released)
{
	holder_cycle(_metadata, self, HOLDER_ECRYPTFS);
}

TEST_F(loop_cycle, binfmt_misc_interpreter_on_dead_mount_released)
{
	holder_cycle(_metadata, self, HOLDER_BINFMT_MISC);
}

TEST_F(loop_cycle, fuse_backing_file_on_dead_mount_released)
{
	holder_cycle(_metadata, self, HOLDER_FUSE);
}

TEST_F(loop_cycle, zloop_zone_files_on_dead_mount_released)
{
	holder_cycle(_metadata, self, HOLDER_ZLOOP);
}

TEST_F(loop_cycle, mass_storage_lun_on_dead_mount_released)
{
	holder_cycle(_metadata, self, HOLDER_GADGET);
}

TEST_F(loop_cycle, md_bitmap_file_on_dead_mount_released)
{
	holder_cycle(_metadata, self, HOLDER_MD);
}

TEST_HARNESS_MAIN
