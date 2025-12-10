// SPDX-License-Identifier: GPL-2.0-only
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <linux/limits.h>

#include <libvfio.h>

static int sysfs_get_val(const char *component, const char *name,
			 const char *file)
{
	char path[PATH_MAX] = {0};
	char buf[32] = {0};
	int fd;

	snprintf(path, PATH_MAX, "/sys/bus/pci/%s/%s/%s", component, name, file);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return fd;

	VFIO_ASSERT_GT(read(fd, buf, ARRAY_SIZE(buf)), 0);
	VFIO_ASSERT_EQ(close(fd), 0);

	return strtol(buf, NULL, 0);
}

static void sysfs_set_val(const char *component, const char *name,
			  const char *file, const char *val)
{
	char path[PATH_MAX] = {0};
	int fd;

	snprintf(path, PATH_MAX, "/sys/bus/pci/%s/%s/%s", component, name, file);
	VFIO_ASSERT_GT(fd = open(path, O_WRONLY), 0);

	VFIO_ASSERT_EQ(write(fd, val, strlen(val)), strlen(val));
	VFIO_ASSERT_EQ(close(fd), 0);
}

static int sysfs_get_device_val(const char *bdf, const char *file)
{
	sysfs_get_val("devices", bdf, file);
}

static void sysfs_set_device_val(const char *bdf, const char *file, const char *val)
{
	sysfs_set_val("devices", bdf, file, val);
}

static void sysfs_set_driver_val(const char *driver, const char *file, const char *val)
{
	sysfs_set_val("drivers", driver, file, val);
}

static void sysfs_set_device_val_int(const char *bdf, const char *file, int val)
{
	char val_str[32] = {0};

	snprintf(val_str, sizeof(val_str), "%d", val);
	sysfs_set_device_val(bdf, file, val_str);
}

int sysfs_get_sriov_totalvfs(const char *bdf)
{
	return sysfs_get_device_val(bdf, "sriov_totalvfs");
}

int sysfs_get_sriov_numvfs(const char *bdf)
{
	return sysfs_get_device_val(bdf, "sriov_numvfs");
}

void sysfs_set_sriov_numvfs(const char *bdf, int numvfs)
{
	sysfs_set_device_val_int(bdf, "sriov_numvfs", numvfs);
}

bool sysfs_get_sriov_drivers_autoprobe(const char *bdf)
{
	return (bool)sysfs_get_device_val(bdf, "sriov_drivers_autoprobe");
}

void sysfs_set_sriov_drivers_autoprobe(const char *bdf, bool val)
{
	sysfs_set_device_val_int(bdf, "sriov_drivers_autoprobe", val);
}

void sysfs_bind_driver(const char *bdf, const char *driver)
{
	sysfs_set_driver_val(driver, "bind", bdf);
}

void sysfs_unbind_driver(const char *bdf, const char *driver)
{
	sysfs_set_driver_val(driver, "unbind", bdf);
}

void sysfs_get_sriov_vf_bdf(const char *pf_bdf, int i, char *out_vf_bdf)
{
	char vf_path[PATH_MAX] = {0};
	char path[PATH_MAX] = {0};
	int ret;

	snprintf(path, PATH_MAX, "/sys/bus/pci/devices/%s/virtfn%d", pf_bdf, i);

	ret = readlink(path, vf_path, PATH_MAX);
	VFIO_ASSERT_NE(ret, -1);

	ret = sscanf(basename(vf_path), "%s", out_vf_bdf);
	VFIO_ASSERT_EQ(ret, 1);
}

unsigned int sysfs_get_device_group(const char *bdf)
{
	char dev_iommu_group_path[PATH_MAX] = {0};
	char path[PATH_MAX] = {0};
	unsigned int group;
	int ret;

	snprintf(path, PATH_MAX, "/sys/bus/pci/devices/%s/iommu_group", bdf);

	ret = readlink(path, dev_iommu_group_path, sizeof(dev_iommu_group_path));
	VFIO_ASSERT_NE(ret, -1, "Failed to get the IOMMU group for device: %s\n", bdf);

	ret = sscanf(basename(dev_iommu_group_path), "%u", &group);
	VFIO_ASSERT_EQ(ret, 1, "Failed to get the IOMMU group for device: %s\n", bdf);

	return group;
}

int sysfs_get_driver(const char *bdf, char *out_driver)
{
	char driver_path[PATH_MAX] = {0};
	char path[PATH_MAX] = {0};
	int ret;

	snprintf(path, PATH_MAX, "/sys/bus/pci/devices/%s/driver", bdf);
	ret = readlink(path, driver_path, PATH_MAX);
	if (ret == -1) {
		if (errno == ENOENT)
			return -1;

		VFIO_FAIL("Failed to read %s\n", path);
	}

	ret = sscanf(basename(driver_path), "%s", out_driver);
	VFIO_ASSERT_EQ(ret, 1);

	return 0;
}
