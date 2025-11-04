// SPDX-License-Identifier: GPL-2.0-only
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <linux/limits.h>

#include <vfio_util.h>

#include "../kselftest_harness.h"

#define PCI_SYSFS_PATH "/sys/bus/pci/devices"

#define UUID_1 "52ac9bff-3a88-4fbd-901a-0d767c3b6c97"
#define UUID_2 "88594674-90a0-47a9-aea8-9d9b352ac08a"

static const char *pf_dev_bdf;
static char vf_dev_bdf[16];

struct vfio_pci_device *pf_device;
struct vfio_pci_device *vf_device;

static void test_vfio_pci_container_setup(struct vfio_pci_device *device,
					   const char *bdf,
					   const char *vf_token)
{
	vfio_container_open(device);
	vfio_pci_group_setup(device, bdf);
	vfio_container_set_iommu(device);
	__vfio_container_get_device_fd(device, bdf, vf_token);
}

static int test_vfio_pci_iommufd_setup(struct vfio_pci_device *device,
					const char *bdf, const char *vf_token)
{
	vfio_pci_iommufd_cdev_open(device, bdf);
	vfio_pci_iommufd_iommudev_open(device);
	return __vfio_device_bind_iommufd(device->fd, device->iommufd, vf_token);
}

static struct vfio_pci_device *test_vfio_pci_device_init(const char *bdf,
							  const char *iommu_mode,
							  const char *vf_token,
							  int *out_ret)
{
	struct vfio_pci_device *device;

	device = calloc(1, sizeof(*device));
	VFIO_ASSERT_NOT_NULL(device);

	device->iommu_mode = lookup_iommu_mode(iommu_mode);

	if (iommu_mode_container_path(iommu_mode)) {
		test_vfio_pci_container_setup(device, bdf, vf_token);
		/* The device fd will be -1 in case of mismatched tokens */
		*out_ret = (device->fd < 0);
	} else {
		*out_ret = test_vfio_pci_iommufd_setup(device, bdf, vf_token);
	}

	return device;
}

static void test_vfio_pci_device_cleanup(struct vfio_pci_device *device)
{
	if (device->fd > 0)
		VFIO_ASSERT_EQ(close(device->fd), 0);

	if (device->iommufd) {
		VFIO_ASSERT_EQ(close(device->iommufd), 0);
	} else {
		VFIO_ASSERT_EQ(close(device->group_fd), 0);
		VFIO_ASSERT_EQ(close(device->container_fd), 0);
	}

	free(device);
}

FIXTURE(vfio_pci_sriov_uapi_test) {};

FIXTURE_SETUP(vfio_pci_sriov_uapi_test)
{
	char vf_path[PATH_MAX] = {0};
	char path[PATH_MAX] = {0};
	unsigned int nr_vfs;
	char buf[32] = {0};
	int ret;
	int fd;

	/* Check if SR-IOV is supported by the device */
	snprintf(path, PATH_MAX, "%s/%s/sriov_totalvfs", PCI_SYSFS_PATH, pf_dev_bdf);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "SR-IOV may not be supported by the device\n");
		exit(KSFT_SKIP);
	}

	ASSERT_GT(read(fd, buf, ARRAY_SIZE(buf)), 0);
	ASSERT_EQ(close(fd), 0);
	nr_vfs = strtoul(buf, NULL, 0);
	if (nr_vfs < 0) {
		fprintf(stderr, "SR-IOV may not be supported by the device\n");
		exit(KSFT_SKIP);
	}

	/* Setup VFs, if already not done */
	snprintf(path, PATH_MAX, "%s/%s/sriov_numvfs", PCI_SYSFS_PATH, pf_dev_bdf);
	ASSERT_GT(fd = open(path, O_RDWR), 0);
	ASSERT_GT(read(fd, buf, ARRAY_SIZE(buf)), 0);
	nr_vfs = strtoul(buf, NULL, 0);
	if (nr_vfs == 0)
		ASSERT_EQ(write(fd, "1", 1), 1);
	ASSERT_EQ(close(fd), 0);

	/* Get the BDF of the first VF */
	snprintf(path, PATH_MAX, "%s/%s/virtfn0", PCI_SYSFS_PATH, pf_dev_bdf);
	ret = readlink(path, vf_path, PATH_MAX);
	ASSERT_NE(ret, -1);
	ret = sscanf(basename(vf_path), "%s", vf_dev_bdf);
	ASSERT_EQ(ret, 1);
}

