// SPDX-License-Identifier: GPL-2.0-only

#include <linux/sizes.h>
#include <sys/mman.h>

#include <libliveupdate.h>
#include <libvfio.h>

#define MEMCPY_SIZE SZ_1G
#define DRIVER_SIZE SZ_1M
#define MEMFD_SIZE (MEMCPY_SIZE + DRIVER_SIZE)

static struct dma_region memcpy_region;
static const char *device_bdf;

static char state_session[LIVEUPDATE_SESSION_NAME_LENGTH];
static char device_session[LIVEUPDATE_SESSION_NAME_LENGTH];

enum {
	STATE_TOKEN,
	DEVICE_TOKEN,
	MEMFD_TOKEN,
};

static void dma_memcpy_one(struct vfio_pci_device *device)
{
	void *src = memcpy_region.vaddr, *dst;
	u64 size;

	size = min_t(u64, memcpy_region.size / 2, device->driver.max_memcpy_size);
	dst = src + size;

	memset(src, 1, size);
	memset(dst, 0, size);

	printf("Kicking off 1 DMA memcpy operations of size 0x%lx...\n", size);
	vfio_pci_driver_memcpy(device,
			       to_iova(device, src),
			       to_iova(device, dst),
			       size);

	VFIO_ASSERT_EQ(memcmp(src, dst, size), 0);
}

static void dma_memcpy_start(struct vfio_pci_device *device)
{
	void *src = memcpy_region.vaddr, *dst;
	u64 count, size;

	size = min_t(u64, memcpy_region.size / 2, device->driver.max_memcpy_size);
	dst = src + size;

	/*
	 * Rough Math: If we assume the device will perform memcpy at a rate of
	 * 30GB/s then 7200GB of transfers will run for about 4 minutes.
	 */
	count = (u64)7200 * SZ_1G / size;
	count = min_t(u64, count, device->driver.max_memcpy_count);

	memset(src, 1, size / 2);
	memset(dst, 0, size / 2);

	printf("Kicking off %lu DMA memcpy operations of size 0x%lx...\n", count, size);
	vfio_pci_driver_memcpy_start(device,
				     to_iova(device, src),
				     to_iova(device, dst),
				     size, count);
}

static void dma_memfd_map(struct vfio_pci_device *device, int fd)
{
	void *vaddr;

	vaddr = mmap(NULL, MEMFD_SIZE, PROT_WRITE, MAP_SHARED, fd, 0);
	VFIO_ASSERT_NE(vaddr, MAP_FAILED);

	memcpy_region.iova = SZ_4G;
	memcpy_region.size = MEMCPY_SIZE;
	memcpy_region.vaddr = vaddr;
	iommu_map(device->iommu, &memcpy_region);

	device->driver.region.iova = memcpy_region.iova + memcpy_region.size;
	device->driver.region.size = DRIVER_SIZE;
	device->driver.region.vaddr = vaddr + memcpy_region.size;
	iommu_map(device->iommu, &device->driver.region);
}

static void dma_memfd_setup(struct vfio_pci_device *device, int session_fd)
{
	int fd, ret;

	fd = memfd_create("dma-buffer", 0);
	VFIO_ASSERT_GE(fd, 0);

	ret = fallocate(fd, 0, 0, MEMFD_SIZE);
	VFIO_ASSERT_EQ(ret, 0);

	printf("Preserving memfd of size 0x%x in session\n", MEMFD_SIZE);
	ret = luo_session_preserve_fd(session_fd, fd, MEMFD_TOKEN);
	VFIO_ASSERT_EQ(ret, 0);

	dma_memfd_map(device, fd);
}

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

	dma_memfd_setup(device, session_fd);

	/*
	 * If the device has a selftests driver, kick off a long-running DMA
	 * operation to exercise the device trying to DMA during a Live Update.
	 * Since iommufd preservation is not supported yet, these DMAs should be
	 * dropped. So this is just looking to verify that the system does not
	 * fall over and crash as a result of a busy device being preserved.
	 */
	if (device->driver.ops) {
		vfio_pci_driver_init(device);
		dma_memcpy_start(device);

		/*
		 * Disable interrupts on the device or freeze() will fail.
		 * Unfortunately there isn't a way to easily have a test for
		 * that here since the check happens during shutdown.
		 */
		vfio_pci_msix_disable(device);
	}

	close(luo_fd);
	daemonize_and_wait();
}

static void check_open_vfio_device_fails(void)
{
	const char *cdev_path = vfio_pci_get_cdev_path(device_bdf);
	struct vfio_pci_device *device;
	struct iommu *iommu;
	int ret, i;

	printf("Checking open(%s) fails\n", cdev_path);
	ret = open(cdev_path, O_RDWR);
	VFIO_ASSERT_EQ(ret, -1);
	VFIO_ASSERT_EQ(errno, EBUSY);
	free((void *)cdev_path);

	for (i = 0; i < nr_iommu_modes; i++) {
		if (!iommu_modes[i].container_path)
			continue;

		iommu = iommu_init(iommu_modes[i].name);

		device = vfio_pci_device_alloc(device_bdf, iommu);
		vfio_pci_group_setup(device);
		vfio_pci_iommu_setup(device);

		printf("Checking ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, \"%s\") fails (%s)\n",
		       device_bdf, iommu_modes[i].name);

		ret = ioctl(device->group_fd, VFIO_GROUP_GET_DEVICE_FD, device->bdf);
		VFIO_ASSERT_EQ(ret, -1);
		VFIO_ASSERT_EQ(errno, EBUSY);

		close(device->group_fd);
		free(device);
		iommu_cleanup(iommu);
	}
}

static void after_kexec(int luo_fd, int state_session_fd)
{
	struct vfio_pci_device *device;
	struct iommu *iommu;
	int session_fd;
	int device_fd;
	int memfd;
	int stage;

	check_open_vfio_device_fails();

	restore_and_read_stage(state_session_fd, STATE_TOKEN, &stage);
	VFIO_ASSERT_EQ(stage, 2);

	session_fd = luo_retrieve_session(luo_fd, device_session);
	VFIO_ASSERT_GE(session_fd, 0);

	printf("Retrieving memfd from LUO\n");
	memfd = luo_session_retrieve_fd(session_fd, MEMFD_TOKEN);
	VFIO_ASSERT_GE(memfd, 0);

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

	dma_memfd_map(device, memfd);

	printf("Finishing the session\n");
	VFIO_ASSERT_EQ(luo_session_finish(session_fd), 0);

	/*
	 * Once iommufd preservation is supported and the device is kept fully
	 * running across the Live Update, this should wait for the long-
	 * running DMA memcpy operation kicked off in before_kexec() to
	 * complete. But for now we expect the device to be reset so just
	 * trigger a single memcpy to make sure it's still functional.
	 */
	if (device->driver.ops) {
		vfio_pci_driver_init(device);
		dma_memcpy_one(device);
	}

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
