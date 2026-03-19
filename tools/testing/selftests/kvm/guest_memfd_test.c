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
#include <linux/prctl.h>
#include <sys/prctl.h>

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

static size_t page_size;

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

static void test_mmap_cow(int fd, size_t size)
{
	void *mem;

	mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	TEST_ASSERT(mem == MAP_FAILED, "Copy-on-write not allowed by guest_memfd.");
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

static void test_mbind(int fd, size_t total_size)
{
	const unsigned long nodemask_0 = 1; /* nid: 0 */
	unsigned long nodemask = 0;
	unsigned long maxnode = 8;
	int policy;
	char *mem;
	int ret;

	if (!is_multi_numa_node_system())
		return;

	mem = kvm_mmap(total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd);

	/* Test MPOL_INTERLEAVE policy */
	kvm_mbind(mem, page_size * 2, MPOL_INTERLEAVE, &nodemask_0, maxnode, 0);
	kvm_get_mempolicy(&policy, &nodemask, maxnode, mem, MPOL_F_ADDR);
	TEST_ASSERT(policy == MPOL_INTERLEAVE && nodemask == nodemask_0,
		    "Wanted MPOL_INTERLEAVE (%u) and nodemask 0x%lx, got %u and 0x%lx",
		    MPOL_INTERLEAVE, nodemask_0, policy, nodemask);

	/* Test basic MPOL_BIND policy */
	kvm_mbind(mem + page_size * 2, page_size * 2, MPOL_BIND, &nodemask_0, maxnode, 0);
	kvm_get_mempolicy(&policy, &nodemask, maxnode, mem + page_size * 2, MPOL_F_ADDR);
	TEST_ASSERT(policy == MPOL_BIND && nodemask == nodemask_0,
		    "Wanted MPOL_BIND (%u) and nodemask 0x%lx, got %u and 0x%lx",
		    MPOL_BIND, nodemask_0, policy, nodemask);

	/* Test MPOL_DEFAULT policy */
	kvm_mbind(mem, total_size, MPOL_DEFAULT, NULL, 0, 0);
	kvm_get_mempolicy(&policy, &nodemask, maxnode, mem, MPOL_F_ADDR);
	TEST_ASSERT(policy == MPOL_DEFAULT && !nodemask,
		    "Wanted MPOL_DEFAULT (%u) and nodemask 0x0, got %u and 0x%lx",
		    MPOL_DEFAULT, policy, nodemask);

	/* Test with invalid policy */
	ret = mbind(mem, page_size, 999, &nodemask_0, maxnode, 0);
	TEST_ASSERT(ret == -1 && errno == EINVAL,
		    "mbind with invalid policy should fail with EINVAL");

	kvm_munmap(mem, total_size);
}

static void test_numa_allocation(int fd, size_t total_size)
{
	unsigned long node0_mask = 1;  /* Node 0 */
	unsigned long node1_mask = 2;  /* Node 1 */
	unsigned long maxnode = 8;
	void *pages[4];
	int status[4];
	char *mem;
	int i;

	if (!is_multi_numa_node_system())
		return;

	mem = kvm_mmap(total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd);

	for (i = 0; i < 4; i++)
		pages[i] = (char *)mem + page_size * i;

	/* Set NUMA policy after allocation */
	memset(mem, 0xaa, page_size);
	kvm_mbind(pages[0], page_size, MPOL_BIND, &node0_mask, maxnode, 0);
	kvm_fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, page_size);

	/* Set NUMA policy before allocation */
	kvm_mbind(pages[0], page_size * 2, MPOL_BIND, &node1_mask, maxnode, 0);
	kvm_mbind(pages[2], page_size * 2, MPOL_BIND, &node0_mask, maxnode, 0);
	memset(mem, 0xaa, total_size);

	/* Validate if pages are allocated on specified NUMA nodes */
	kvm_move_pages(0, 4, pages, NULL, status, 0);
	TEST_ASSERT(status[0] == 1, "Expected page 0 on node 1, got it on node %d", status[0]);
	TEST_ASSERT(status[1] == 1, "Expected page 1 on node 1, got it on node %d", status[1]);
	TEST_ASSERT(status[2] == 0, "Expected page 2 on node 0, got it on node %d", status[2]);
	TEST_ASSERT(status[3] == 0, "Expected page 3 on node 0, got it on node %d", status[3]);

	/* Punch hole for all pages */
	kvm_fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, total_size);

	/* Change NUMA policy nodes and reallocate */
	kvm_mbind(pages[0], page_size * 2, MPOL_BIND, &node0_mask, maxnode, 0);
	kvm_mbind(pages[2], page_size * 2, MPOL_BIND, &node1_mask, maxnode, 0);
	memset(mem, 0xaa, total_size);

	kvm_move_pages(0, 4, pages, NULL, status, 0);
	TEST_ASSERT(status[0] == 0, "Expected page 0 on node 0, got it on node %d", status[0]);
	TEST_ASSERT(status[1] == 0, "Expected page 1 on node 0, got it on node %d", status[1]);
	TEST_ASSERT(status[2] == 1, "Expected page 2 on node 1, got it on node %d", status[2]);
	TEST_ASSERT(status[3] == 1, "Expected page 3 on node 1, got it on node %d", status[3]);

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
	test_fault_sigbus(fd, total_size, total_size * 4);
}

