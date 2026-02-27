// SPDX-License-Identifier: GPL-2.0-only
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <linux/limits.h>

#include <libvfio.h>

static int sysfs_val_get_int(const char *component, const char *name,
			     const char *file)
{
	char path[PATH_MAX];
	char buf[32];
	int ret;
	int fd;

	snprintf_assert(path, PATH_MAX, "/sys/bus/pci/%s/%s/%s", component, name, file);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return fd;

	VFIO_ASSERT_GT(read(fd, buf, ARRAY_SIZE(buf)), 0);
	VFIO_ASSERT_EQ(close(fd), 0);

	errno = 0;
	ret = strtol(buf, NULL, 0);
	VFIO_ASSERT_EQ(errno, 0, "sysfs path \"%s\" is not an integer: \"%s\"\n", path, buf);

	return ret;
}

static void sysfs_val_set(const char *component, const char *name,
			  const char *file, const char *val)
{
	char path[PATH_MAX];
	int fd;

	snprintf_assert(path, PATH_MAX, "/sys/bus/pci/%s/%s/%s", component, name, file);
	VFIO_ASSERT_GT(fd = open(path, O_WRONLY), 0);

	VFIO_ASSERT_EQ(write(fd, val, strlen(val)), strlen(val));
	VFIO_ASSERT_EQ(close(fd), 0);
}

static int sysfs_device_val_get(const char *bdf, const char *file)
{
	return sysfs_val_get_int("devices", bdf, file);
}

static void sysfs_device_val_set(const char *bdf, const char *file, const char *val)
{
	sysfs_val_set("devices", bdf, file, val);
}

static void sysfs_device_val_set_int(const char *bdf, const char *file, int val)
{
	char val_str[32];

	snprintf_assert(val_str, sizeof(val_str), "%d", val);
	sysfs_device_val_set(bdf, file, val_str);
}

int sysfs_sriov_totalvfs_get(const char *bdf)
{
	return sysfs_device_val_get(bdf, "sriov_totalvfs");
}

int sysfs_sriov_numvfs_get(const char *bdf)
{
	return sysfs_device_val_get(bdf, "sriov_numvfs");
}

void sysfs_sriov_numvfs_set(const char *bdf, int numvfs)
{
	sysfs_device_val_set_int(bdf, "sriov_numvfs", numvfs);
}

char *sysfs_sriov_vf_bdf_get(const char *pf_bdf, int i)
{
	char vf_path[PATH_MAX];
	char path[PATH_MAX];
	char *out_vf_bdf;
	int ret;

	out_vf_bdf = calloc(16, sizeof(char));
	VFIO_ASSERT_NOT_NULL(out_vf_bdf);

	snprintf_assert(path, PATH_MAX, "/sys/bus/pci/devices/%s/virtfn%d", pf_bdf, i);

	ret = readlink(path, vf_path, PATH_MAX);
	VFIO_ASSERT_NE(ret, -1);

	ret = sscanf(basename(vf_path), "%s", out_vf_bdf);
	VFIO_ASSERT_EQ(ret, 1);

	return out_vf_bdf;
}

unsigned int sysfs_iommu_group_get(const char *bdf)
{
	char dev_iommu_group_path[PATH_MAX];
	char path[PATH_MAX];
	unsigned int group;
	int ret;

	snprintf_assert(path, PATH_MAX, "/sys/bus/pci/devices/%s/iommu_group", bdf);

	ret = readlink(path, dev_iommu_group_path, sizeof(dev_iommu_group_path));
	VFIO_ASSERT_NE(ret, -1, "Failed to get the IOMMU group for device: %s\n", bdf);

	ret = sscanf(basename(dev_iommu_group_path), "%u", &group);
	VFIO_ASSERT_EQ(ret, 1, "Failed to get the IOMMU group for device: %s\n", bdf);

	return group;
}

char *sysfs_driver_get(const char *bdf)
{
	char driver_path[PATH_MAX];
	char path[PATH_MAX];
	char *out_driver;
	int ret;

	out_driver = calloc(64, sizeof(char));
	VFIO_ASSERT_NOT_NULL(out_driver);

	snprintf_assert(path, PATH_MAX, "/sys/bus/pci/devices/%s/driver", bdf);
	ret = readlink(path, driver_path, PATH_MAX);
	if (ret == -1) {
		free(out_driver);

		if (errno == ENOENT)
			return NULL;

		VFIO_FAIL("Failed to read %s\n", path);
	}

	strcpy(out_driver, basename(driver_path));
	return out_driver;
}
