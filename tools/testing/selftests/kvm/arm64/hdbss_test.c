// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test the KVM_CAP_ARM_HDBSS_BUFFER_SIZE ioctl interface: query
 * semantics, accepted/rejected sizes, and the interaction with the
 * dirty ring. Does not run guest code.
 *
 * Copyright (C) 2026 Huawei Technologies Co., Ltd
 * Author: Tian Zheng <zhengtian10@huawei.com>
 */

#include <errno.h>
#include <linux/sizes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kvm_util.h"
#include "test_util.h"

/*
 * The kernel default is PAGE_SIZE: derive it from the running kernel so
 * the test also holds on 16KB/64KB page systems.
 */
#define HDBSS_DEFAULT_SIZE	((u64)getpagesize())
#define HDBSS_MAX_SIZE		SZ_2M

static bool hdbss_supported(void)
{
	return kvm_check_cap(KVM_CAP_ARM_HDBSS_BUFFER_SIZE) > 0;
}

static long hdbss_global_query(void)
{
	return (long)kvm_check_cap(KVM_CAP_ARM_HDBSS_BUFFER_SIZE);
}

static long hdbss_vm_query(struct kvm_vm *vm)
{
	return (long)vm_check_cap(vm, KVM_CAP_ARM_HDBSS_BUFFER_SIZE);
}

static int hdbss_set(struct kvm_vm *vm, u64 size)
{
	return __vm_enable_cap(vm, KVM_CAP_ARM_HDBSS_BUFFER_SIZE, size);
}

static void test_global_query(void)
{
	long ret;

	ret = hdbss_global_query();
	TEST_ASSERT(ret == HDBSS_MAX_SIZE,
		    "Global HDBSS query returned %ld (0x%lx), expected %d (0x%x)",
		    ret, ret, HDBSS_MAX_SIZE, HDBSS_MAX_SIZE);
	pr_info("Global HDBSS max size: %ld bytes\n", ret);
}

static void test_vm_query_default(void)
{
	struct kvm_vm *vm;
	long ret;

	vm = vm_create_barebones();
	ret = hdbss_vm_query(vm);
	TEST_ASSERT(ret == HDBSS_DEFAULT_SIZE,
		    "Per-VM HDBSS query (unconfigured) returned %ld, expected %ld",
		    ret, (long)HDBSS_DEFAULT_SIZE);
	pr_info("Default HDBSS buffer size: %ld bytes\n", ret);
	kvm_vm_free(vm);
}

static void test_vm_query_after_set(u64 set_size)
{
	struct kvm_vm *vm;
	long ret;

	vm = vm_create_barebones();
	TEST_ASSERT_EQ(hdbss_set(vm, set_size), 0);
	ret = hdbss_vm_query(vm);
	TEST_ASSERT(ret == (long)set_size,
		    "Per-VM HDBSS query after set returned %ld, expected %lld",
		    ret, (unsigned long long)set_size);
	kvm_vm_free(vm);
}

static void test_set_valid(u64 size)
{
	struct kvm_vm *vm;
	int ret;

	vm = vm_create_barebones();
	ret = hdbss_set(vm, size);
	TEST_ASSERT(ret == 0,
		    "Setting HDBSS buffer size to %lld (0x%llx) failed: %d (%s)",
		    (unsigned long long)size, (unsigned long long)size,
		    ret, strerror(errno));
	pr_info("Set HDBSS buffer size to %lld bytes: OK\n",
		(unsigned long long)size);
	kvm_vm_free(vm);
}

static void test_set_invalid(u64 size, int expected_errno)
{
	struct kvm_vm *vm;
	int ret;

	vm = vm_create_barebones();
	ret = hdbss_set(vm, size);
	TEST_ASSERT(ret == -1 && errno == expected_errno,
		    "HDBSS buffer size set to %lld (0x%llx) should fail with %d, got ret=%d errno=%d (%s)",
		    (unsigned long long)size, (unsigned long long)size,
		    expected_errno, ret, errno, strerror(errno));
	kvm_vm_free(vm);
}

