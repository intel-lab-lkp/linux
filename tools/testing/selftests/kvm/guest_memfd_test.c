// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright Intel Corporation, 2023
 *
 * Author: Chao Peng <chao.p.peng@linux.intel.com>
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>

#include <linux/bitmap.h>
#include <linux/falloc.h>
#include <linux/mempolicy.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>

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

static void test_mmap(int fd, size_t page_size, size_t total_size)
{
	char *mem;

	mem = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	TEST_ASSERT(mem != MAP_FAILED, "mmap should succeed");
	TEST_ASSERT(munmap(mem, total_size) == 0, "munmap should succeed");
}

static void test_mbind(int fd, size_t page_size, size_t total_size)
{
	unsigned long nodemask = 1; /* nid: 0 */
	unsigned long maxnode = 8;
	unsigned long get_nodemask;
	int get_policy;
	void *mem;
	int ret;

	mem = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	TEST_ASSERT(mem != MAP_FAILED, "mmap for mbind test should succeed");

	/* Test MPOL_INTERLEAVE policy */
	ret = syscall(__NR_mbind, mem, page_size * 2, MPOL_INTERLEAVE,
		      &nodemask, maxnode, 0);
	TEST_ASSERT(!ret, "mbind with INTERLEAVE to node 0 should succeed");
	ret = syscall(__NR_get_mempolicy, &get_policy, &get_nodemask,
		      maxnode, mem, MPOL_F_ADDR);
	TEST_ASSERT(!ret && get_policy == MPOL_INTERLEAVE && get_nodemask == nodemask,
		    "Policy should be MPOL_INTERLEAVE and nodes match");

	/* Test basic MPOL_BIND policy */
	ret = syscall(__NR_mbind, mem + page_size * 2, page_size * 2, MPOL_BIND,
		      &nodemask, maxnode, 0);
	TEST_ASSERT(!ret, "mbind with MPOL_BIND to node 0 should succeed");
	ret = syscall(__NR_get_mempolicy, &get_policy, &get_nodemask,
		      maxnode, mem + page_size * 2, MPOL_F_ADDR);
	TEST_ASSERT(!ret && get_policy == MPOL_BIND && get_nodemask == nodemask,
		    "Policy should be MPOL_BIND and nodes match");

	/* Test MPOL_DEFAULT policy */
	ret = syscall(__NR_mbind, mem, total_size, MPOL_DEFAULT, NULL, 0, 0);
	TEST_ASSERT(!ret, "mbind with MPOL_DEFAULT should succeed");
	ret = syscall(__NR_get_mempolicy, &get_policy, &get_nodemask,
		      maxnode, mem, MPOL_F_ADDR);
	TEST_ASSERT(!ret && get_policy == MPOL_DEFAULT && get_nodemask == 0,
		    "Policy should be MPOL_DEFAULT and nodes zero");

	/* Test with invalid policy */
	ret = syscall(__NR_mbind, mem, page_size, 999, &nodemask, maxnode, 0);
	TEST_ASSERT(ret == -1 && errno == EINVAL,
		    "mbind with invalid policy should fail with EINVAL");

	TEST_ASSERT(munmap(mem, total_size) == 0, "munmap should succeed");
}

static void test_numa_allocation(int fd, size_t page_size, size_t total_size)
{
	unsigned long nodemask = 1;  /* Node 0 */
	unsigned long maxnode = 8;
	void *mem;
	int ret;

	mem = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	TEST_ASSERT(mem != MAP_FAILED, "mmap should succeed");

	/* Set NUMA policy after allocation */
	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, page_size * 2);
	TEST_ASSERT(!ret, "fallocate with aligned offset and size should succeed");
	ret = syscall(__NR_mbind, mem, page_size * 2, MPOL_BIND, &nodemask,
		      maxnode, 0);
	TEST_ASSERT(!ret, "mbind should succeed");

	/* Set NUMA policy before allocation */
	ret = syscall(__NR_mbind, mem + page_size * 2, page_size, MPOL_BIND,
		      &nodemask, maxnode, 0);
	TEST_ASSERT(!ret, "mbind should succeed");
	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE, page_size * 2, page_size * 2);
	TEST_ASSERT(!ret, "fallocate with aligned offset and size should succeed");

	TEST_ASSERT(munmap(mem, total_size) == 0, "munmap should succeed");
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

	for (flag = BIT(0); flag; flag <<= 1) {
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

int main(int argc, char *argv[])
{
	size_t page_size;
	size_t total_size;
	int fd;
	struct kvm_vm *vm;

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_GUEST_MEMFD));

	page_size = getpagesize();
	total_size = page_size * 4;

	vm = vm_create_barebones();

	test_create_guest_memfd_invalid(vm);
	test_create_guest_memfd_multiple(vm);

	fd = vm_create_guest_memfd(vm, total_size, 0);

	test_file_read_write(fd);
	test_mmap(fd, page_size, total_size);
	test_mbind(fd, page_size, total_size);
	test_numa_allocation(fd, page_size, total_size);
	test_file_size(fd, page_size, total_size);
	test_fallocate(fd, page_size, total_size);
	test_invalid_punch_hole(fd, page_size, total_size);

	close(fd);
}
