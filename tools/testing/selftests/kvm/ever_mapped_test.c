// SPDX-License-Identifier: GPL-2.0
/*
 * KVM ever-mapped bitmap test
 *
 * Copyright (C) 2026, Nutanix, Inc.
 */

#include <test_util.h>
#include <kvm_util.h>
#include <processor.h>

#define KiB 1024u
#define MiB (1024 * KiB)

static void guest_code(uint64_t base_gpa, size_t len)
{
	GUEST_DONE();
}

static void assert_bitmaps_equal(u8 expected[], u8 actual[], size_t len)
{
	for (size_t i = 0; i < len; i++) {
		TEST_ASSERT(expected[i] == actual[i],
			    "byte %ld, expected 0x%02x, got 0x%02x",
			    i, expected[i], actual[i]);
	}
}

static void pre_fault(struct kvm_vcpu *vcpu, u64 start, u64 len)
{
	struct kvm_pre_fault_memory range = {
		.gpa = start,
		.size = len,
		.flags = 0,
	};
	vcpu_ioctl(vcpu, KVM_PRE_FAULT_MEMORY, &range);
}

static void test_one_page(void)
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	struct kvm_pre_fault_memory range = {
		.gpa = 0x0,
		.size = PAGE_SIZE,
		.flags = 0,
	};
	unsigned char mapped_bitmap;
	struct kvm_ever_mapped_log log = {
		.first_granule = 0,
		.num_granules = 1,
		.granule_shift = 21,
		.flags = 0,
		.bitmap = &mapped_bitmap,
	};

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	vm_enable_cap(vm, KVM_CAP_EVER_MAPPED, PAGE_SIZE);
	vcpu_ioctl(vcpu, KVM_PRE_FAULT_MEMORY, &range);

	vm_ioctl(vm, KVM_GET_EVER_MAPPED_LOG, &log);
	TEST_ASSERT(mapped_bitmap == 0x1, "expected 0x1, got 0x%x", mapped_bitmap);
}

/*
 * Add 64MB or memory at GPA 16MB ( --> [16MB, 80MB) ),
 * pre-fault [16MB, 48MB), request and check bitmap for [16MB, 80MB).
 */
static void test_one_block(void)
{
	int ret;
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	const uint64_t map_start = 0x1000000;
	const uint64_t map_len = 0x4000000;
	u8 mapped_bitmap[4] = { 0xa5, 0xa5, 0xa5, 0xa5 };
	u8 expected_bitmap[4] = { 0xff, 0xff, 0x00, 0x00 };
	struct kvm_ever_mapped_log log = {
		.first_granule = 0x8,
		.num_granules = 0x20,
		.granule_shift = 21,
		.flags = 0,
		.bitmap = mapped_bitmap,
	};

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, map_start, 1, map_len / PAGE_SIZE, 0);

	ret = __vm_ioctl(vm, KVM_GET_EVER_MAPPED_LOG, &log);
	TEST_ASSERT(ret && errno == ENOENT,
		    "expected ENOENT when querying ever-mapped log before enablement, got %d",
		    errno);
	vm_enable_cap(vm, KVM_CAP_EVER_MAPPED, map_start + map_len);
	pre_fault(vcpu, 16 * MiB, 32 * MiB);

	ret = __vm_enable_cap(vm, KVM_CAP_EVER_MAPPED, PAGE_SIZE);
	TEST_ASSERT(ret && errno == EEXIST,
		    "expected EEXIST when enabling KVM_CAP_EVER_MAPPED twice, got %d",
		    errno);

	vm_ioctl(vm, KVM_GET_EVER_MAPPED_LOG, &log);
	assert_bitmaps_equal(expected_bitmap, mapped_bitmap, 4);

	log.granule_shift = 22;
	log.first_granule = 0x4;
	log.num_granules = 0x10;
	memcpy(mapped_bitmap, (u8[]){ 0xa5, 0xa5, 0xa5, 0xa5 }, 4);
	memcpy(expected_bitmap, (u8[]){ 0xff, 0x00, 0xa5, 0xa5 }, 4);
	vm_ioctl(vm, KVM_GET_EVER_MAPPED_LOG, &log);
	assert_bitmaps_equal(expected_bitmap, mapped_bitmap, 4);

	log.granule_shift = 23;
	log.first_granule = 0x2;
	log.num_granules = 0x8;
	memcpy(mapped_bitmap, (u8[]){ 0xa5, 0xa5, 0xa5, 0xa5 }, 4);
	memcpy(expected_bitmap, (u8[]){ 0x0f, 0xa5, 0xa5, 0xa5 }, 4);
	vm_ioctl(vm, KVM_GET_EVER_MAPPED_LOG, &log);
	assert_bitmaps_equal(expected_bitmap, mapped_bitmap, 4);

	log.num_granules = 0xff;
	ret = __vm_ioctl(vm, KVM_GET_EVER_MAPPED_LOG, &log);
	TEST_ASSERT(ret && errno == EINVAL,
		    "expected EINVAL when querying beyond range, got %d",
		    errno);

	log.num_granules = 0x10;
	log.granule_shift = 1;
	ret = __vm_ioctl(vm, KVM_GET_EVER_MAPPED_LOG, &log);
	TEST_ASSERT(ret && errno == EINVAL,
		    "expected EINVAL with too small grnaule shift, got %d",
		    errno);
}

