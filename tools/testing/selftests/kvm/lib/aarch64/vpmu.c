// SPDX-License-Identifier: GPL-2.0

#include <kvm_util.h>
#include <processor.h>
#include <test_util.h>
#include <vgic.h>
#include <vpmu.h>
#include <perf/arm_pmuv3.h>

/* Create a VM that has one vCPU with PMUv3 configured. */
struct vpmu_vm *create_vpmu_vm(void *guest_code)
{
	struct kvm_vcpu_init init;
	uint8_t pmuver;
	uint64_t dfr0, irq = 23;
	struct kvm_device_attr irq_attr = {
		.group = KVM_ARM_VCPU_PMU_V3_CTRL,
		.attr = KVM_ARM_VCPU_PMU_V3_IRQ,
		.addr = (uint64_t)&irq,
	};
	struct kvm_device_attr init_attr = {
		.group = KVM_ARM_VCPU_PMU_V3_CTRL,
		.attr = KVM_ARM_VCPU_PMU_V3_INIT,
	};
	struct vpmu_vm *vpmu_vm;

	vpmu_vm = calloc(1, sizeof(*vpmu_vm));
	TEST_ASSERT(vpmu_vm != NULL, "Insufficient Memory");
	memset(vpmu_vm, 0, sizeof(vpmu_vm));

	vpmu_vm->vm = vm_create(1);
	vm_init_descriptor_tables(vpmu_vm->vm);

	/* Create vCPU with PMUv3 */
	vm_ioctl(vpmu_vm->vm, KVM_ARM_PREFERRED_TARGET, &init);
	init.features[0] |= (1 << KVM_ARM_VCPU_PMU_V3);
	vpmu_vm->vcpu = aarch64_vcpu_add(vpmu_vm->vm, 0, &init, guest_code);
	vcpu_init_descriptor_tables(vpmu_vm->vcpu);
	vpmu_vm->gic_fd = vgic_v3_setup(vpmu_vm->vm, 1, 64,
					GICD_BASE_GPA, GICR_BASE_GPA);
	__TEST_REQUIRE(vpmu_vm->gic_fd >= 0,
		       "Failed to create vgic-v3, skipping");

	/* Make sure that PMUv3 support is indicated in the ID register */
	vcpu_get_reg(vpmu_vm->vcpu,
		     KVM_ARM64_SYS_REG(SYS_ID_AA64DFR0_EL1), &dfr0);
	pmuver = FIELD_GET(ARM64_FEATURE_MASK(ID_AA64DFR0_EL1_PMUVer), dfr0);
	TEST_ASSERT(pmuver != ID_AA64DFR0_EL1_PMUVer_IMP_DEF &&
		    pmuver >= ID_AA64DFR0_EL1_PMUVer_IMP,
		    "Unexpected PMUVER (0x%x) on the vCPU with PMUv3", pmuver);

	/* Initialize vPMU */
	vcpu_ioctl(vpmu_vm->vcpu, KVM_SET_DEVICE_ATTR, &irq_attr);
	vcpu_ioctl(vpmu_vm->vcpu, KVM_SET_DEVICE_ATTR, &init_attr);

	return vpmu_vm;
}

void destroy_vpmu_vm(struct vpmu_vm *vpmu_vm)
{
	close(vpmu_vm->gic_fd);
	kvm_vm_free(vpmu_vm->vm);
	free(vpmu_vm);
}
