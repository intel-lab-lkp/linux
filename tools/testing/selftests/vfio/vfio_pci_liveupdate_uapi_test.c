// SPDX-License-Identifier: GPL-2.0-only

#include <libliveupdate.h>
#include <libvfio.h>
#include <kselftest_harness.h>

static const char *device_bdf;

FIXTURE(vfio_pci_liveupdate_uapi_test) {
	int luo_fd;
	int session_fd;
	struct iommu *iommu;
	struct vfio_pci_device *device;
};

FIXTURE_VARIANT(vfio_pci_liveupdate_uapi_test) {
	const char *iommu_mode;
};

#define FIXTURE_VARIANT_ADD_IOMMU_MODE(_iommu_mode)			\
FIXTURE_VARIANT_ADD(vfio_pci_liveupdate_uapi_test, _iommu_mode) {	\
	.iommu_mode = #_iommu_mode,					\
}

FIXTURE_VARIANT_ADD_ALL_IOMMU_MODES();
#undef FIXTURE_VARIANT_ADD_IOMMU_MODE

FIXTURE_SETUP(vfio_pci_liveupdate_uapi_test)
{
	self->luo_fd = luo_open_device();
	ASSERT_GE(self->luo_fd, 0);

	self->session_fd = luo_create_session(self->luo_fd, "session");
	ASSERT_GE(self->session_fd, 0);

	self->iommu = iommu_init(variant->iommu_mode);
	self->device = vfio_pci_device_init(device_bdf, self->iommu);
}

FIXTURE_TEARDOWN(vfio_pci_liveupdate_uapi_test)
{
	vfio_pci_device_cleanup(self->device);
	iommu_cleanup(self->iommu);
	close(self->session_fd);
	close(self->luo_fd);
}

TEST_F(vfio_pci_liveupdate_uapi_test, preserve_device)
{
	int ret;

	ret = luo_session_preserve_fd(self->session_fd, self->device->fd, 0);

	/* Preservation should only be supported for VFIO cdev files. */
	ASSERT_EQ(ret, self->iommu->iommufd ? 0 : -ENOENT);
}

TEST_F(vfio_pci_liveupdate_uapi_test, preserve_group_fails)
{
	int ret;

	if (self->iommu->iommufd)
		SKIP(return, "iommufd-mode does not have group files");

	ret = luo_session_preserve_fd(self->session_fd, self->device->group_fd, 0);
	ASSERT_EQ(ret, -ENOENT);
}

TEST_F(vfio_pci_liveupdate_uapi_test, preserve_container_fails)
{
	int ret;

	if (self->iommu->iommufd)
		SKIP(return, "iommufd-mode does not have container files");

	ret = luo_session_preserve_fd(self->session_fd, self->iommu->container_fd, 0);
	ASSERT_EQ(ret, -ENOENT);
}

int main(int argc, char *argv[])
{
	int fd;

	fd = luo_open_device();
	if (fd < 0)
		ksft_exit_skip("open(%s) failed: %s, skipping\n",
			       LUO_DEVICE, strerror(errno));

	close(fd);

	device_bdf = vfio_selftests_get_bdf(&argc, argv);
	return test_harness_run(argc, argv);
}
