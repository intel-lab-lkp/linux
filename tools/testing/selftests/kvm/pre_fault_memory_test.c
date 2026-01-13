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
#include <pthread.h>
#include <guest_modes.h>

/* Arbitrarily chosen values */
#define TEST_BASE_SIZE		SZ_2M
#define TEST_SLOT		10

/* Storage of test info to share with guest code */
struct test_config {
	uint64_t page_size;
	uint64_t test_size;
	uint64_t test_num_pages;
};

static struct test_config test_config;

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

struct slot_worker_data {
	struct kvm_vm *vm;
	u64 gpa;
	uint32_t flags;
	bool worker_ready;
	bool prefault_ready;
	bool recreate_slot;
};

static void *delete_slot_worker(void *__data)
{
	struct slot_worker_data *data = __data;
	struct kvm_vm *vm = data->vm;

	WRITE_ONCE(data->worker_ready, true);

	while (!READ_ONCE(data->prefault_ready))
		cpu_relax();

	vm_mem_region_delete(vm, TEST_SLOT);

	while (!READ_ONCE(data->recreate_slot))
		cpu_relax();

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, data->gpa,
				    TEST_SLOT, test_config.test_num_pages, data->flags);

	return NULL;
}

static void pre_fault_memory(struct kvm_vcpu *vcpu, u64 base_gpa, u64 offset,
			     u64 size, u64 expected_left, bool private)
{
	struct kvm_pre_fault_memory range = {
		.gpa = base_gpa + offset,
		.size = size,
		.flags = 0,
	};
	struct slot_worker_data data = {
		.vm = vcpu->vm,
		.gpa = base_gpa,
		.flags = private ? KVM_MEM_GUEST_MEMFD : 0,
	};
	bool slot_recreated = false;
	pthread_t slot_worker;
	int ret, save_errno;
	u64 prev;

	/*
	 * Concurrently delete (and recreate) the slot to test KVM's handling
	 * of a racing memslot deletion with prefaulting.
	 */
	pthread_create(&slot_worker, NULL, delete_slot_worker, &data);

	while (!READ_ONCE(data.worker_ready))
		cpu_relax();

	WRITE_ONCE(data.prefault_ready, true);

	for (;;) {
		prev = range.size;
		ret = __vcpu_ioctl(vcpu, KVM_PRE_FAULT_MEMORY, &range);
		save_errno = errno;
		TEST_ASSERT((range.size < prev) ^ (ret < 0),
			    "%sexpecting range.size to change on %s",
			    ret < 0 ? "not " : "",
			    ret < 0 ? "failure" : "success");

		/*
		 * Immediately retry prefaulting if KVM was interrupted by an
		 * unrelated signal/event.
		 */
		if (ret < 0 && save_errno == EINTR)
			continue;

		/*
		 * Tell the worker to recreate the slot in order to complete
		 * prefaulting (if prefault didn't already succeed before the
		 * slot was deleted) and/or to prepare for the next testcase.
		 * Wait for the worker to exit so that the next invocation of
		 * prefaulting is guaranteed to complete (assuming no KVM bugs).
		 */
		if (!slot_recreated) {
			WRITE_ONCE(data.recreate_slot, true);
			pthread_join(slot_worker, NULL);
			slot_recreated = true;

			/*
			 * Retry prefaulting to get a stable result, i.e. to
			 * avoid seeing random EAGAIN failures.  Don't retry if
			 * prefaulting already succeeded, as KVM disallows
			 * prefaulting with size=0, i.e. blindly retrying would
			 * result in test failures due to EINVAL.  KVM should
			 * always return success if all bytes are prefaulted,
			 * i.e. there is no need to guard against EAGAIN being
			 * returned.
			 */
			if (range.size)
				continue;
		}

		/*
		 * All done if there are no remaining bytes to prefault, or if
		 * prefaulting failed (EINTR was handled above, and EAGAIN due
		 * to prefaulting a memslot that's being actively deleted should
		 * be impossible since the memslot has already been recreated).
		 */
		if (!range.size || ret < 0)
			break;
	}

	TEST_ASSERT(range.size == expected_left,
		    "Completed with %llu bytes left, expected %lu",
		    range.size, expected_left);

	/*
	 * Assert success if prefaulting the entire range should succeed, i.e.
	 * complete with no bytes remaining.  Otherwise prefaulting should have
	 * failed due to ENOENT (due to RET_PF_EMULATE for emulated MMIO when
	 * no memslot exists).
	 */
	if (!expected_left)
		TEST_ASSERT_VM_VCPU_IOCTL(!ret, KVM_PRE_FAULT_MEMORY, ret, vcpu->vm);
	else
		TEST_ASSERT_VM_VCPU_IOCTL(ret && save_errno == ENOENT,
					  KVM_PRE_FAULT_MEMORY, ret, vcpu->vm);
}

