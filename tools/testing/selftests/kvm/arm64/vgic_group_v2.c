// SPDX-License-Identifier: GPL-2.0
/*
 * vgic_group_v2.c - Test GICv2 IGROUPR behaviour across IIDR revisions
 *
 * Validate that the GICD_IIDR implementation revision controls GICv2
 * IGROUPR writability for both guest and userspace:
 *   Default (no IIDR write): groups writable (implementation_rev defaults to 3)
 *   Rev 1: IGROUPR reads as zero (group 0), writes ignored
 *   Rev 2: IGROUPR is guest and userspace configurable
 */
#include <linux/sizes.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "gic.h"
#include "gic_v3.h"
#include "vgic.h"

#define NR_IRQS		64

#define V2_DIST_BASE	0x8000000ULL
#define V2_CPU_BASE	0x8010000ULL
#define V2_DIST_GVA	((volatile void *)V2_DIST_BASE)

#define SPI_IGROUPR	(GICD_IGROUPR + (32 / 32) * 4)

static uint64_t shared_rev;
static uint64_t guest_result;

static void guest_code(void)
{
	uint32_t before, after;

	before = readl(V2_DIST_GVA + SPI_IGROUPR);
	writel(0x5a5a5a5a, V2_DIST_GVA + SPI_IGROUPR);
	after = readl(V2_DIST_GVA + SPI_IGROUPR);

	guest_result = ((uint64_t)before << 32) | after;
	GUEST_DONE();
}

static int create_v2_gic(struct kvm_vm *vm)
{
	uint32_t nr_irqs = NR_IRQS;
	uint64_t addr;
	int gic_fd;

	gic_fd = __kvm_create_device(vm, KVM_DEV_TYPE_ARM_VGIC_V2);
	if (gic_fd < 0)
		return gic_fd;

	addr = V2_DIST_BASE;
	kvm_device_attr_set(gic_fd, KVM_DEV_ARM_VGIC_GRP_ADDR,
			    KVM_VGIC_V2_ADDR_TYPE_DIST, &addr);
	addr = V2_CPU_BASE;
	kvm_device_attr_set(gic_fd, KVM_DEV_ARM_VGIC_GRP_ADDR,
			    KVM_VGIC_V2_ADDR_TYPE_CPU, &addr);

	virt_map(vm, V2_DIST_BASE, V2_DIST_BASE,
		 vm_calc_num_guest_pages(vm->mode, SZ_64K));
	virt_map(vm, V2_CPU_BASE, V2_CPU_BASE,
		 vm_calc_num_guest_pages(vm->mode, SZ_64K));

	kvm_device_attr_set(gic_fd, KVM_DEV_ARM_VGIC_GRP_NR_IRQS,
			    0, &nr_irqs);
	return gic_fd;
}

static void run_test(int set_iidr_rev)
{
	struct kvm_vcpu *vcpus[1];
	struct kvm_vm *vm;
	struct ucall uc;
	uint32_t before, after, igroupr, iidr;
	int gic_fd;
	bool expect_writable;

	if (set_iidr_rev >= 0)
		pr_info("Testing GICv2 IIDR revision %d\n", set_iidr_rev);
	else
		pr_info("Testing GICv2 IIDR default (no write)\n");

	test_disable_default_vgic();
	vm = vm_create_with_vcpus(1, guest_code, vcpus);

	gic_fd = create_v2_gic(vm);
	TEST_REQUIRE(gic_fd >= 0);

	if (set_iidr_rev >= 0) {
		kvm_device_attr_get(gic_fd, KVM_DEV_ARM_VGIC_GRP_DIST_REGS,
				    GICD_IIDR, &iidr);
		iidr &= ~GICD_IIDR_REVISION_MASK;
		iidr |= set_iidr_rev << GICD_IIDR_REVISION_SHIFT;
		kvm_device_attr_set(gic_fd, KVM_DEV_ARM_VGIC_GRP_DIST_REGS,
				    GICD_IIDR, &iidr);
	}

	kvm_device_attr_set(gic_fd, KVM_DEV_ARM_VGIC_GRP_CTRL,
			    KVM_DEV_ARM_VGIC_CTRL_INIT, NULL);

	/*
	 * Default (no IIDR write) gets implementation_rev=3 from vgic_init(),
	 * so groups should be writable. Rev 1 = not writable. Rev 2+ = writable.
	 */
	expect_writable = (set_iidr_rev != 1);

	/* Test userspace IGROUPR write */
	igroupr = 0xa5a5a5a5;
	kvm_device_attr_set(gic_fd, KVM_DEV_ARM_VGIC_GRP_DIST_REGS,
			    SPI_IGROUPR, &igroupr);
	igroupr = 0;
	kvm_device_attr_get(gic_fd, KVM_DEV_ARM_VGIC_GRP_DIST_REGS,
			    SPI_IGROUPR, &igroupr);

	if (expect_writable)
		TEST_ASSERT(igroupr == 0xa5a5a5a5,
			    "Userspace write should succeed: got 0x%08x", igroupr);
	else
		TEST_ASSERT(igroupr == 0x00000000,
			    "Userspace write should be ignored: got 0x%08x", igroupr);

	/* Reset IGROUPR to 0 via userspace for rev 2+ before guest test */
	if (expect_writable) {
		igroupr = 0;
		kvm_device_attr_set(gic_fd, KVM_DEV_ARM_VGIC_GRP_DIST_REGS,
				    SPI_IGROUPR, &igroupr);
	}

	/* Test guest IGROUPR write */
	sync_global_to_guest(vm, guest_result);
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

	sync_global_from_guest(vm, guest_result);
	before = guest_result >> 32;
	after = guest_result & 0xffffffff;

	TEST_ASSERT(before == 0x00000000,
		    "Initial IGROUPR should be 0 (group 0): got 0x%08x", before);

	if (expect_writable)
		TEST_ASSERT(after == 0x5a5a5a5a,
			    "Guest write should succeed: got 0x%08x", after);
	else
		TEST_ASSERT(after == 0x00000000,
			    "Guest write should be ignored: got 0x%08x", after);

	close(gic_fd);
	kvm_vm_free(vm);
}

int main(int argc, char *argv[])
{
	run_test(-1);  /* default */
	run_test(1);   /* rev 1 */
	run_test(2);   /* rev 2 */
	return 0;
}