static unsigned long addr_to_pfn(void *addr)
{
	const uint64_t pagemap_pfn_mask = BIT(54) - 1;
	const uint64_t pagemap_page_present = BIT(63);
	uint64_t page_info;
	ssize_t n_bytes;
	int pagemap_fd;

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	TEST_ASSERT(pagemap_fd > 0, "Opening pagemap should succeed.");

	n_bytes = pread(pagemap_fd, &page_info, 8, (uint64_t)addr / page_size * 8);
	TEST_ASSERT(n_bytes == 8, "pread of pagemap failed. n_bytes=%ld", n_bytes);

	close(pagemap_fd);

	TEST_ASSERT(page_info & pagemap_page_present, "The page for addr should be present");
	return page_info & pagemap_pfn_mask;
}

static void write_memory_failure(unsigned long pfn, bool mark, int return_code)
{
	char path[PATH_MAX];
	char *filename;
	char buf[20];
	int ret;
	int len;
	int fd;

	filename = mark ? "corrupt-pfn" : "unpoison-pfn";
	snprintf(path, PATH_MAX, "/sys/kernel/debug/hwpoison/%s", filename);

	fd = open(path, O_WRONLY);
	TEST_ASSERT(fd > 0, "Failed to open %s.", path);

	len = snprintf(buf, sizeof(buf), "0x%lx\n", pfn);
	if (len < 0 || (unsigned int)len > sizeof(buf))
		TEST_ASSERT(0, "snprintf failed or truncated.");

	ret = write(fd, buf, len);
	if (return_code == 0) {
		/*
		 * If the memory_failure() returns 0, write() should be successful,
		 * which returns how many bytes it writes.
		 */
		TEST_ASSERT(ret > 0, "Writing memory failure (path: %s) failed: %s", path,
			    strerror(errno));
	} else {
		TEST_ASSERT_EQ(ret, -1);
		/* errno is memory_failure() return code. */
		TEST_ASSERT_EQ(errno, return_code);
	}

	close(fd);
}

static void mark_memory_failure(unsigned long pfn, int return_code)
{
	write_memory_failure(pfn, true, return_code);
}

static void unmark_memory_failure(unsigned long pfn, int return_code)
{
	write_memory_failure(pfn, false, return_code);
}

enum memory_failure_injection_method {
	MF_INJECT_DEBUGFS,
	MF_INJECT_MADVISE,
};

