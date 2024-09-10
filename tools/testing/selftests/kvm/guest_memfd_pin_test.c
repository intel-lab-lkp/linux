// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test that pinned pages block KVM from setting memory attributes to PRIVATE.
 *
 * Copyright (c) 2024, Google LLC.
 */
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "test_util.h"
#include "kvm_util.h"
#include "../../../../mm/gup_test.h"

#define GUEST_MEMFD_PIN_TEST_SLOT 10
#define GUEST_MEMFD_PIN_TEST_GPA 0x50000000ULL
#define GUEST_MEMFD_PIN_TEST_OFFSET 0

static int gup_test_fd;

void pin_pages(void *vaddr, uint64_t size)
{
	const struct pin_longterm_test args = {
		.addr = (uint64_t)vaddr,
		.size = size,
		.flags = PIN_LONGTERM_TEST_FLAG_USE_WRITE,
	};

	TEST_ASSERT_EQ(ioctl(gup_test_fd, PIN_LONGTERM_TEST_START, &args), 0);
}

void unpin_pages(void)
{
	TEST_ASSERT_EQ(ioctl(gup_test_fd, PIN_LONGTERM_TEST_STOP), 0);
}

void run_test(void)
{
	struct kvm_vm *vm;
	size_t page_size;
	void *mem;
	int fd;

	vm = vm_create_barebones_type(KVM_X86_SW_PROTECTED_VM);

	page_size = getpagesize();
	fd = vm_create_guest_memfd(vm, page_size, 0);

	mem = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		   GUEST_MEMFD_PIN_TEST_OFFSET);
	TEST_ASSERT(mem != MAP_FAILED, "mmap should return valid address");

	/*
	 * Setting up this memslot with a KVM_X86_SW_PROTECTED_VM marks all
	 * offsets in the file as shared.
	 */
	vm_set_user_memory_region2(vm, GUEST_MEMFD_PIN_TEST_SLOT,
				   KVM_MEM_GUEST_MEMFD,
				   GUEST_MEMFD_PIN_TEST_GPA, page_size, mem, fd,
				   GUEST_MEMFD_PIN_TEST_OFFSET);

	/* Before pinning pages, toggling memory attributes should be fine. */
	vm_mem_set_private(vm, GUEST_MEMFD_PIN_TEST_GPA, page_size);
	vm_mem_set_shared(vm, GUEST_MEMFD_PIN_TEST_GPA, page_size);

	pin_pages(mem, page_size);

	/*
	 * Pinning also faults pages in, so remove these pages from userspace
	 * page tables to properly test that pinning blocks setting memory
	 * attributes to private.
	 */
	TEST_ASSERT_EQ(madvise(mem, page_size, MADV_DONTNEED), 0);

	/* Should fail since the page is still faulted in. */
	TEST_ASSERT_EQ(__vm_set_memory_attributes(vm, GUEST_MEMFD_PIN_TEST_GPA,
						  page_size,
						  KVM_MEMORY_ATTRIBUTE_PRIVATE),
		       -1);
	TEST_ASSERT_EQ(errno, EINVAL);

	unpin_pages();

	/* With the pages unpinned, kvm can set this page to private. */
	vm_mem_set_private(vm, GUEST_MEMFD_PIN_TEST_GPA, page_size);

	kvm_vm_free(vm);
	close(fd);
}

int main(int argc, char *argv[])
{
	gup_test_fd = open("/sys/kernel/debug/gup_test", O_RDWR);
	/*
	 * This test depends on CONFIG_GUP_TEST to provide a kernel module that
	 * exposes pin_user_pages() to userspace.
	 */
	TEST_REQUIRE(gup_test_fd != -1);
	TEST_REQUIRE(kvm_check_cap(KVM_CAP_VM_TYPES) & BIT(KVM_X86_SW_PROTECTED_VM));

	run_test();

	return 0;
}
