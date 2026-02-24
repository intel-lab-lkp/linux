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

static const char *pf_bdf;

static int container_setup(struct vfio_pci_device *device, const char *bdf,
			   const char *vf_token)
{
	vfio_pci_group_setup(device, bdf);
	vfio_container_set_iommu(device);
	__vfio_pci_group_get_device_fd(device, bdf, vf_token);

	/* The device fd will be -1 in case of mismatched tokens */
	return (device->fd < 0);
}

static int iommufd_setup(struct vfio_pci_device *device, const char *bdf,
			 const char *vf_token)
{
	vfio_pci_cdev_open(device, bdf);
	return __vfio_device_bind_iommufd(device->fd,
					  device->iommu->iommufd, vf_token);
}

static struct vfio_pci_device *device_init(const char *bdf, struct iommu *iommu,
					   const char *vf_token, int *out_ret)
{
	struct vfio_pci_device *device = vfio_pci_device_alloc(bdf, iommu);

	if (iommu->mode->container_path)
		*out_ret = container_setup(device, bdf, vf_token);
	else
		*out_ret = iommufd_setup(device, bdf, vf_token);

	return device;
}

static void device_cleanup(struct vfio_pci_device *device)
{
	if (device->fd > 0)
		VFIO_ASSERT_EQ(close(device->fd), 0);

	if (device->group_fd)
		VFIO_ASSERT_EQ(close(device->group_fd), 0);

	vfio_pci_device_free(device);
}

FIXTURE(vfio_pci_sriov_uapi_test) {
	char *vf_bdf;
};

FIXTURE_SETUP(vfio_pci_sriov_uapi_test)
{
	char *vf_driver;
	int nr_vfs;

	nr_vfs = sysfs_sriov_totalvfs_get(pf_bdf);
	if (nr_vfs <= 0)
		SKIP(return, "SR-IOV may not be supported by the PF: %s\n", pf_bdf);

	nr_vfs = sysfs_sriov_numvfs_get(pf_bdf);
	if (nr_vfs != 0)
		SKIP(return, "SR-IOV already configured for the PF: %s\n", pf_bdf);

	/* Create only one VF for testing */
	sysfs_sriov_numvfs_set(pf_bdf, 1);
	self->vf_bdf = sysfs_sriov_vf_bdf_get(pf_bdf, 0);

	/*
	 * The VF inherits the driver from the PF.
	 * Ensure this is 'vfio-pci' before proceeding.
	 */
	vf_driver = sysfs_driver_get(self->vf_bdf);
	ASSERT_NE(vf_driver, NULL);
	ASSERT_EQ(strcmp(vf_driver, "vfio-pci"), 0);
	free(vf_driver);

	printf("Created 1 VF (%s) under the PF: %s\n", self->vf_bdf, pf_bdf);
}

FIXTURE_TEARDOWN(vfio_pci_sriov_uapi_test)
{
	free(self->vf_bdf);
	sysfs_sriov_numvfs_set(pf_bdf, 0);
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
	struct vfio_pci_device *pf;
	struct vfio_pci_device *vf;
	struct iommu *iommu;
	int ret;

	iommu = iommu_init(variant->iommu_mode);
	pf = device_init(pf_bdf, iommu, UUID_1, &ret);
	vf = device_init(self->vf_bdf, iommu, variant->vf_token, &ret);

	ASSERT_VF_CREATION(ret);

	device_cleanup(vf);
	device_cleanup(pf);
	iommu_cleanup(iommu);
}

/*
 * After setting a token on the PF, validate if the VF can still set the
 * expected token.
 */
TEST_F(vfio_pci_sriov_uapi_test, pf_early_close)
{
	struct vfio_pci_device *pf;
	struct vfio_pci_device *vf;
	struct iommu *iommu;
	int ret;

	iommu = iommu_init(variant->iommu_mode);
	pf = device_init(pf_bdf, iommu, UUID_1, &ret);
	device_cleanup(pf);

	vf = device_init(self->vf_bdf, iommu, variant->vf_token, &ret);

	ASSERT_VF_CREATION(ret);

	device_cleanup(vf);
	iommu_cleanup(iommu);
}

/*
 * After PF device init, override the existing token and validate if the newly
 * set token is the one that's active.
 */
TEST_F(vfio_pci_sriov_uapi_test, override_token)
{
	struct vfio_pci_device *pf;
	struct vfio_pci_device *vf;
	struct iommu *iommu;
	int ret;

	iommu = iommu_init(variant->iommu_mode);
	pf = device_init(pf_bdf, iommu, UUID_2, &ret);
	vfio_device_set_vf_token(pf->fd, UUID_1);

	vf = device_init(self->vf_bdf, iommu, variant->vf_token, &ret);

	ASSERT_VF_CREATION(ret);

	device_cleanup(vf);
	device_cleanup(pf);
	iommu_cleanup(iommu);
}

int main(int argc, char *argv[])
{
	pf_bdf = vfio_selftests_get_bdf(&argc, argv);
	return test_harness_run(argc, argv);
}
