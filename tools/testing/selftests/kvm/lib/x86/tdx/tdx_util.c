// SPDX-License-Identifier: GPL-2.0-only

#include <stdint.h>

#include "kvm_util.h"
#include "processor.h"
#include "tdx/td_boot.h"
#include "tdx/td_boot_asm.h"
#include "tdx/tdx_util.h"

/* Arbitrarily selected to avoid overlaps with anything else */
#define TD_BOOT_CODE_SLOT	20
#define TD_BOOT_PARAMETERS_SLOT	21

#define X86_RESET_VECTOR	0xfffffff0ul
#define X86_RESET_VECTOR_SIZE	16

void vm_tdx_setup_boot_code_region(struct kvm_vm *vm)
{
	size_t total_code_size = TD_BOOT_CODE_SIZE + X86_RESET_VECTOR_SIZE;
	vm_paddr_t boot_code_gpa = X86_RESET_VECTOR - TD_BOOT_CODE_SIZE;
	vm_paddr_t alloc_gpa = round_down(boot_code_gpa, PAGE_SIZE);
	size_t nr_pages = DIV_ROUND_UP(total_code_size, PAGE_SIZE);
	vm_paddr_t gpa;
	uint8_t *hva;

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    alloc_gpa,
				    TD_BOOT_CODE_SLOT, nr_pages,
				    KVM_MEM_GUEST_MEMFD);

	gpa = vm_phy_pages_alloc(vm, nr_pages, alloc_gpa, TD_BOOT_CODE_SLOT);
	TEST_ASSERT(gpa == alloc_gpa, "Failed vm_phy_pages_alloc\n");

	virt_map(vm, alloc_gpa, alloc_gpa, nr_pages);
	hva = addr_gpa2hva(vm, boot_code_gpa);
	memcpy(hva, td_boot, TD_BOOT_CODE_SIZE);

	hva += TD_BOOT_CODE_SIZE;
	TEST_ASSERT(hva == addr_gpa2hva(vm, X86_RESET_VECTOR),
		    "Expected RESET vector at hva 0x%lx, got %lx",
		    (unsigned long)addr_gpa2hva(vm, X86_RESET_VECTOR), (unsigned long)hva);

	/*
	 * Handcode "JMP rel8" at the RESET vector to jump back to the TD boot
	 * code, as there are only 16 bytes at the RESET vector before RIP will
	 * wrap back to zero.  Insert a trailing int3 so that the vCPU crashes
	 * in case the JMP somehow falls through.  Note!  The target address is
	 * relative to the end of the instruction!
	 */
	TEST_ASSERT(TD_BOOT_CODE_SIZE < 256,
		    "TD boot code not addressable by 'JMP rel8'");
	hva[0] = 0xeb;
	hva[1] = 256 - 2 - TD_BOOT_CODE_SIZE;
	hva[2] = 0xcc;
}

void vm_tdx_setup_boot_parameters_region(struct kvm_vm *vm, uint32_t nr_runnable_vcpus)
{
	size_t boot_params_size =
		sizeof(struct td_boot_parameters) +
		nr_runnable_vcpus * sizeof(struct td_per_vcpu_parameters);
	int npages = DIV_ROUND_UP(boot_params_size, PAGE_SIZE);
	vm_paddr_t gpa;

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    TD_BOOT_PARAMETERS_GPA,
				    TD_BOOT_PARAMETERS_SLOT, npages,
				    KVM_MEM_GUEST_MEMFD);
	gpa = vm_phy_pages_alloc(vm, npages, TD_BOOT_PARAMETERS_GPA, TD_BOOT_PARAMETERS_SLOT);
	TEST_ASSERT(gpa == TD_BOOT_PARAMETERS_GPA, "Failed vm_phy_pages_alloc\n");

	virt_map(vm, TD_BOOT_PARAMETERS_GPA, TD_BOOT_PARAMETERS_GPA, npages);
}

void vm_tdx_load_common_boot_parameters(struct kvm_vm *vm)
{
	struct td_boot_parameters *params =
		addr_gpa2hva(vm, TD_BOOT_PARAMETERS_GPA);
	uint32_t cr4;

	TEST_ASSERT_EQ(vm->mode, VM_MODE_PXXV48_4K);

	cr4 = kvm_get_default_cr4();

	/* TDX spec 11.6.2: CR4 bit MCE is fixed to 1 */
	cr4 |= X86_CR4_MCE;

	/* Set this because UEFI also sets this up, to handle XMM exceptions */
	cr4 |= X86_CR4_OSXMMEXCPT;

	/* TDX spec 11.6.2: CR4 bit VMXE and SMXE are fixed to 0 */
	cr4 &= ~(X86_CR4_VMXE | X86_CR4_SMXE);

	/* Set parameters! */
	params->cr0 = kvm_get_default_cr0();
	params->cr3 = vm->pgd;
	params->cr4 = cr4;
	params->idtr.base = vm->arch.idt;
	params->idtr.limit = kvm_get_default_idt_limit();
	params->gdtr.base = vm->arch.gdt;
	params->gdtr.limit = kvm_get_default_gdt_limit();

	TEST_ASSERT(params->cr0 != 0, "cr0 should not be 0");
	TEST_ASSERT(params->cr3 != 0, "cr3 should not be 0");
	TEST_ASSERT(params->cr4 != 0, "cr4 should not be 0");
	TEST_ASSERT(params->gdtr.base != 0, "gdt base address should not be 0");
	TEST_ASSERT(params->idtr.base != 0, "idt base address should not be 0");
}

void vm_tdx_load_vcpu_boot_parameters(struct kvm_vm *vm, struct kvm_vcpu *vcpu)
{
	struct td_boot_parameters *params =
		addr_gpa2hva(vm, TD_BOOT_PARAMETERS_GPA);
	struct td_per_vcpu_parameters *vcpu_params =
		&params->per_vcpu[vcpu->id];

	vcpu_params->esp_gva = kvm_allocate_vcpu_stack(vm);
}

void vm_tdx_set_vcpu_entry_point(struct kvm_vcpu *vcpu, void *guest_code)
{
	struct td_boot_parameters *params = addr_gpa2hva(vcpu->vm, TD_BOOT_PARAMETERS_GPA);
	struct td_per_vcpu_parameters *vcpu_params = &params->per_vcpu[vcpu->id];

	vcpu_params->guest_code = (uint64_t)guest_code;
}
