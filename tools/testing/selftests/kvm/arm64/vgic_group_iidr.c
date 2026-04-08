// SPDX-License-Identifier: GPL-2.0
/*
 * vgic_group_iidr.c - Test IGROUPR behaviour across IIDR revisions
 *
 * Validate that the GICD_IIDR implementation revision controls
 * IGROUPR semantics for GICv3:
 *   Rev 1: IGROUPR reads as all-ones (group 1), writes ignored
 *   Rev 2+: IGROUPR is guest-configurable (read/write)
 */
#include <linux/sizes.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "gic.h"
#include "gic_v3.h"
#include "vgic.h"

#define NR_IRQS		128
#define SPI_IGROUPR	(GICD_IGROUPR + (32 / 32) * 4) /* intids 32-63 */

static uint64_t shared_rev;

static void guest_code(void)
{
	uint32_t val;

	val = readl(GICD_BASE_GVA + SPI_IGROUPR);

	if (shared_rev == 1) {
		/* Rev 1: all group 1, guest writes must be ignored */
		GUEST_ASSERT_EQ(val, 0xffffffff);
		writel(0x0, GICD_BASE_GVA + SPI_IGROUPR);
		val = readl(GICD_BASE_GVA + SPI_IGROUPR);
		GUEST_ASSERT_EQ(val, 0xffffffff);
		writel(0x55aa55aa, GICD_BASE_GVA + SPI_IGROUPR);
		val = readl(GICD_BASE_GVA + SPI_IGROUPR);
		GUEST_ASSERT_EQ(val, 0xffffffff);
	} else {
		/* Rev 2/3: guest-configurable */
		writel(0xa5a5a5a5, GICD_BASE_GVA + SPI_IGROUPR);
		val = readl(GICD_BASE_GVA + SPI_IGROUPR);
		GUEST_ASSERT_EQ(val, 0xa5a5a5a5);
		writel(0x0, GICD_BASE_GVA + SPI_IGROUPR);
		val = readl(GICD_BASE_GVA + SPI_IGROUPR);
		GUEST_ASSERT_EQ(val, 0x0);
	}

	/* Rev 3: GICR_CTLR advertises IR and CES. Rev 1/2: it does not. */
	val = readl(GICR_BASE_GVA + GICR_CTLR);
	if (shared_rev >= 3)
		GUEST_ASSERT(val & (GICR_CTLR_IR | GICR_CTLR_CES));
	else
		GUEST_ASSERT(!(val & (GICR_CTLR_IR | GICR_CTLR_CES)));

	GUEST_DONE();
}

static void run_test(int rev)
{
	struct kvm_vcpu *vcpus[1];
	struct kvm_vm *vm;
	struct ucall uc;
	uint32_t iidr;
	int gic_fd;

	pr_info("Testing IIDR revision %d\n", rev);

	test_disable_default_vgic();
	vm = vm_create_with_vcpus(1, guest_code, vcpus);

	gic_fd = __vgic_v3_setup(vm, 1, NR_IRQS);
	TEST_ASSERT(gic_fd >= 0, "Failed to create vGICv3");

	/* Set the requested IIDR revision before init. */
	kvm_device_attr_get(gic_fd, KVM_DEV_ARM_VGIC_GRP_DIST_REGS,
			    GICD_IIDR, &iidr);
	iidr &= ~GICD_IIDR_REVISION_MASK;
	iidr |= rev << GICD_IIDR_REVISION_SHIFT;
	kvm_device_attr_set(gic_fd, KVM_DEV_ARM_VGIC_GRP_DIST_REGS,
			    GICD_IIDR, &iidr);

	__vgic_v3_init(gic_fd);

	/* Verify the revision was applied. */
	kvm_device_attr_get(gic_fd, KVM_DEV_ARM_VGIC_GRP_DIST_REGS,
			    GICD_IIDR, &iidr);
	TEST_ASSERT(((iidr & GICD_IIDR_REVISION_MASK) >> GICD_IIDR_REVISION_SHIFT) == rev,
		    "IIDR revision readback: expected %d, got %d",
		    rev, (iidr & GICD_IIDR_REVISION_MASK) >> GICD_IIDR_REVISION_SHIFT);

	/* Tell the guest which revision we set. */
	sync_global_to_guest(vm, shared_rev);
	shared_rev = rev;
	sync_global_to_guest(vm, shared_rev);

	vcpu_run(vcpus[0]);
	switch (get_ucall(vcpus[0], &uc)) {
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
		break;
	case UCALL_DONE:
		break;
	default:
		TEST_FAIL("Unexpected ucall %lu", uc.cmd);
	}

	close(gic_fd);
	kvm_vm_free(vm);
}

int main(int argc, char *argv[])
{
	run_test(1);
	run_test(2);
	run_test(3);
	return 0;
}
