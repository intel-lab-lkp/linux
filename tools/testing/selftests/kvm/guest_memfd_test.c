// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright Intel Corporation, 2023
 *
 * Author: Chao Peng <chao.p.peng@linux.intel.com>
 */
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>

#include <linux/bitmap.h>
#include <linux/falloc.h>
#include <linux/kvm.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "kvm_util.h"
#include "test_util.h"

static void test_file_read_write(int fd)
{
	char buf[64];

	TEST_ASSERT(read(fd, buf, sizeof(buf)) < 0,
		    "read on a guest_mem fd should fail");
	TEST_ASSERT(write(fd, buf, sizeof(buf)) < 0,
		    "write on a guest_mem fd should fail");
	TEST_ASSERT(pread(fd, buf, sizeof(buf), 0) < 0,
		    "pread on a guest_mem fd should fail");
	TEST_ASSERT(pwrite(fd, buf, sizeof(buf), 0) < 0,
		    "pwrite on a guest_mem fd should fail");
}

static void test_mmap_should_map_pages_into_userspace(int fd, size_t page_size)
{
	char *mem;

	mem = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	TEST_ASSERT(mem != MAP_FAILED, "mmap should return valid address");

	TEST_ASSERT_EQ(munmap(mem, page_size), 0);
}

static void test_madvise_no_error_when_pages_not_faulted(int fd, size_t page_size)
{
	char *mem;

	mem = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	TEST_ASSERT(mem != MAP_FAILED, "mmap should return valid address");

	TEST_ASSERT_EQ(madvise(mem, page_size, MADV_DONTNEED), 0);

	TEST_ASSERT_EQ(munmap(mem, page_size), 0);
}

static void assert_not_faultable(char *address)
{
	pid_t child_pid;

	child_pid = fork();
	TEST_ASSERT(child_pid != -1, "fork failed");

	if (child_pid == 0) {
		*address = 'A';
	} else {
		int status;
		waitpid(child_pid, &status, 0);

		TEST_ASSERT(WIFSIGNALED(status),
			    "Child should have exited with a signal");
		TEST_ASSERT_EQ(WTERMSIG(status), SIGBUS);
	}
}

/*
 * Pages should not be faultable before association with memslot because pages
 * (in a KVM_X86_SW_PROTECTED_VM) only default to faultable at memslot
 * association time.
 */
static void test_pages_not_faultable_if_not_associated_with_memslot(int fd,
								    size_t page_size)
{
	char *mem = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fd, 0);
	TEST_ASSERT(mem != MAP_FAILED, "mmap should return valid address");

	assert_not_faultable(mem);

	TEST_ASSERT_EQ(munmap(mem, page_size), 0);
}

static void test_pages_faultable_if_marked_faultable(struct kvm_vm *vm, int fd,
						     size_t page_size)
{
	char *mem;
	uint64_t gpa = 0;
	uint64_t guest_memfd_offset = 0;

	/*
	 * This test uses KVM_X86_SW_PROTECTED_VM is required to set
	 * arch.has_private_mem, to add a memslot with guest_memfd to a VM.
	 */
	if (!(kvm_check_cap(KVM_CAP_VM_TYPES) & BIT(KVM_X86_SW_PROTECTED_VM))) {
		printf("Faultability test skipped since KVM_X86_SW_PROTECTED_VM is not supported.");
		return;
	}

	mem = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		   guest_memfd_offset);
	TEST_ASSERT(mem != MAP_FAILED, "mmap should return valid address");

	/*
	 * Setting up this memslot with a KVM_X86_SW_PROTECTED_VM marks all
	 * offsets in the file as shared, allowing pages to be faulted in.
	 */
	vm_set_user_memory_region2(vm, 0, KVM_MEM_GUEST_MEMFD, gpa, page_size,
				   mem, fd, guest_memfd_offset);

	*mem = 'A';
	TEST_ASSERT_EQ(*mem, 'A');

	/* Should fail since the page is still faulted in. */
	TEST_ASSERT_EQ(__vm_set_memory_attributes(vm, gpa, page_size,
						  KVM_MEMORY_ATTRIBUTE_PRIVATE),
		       -1);
	TEST_ASSERT_EQ(errno, EINVAL);

	/*
	 * Use madvise() to remove the pages from userspace page tables, then
	 * test that the page is still faultable, and that page contents remain
	 * the same.
	 */
	madvise(mem, page_size, MADV_DONTNEED);
	TEST_ASSERT_EQ(*mem, 'A');

	/* Tell kernel to unmap the page from userspace. */
	madvise(mem, page_size, MADV_DONTNEED);

	/* Now kernel can set this page to private. */
	vm_mem_set_private(vm, gpa, page_size);
	assert_not_faultable(mem);

	/*
	 * Should be able to fault again after setting this back to shared, and
	 * memory contents should be cleared since pages must be re-prepared for
	 * SHARED use.
	 */
	vm_mem_set_shared(vm, gpa, page_size);
	TEST_ASSERT_EQ(*mem, 0);

	/* Cleanup */
	vm_set_user_memory_region2(vm, 0, KVM_MEM_GUEST_MEMFD, gpa, 0, mem, fd,
				   guest_memfd_offset);

	TEST_ASSERT_EQ(munmap(mem, page_size), 0);
}