static void test_set_after_vcpu(void)
{
	struct kvm_vm *vm;
	int ret;

	vm = vm_create_barebones();
	__vm_vcpu_add(vm, 0);
	ret = hdbss_set(vm, HDBSS_DEFAULT_SIZE);
	TEST_ASSERT(ret == -1 && errno == EINVAL,
		    "Setting HDBSS buffer size after vCPU creation should fail with EINVAL, got ret=%d errno=%d",
		    ret, errno);
	pr_info("Set HDBSS after vCPU creation: correctly rejected\n");
	kvm_vm_free(vm);
}

static void test_set_twice(void)
{
	struct kvm_vm *vm;
	int ret;

	vm = vm_create_barebones();
	TEST_ASSERT_EQ(hdbss_set(vm, HDBSS_DEFAULT_SIZE), 0);
	ret = hdbss_set(vm, HDBSS_DEFAULT_SIZE * 2);
	TEST_ASSERT(ret == -1 && errno == EBUSY,
		    "Duplicate HDBSS buffer size set should fail with EBUSY, got ret=%d errno=%d",
		    ret, errno);
	pr_info("Duplicate HDBSS set: correctly rejected\n");
	kvm_vm_free(vm);
}

static void test_mutex_dirty_ring_then_hdbss(void)
{
	struct kvm_vm *vm;
	int ret;

	vm = vm_create_barebones();

	/* Sized to cover the HDBSS reservation on any page size. */
	vm_enable_dirty_ring(vm, 16384 * sizeof(struct kvm_dirty_gfn));

	ret = hdbss_set(vm, HDBSS_DEFAULT_SIZE);
	TEST_ASSERT(ret == -1 && errno == EINVAL,
		    "Setting HDBSS after dirty ring should fail with EINVAL, got ret=%d errno=%d",
		    ret, errno);
	pr_info("HDBSS after dirty ring: correctly rejected\n");
	kvm_vm_free(vm);
}

static void test_dirty_ring_resets_size(void)
{
	struct kvm_vm *vm;
	long ret;

	vm = vm_create_barebones();

	/*
	 * The ring must cover the HDBSS reservation, 2 * buffer + 1KB.
	 * Ring mode then pins the buffer to the default.
	 */
	TEST_ASSERT_EQ(hdbss_set(vm, SZ_256K), 0);
	vm_enable_dirty_ring(vm, 65536 * sizeof(struct kvm_dirty_gfn));
	ret = hdbss_vm_query(vm);
	TEST_ASSERT(ret == (long)HDBSS_DEFAULT_SIZE,
		    "Dirty ring mode should reset the HDBSS buffer size to the default (%lld), got %ld",
		    (unsigned long long)HDBSS_DEFAULT_SIZE, ret);
	pr_info("Dirty ring mode resets HDBSS buffer size to default\n");
	kvm_vm_free(vm);
}

int main(void)
{
	/*
	 * Valid sizes depend on the page size: the kernel minimum is
	 * PAGE_SIZE, so only exercise sizes at or above it.
	 */
	u64 page_size = getpagesize();

	TEST_REQUIRE(hdbss_supported());

	pr_info("Starting HDBSS ioctl interface tests\n\n");

	test_global_query();
	test_vm_query_default();
	if (page_size < SZ_16K)
		test_vm_query_after_set(SZ_16K);
	test_vm_query_after_set(HDBSS_MAX_SIZE);

	test_set_valid(HDBSS_DEFAULT_SIZE);
	if (page_size < SZ_8K)
		test_set_valid(SZ_8K);
	if (page_size < SZ_64K)
		test_set_valid(SZ_64K);
	test_set_valid(HDBSS_MAX_SIZE);

	test_set_invalid(0, EINVAL);
	test_set_invalid(3, EINVAL);
	test_set_invalid(page_size - 1, EINVAL);
	test_set_invalid(page_size + 1, EINVAL);
	test_set_invalid(HDBSS_MAX_SIZE * 2, EINVAL);

	test_set_after_vcpu();
	test_set_twice();
	test_mutex_dirty_ring_then_hdbss();
	test_dirty_ring_resets_size();

	pr_info("\nAll HDBSS ioctl interface tests passed!\n");
	return 0;
}
