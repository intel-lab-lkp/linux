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

/*
 * Helper functions for cgroup
 */

#define CGROUP_BASE "/sys/fs/cgroup/"
#define CGROUP_TEST "mshare-test-XXXXXX"

bool is_cgroup_v2;

__attribute__((constructor))
void get_cgroup_version(void)
{
	if (access(CGROUP_BASE "cgroup.controllers", F_OK) == 0)
		is_cgroup_v2 = true;
}

int create_mshare_test_cgroup(char *cgroup, size_t len)
{
	if (is_cgroup_v2)
		snprintf(cgroup, len, "%s/%s", CGROUP_BASE, CGROUP_TEST);
	else
		snprintf(cgroup, len, "%s/memory/%s", CGROUP_BASE, CGROUP_TEST);

	char *path = mkdtemp(cgroup);

	if (!path) {
		perror("mkdtemp");
		return -1;
	}

	return 0;
}

int remove_cgroup(char *cgroup)
{
	return rmdir(cgroup);
}

int write_data_to_cgroup(char *cgroup, char *file, char *data)
{
	char filename[128];
	int fd;
	int ret;

	snprintf(filename, sizeof(filename), "%s/%s", cgroup, file);
	fd = open(filename, O_RDWR);

	if (fd == -1)
		return -1;

	ret = write(fd, data, strlen(data));
	close(fd);

	return ret;
}

int attach_to_cgroup(char *cgroup)
{
	char pid_str[32];

	snprintf(pid_str, sizeof(pid_str), "%d", getpid());
	return write_data_to_cgroup(cgroup, "cgroup.procs", pid_str);
}

/*
 * Simplely, just move the pid to root memcg as avoid
 * complicated consideration.
 */
int dettach_from_cgroup(char *cgroup)
{
	char pid_str[32];
	char *root_memcg;

	if (is_cgroup_v2)
		root_memcg = CGROUP_BASE;
	else
		root_memcg = CGROUP_BASE "memory";

	snprintf(pid_str, sizeof(pid_str), "%d", getpid());
	return write_data_to_cgroup(root_memcg, "cgroup.procs", pid_str);
}

size_t read_data_from_cgroup(char *cgroup, char *file, char *field)
{
	char filename[128];
	FILE *fp;
	char line[80];
	size_t size = -1;

	snprintf(filename, sizeof(filename), "%s/%s", cgroup, file);
	fp = fopen(filename, "r");
	if (!fp) {
		perror("fopen");
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (!strncmp(line, field, strlen(field))) {
			char *value = line + strlen(field) + 1;

			size = atol(value);
			break;
		}
	}

	fclose(fp);

	return size;
}

size_t read_swap_from_cgroup(char *cgroup)
{
	if (is_cgroup_v2)
		return read_data_from_cgroup(cgroup, "memory.stat", "pswpout");
	else
		return read_data_from_cgroup(cgroup, "memory.stat", "swap");
}

size_t read_huge_from_cgroup(char *cgroup)
{
	if (is_cgroup_v2)
		return read_data_from_cgroup(cgroup, "memory.stat", "file_thp")
		     + read_data_from_cgroup(cgroup, "memory.stat", "anon_thp")
		     + read_data_from_cgroup(cgroup, "memory.stat", "shmem_thp");
	else
		return read_data_from_cgroup(cgroup, "memory.stat", "rss_huge");
}
