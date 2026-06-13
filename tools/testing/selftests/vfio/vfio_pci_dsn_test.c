// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tests for the PCIe Device Serial Number (DSN) handling in vfio-pci:
 *  - the physical serial is scrubbed (read as zero) by default,
 *  - guest writes to the DSN bytes are rejected (no change), and
 *  - VFIO_DEVICE_FEATURE_PCI_DSN can probe/set/get the presented serial.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include <sys/ioctl.h>

#include <linux/limits.h>
#include <linux/pci_regs.h>
#include <linux/vfio.h>

#include <libvfio.h>

#include "kselftest_harness.h"

static const char *device_bdf;

/* Walk the extended capability chain and return the DSN cap offset, or 0. */
static u16 find_dsn_cap(struct vfio_pci_device *device)
{
	u16 pos = PCI_CFG_SPACE_SIZE;
	int loops = (PCI_CFG_SPACE_EXP_SIZE - PCI_CFG_SPACE_SIZE) /
		    PCI_CAP_SIZEOF;

	while (pos >= PCI_CFG_SPACE_SIZE && loops--) {
		u32 header = vfio_pci_config_readl(device, pos);

		if (!header)
			break;

		if (PCI_EXT_CAP_ID(header) == PCI_EXT_CAP_ID_DSN)
			return pos;

		pos = PCI_EXT_CAP_NEXT(header);
	}

	return 0;
}

/*
 * Issue the DSN device feature.  @serial may be NULL for PROBE (no data is
 * read or written in that case); for GET/SET it is required.
 */
static int dsn_feature(struct vfio_pci_device *device, u32 op, u64 *serial)
{
	u8 buf[sizeof(struct vfio_device_feature) +
	       sizeof(struct vfio_device_feature_pci_dsn)] = {};
	struct vfio_device_feature *feature = (void *)buf;
	struct vfio_device_feature_pci_dsn *dsn = (void *)feature->data;

	feature->argsz = sizeof(buf);
	feature->flags = op | VFIO_DEVICE_FEATURE_PCI_DSN;

	if ((op & VFIO_DEVICE_FEATURE_SET) && serial)
		dsn->serial_number = *serial;

	if (ioctl(device->fd, VFIO_DEVICE_FEATURE, feature))
		return -errno;

	if ((op & VFIO_DEVICE_FEATURE_GET) && serial)
		*serial = dsn->serial_number;

	return 0;
}

FIXTURE(vfio_pci_dsn_test) {
	struct iommu *iommu;
	struct vfio_pci_device *device;
	u16 dsn_pos;
};

FIXTURE_SETUP(vfio_pci_dsn_test)
{
	self->iommu = iommu_init(default_iommu_mode);
	self->device = vfio_pci_device_init(device_bdf, self->iommu);
	self->dsn_pos = find_dsn_cap(self->device);
}

FIXTURE_TEARDOWN(vfio_pci_dsn_test)
{
	vfio_pci_device_cleanup(self->device);
	iommu_cleanup(self->iommu);
}

/* The physical serial number must not be visible; reads must be zero. */
TEST_F(vfio_pci_dsn_test, serial_scrubbed)
{
	if (!self->dsn_pos)
		SKIP(return, "Device has no DSN capability\n");

	ASSERT_EQ(0, vfio_pci_config_readl(self->device, self->dsn_pos + PCI_DSN_LOW_DW));
	ASSERT_EQ(0, vfio_pci_config_readl(self->device, self->dsn_pos + PCI_DSN_HIGH_DW));

	/* The capability header itself must still be present. */
	ASSERT_EQ(PCI_EXT_CAP_ID_DSN,
		  PCI_EXT_CAP_ID(vfio_pci_config_readl(self->device,
						       self->dsn_pos)));
}

/* Guest writes to the DSN bytes must be ignored (read-only state). */
TEST_F(vfio_pci_dsn_test, write_rejected)
{
	if (!self->dsn_pos)
		SKIP(return, "Device has no DSN capability\n");

	vfio_pci_config_writel(self->device, self->dsn_pos + PCI_DSN_LOW_DW, 0xdeadbeef);
	vfio_pci_config_writel(self->device, self->dsn_pos + PCI_DSN_HIGH_DW, 0xcafef00d);

	ASSERT_EQ(0, vfio_pci_config_readl(self->device, self->dsn_pos + PCI_DSN_LOW_DW));
	ASSERT_EQ(0, vfio_pci_config_readl(self->device, self->dsn_pos + PCI_DSN_HIGH_DW));
}