struct test_params {
	unsigned long vm_type;
	bool private;
	enum vm_mem_backing_src_type mem_backing_src;
};

static void __test_pre_fault_memory(enum vm_guest_mode guest_mode, void *arg)
{
	uint64_t gpa, gva, alignment, guest_page_size;
	struct test_params *p = arg;
	const struct vm_shape shape = {
		.mode = guest_mode,
		.type = p->vm_type,
	};
	struct kvm_vcpu *vcpu;
	struct kvm_run *run;
	struct kvm_vm *vm;
	struct ucall uc;

	size_t backing_src_pagesz = get_backing_src_pagesz(p->mem_backing_src);

	pr_info("Testing guest mode: %s\n", vm_guest_mode_string(guest_mode));
	pr_info("Testing memory backing src type: %s\n",
		vm_mem_backing_src_alias(p->mem_backing_src)->name);

	vm = vm_create_shape_with_one_vcpu(shape, &vcpu, guest_code);

	guest_page_size = vm_guest_mode_params[guest_mode].page_size;

	test_config.page_size = guest_page_size;
	test_config.test_size = align_up(TEST_BASE_SIZE + test_config.page_size,
					 backing_src_pagesz);
	test_config.test_num_pages = vm_calc_num_guest_pages(vm->mode, test_config.test_size);

	gpa = (vm->max_gfn - test_config.test_num_pages) * test_config.page_size;
#ifdef __s390x__
	alignment = max(0x100000UL, guest_page_size);
#else
	alignment = SZ_2M;
#endif
	alignment = max(alignment, backing_src_pagesz);
	gpa = align_down(gpa, alignment);
	gva = gpa & ((1ULL << (vm->va_bits - 1)) - 1);

	vm_userspace_mem_region_add(vm, p->mem_backing_src,
				    gpa, TEST_SLOT, test_config.test_num_pages,
				    p->private ? KVM_MEM_GUEST_MEMFD : 0);
	virt_map(vm, gva, gpa, test_config.test_num_pages);

	if (p->private)
		vm_mem_set_private(vm, gpa, test_config.test_size);

	pre_fault_memory(vcpu, gpa, 0, test_config.test_size, 0, p->private);
	/* Test pre-faulting over an already faulted range */
	pre_fault_memory(vcpu, gpa, 0, test_config.test_size, 0, p->private);
	pre_fault_memory(vcpu, gpa,
			 test_config.test_size - test_config.page_size,
			 test_config.page_size * 2, test_config.page_size, p->private);
	pre_fault_memory(vcpu, gpa, test_config.test_size,
			 test_config.page_size, test_config.page_size, p->private);

	vcpu_args_set(vcpu, 1, gva);

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

static void test_pre_fault_memory(unsigned long vm_type, enum vm_mem_backing_src_type backing_src,
				  bool private)
{
	struct test_params p = {
		.vm_type = vm_type,
		.private = private,
		.mem_backing_src = backing_src,
	};

	if (vm_type && !(kvm_check_cap(KVM_CAP_VM_TYPES) & BIT(vm_type))) {
		pr_info("Skipping tests for vm_type 0x%lx\n", vm_type);
		return;
	}

	for_each_guest_mode(__test_pre_fault_memory, &p);
}

int main(int argc, char *argv[])
{
	enum vm_mem_backing_src_type backing = VM_MEM_SRC_ANONYMOUS;
	int opt;

	TEST_REQUIRE(kvm_check_cap(KVM_CAP_PRE_FAULT_MEMORY));

	guest_modes_append_default();

	while ((opt = getopt(argc, argv, "m:")) != -1) {
		switch (opt) {
		case 'm':
			backing = parse_backing_src_type(optarg);
			break;
		default:
			break;
		}
	}

	test_pre_fault_memory(0, backing, false);
#ifdef __x86_64__
	test_pre_fault_memory(KVM_X86_SW_PROTECTED_VM, backing, false);
	test_pre_fault_memory(KVM_X86_SW_PROTECTED_VM, backing, true);
#endif
	return 0;
}
