// SPDX-License-Identifier: GPL-2.0-only

#include <stdint.h>

#include "kvm_util.h"
#include "processor.h"
#include "tdx/td_boot.h"
#include "tdx/tdx_util.h"

/* Arbitrarily selected to avoid overlaps with anything else */
#define TD_BOOT_CODE_SLOT	20

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
