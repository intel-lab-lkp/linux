// SPDX-License-Identifier: GPL-2.0-only

#include <libliveupdate.h>
#include <libvfio.h>

static const char *device_bdf;

static char state_session[LIVEUPDATE_SESSION_NAME_LENGTH];
static char device_session[LIVEUPDATE_SESSION_NAME_LENGTH];

enum {
	STATE_TOKEN,
	DEVICE_TOKEN,
};

static void before_kexec(int luo_fd)
{
	struct vfio_pci_device *device;
	struct iommu *iommu;
	int session_fd;
	int ret;

	iommu = iommu_init("iommufd");
	device = vfio_pci_device_init(device_bdf, iommu);

	create_state_file(luo_fd, state_session, STATE_TOKEN, /*next_stage=*/2);

	session_fd = luo_create_session(luo_fd, device_session);
	VFIO_ASSERT_GE(session_fd, 0);

	printf("Preserving device in session\n");
	ret = luo_session_preserve_fd(session_fd, device->fd, DEVICE_TOKEN);
	VFIO_ASSERT_EQ(ret, 0);

	close(luo_fd);
	daemonize_and_wait();
}

static void after_kexec(int luo_fd, int state_session_fd)
{
	struct vfio_pci_device *device;
	struct iommu *iommu;
	int session_fd;
	int device_fd;
	int stage;

	restore_and_read_stage(state_session_fd, STATE_TOKEN, &stage);
	VFIO_ASSERT_EQ(stage, 2);

	session_fd = luo_retrieve_session(luo_fd, device_session);
	VFIO_ASSERT_GE(session_fd, 0);

	printf("Finishing the session before retrieving the device (should fail)\n");
	VFIO_ASSERT_NE(luo_session_finish(session_fd), 0);

	printf("Retrieving the device FD from LUO\n");
	device_fd = luo_session_retrieve_fd(session_fd, DEVICE_TOKEN);
	VFIO_ASSERT_GE(device_fd, 0);

	printf("Finishing the session before binding to iommufd (should fail)\n");
	VFIO_ASSERT_NE(luo_session_finish(session_fd), 0);

	printf("Binding the device to an iommufd and setting it up\n");
	iommu = iommu_init("iommufd");

	/*
	 * This will invoke various ioctls on device_fd such as
	 * VFIO_DEVICE_GET_INFO. So this is a decent sanity test
	 * that LUO actually handed us back a valid VFIO device
	 * file and not something else.
	 */
	device = __vfio_pci_device_init(device_bdf, iommu, device_fd);

	printf("Finishing the session\n");
	VFIO_ASSERT_EQ(luo_session_finish(session_fd), 0);

	vfio_pci_device_cleanup(device);
	iommu_cleanup(iommu);
}

int main(int argc, char *argv[])
{
	device_bdf = vfio_selftests_get_bdf(&argc, argv);

	sprintf(device_session, "device-%s", device_bdf);
	sprintf(state_session, "state-%s", device_bdf);

	return luo_test(argc, argv, state_session, before_kexec, after_kexec);
}