static void test_madvise_remove_releases_pages(struct kvm_vm *vm, int fd,
					       size_t page_size)
{
	char *mem;
	uint64_t gpa = 0;
	uint64_t guest_memfd_offset = 0;

	/*
	 * This test uses KVM_X86_SW_PROTECTED_VM is required to set
	 * arch.has_private_mem, to add a memslot with guest_memfd to a VM.
	 */
	if (!(kvm_check_cap(KVM_CAP_VM_TYPES) & BIT(KVM_X86_SW_PROTECTED_VM))) {
		printf("madvise test skipped since KVM_X86_SW_PROTECTED_VM is not supported.");
		return;
	}

	mem = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	TEST_ASSERT(mem != MAP_FAILED, "mmap should return valid address");

	/*
	 * Setting up this memslot with a KVM_X86_SW_PROTECTED_VM marks all
	 * offsets in the file as shared, allowing pages to be faulted in.
	 */
	vm_set_user_memory_region2(vm, 0, KVM_MEM_GUEST_MEMFD, gpa, page_size,
				   mem, fd, guest_memfd_offset);

	*mem = 'A';
	TEST_ASSERT_EQ(*mem, 'A');

	/*
	 * MADV_DONTNEED causes pages to be removed from userspace page tables
	 * but should not release pages, hence page contents are kept.
	 */
	TEST_ASSERT_EQ(madvise(mem, page_size, MADV_DONTNEED), 0);
	TEST_ASSERT_EQ(*mem, 'A');

	/*
	 * MADV_REMOVE causes pages to be released. Pages are then zeroed when
	 * prepared for shared use, hence 0 is expected on next fault.
	 */
	TEST_ASSERT_EQ(madvise(mem, page_size, MADV_REMOVE), 0);
	TEST_ASSERT_EQ(*mem, 0);

	TEST_ASSERT_EQ(munmap(mem, page_size), 0);

	/* Cleanup */
	vm_set_user_memory_region2(vm, 0, KVM_MEM_GUEST_MEMFD, gpa, 0, mem, fd,
				   guest_memfd_offset);
}

static void test_using_memory_directly_from_userspace(struct kvm_vm *vm,
						      int fd, size_t page_size)
{
	test_mmap_should_map_pages_into_userspace(fd, page_size);

	test_madvise_no_error_when_pages_not_faulted(fd, page_size);

	test_pages_not_faultable_if_not_associated_with_memslot(fd, page_size);

	test_pages_faultable_if_marked_faultable(vm, fd, page_size);

	test_madvise_remove_releases_pages(vm, fd, page_size);
}

static void test_file_size(int fd, size_t page_size, size_t total_size)
{
	struct stat sb;
	int ret;

	ret = fstat(fd, &sb);
	TEST_ASSERT(!ret, "fstat should succeed");
	TEST_ASSERT_EQ(sb.st_size, total_size);
	TEST_ASSERT_EQ(sb.st_blksize, page_size);
}

static void test_fallocate(int fd, size_t page_size, size_t total_size)
{
	int ret;

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, total_size);
	TEST_ASSERT(!ret, "fallocate with aligned offset and size should succeed");

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
			page_size - 1, page_size);
	TEST_ASSERT(ret, "fallocate with unaligned offset should fail");

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE, total_size, page_size);
	TEST_ASSERT(ret, "fallocate beginning at total_size should fail");

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE, total_size + page_size, page_size);
	TEST_ASSERT(ret, "fallocate beginning after total_size should fail");

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
			total_size, page_size);
	TEST_ASSERT(!ret, "fallocate(PUNCH_HOLE) at total_size should succeed");

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
			total_size + page_size, page_size);
	TEST_ASSERT(!ret, "fallocate(PUNCH_HOLE) after total_size should succeed");

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
			page_size, page_size - 1);
	TEST_ASSERT(ret, "fallocate with unaligned size should fail");

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
			page_size, page_size);
	TEST_ASSERT(!ret, "fallocate(PUNCH_HOLE) with aligned offset and size should succeed");

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE, page_size, page_size);
	TEST_ASSERT(!ret, "fallocate to restore punched hole should succeed");
}