/*
 * Add 128MB or memory at GPA 32MB ( --> [32MB, 160MB) ),
 * pre-fault [32MB, 64MB), and [90MB, 94MB),
 * request and check bitmap for [16MB, 120MB).
 */
static void test_two_blocks(void)
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	const uint64_t map_start = 0x2000000;
	const uint64_t map_len = 0x8000000;
	u8 mapped_bitmap[7] = { 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5 };
	u8 expected_bitmap[7] = { 0x00, 0xff, 0xff, 0x00, 0x60, 0x00, 0x00 };
	struct kvm_ever_mapped_log log = {
		.first_granule = 0x8,
		.num_granules = 0x34,
		.granule_shift = 21,
		.flags = 0,
		.bitmap = mapped_bitmap,
	};

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, map_start, 1, map_len / PAGE_SIZE, 0);

	vm_enable_cap(vm, KVM_CAP_EVER_MAPPED, map_start + map_len);
	pre_fault(vcpu, 32 * MiB, 32 * MiB);
	pre_fault(vcpu, 90 * MiB, 4 * MiB);

	vm_ioctl(vm, KVM_GET_EVER_MAPPED_LOG, &log);
	assert_bitmaps_equal(expected_bitmap, mapped_bitmap, 4);

	log.granule_shift = 22;
	log.first_granule = 0x4;
	log.num_granules = 0x1a;
	memcpy(mapped_bitmap, (u8[]){ 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5}, 7);
	memcpy(expected_bitmap, (u8[]){ 0xf0, 0x0f, 0x0c, 0x00, 0xa5, 0xa5, 0xa5 }, 7);
	vm_ioctl(vm, KVM_GET_EVER_MAPPED_LOG, &log);
	assert_bitmaps_equal(expected_bitmap, mapped_bitmap, 4);

	log.granule_shift = 23;
	log.first_granule = 0x2;
	log.num_granules = 0xd;
	memcpy(mapped_bitmap, (u8[]){ 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5 }, 7);
	memcpy(expected_bitmap, (u8[]){ 0x3c, 0x02, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5 }, 7);
	vm_ioctl(vm, KVM_GET_EVER_MAPPED_LOG, &log);
	assert_bitmaps_equal(expected_bitmap, mapped_bitmap, 4);
}

int main(int argc, char *argv[])
{
	TEST_REQUIRE(kvm_check_cap(KVM_CAP_PRE_FAULT_MEMORY));
	TEST_REQUIRE(kvm_check_cap(KVM_CAP_EVER_MAPPED));

	test_one_page();
	test_one_block();
	test_two_blocks();
}
