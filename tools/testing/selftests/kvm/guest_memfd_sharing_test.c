// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal test for guest_memfd to test that when memory is marked shared in a
 * VM, the host can read and write to it via an mmap()ed address, and the guest
 * can also read and write to it.
 *
 * Copyright (c) 2024, Google LLC.
 */
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "test_util.h"
#include "kvm_util.h"
#include "ucall_common.h"

#define GUEST_MEMFD_SHARING_TEST_SLOT 10
#define GUEST_MEMFD_SHARING_TEST_GPA 0x50000000ULL
#define GUEST_MEMFD_SHARING_TEST_GVA 0x90000000ULL
#define GUEST_MEMFD_SHARING_TEST_OFFSET 0
#define GUEST_MEMFD_SHARING_TEST_GUEST_TO_HOST_VALUE 0x11
#define GUEST_MEMFD_SHARING_TEST_HOST_TO_GUEST_VALUE 0x22

static void guest_code(int page_size)
{
	char *mem;
	int i;

	mem = (char *)GUEST_MEMFD_SHARING_TEST_GVA;

	for (i = 0; i < page_size; ++i) {
		GUEST_ASSERT_EQ(mem[i], GUEST_MEMFD_SHARING_TEST_HOST_TO_GUEST_VALUE);
	}

	memset(mem, GUEST_MEMFD_SHARING_TEST_GUEST_TO_HOST_VALUE, page_size);

	GUEST_DONE();
}

int run_test(struct kvm_vcpu *vcpu, void *hva, int page_size)
{
	struct ucall uc;
	uint64_t uc_cmd;

	memset(hva, GUEST_MEMFD_SHARING_TEST_HOST_TO_GUEST_VALUE, page_size);
	vcpu_args_set(vcpu, 1, page_size);

	/* Reset vCPU to guest_code every time run_test is called. */
	vcpu_arch_set_entry_point(vcpu, guest_code);

	vcpu_run(vcpu);
	uc_cmd = get_ucall(vcpu, &uc);

	if (uc_cmd == UCALL_ABORT) {
		REPORT_GUEST_ASSERT(uc);
		return 1;
	} else if (uc_cmd == UCALL_DONE) {
		char *mem;
		int i;

		mem = hva;
		for (i = 0; i < page_size; ++i)
			TEST_ASSERT_EQ(mem[i], GUEST_MEMFD_SHARING_TEST_GUEST_TO_HOST_VALUE);

		return 0;
	} else {
		TEST_FAIL("Unknown ucall 0x%lx.", uc.cmd);
		return 1;
	}
}

void *add_memslot(struct kvm_vm *vm, int guest_memfd, size_t page_size,
		  bool back_shared_memory_with_guest_memfd)
{
	void *mem;

	if (back_shared_memory_with_guest_memfd) {
		mem = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			   guest_memfd, GUEST_MEMFD_SHARING_TEST_OFFSET);
	} else {
		mem = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	}
	TEST_ASSERT(mem != MAP_FAILED, "mmap should return valid address");

	/*
	 * Setting up this memslot with a KVM_X86_SW_PROTECTED_VM marks all
	 * offsets in the file as shared.
	 */
	vm_set_user_memory_region2(vm, GUEST_MEMFD_SHARING_TEST_SLOT,
				   KVM_MEM_GUEST_MEMFD,
				   GUEST_MEMFD_SHARING_TEST_GPA, page_size, mem,
				   guest_memfd, GUEST_MEMFD_SHARING_TEST_OFFSET);

	return mem;
}

void test_sharing(bool back_shared_memory_with_guest_memfd)
{
	const struct vm_shape shape = {
		.mode = VM_MODE_DEFAULT,
		.type = KVM_X86_SW_PROTECTED_VM,
	};
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	size_t page_size;
	int guest_memfd;
	void *mem;

	TEST_REQUIRE(kvm_check_cap(KVM_CAP_VM_TYPES) & BIT(KVM_X86_SW_PROTECTED_VM));

	vm = vm_create_shape_with_one_vcpu(shape, &vcpu, &guest_code);

	page_size = getpagesize();

	guest_memfd = vm_create_guest_memfd(vm, page_size, 0);

	mem = add_memslot(vm, guest_memfd, page_size, back_shared_memory_with_guest_memfd);

	virt_map(vm, GUEST_MEMFD_SHARING_TEST_GVA, GUEST_MEMFD_SHARING_TEST_GPA, 1);

	run_test(vcpu, mem, page_size);

	/* Toggle private flag of memory attributes and run the test again. */
	if (back_shared_memory_with_guest_memfd) {
		/*
		 * Use MADV_REMOVE to release the backing guest_memfd memory
		 * back to the system before it is used again. Test that this is
		 * only necessary when guest_memfd is used to back shared
		 * memory.
		 */
		madvise(mem, page_size, MADV_REMOVE);
	}
	vm_mem_set_private(vm, GUEST_MEMFD_SHARING_TEST_GPA, page_size);
	vm_mem_set_shared(vm, GUEST_MEMFD_SHARING_TEST_GPA, page_size);

	run_test(vcpu, mem, page_size);

	kvm_vm_free(vm);
	munmap(mem, page_size);
	close(guest_memfd);
}

int main(int argc, char *argv[])
{
	/*
	 * Confidence check that when guest_memfd is associated with a memslot
	 * but only anonymous memory is used to back shared memory, sharing
	 * memory between guest and host works as expected.
	 */
	test_sharing(false);

	/*
	 * Memory sharing should work as expected when shared memory is backed
	 * with guest_memfd.
	 */
	test_sharing(true);

	return 0;
}