static void test_invalid_punch_hole(int fd, size_t page_size, size_t total_size)
{
	struct {
		off_t offset;
		off_t len;
	} testcases[] = {
		{0, 1},
		{0, page_size - 1},
		{0, page_size + 1},

		{1, 1},
		{1, page_size - 1},
		{1, page_size},
		{1, page_size + 1},

		{page_size, 1},
		{page_size, page_size - 1},
		{page_size, page_size + 1},
	};
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(testcases); i++) {
		ret = fallocate(fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
				testcases[i].offset, testcases[i].len);
		TEST_ASSERT(ret == -1 && errno == EINVAL,
			    "PUNCH_HOLE with !PAGE_SIZE offset (%lx) and/or length (%lx) should fail",
			    testcases[i].offset, testcases[i].len);
	}
}

static void test_create_guest_memfd_invalid(struct kvm_vm *vm)
{
	uint64_t valid_flags = KVM_GUEST_MEMFD_HUGETLB;
	size_t page_size = getpagesize();
	uint64_t flag;
	size_t size;
	int fd;

	for (size = 1; size < page_size; size++) {
		fd = __vm_create_guest_memfd(vm, size, 0);
		TEST_ASSERT(fd == -1 && errno == EINVAL,
			    "guest_memfd() with non-page-aligned page size '0x%lx' should fail with EINVAL",
			    size);
	}

	for (flag = 0; flag; flag <<= 1) {
		if (flag & valid_flags)
			continue;

		fd = __vm_create_guest_memfd(vm, page_size, flag);
		TEST_ASSERT(fd == -1 && errno == EINVAL,
			    "guest_memfd() with flag '0x%lx' should fail with EINVAL",
			    flag);
	}
}

static void test_create_guest_memfd_multiple(struct kvm_vm *vm)
{
	int fd1, fd2, ret;
	struct stat st1, st2;

	fd1 = __vm_create_guest_memfd(vm, 4096, 0);
	TEST_ASSERT(fd1 != -1, "memfd creation should succeed");

	ret = fstat(fd1, &st1);
	TEST_ASSERT(ret != -1, "memfd fstat should succeed");
	TEST_ASSERT(st1.st_size == 4096, "memfd st_size should match requested size");

	fd2 = __vm_create_guest_memfd(vm, 8192, 0);
	TEST_ASSERT(fd2 != -1, "memfd creation should succeed");

	ret = fstat(fd2, &st2);
	TEST_ASSERT(ret != -1, "memfd fstat should succeed");
	TEST_ASSERT(st2.st_size == 8192, "second memfd st_size should match requested size");

	ret = fstat(fd1, &st1);
	TEST_ASSERT(ret != -1, "memfd fstat should succeed");
	TEST_ASSERT(st1.st_size == 4096, "first memfd st_size should still match requested size");
	TEST_ASSERT(st1.st_ino != st2.st_ino, "different memfd should have different inode numbers");

	close(fd2);
	close(fd1);
}

static void test_guest_memfd(struct kvm_vm *vm, uint32_t flags, size_t page_size)
{
	size_t total_size;
	int fd;

	total_size = page_size * 4;

	fd = vm_create_guest_memfd(vm, total_size, flags);

	test_file_read_write(fd);
	test_file_size(fd, page_size, total_size);
	test_fallocate(fd, page_size, total_size);
	test_invalid_punch_hole(fd, page_size, total_size);

	test_using_memory_directly_from_userspace(vm, fd, page_size);

	close(fd);
}

int main(int argc, char *argv[])
{
	struct kvm_vm *vm;

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_GUEST_MEMFD));

	if ((kvm_check_cap(KVM_CAP_VM_TYPES) & BIT(KVM_X86_SW_PROTECTED_VM)))
		vm = vm_create_barebones_type(KVM_X86_SW_PROTECTED_VM);
	else
		vm = vm_create_barebones();

	test_create_guest_memfd_invalid(vm);
	test_create_guest_memfd_multiple(vm);

	printf("Test guest_memfd with 4K pages\n");
	test_guest_memfd(vm, 0, getpagesize());
	printf("\tPASSED\n");

	printf("Test guest_memfd with 2M pages\n");
	test_guest_memfd(vm, KVM_GUEST_MEMFD_HUGETLB | KVM_GUEST_MEMFD_HUGE_2MB,
			 2UL << 20);
	printf("\tPASSED\n");

	printf("Test guest_memfd with 1G pages\n");
	test_guest_memfd(vm, KVM_GUEST_MEMFD_HUGETLB | KVM_GUEST_MEMFD_HUGE_1GB,
			 1UL << 30);
	printf("\tPASSED\n");

	return 0;
}