FIXTURE_TEARDOWN(vfio_pci_sriov_uapi_test)
{
}

FIXTURE_VARIANT(vfio_pci_sriov_uapi_test) {
	const char *iommu_mode;
	char *vf_token;
};

#define FIXTURE_VARIANT_ADD_IOMMU_MODE(_iommu_mode, _name, _vf_token)		\
FIXTURE_VARIANT_ADD(vfio_pci_sriov_uapi_test, _iommu_mode ## _ ## _name) {	\
	.iommu_mode = #_iommu_mode,						\
	.vf_token = (_vf_token),						\
}

FIXTURE_VARIANT_ADD_ALL_IOMMU_MODES(same_uuid, UUID_1);
FIXTURE_VARIANT_ADD_ALL_IOMMU_MODES(diff_uuid, UUID_2);
FIXTURE_VARIANT_ADD_ALL_IOMMU_MODES(null_uuid, NULL);

/*
 * PF's token is always set with UUID_1 and VF's token is rotated with
 * various tokens (including UUID_1 and NULL).
 * This asserts if the VF device is successfully created for a match
 * in the token or actually fails during a mismatch.
 */
#define ASSERT_VF_CREATION(_ret) do {				\
	if (variant->vf_token == NULL ||			\
	    strcmp(UUID_1, variant->vf_token)) {		\
	    ASSERT_NE((_ret), 0);				\
	} else {						\
	    ASSERT_EQ((_ret), 0);				\
	}							\
} while (0)

/*
 * Validate if the UAPI handles correctly and incorrectly set token on the VF.
 */
TEST_F(vfio_pci_sriov_uapi_test, init_token_match)
{
	int ret;

	pf_device = test_vfio_pci_device_init(pf_dev_bdf, variant->iommu_mode,
					      UUID_1, &ret);
	vf_device = test_vfio_pci_device_init(vf_dev_bdf, variant->iommu_mode,
					      variant->vf_token, &ret);

	ASSERT_VF_CREATION(ret);

	test_vfio_pci_device_cleanup(vf_device);
	test_vfio_pci_device_cleanup(pf_device);
}

/*
 * After setting a token on the PF, validate if the VF can still set the
 * expected token.
 */
TEST_F(vfio_pci_sriov_uapi_test, pf_early_close)
{
	int ret;

	pf_device = test_vfio_pci_device_init(pf_dev_bdf, variant->iommu_mode,
					      UUID_1, &ret);
	test_vfio_pci_device_cleanup(pf_device);

	vf_device = test_vfio_pci_device_init(vf_dev_bdf, variant->iommu_mode,
					      variant->vf_token, &ret);

	ASSERT_VF_CREATION(ret);

	test_vfio_pci_device_cleanup(vf_device);
}

/*
 * After PF device init, override the exsiting token and validate if the newly
 * set token is the one that's active.
 */
TEST_F(vfio_pci_sriov_uapi_test, override_token)
{
	int ret;

	pf_device = test_vfio_pci_device_init(pf_dev_bdf, variant->iommu_mode,
					      UUID_2, &ret);
	vfio_device_set_vf_token(pf_device->fd, UUID_1);

	vf_device = test_vfio_pci_device_init(vf_dev_bdf, variant->iommu_mode,
					      variant->vf_token, &ret);

	ASSERT_VF_CREATION(ret);

	test_vfio_pci_device_cleanup(vf_device);
	test_vfio_pci_device_cleanup(pf_device);
}

int main(int argc, char *argv[])
{
	pf_dev_bdf = vfio_selftests_get_bdf(&argc, argv);
	return test_harness_run(argc, argv);
}