/* PROBE must succeed iff the device has a DSN capability. */
TEST_F(vfio_pci_dsn_test, probe)
{
	/* PROBE must include at least one supported op flag to pass. */
	int ret = dsn_feature(self->device,
			      VFIO_DEVICE_FEATURE_PROBE |
			      VFIO_DEVICE_FEATURE_GET |
			      VFIO_DEVICE_FEATURE_SET, NULL);

	if (!self->dsn_pos)
		ASSERT_EQ(-ENOTTY, ret);
	else
		ASSERT_EQ(0, ret);
}

/* SET then GET must round-trip, and the guest-visible bytes must match. */
TEST_F(vfio_pci_dsn_test, set_get_roundtrip)
{
	u64 want = 0x0123456789abcdefULL;
	u64 got = 0;

	if (!self->dsn_pos)
		SKIP(return, "Device has no DSN capability\n");

	ASSERT_EQ(0, dsn_feature(self->device, VFIO_DEVICE_FEATURE_SET, &want));
	ASSERT_EQ(0, dsn_feature(self->device, VFIO_DEVICE_FEATURE_GET, &got));
	ASSERT_EQ(want, got);

	ASSERT_EQ((u32)want,
		  vfio_pci_config_readl(self->device, self->dsn_pos + PCI_DSN_LOW_DW));
	ASSERT_EQ((u32)(want >> 32),
		  vfio_pci_config_readl(self->device, self->dsn_pos + PCI_DSN_HIGH_DW));
}

/* SET twice; GET must return the latest value. */
TEST_F(vfio_pci_dsn_test, set_twice)
{
	u64 first = 0x1111222233334444ULL;
	u64 second = 0xaaaabbbbccccddddULL;
	u64 got = 0;

	if (!self->dsn_pos)
		SKIP(return, "Device has no DSN capability\n");

	ASSERT_EQ(0, dsn_feature(self->device, VFIO_DEVICE_FEATURE_SET, &first));
	ASSERT_EQ(0, dsn_feature(self->device, VFIO_DEVICE_FEATURE_SET, &second));
	ASSERT_EQ(0, dsn_feature(self->device, VFIO_DEVICE_FEATURE_GET, &got));
	ASSERT_EQ(second, got);
}

/* The presented serial persists across a device reset (FLR/SBR). */
TEST_F(vfio_pci_dsn_test, persists_across_reset)
{
	u64 want = 0x5555666677778888ULL;
	u64 got = 0;

	if (!self->dsn_pos)
		SKIP(return, "Device has no DSN capability\n");
	if (!(self->device->info.flags & VFIO_DEVICE_FLAGS_RESET))
		SKIP(return, "Device does not support reset\n");

	ASSERT_EQ(0, dsn_feature(self->device, VFIO_DEVICE_FEATURE_SET, &want));
	vfio_pci_device_reset(self->device);
	ASSERT_EQ(0, dsn_feature(self->device, VFIO_DEVICE_FEATURE_GET, &got));
	ASSERT_EQ(want, got);
}

/* A short argsz must be rejected with -EINVAL. */
TEST_F(vfio_pci_dsn_test, bad_argsz)
{
	struct vfio_device_feature feature = {
		.argsz = sizeof(struct vfio_device_feature),
		.flags = VFIO_DEVICE_FEATURE_GET | VFIO_DEVICE_FEATURE_PCI_DSN,
	};

	if (!self->dsn_pos)
		SKIP(return, "Device has no DSN capability\n");

	ASSERT_EQ(-1, ioctl(self->device->fd, VFIO_DEVICE_FEATURE, &feature));
	ASSERT_EQ(EINVAL, errno);
}

int main(int argc, char *argv[])
{
	device_bdf = vfio_selftests_get_bdf(&argc, argv);
	return test_harness_run(argc, argv);
}
