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
#include <linux/sizes.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "kvm_util.h"
#include "numaif.h"
#include "test_util.h"
#include "ucall_common.h"

static const char dax_path[] = "/dev/dax0.1";
static const size_t dax_size = SZ_4G;

static size_t page_size;

static int vm_create_guest_daxfd(void)
{
	int fd;

	fd = open(dax_path, O_RDWR | O_LARGEFILE);
	TEST_ASSERT(fd != -1, "Cannot open %s: %s", dax_path, strerror(errno));

	return fd;
}

static void test_file_read_write(int fd, size_t total_size)
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

static void test_mmap_supported(int fd, size_t total_size)
{
	const char val = 0xaa;
	char *mem;
	size_t i;
	int ret;

	mem = kvm_mmap(total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd);

	memset(mem, val, total_size);
	for (i = 0; i < total_size; i++)
		TEST_ASSERT_EQ(READ_ONCE(mem[i]), val);

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE, 0,
			page_size);
	TEST_ASSERT(!ret, "fallocate the first page should succeed.");

	for (i = 0; i < page_size; i++)
		TEST_ASSERT_EQ(READ_ONCE(mem[i]), 0x00);
	for (; i < total_size; i++)
		TEST_ASSERT_EQ(READ_ONCE(mem[i]), val);

	memset(mem, val, page_size);
	for (i = 0; i < total_size; i++)
		TEST_ASSERT_EQ(READ_ONCE(mem[i]), val);

	kvm_munmap(mem, total_size);
}

static void test_fault_sigbus(int fd, size_t accessible_size, size_t map_size)
{
	const char val = 0xaa;
	char *mem;
	size_t i;

	mem = kvm_mmap(map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd);

	TEST_EXPECT_SIGBUS(memset(mem, val, map_size));
	TEST_EXPECT_SIGBUS((void)READ_ONCE(mem[accessible_size]));

	for (i = 0; i < accessible_size; i++)
		TEST_ASSERT_EQ(READ_ONCE(mem[i]), val);

	kvm_munmap(mem, map_size);
}

static void test_fault_overflow(int fd, size_t total_size)
{
	total_size = dax_size;

	test_fault_sigbus(fd, total_size, total_size * 4);
}

static void test_file_size(int fd, size_t total_size)
{
	struct stat sb;
	int ret;

	ret = fstat(fd, &sb);
	TEST_ASSERT(!ret, "fstat should succeed");
	/* Can't test total size because dax you get the whole device size */
	//TEST_ASSERT_EQ(sb.st_size, total_size);
	TEST_ASSERT_EQ(sb.st_blksize, page_size);
}

static void test_fallocate(int fd, size_t total_size)
{
	int ret;

	total_size = dax_size;

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, total_size);
	TEST_ASSERT(!ret, "fallocate with aligned offset and size should succeed");

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
			page_size - 1, page_size);
	TEST_ASSERT(ret, "fallocate with unaligned offset should fail");

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE, total_size, page_size);
	TEST_ASSERT(ret, "fallocate beginning at total_size should fail");

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE, total_size + page_size, page_size);
	TEST_ASSERT(ret, "fallocate beginning after total_size should fail");

	/*
	 * The next 2 have opposite behavior of a file. DAX is finite and
	 * therefore those should fail.
	 */
	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
			total_size, page_size);
	TEST_ASSERT(ret, "fallocate(PUNCH_HOLE) at total_size should fail");

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
			total_size + page_size, page_size);
	TEST_ASSERT(ret, "fallocate(PUNCH_HOLE) after total_size should fail");


	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
			page_size, page_size - 1);
	TEST_ASSERT(ret, "fallocate with unaligned size should fail");

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
			page_size, page_size);
	TEST_ASSERT(!ret, "fallocate(PUNCH_HOLE) with aligned offset and size should succeed");

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE, page_size, page_size);
	TEST_ASSERT(!ret, "fallocate to restore punched hole should succeed");
}

static void test_invalid_punch_hole(int fd, size_t total_size)
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

static void test_create_guest_daxfd(void)
{
	int fd1, ret;
	struct stat st1;

	fd1 = vm_create_guest_daxfd();
	TEST_ASSERT(fd1 != -1, "memfd creation should succeed");

	ret = fstat(fd1, &st1);
	TEST_ASSERT(ret != -1, "memfd fstat should succeed");

	close(fd1);
}

