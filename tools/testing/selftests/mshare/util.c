// SPDX-License-Identifier: GPL-2.0

#include <linux/msharefs.h>
#include <stdio.h>
#include <mntent.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

/*
 *  Helper functions for mounting msharefs
 */

#define MOUNT_POINT "/sys/fs/mshare"
#define FS_TYPE     "msharefs"

bool is_msharefs_mounted(void)
{
	FILE *fp;
	struct mntent *ent;
	bool found = false;

	fp = setmntent("/proc/mounts", "r");
	if (!fp) {
		perror("setmntent");
		exit(1);
	}

	while ((ent = getmntent(fp)) != NULL) {
		if (strcmp(ent->mnt_dir, MOUNT_POINT) == 0 &&
			strcmp(ent->mnt_type, FS_TYPE) == 0) {
			found = true;
			break;
		}
	}

	endmntent(fp);
	return found;
}

bool msharefs_premounted;

__attribute__((constructor))
void mount_sharefs(void)
{
	msharefs_premounted = is_msharefs_mounted();
	if (msharefs_premounted)
		return;

	if (mount(FS_TYPE, MOUNT_POINT, FS_TYPE, 0, NULL) != 0) {
		perror("mount");
		exit(1);
	}
}

__attribute__((destructor))
void umount_sharefs(void)
{
	if (!msharefs_premounted && umount(MOUNT_POINT) != 0) {
		perror("umount");
		exit(1);
	}
}

/*
 *  Helper functions for mshare files
 */

#define MSHARE_INFO MOUNT_POINT "/mshare_info"
#define MSHARE_TEST MOUNT_POINT "/mshare-test-XXXXXX"

size_t mshare_get_info(void)
{
	char req[128];
	size_t size;
	int fd;

	fd = open(MSHARE_INFO, O_RDONLY);
	if (fd == -1)
		return -1;

	read(fd, req, sizeof(req));
	size = atoll(req);
	close(fd);

	return size;
}

int create_mshare_file(char *filename, size_t len)
{
	int fd;

	strncpy(filename, MSHARE_TEST, len - 1);
	fd = mkstemp(filename);

	return fd;
}


int mshare_ioctl_mapping(int fd, size_t size, int flags)
{
	struct mshare_create mcreate;

	mcreate.region_offset = 0;
	mcreate.size = size;
	mcreate.offset = 0;
	mcreate.prot = PROT_READ | PROT_WRITE;
	mcreate.flags = flags;
	mcreate.fd = -1;

	return ioctl(fd, MSHAREFS_CREATE_MAPPING, &mcreate);
}

int mshare_ioctl_munmap(int fd, size_t size)
{
	struct mshare_unmap munmap;

	munmap.region_offset = 0;
	munmap.size = size;

	return ioctl(fd, MSHAREFS_UNMAP, &munmap);
}