static void do_test_memory_failure(int fd, size_t total_size,
				   enum memory_failure_injection_method method, int kill_config,
				   bool map_page, bool dirty_page, bool sigbus_expected,
				   int return_code)
{
	unsigned long memory_failure_pfn;
	char *memory_failure_addr;
	char *mem;
	int ret;

	mem = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	TEST_ASSERT(mem != MAP_FAILED, "mmap() for guest_memfd should succeed.");
	memory_failure_addr = mem + page_size;
	if (dirty_page)
		*memory_failure_addr = 'A';
	else
		READ_ONCE(*memory_failure_addr);

	/* Fault in page to read pfn, then unmap page for testing if needed. */
	memory_failure_pfn = addr_to_pfn(memory_failure_addr);
	if (!map_page)
		madvise(memory_failure_addr, page_size, MADV_DONTNEED);

	ret = prctl(PR_MCE_KILL, PR_MCE_KILL_SET, kill_config, 0, 0);
	TEST_ASSERT_EQ(ret, 0);

	ret = 0;
	switch (method) {
	case MF_INJECT_DEBUGFS: {
		/* DEBUGFS injection handles return_code test inside the mark_memory_failure(). */
		if (sigbus_expected)
			TEST_EXPECT_SIGBUS(mark_memory_failure(memory_failure_pfn, return_code));
		else
			mark_memory_failure(memory_failure_pfn, return_code);
		break;
	}
	case MF_INJECT_MADVISE: {
		/*
		 * MADV_HWPOISON uses get_user_pages() so the page will always
		 * be faulted in at the point of memory_failure()
		 */
		if (sigbus_expected)
			TEST_EXPECT_SIGBUS(ret = madvise(memory_failure_addr,
							 page_size, MADV_HWPOISON));
		else
			ret = madvise(memory_failure_addr, page_size, MADV_HWPOISON);

		if (return_code == 0)
			TEST_ASSERT(ret == return_code, "Memory failure failed. Errno: %s",
							strerror(errno));
		else {
			/* errno is memory_failure() return code. */
			TEST_ASSERT_EQ(errno, return_code);
		}
		break;
	}
	default:
		TEST_FAIL("Unhandled memory failure injection method %d.", method);
	}

	TEST_EXPECT_SIGBUS(READ_ONCE(*memory_failure_addr));
	TEST_EXPECT_SIGBUS(*memory_failure_addr = 'A');

	ret = munmap(mem, total_size);
	TEST_ASSERT(!ret, "munmap() should succeed.");

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE, 0,
			total_size);
	TEST_ASSERT(!ret, "Truncate the entire file (cleanup) should succeed.");

	ret = prctl(PR_MCE_KILL, PR_MCE_KILL_SET, PR_MCE_KILL_DEFAULT, 0, 0);
	TEST_ASSERT_EQ(ret, 0);

	unmark_memory_failure(memory_failure_pfn, 0);
}

static void test_memory_failure(int fd, size_t total_size)
{
	do_test_memory_failure(fd, total_size, MF_INJECT_DEBUGFS, PR_MCE_KILL_EARLY, true, true, true, 0);
	do_test_memory_failure(fd, total_size, MF_INJECT_DEBUGFS, PR_MCE_KILL_EARLY, true, false, false, 0);
	do_test_memory_failure(fd, total_size, MF_INJECT_DEBUGFS, PR_MCE_KILL_EARLY, false, true, false, 0);
	do_test_memory_failure(fd, total_size, MF_INJECT_DEBUGFS, PR_MCE_KILL_LATE, true, true, false, 0);
	do_test_memory_failure(fd, total_size, MF_INJECT_DEBUGFS, PR_MCE_KILL_LATE, true, false, false, 0);
	do_test_memory_failure(fd, total_size, MF_INJECT_DEBUGFS, PR_MCE_KILL_LATE, false, true, false, 0);
	/*
	 * If madvise() is used to inject errors, memory_failure() handling is invoked with the
	 * MF_ACTION_REQUIRED flag set, aligned with memory failure handling for a consumed memory
	 * error, where the machine check memory corruption kill policy is ignored. Hence, testing with
	 * PR_MCE_KILL_DEFAULT covers all cases.
	 */
	do_test_memory_failure(fd, total_size, MF_INJECT_MADVISE, PR_MCE_KILL_DEFAULT, true, true, true, 0);
	do_test_memory_failure(fd, total_size, MF_INJECT_MADVISE, PR_MCE_KILL_DEFAULT, true, false, false, 0);
}

static void test_fault_private(int fd, size_t total_size)
{
	test_fault_sigbus(fd, 0, total_size);
}

static void test_mmap_not_supported(int fd, size_t total_size)
{
	char *mem;

	mem = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	TEST_ASSERT_EQ(mem, MAP_FAILED);

	mem = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	TEST_ASSERT_EQ(mem, MAP_FAILED);
}

static void test_file_size(int fd, size_t total_size)
{
	struct stat sb;
	int ret;

	ret = fstat(fd, &sb);
	TEST_ASSERT(!ret, "fstat should succeed");
	TEST_ASSERT_EQ(sb.st_size, total_size);
	TEST_ASSERT_EQ(sb.st_blksize, page_size);
}

static void test_fallocate(int fd, size_t total_size)
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

static void test_create_guest_memfd_invalid_sizes(struct kvm_vm *vm,
						  uint64_t guest_memfd_flags)
{
	size_t size;
	int fd;

	for (size = 1; size < page_size; size++) {
		fd = __vm_create_guest_memfd(vm, size, guest_memfd_flags);
		TEST_ASSERT(fd < 0 && errno == EINVAL,
			    "guest_memfd() with non-page-aligned page size '0x%lx' should fail with EINVAL",
			    size);
	}
}

