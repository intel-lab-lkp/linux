// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024, Intel, Inc
 *
 * Author:
 * Isaku Yamahata <isaku.yamahata at gmail.com>
 */
#include <linux/sizes.h>

#include <test_util.h>
#include <kvm_util.h>
#include <processor.h>
#include <guest_modes.h>

/* Arbitrarily chosen values */
#define TEST_BASE_SIZE		SZ_2M
#define TEST_SLOT		10

/* Storage of test info to share with guest code */
struct test_config {
	int page_size;
	uint64_t test_size;
	uint64_t test_num_pages;
};

struct test_config test_config;

static void guest_code(uint64_t base_gpa)
{
	volatile uint64_t val __used;
	struct test_config *config = &test_config;
	int i;

	for (i = 0; i < config->test_num_pages; i++) {
		uint64_t *src = (uint64_t *)(base_gpa + i * config->page_size);

		val = *src;
	}

	GUEST_DONE();
}

static void pre_fault_memory(struct kvm_vcpu *vcpu, u64 gpa, u64 size,
			     u64 left)
{
	struct kvm_pre_fault_memory range = {
		.gpa = gpa,
		.size = size,
		.flags = 0,
	};
	u64 prev;
	int ret, save_errno;

	do {
		prev = range.size;
		ret = __vcpu_ioctl(vcpu, KVM_PRE_FAULT_MEMORY, &range);
		save_errno = errno;
		TEST_ASSERT((range.size < prev) ^ (ret < 0),
			    "%sexpecting range.size to change on %s",
			    ret < 0 ? "not " : "",
			    ret < 0 ? "failure" : "success");
	} while (ret >= 0 ? range.size : save_errno == EINTR);

	TEST_ASSERT(range.size == left,
		    "Completed with %lld bytes left, expected %" PRId64,
		    range.size, left);

	if (left == 0)
		__TEST_ASSERT_VM_VCPU_IOCTL(!ret, "KVM_PRE_FAULT_MEMORY", ret, vcpu->vm);
	else
		/* No memory slot causes RET_PF_EMULATE. it results in -ENOENT. */
		__TEST_ASSERT_VM_VCPU_IOCTL(ret && save_errno == ENOENT,
					    "KVM_PRE_FAULT_MEMORY", ret, vcpu->vm);
}

struct test_params {
	unsigned long vm_type;
	bool private;
};

static void __test_pre_fault_memory(enum vm_guest_mode guest_mode, void *arg)
{
	struct test_params *p = arg;
	const struct vm_shape shape = {
		.mode = guest_mode,
		.type = p->vm_type,
	};
	struct kvm_vcpu *vcpu;
	struct kvm_run *run;
	struct kvm_vm *vm;
	struct ucall uc;

	uint64_t guest_test_phys_mem;
	uint64_t guest_test_virt_mem;
	uint64_t alignment, guest_page_size;

	pr_info("Testing guest mode: %s\n", vm_guest_mode_string(guest_mode));

	vm = vm_create_shape_with_one_vcpu(shape, &vcpu, guest_code);

	guest_page_size = vm_guest_mode_params[guest_mode].page_size;

	test_config.page_size = guest_page_size;
	test_config.test_size = TEST_BASE_SIZE + test_config.page_size;
	test_config.test_num_pages = vm_calc_num_guest_pages(vm->mode, test_config.test_size);

	guest_test_phys_mem = (vm->max_gfn - test_config.test_num_pages) * test_config.page_size;
#ifdef __s390x__
	alignment = max(0x100000UL, guest_page_size);
#else
	alignment = SZ_2M;
#endif
	guest_test_phys_mem = align_down(guest_test_phys_mem, alignment);
	guest_test_virt_mem = guest_test_phys_mem & ((1ULL << (vm->va_bits - 1)) - 1);

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    guest_test_phys_mem, TEST_SLOT, test_config.test_num_pages,
				    p->private ? KVM_MEM_GUEST_MEMFD : 0);
	virt_map(vm, guest_test_virt_mem, guest_test_phys_mem, test_config.test_num_pages);

	if (p->private)
		vm_mem_set_private(vm, guest_test_phys_mem, test_config.test_size);
	pre_fault_memory(vcpu, guest_test_phys_mem, TEST_BASE_SIZE, 0);
	/* Test pre-faulting over an already faulted range */
	pre_fault_memory(vcpu, guest_test_phys_mem, TEST_BASE_SIZE, 0);
	pre_fault_memory(vcpu, guest_test_phys_mem + TEST_BASE_SIZE,
			 test_config.page_size * 2, test_config.page_size);
	pre_fault_memory(vcpu, guest_test_phys_mem + test_config.test_size,
			 test_config.page_size, test_config.page_size);

	vcpu_args_set(vcpu, 1, guest_test_virt_mem);

	/* Export the shared variables to the guest. */
	sync_global_to_guest(vm, test_config);

	vcpu_run(vcpu);

	run = vcpu->run;
	TEST_ASSERT(run->exit_reason == UCALL_EXIT_REASON,
		    "Wanted %s, got exit reason: %u (%s)",
		    exit_reason_str(UCALL_EXIT_REASON),
		    run->exit_reason, exit_reason_str(run->exit_reason));

	switch (get_ucall(vcpu, &uc)) {
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
		break;
	case UCALL_DONE:
		break;
	default:
		TEST_FAIL("Unknown ucall 0x%lx.", uc.cmd);
		break;
	}

	kvm_vm_free(vm);
}

static void test_pre_fault_memory(unsigned long vm_type, bool private)
{
	if (vm_type && !(kvm_check_cap(KVM_CAP_VM_TYPES) & BIT(vm_type))) {
		pr_info("Skipping tests for vm_type 0x%lx\n", vm_type);
		return;
	}

	struct test_params p = {
		.vm_type = vm_type,
		.private = private,
	};

	for_each_guest_mode(__test_pre_fault_memory, &p);
}

int main(int argc, char *argv[])
{
	TEST_REQUIRE(kvm_check_cap(KVM_CAP_PRE_FAULT_MEMORY));

	test_pre_fault_memory(0, false);
#ifdef __x86_64__
	test_pre_fault_memory(KVM_X86_SW_PROTECTED_VM, false);
	test_pre_fault_memory(KVM_X86_SW_PROTECTED_VM, true);
#endif
	return 0;
}