#define gmem_test(__test, __vm, __flags)				\
do {									\
	int fd = vm_create_guest_daxfd();				\
									\
	test_##__test(fd, page_size * 4);				\
	close(fd);							\
} while (0)

static void __test_guest_daxfd(struct kvm_vm *vm, uint64_t flags)
{
	pr_info("Testing guest_daxfd with flags 0x%lx\n", flags);
	test_create_guest_daxfd();
	pr_info("test create_guest_daxfd() passed\n");

	gmem_test(file_read_write, vm, flags);
	pr_info("test file_read_write passed\n");

	gmem_test(mmap_supported, vm, flags);
	pr_info("test mmap_supported passed\n");
	gmem_test(fault_overflow, vm, flags);
	pr_info("test fault overflow passed\n");

	gmem_test(file_size, vm, flags);
	pr_info("test file_size passed\n");
	gmem_test(fallocate, vm, flags);
	pr_info("test fallocate passed\n");
	gmem_test(invalid_punch_hole, vm, flags);
	pr_info("test invalid_punch_hole passed\n");
}

static void test_guest_daxfd(unsigned long vm_type)
{
	struct kvm_vm *vm = vm_create_barebones_type(vm_type);

	__test_guest_daxfd(vm, 0);

	kvm_vm_free(vm);
}

static void guest_code(uint8_t *mem, uint64_t size)
{
	size_t i;

	for (i = 0; i < size; i++)
		__GUEST_ASSERT(mem[i] == 0xaa,
			       "Guest expected 0xaa at offset %lu, got 0x%x", i, mem[i]);

	memset(mem, 0xff, size);
	GUEST_DONE();
}

static void test_guest_daxfd_guest(void)
{
	/*
	 * Skip the first 4gb and slot0.  slot0 maps <1gb and is used to back
	 * the guest's code, stack, and page tables, and low memory contains
	 * the PCI hole and other MMIO regions that need to be avoided.
	 */
	const uint64_t gpa = SZ_4G;
	const uint64_t test_size = SZ_4G;
	const int slot = 1;

	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	uint8_t *mem;
	size_t size;
	int fd, i;

	if (!kvm_check_cap(KVM_CAP_GUEST_DAXFD_FLAGS))
		return;

	vm = __vm_create_shape_with_one_vcpu(VM_SHAPE_DEFAULT, &vcpu, 1, guest_code);

	TEST_ASSERT(vm_check_cap(vm, KVM_CAP_GUEST_DAXFD_FLAGS) & GUEST_DAXFD_FLAG_MMAP,
		    "Default VM type should support MMAP, supported flags = 0x%x",
		    vm_check_cap(vm, KVM_CAP_GUEST_DAXFD_FLAGS));

	size = vm->page_size;
	fd = vm_create_guest_daxfd();

	/* Do we need to do this? It's necessary for gmem fd */
	vm_set_user_memory_region2(vm, slot, KVM_MEM_GUEST_DAXFD, gpa, size, NULL, fd, 0);

	mem = kvm_mmap(test_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd);
	memset(mem, 0xaa, test_size);
	kvm_munmap(mem, test_size);

	virt_pg_map(vm, gpa, gpa);
	vcpu_args_set(vcpu, 2, gpa, size);
	vcpu_run(vcpu);

	TEST_ASSERT_EQ(get_ucall(vcpu, NULL), UCALL_DONE);

	mem = kvm_mmap(size, PROT_READ | PROT_WRITE, MAP_SHARED, fd);
	for (i = 0; i < size; i++)
		TEST_ASSERT_EQ(mem[i], 0xff);

	close(fd);
	kvm_vm_free(vm);
}

int main(int argc, char *argv[])
{
	unsigned long vm_types, vm_type;

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_GUEST_DAXFD));

	page_size = getpagesize();

	/*
	 * Not all architectures support KVM_CAP_VM_TYPES. However, those that
	 * support guest_memfd have that support for the default VM type.
	 */
	vm_types = kvm_check_cap(KVM_CAP_VM_TYPES);
	if (!vm_types)
		vm_types = BIT(VM_TYPE_DEFAULT);

	for_each_set_bit(vm_type, &vm_types, BITS_PER_TYPE(vm_types))
		test_guest_daxfd(vm_type);

	test_guest_daxfd_guest();
}