static void test_create_guest_memfd_multiple(struct kvm_vm *vm)
{
	int fd1, fd2, ret;
	struct stat st1, st2;

	fd1 = __vm_create_guest_memfd(vm, page_size, 0);
	TEST_ASSERT(fd1 != -1, "memfd creation should succeed");

	ret = fstat(fd1, &st1);
	TEST_ASSERT(ret != -1, "memfd fstat should succeed");
	TEST_ASSERT(st1.st_size == page_size, "memfd st_size should match requested size");

	fd2 = __vm_create_guest_memfd(vm, page_size * 2, 0);
	TEST_ASSERT(fd2 != -1, "memfd creation should succeed");

	ret = fstat(fd2, &st2);
	TEST_ASSERT(ret != -1, "memfd fstat should succeed");
	TEST_ASSERT(st2.st_size == page_size * 2, "second memfd st_size should match requested size");

	ret = fstat(fd1, &st1);
	TEST_ASSERT(ret != -1, "memfd fstat should succeed");
	TEST_ASSERT(st1.st_size == page_size, "first memfd st_size should still match requested size");
	TEST_ASSERT(st1.st_ino != st2.st_ino, "different memfd should have different inode numbers");

	close(fd2);
	close(fd1);
}

static void test_guest_memfd_flags(struct kvm_vm *vm)
{
	uint64_t valid_flags = vm_check_cap(vm, KVM_CAP_GUEST_MEMFD_FLAGS);
	uint64_t flag;
	int fd;

	for (flag = BIT(0); flag; flag <<= 1) {
		fd = __vm_create_guest_memfd(vm, page_size, flag);
		if (flag & valid_flags) {
			TEST_ASSERT(fd >= 0,
				    "guest_memfd() with flag '0x%lx' should succeed",
				    flag);
			close(fd);
		} else {
			TEST_ASSERT(fd < 0 && errno == EINVAL,
				    "guest_memfd() with flag '0x%lx' should fail with EINVAL",
				    flag);
		}
	}
}

#define gmem_test(__test, __vm, __flags)				\
do {									\
	int fd = vm_create_guest_memfd(__vm, page_size * 4, __flags);	\
									\
	test_##__test(fd, page_size * 4);				\
	close(fd);							\
} while (0)

static void __test_guest_memfd(struct kvm_vm *vm, uint64_t flags)
{
	test_create_guest_memfd_multiple(vm);
	test_create_guest_memfd_invalid_sizes(vm, flags);

	gmem_test(file_read_write, vm, flags);

	if (flags & GUEST_MEMFD_FLAG_MMAP) {
		if (flags & GUEST_MEMFD_FLAG_INIT_SHARED) {
			gmem_test(mmap_supported, vm, flags);
			gmem_test(fault_overflow, vm, flags);
			gmem_test(numa_allocation, vm, flags);
			gmem_test(memory_failure, vm, flags);
		} else {
			gmem_test(fault_private, vm, flags);
		}

		gmem_test(mmap_cow, vm, flags);
		gmem_test(mbind, vm, flags);
	} else {
		gmem_test(mmap_not_supported, vm, flags);
	}

	gmem_test(file_size, vm, flags);
	gmem_test(fallocate, vm, flags);
	gmem_test(invalid_punch_hole, vm, flags);
}

