// SPDX-License-Identifier: GPL-2.0
/*
 * translate: Test the KVM_TRANSLATE ioctl on AArch64 by setting up
 * guest page table mappings and verifying that the ioctl correctly
 * translates guest virtual addresses to guest physical addresses.
 */

#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"

#define GUEST_TEST_GVA1		0x400000
#define GUEST_TEST_GVA2		0x500000
#define GUEST_UNMAPPED_GVA	0x600000

/* AArch64 page table entry flags */
#define PTE_RDONLY		(1ULL << 7)	/* AP[2] - Read-only */

static void guest_code(void)
{
	GUEST_DONE();
}

/*
 * Create a read-only page mapping by first creating a normal mapping
 * and then modifying the PTE to add the read-only flag.
 */
static void virt_pg_map_readonly(struct kvm_vm *vm, uint64_t vaddr, uint64_t paddr)
{
	uint64_t *ptep;

	/* First create a normal read-write mapping */
	virt_pg_map(vm, vaddr, paddr);

	/* Now find the PTE and modify it to be read-only */
	ptep = virt_get_pte_hva(vm, vaddr);
	TEST_ASSERT(ptep, "Failed to get PTE for GVA 0x%lx", vaddr);

	/* Set the read-only bit in the PTE */
	*ptep |= PTE_RDONLY;
}

int main(void)
{
	struct kvm_translation tr;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	vm_vaddr_t gva1, gva2;
	vm_paddr_t gpa1, gpa2;

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	/* Set up two different GVA to GPA mappings with different permissions. */
	gva1 = GUEST_TEST_GVA1;
	gpa1 = vm_phy_page_alloc(vm, vm->page_size, vm->memslots[MEM_REGION_TEST_DATA]);
	printf("Allocated GPA1: 0x%lx for GVA1: 0x%lx\n", (unsigned long)gpa1, (unsigned long)gva1);
	virt_pg_map(vm, gva1, gpa1);  /* Read-write mapping */

	gva2 = GUEST_TEST_GVA2;
	gpa2 = vm_phy_page_alloc(vm, vm->page_size, vm->memslots[MEM_REGION_TEST_DATA]);
	printf("Allocated GPA2: 0x%lx for GVA2: 0x%lx\n", (unsigned long)gpa2, (unsigned long)gva2);
	virt_pg_map_readonly(vm, gva2, gpa2);  /* Read-only mapping */

	/*
	 * The vCPU must be run at least once to initialize the system
	 * registers needed for guest address translation.
	 */
	vcpu_run(vcpu);
	TEST_ASSERT_EQ(get_ucall(vcpu, NULL), UCALL_DONE);

	/* Verify the first mapping (read-write) translates correctly. */
	memset(&tr, 0, sizeof(tr));
	tr.linear_address = gva1;
	vcpu_ioctl(vcpu, KVM_TRANSLATE, &tr);

	printf("RW mapping: GVA=0x%lx -> GPA=0x%llx, valid=%d, writeable=%d\n",
	       (unsigned long)gva1, (unsigned long long)tr.physical_address,
	       tr.valid, tr.writeable);
	TEST_ASSERT(tr.valid, "Translation should succeed for mapped GVA");
	TEST_ASSERT_EQ(tr.physical_address, gpa1);
	TEST_ASSERT(tr.writeable, "Read-write GVA should be writeable");

	/* Verify the second mapping (read-only) translates correctly. */
	memset(&tr, 0, sizeof(tr));
	tr.linear_address = gva2;
	vcpu_ioctl(vcpu, KVM_TRANSLATE, &tr);

	printf("RO mapping: GVA=0x%lx -> GPA=0x%llx, valid=%d, writeable=%d\n",
	       (unsigned long)gva2, (unsigned long long)tr.physical_address,
	       tr.valid, tr.writeable);
	TEST_ASSERT(tr.valid, "Translation should succeed for mapped GVA");
	TEST_ASSERT_EQ(tr.physical_address, gpa2);
	TEST_ASSERT(!tr.writeable, "Read-only GVA should not be writeable");

	/* Verify that an unmapped GVA is reported as invalid. */
	memset(&tr, 0, sizeof(tr));
	tr.linear_address = GUEST_UNMAPPED_GVA;
	vcpu_ioctl(vcpu, KVM_TRANSLATE, &tr);

	printf("Unmapped: GVA=0x%lx -> GPA=0x%llx, valid=%d, writeable=%d\n",
	       (unsigned long)GUEST_UNMAPPED_GVA, (unsigned long long)tr.physical_address,
	       tr.valid, tr.writeable);
	TEST_ASSERT(!tr.valid, "Translation should fail for unmapped GVA");

	kvm_vm_free(vm);
	return 0;
}
