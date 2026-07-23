// SPDX-License-Identifier: GPL-2.0
/*
 * LoongArch KVM in-kernel MMIO read fast path test
 *
 * When an MMIO read hits a device emulated inside KVM (such as the
 * PCH-PIC), kvm_emu_mmio_read() completes the access without returning
 * to user space and advances the guest PC by one instruction.
 *
 * This test issues such an in-kernel MMIO read immediately followed by
 * a marker instruction and checks that the marker instruction actually
 * runs, i.e. that the PC advanced by exactly one instruction (4 bytes)
 * rather than skipping the instruction after the MMIO read.
 */
#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"
#include "loongarch/processor.h"

/* Physical base the in-kernel PCH-PIC is mapped at (no memslot backs it) */
#define PCH_PIC_BASE		0x10000000UL

static void guest_code(void)
{
	unsigned long marker = 0;
	unsigned int val = 0;

	/*
	 * 'ld.w' faults out as an MMIO read and is emulated in kernel.
	 * The following 'addi.d' must execute; it is skipped if the MMIO
	 * read fast path advances the PC twice (by 8 instead of 4).
	 */
	asm volatile(
		"ld.w	%[val], %[base], 0\n\t"
		"addi.d	%[marker], %[marker], 1\n\t"
		: [val] "=&r" (val), [marker] "+&r" (marker)
		: [base] "r" (PCH_PIC_BASE)
		: "memory");

	GUEST_PRINTF("mmio read val=0x%x, marker=%lu\n", val, marker);
	GUEST_ASSERT_EQ(marker, 1);
	GUEST_DONE();
}

int main(int argc, char *argv[])
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	uint64_t addr = PCH_PIC_BASE;
	int dev_fd;

	vm = vm_create(VM_MODE_P47V47_16K);
	vcpu = vm_vcpu_add(vm, 0, guest_code);

	/* Create the in-kernel PCH-PIC and map it at PCH_PIC_BASE */
	dev_fd = __kvm_create_device(vm, KVM_DEV_TYPE_LOONGARCH_PCHPIC);
	if (dev_fd < 0) {
		print_skip("PCH-PIC device not supported by host KVM");
		kvm_vm_free(vm);
		return KSFT_SKIP;
	}
	kvm_device_attr_set(dev_fd, KVM_DEV_LOONGARCH_PCH_PIC_GRP_CTRL,
			    KVM_DEV_LOONGARCH_PCH_PIC_CTRL_INIT, &addr);
	close(dev_fd);

	/* Identity-map the MMIO page so the guest can reach the device */
	virt_map(vm, PCH_PIC_BASE, PCH_PIC_BASE, 1);

	while (1) {
		vcpu_run(vcpu);
		switch (get_ucall(vcpu, &uc)) {
		case UCALL_PRINTF:
			pr_info("%s", uc.buffer);
			break;
		case UCALL_DONE:
			goto done;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			goto done;
		default:
			TEST_FAIL("Unexpected exit: %s",
				  exit_reason_str(vcpu->run->exit_reason));
		}
	}

done:
	kvm_vm_free(vm);
	return 0;
}