static void test_guest_memfd(unsigned long vm_type)
{
	struct kvm_vm *vm = vm_create_barebones_type(vm_type);
	uint64_t flags;

	test_guest_memfd_flags(vm);

	__test_guest_memfd(vm, 0);

	flags = vm_check_cap(vm, KVM_CAP_GUEST_MEMFD_FLAGS);
	if (flags & GUEST_MEMFD_FLAG_MMAP)
		__test_guest_memfd(vm, GUEST_MEMFD_FLAG_MMAP);

	/* MMAP should always be supported if INIT_SHARED is supported. */
	if (flags & GUEST_MEMFD_FLAG_INIT_SHARED)
		__test_guest_memfd(vm, GUEST_MEMFD_FLAG_MMAP |
				       GUEST_MEMFD_FLAG_INIT_SHARED);

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

static void test_guest_memfd_guest(void)
{
	/*
	 * Skip the first 4gb and slot0.  slot0 maps <1gb and is used to back
	 * the guest's code, stack, and page tables, and low memory contains
	 * the PCI hole and other MMIO regions that need to be avoided.
	 */
	const uint64_t gpa = SZ_4G;
	const int slot = 1;

	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	uint8_t *mem;
	size_t size;
	int fd, i;

	if (!kvm_check_cap(KVM_CAP_GUEST_MEMFD_FLAGS))
		return;

	vm = __vm_create_shape_with_one_vcpu(VM_SHAPE_DEFAULT, &vcpu, 1, guest_code);

	TEST_ASSERT(vm_check_cap(vm, KVM_CAP_GUEST_MEMFD_FLAGS) & GUEST_MEMFD_FLAG_MMAP,
		    "Default VM type should support MMAP, supported flags = 0x%x",
		    vm_check_cap(vm, KVM_CAP_GUEST_MEMFD_FLAGS));
	TEST_ASSERT(vm_check_cap(vm, KVM_CAP_GUEST_MEMFD_FLAGS) & GUEST_MEMFD_FLAG_INIT_SHARED,
		    "Default VM type should support INIT_SHARED, supported flags = 0x%x",
		    vm_check_cap(vm, KVM_CAP_GUEST_MEMFD_FLAGS));

	size = vm->page_size;
	fd = vm_create_guest_memfd(vm, size, GUEST_MEMFD_FLAG_MMAP |
					     GUEST_MEMFD_FLAG_INIT_SHARED);
	vm_set_user_memory_region2(vm, slot, KVM_MEM_GUEST_MEMFD, gpa, size, NULL, fd, 0);

	mem = kvm_mmap(size, PROT_READ | PROT_WRITE, MAP_SHARED, fd);
	memset(mem, 0xaa, size);
	kvm_munmap(mem, size);

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

static void __guest_code_read(uint8_t *mem)
{
	READ_ONCE(*mem);
	GUEST_SYNC(0);
	READ_ONCE(*mem);
	GUEST_DONE();
}

static void guest_read(struct kvm_vcpu *vcpu, uint64_t gpa, int expected_errno)
{
	vcpu_args_set(vcpu, 1, gpa);

	if (expected_errno) {
		TEST_ASSERT_EQ(_vcpu_run(vcpu), -1);
		TEST_ASSERT_EQ(errno, expected_errno);
	} else {
		vcpu_run(vcpu);
		TEST_ASSERT_EQ(get_ucall(vcpu, NULL), UCALL_SYNC);
	}
}

static void test_memory_failure_guest(void)
{
	const uint64_t gpa = SZ_4G;
	const int slot = 1;

	unsigned long memory_failure_pfn;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	uint8_t *mem;
	size_t size;
	int fd;

	if (!kvm_has_cap(KVM_CAP_GUEST_MEMFD_FLAGS))
		return;

	vm = __vm_create_shape_with_one_vcpu(VM_SHAPE_DEFAULT, &vcpu, 1, __guest_code_read);

	size = vm->page_size;
	fd = vm_create_guest_memfd(vm, size, GUEST_MEMFD_FLAG_MMAP | GUEST_MEMFD_FLAG_INIT_SHARED);
	vm_set_user_memory_region2(vm, slot, KVM_MEM_GUEST_MEMFD, gpa, size, NULL, fd, 0);

	mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	TEST_ASSERT(mem != MAP_FAILED, "mmap() for guest_memfd should succeed.");
	virt_pg_map(vm, gpa, gpa);

	/* Fault in page to read pfn, then unmap page for testing. */
	READ_ONCE(*mem);
	memory_failure_pfn = addr_to_pfn(mem);
	munmap(mem, size);

	/* Fault page into stage2 page tables. */
	guest_read(vcpu, gpa, 0);

	mark_memory_failure(memory_failure_pfn, 0);

	guest_read(vcpu, gpa, EHWPOISON);

	close(fd);
	kvm_vm_free(vm);

	unmark_memory_failure(memory_failure_pfn, 0);
}

int main(int argc, char *argv[])
{
	unsigned long vm_types, vm_type;

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_GUEST_MEMFD));

	page_size = getpagesize();

	/*
	 * Not all architectures support KVM_CAP_VM_TYPES. However, those that
	 * support guest_memfd have that support for the default VM type.
	 */
	vm_types = kvm_check_cap(KVM_CAP_VM_TYPES);
	if (!vm_types)
		vm_types = BIT(VM_TYPE_DEFAULT);

	for_each_set_bit(vm_type, &vm_types, BITS_PER_TYPE(vm_types))
		test_guest_memfd(vm_type);

	test_guest_memfd_guest();
	test_memory_failure_guest();
}
