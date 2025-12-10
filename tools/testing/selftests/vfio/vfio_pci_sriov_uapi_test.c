// SPDX-License-Identifier: GPL-2.0-only
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <linux/limits.h>

#include <libvfio.h>

#include "../kselftest_harness.h"

#define UUID_1 "52ac9bff-3a88-4fbd-901a-0d767c3b6c97"
#define UUID_2 "88594674-90a0-47a9-aea8-9d9b352ac08a"

static const char *pf_dev_bdf;

static int test_vfio_pci_container_setup(struct vfio_pci_device *device,
					 const char *bdf,
					 const char *vf_token)
{
	vfio_pci_group_setup(device, bdf);
	vfio_container_set_iommu(device);
	__vfio_pci_group_get_device_fd(device, bdf, vf_token);

	/* The device fd will be -1 in case of mismatched tokens */
	return (device->fd < 0);
}

static int test_vfio_pci_iommufd_setup(struct vfio_pci_device *device,
				       const char *bdf, const char *vf_token)
{
	vfio_pci_iommufd_cdev_open(device, bdf);
	return __vfio_device_bind_iommufd(device->fd,
					  device->iommu->iommufd, vf_token);
}

static struct vfio_pci_device *test_vfio_pci_device_init(const char *bdf,
							 struct iommu *iommu,
							 const char *vf_token,
							 int *out_ret)
{
	struct vfio_pci_device *device;

	device = calloc(1, sizeof(*device));
	VFIO_ASSERT_NOT_NULL(device);

	device->iommu = iommu;
	device->bdf = bdf;

	if (iommu->mode->container_path)
		*out_ret = test_vfio_pci_container_setup(device, bdf, vf_token);
	else
		*out_ret = test_vfio_pci_iommufd_setup(device, bdf, vf_token);

	return device;
}

static void test_vfio_pci_device_cleanup(struct vfio_pci_device *device)
{
	if (device->fd > 0)
		VFIO_ASSERT_EQ(close(device->fd), 0);

	if (device->group_fd)
		VFIO_ASSERT_EQ(close(device->group_fd), 0);

	free(device);
}

FIXTURE(vfio_pci_sriov_uapi_test) {
	char vf_dev_bdf[16];
	char vf_driver[32];
	bool sriov_drivers_autoprobe;
};

FIXTURE_SETUP(vfio_pci_sriov_uapi_test)
{
	int nr_vfs;
	int ret;

	nr_vfs = sysfs_get_sriov_totalvfs(pf_dev_bdf);
	if (nr_vfs < 0)
		SKIP(return, "SR-IOV may not be supported by the device\n");

	nr_vfs = sysfs_get_sriov_numvfs(pf_dev_bdf);
	if (nr_vfs != 0)
		SKIP(return, "SR-IOV already configured for the PF\n");

	self->sriov_drivers_autoprobe =
		sysfs_get_sriov_drivers_autoprobe(pf_dev_bdf);
	if (self->sriov_drivers_autoprobe)
		sysfs_set_sriov_drivers_autoprobe(pf_dev_bdf, 0);

	/* Export only one VF for testing */
	sysfs_set_sriov_numvfs(pf_dev_bdf, 1);

	sysfs_get_sriov_vf_bdf(pf_dev_bdf, 0, self->vf_dev_bdf);
	if (sysfs_get_driver(self->vf_dev_bdf, self->vf_driver) == 0)
		sysfs_unbind_driver(self->vf_dev_bdf, self->vf_driver);
	sysfs_bind_driver(self->vf_dev_bdf, "vfio-pci");
}

FIXTURE_TEARDOWN(vfio_pci_sriov_uapi_test)
{
	sysfs_unbind_driver(self->vf_dev_bdf, "vfio-pci");
	sysfs_bind_driver(self->vf_dev_bdf, self->vf_driver);
	sysfs_set_sriov_numvfs(pf_dev_bdf, 0);
	sysfs_set_sriov_drivers_autoprobe(pf_dev_bdf,
					  self->sriov_drivers_autoprobe);
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
#define ASSERT_VF_CREATION(_ret) do {					\
	if (!variant->vf_token || strcmp(UUID_1, variant->vf_token)) {	\
		ASSERT_NE((_ret), 0);					\
	} else {							\
		ASSERT_EQ((_ret), 0);					\
	}								\
} while (0)

/*
 * Validate if the UAPI handles correctly and incorrectly set token on the VF.
 */
TEST_F(vfio_pci_sriov_uapi_test, init_token_match)
{
	struct vfio_pci_device *pf_device;
	struct vfio_pci_device *vf_device;
	struct iommu *iommu;
	int ret;

	iommu = iommu_init(variant->iommu_mode);
	pf_device = test_vfio_pci_device_init(pf_dev_bdf, iommu, UUID_1, &ret);
	vf_device = test_vfio_pci_device_init(self->vf_dev_bdf, iommu,
					      variant->vf_token, &ret);

	ASSERT_VF_CREATION(ret);

	test_vfio_pci_device_cleanup(vf_device);
	test_vfio_pci_device_cleanup(pf_device);
	iommu_cleanup(iommu);
}

/*
 * After setting a token on the PF, validate if the VF can still set the
 * expected token.
 */
TEST_F(vfio_pci_sriov_uapi_test, pf_early_close)
{
	struct vfio_pci_device *pf_device;
	struct vfio_pci_device *vf_device;
	struct iommu *iommu;
	int ret;

	iommu = iommu_init(variant->iommu_mode);
	pf_device = test_vfio_pci_device_init(pf_dev_bdf, iommu, UUID_1, &ret);
	test_vfio_pci_device_cleanup(pf_device);

	vf_device = test_vfio_pci_device_init(self->vf_dev_bdf, iommu,
					      variant->vf_token, &ret);

	ASSERT_VF_CREATION(ret);

	test_vfio_pci_device_cleanup(vf_device);
	iommu_cleanup(iommu);
}

/*
 * After PF device init, override the existing token and validate if the newly
 * set token is the one that's active.
 */
TEST_F(vfio_pci_sriov_uapi_test, override_token)
{
	struct vfio_pci_device *pf_device;
	struct vfio_pci_device *vf_device;
	struct iommu *iommu;
	int ret;

	iommu = iommu_init(variant->iommu_mode);
	pf_device = test_vfio_pci_device_init(pf_dev_bdf, iommu, UUID_2, &ret);
	vfio_device_set_vf_token(pf_device->fd, UUID_1);

	vf_device = test_vfio_pci_device_init(self->vf_dev_bdf, iommu,
					      variant->vf_token, &ret);

	ASSERT_VF_CREATION(ret);

	test_vfio_pci_device_cleanup(vf_device);
	test_vfio_pci_device_cleanup(pf_device);
	iommu_cleanup(iommu);
}

int main(int argc, char *argv[])
{
	pf_dev_bdf = vfio_selftests_get_bdf(&argc, argv);
	return test_harness_run(argc, argv);
}
