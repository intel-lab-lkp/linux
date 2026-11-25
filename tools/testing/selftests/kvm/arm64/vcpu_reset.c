// SPDX-License-Identifier: GPL-2.0-only
/*
 * vcpu_reset - Test that resetting a vcpu that has run with KVM_ARM_VCPU_INIT
 * puts it back in the state of a new vcpu.
 *
 * Before the reset, the vcpu has an MMIO read waiting for userspace and a
 * pending SError, and userspace has written to every register it can.  After
 * the reset, every register must match a new vcpu's, except the ones in
 * preserved_regs[], which must keep the value userspace wrote, and the
 * counters, which keep counting.  No SError may be pending, and the next
 * KVM_RUN must not complete the MMIO read.
 */
#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"

#define MMIO_ADDR	0x8000000ULL
#define MAX_REG_SIZE	256

/*
 * Registers that a reset keeps: the configuration that userspace sets for the
 * vcpu, and the counters, which are VM-wide and change on their own.
 */
#define DEMUX_CCSIDR	(KVM_REG_ARM64 | KVM_REG_SIZE_U32 | KVM_REG_ARM_DEMUX | \
			 KVM_REG_ARM_DEMUX_ID_CCSIDR)

static bool is_preserved(u64 id)
{
	if ((id & ~(u64)KVM_REG_ARM_DEMUX_VAL_MASK) == DEMUX_CCSIDR)
		return true;

	switch (id) {
	case KVM_ARM64_SYS_REG(SYS_MPIDR_EL1):
	case KVM_ARM64_SYS_REG(SYS_CLIDR_EL1):
		return true;
	}
	return false;
}

static bool is_counter(u64 id)
{
	return id == KVM_REG_ARM_TIMER_CNT || id == KVM_REG_ARM_PTIMER_CNT;
}

static void guest_code(void)
{
	READ_ONCE(*(u64 *)MMIO_ADDR);
	GUEST_FAIL("The MMIO read completed");
}

struct regs {
	struct kvm_reg_list *list;
	u8 (*val)[MAX_REG_SIZE];
};

static void get_regs(struct kvm_vcpu *vcpu, struct regs *r)
{
	u64 i;

	r->list = vcpu_get_reg_list(vcpu);
	r->val = calloc(r->list->n, MAX_REG_SIZE);
	TEST_ASSERT(r->val, "calloc() failed");
	for (i = 0; i < r->list->n; i++) {
		TEST_ASSERT(KVM_REG_SIZE(r->list->reg[i]) <= MAX_REG_SIZE,
			    "Register 0x%llx is too big", r->list->reg[i]);
		TEST_ASSERT(!__vcpu_get_reg(vcpu, r->list->reg[i], r->val[i]),
			    "Can't read register 0x%llx", r->list->reg[i]);
	}
}

static void free_regs(struct regs *r)
{
	free(r->list);
	free(r->val);
}

/* Write the inverse of each register, or failing that, flip its bit 0 */
static void dirty_regs(struct kvm_vcpu *vcpu)
{
	struct kvm_reg_list *list = vcpu_get_reg_list(vcpu);
	u8 val[MAX_REG_SIZE];
	struct kvm_one_reg reg = { .addr = (u64)val };
	u64 i, j, size;

	for (i = 0; i < list->n; i++) {
		reg.id = list->reg[i];
		size = KVM_REG_SIZE(reg.id);
		if (__vcpu_get_reg(vcpu, reg.id, val))
			continue;
		for (j = 0; j < size; j++)
			val[j] = ~val[j];
		if (!__vcpu_ioctl(vcpu, KVM_SET_ONE_REG, &reg))
			continue;
		for (j = 0; j < size; j++)
			val[j] = ~val[j];
		val[0] ^= 1;
		__vcpu_ioctl(vcpu, KVM_SET_ONE_REG, &reg);
	}
	free(list);
}

static void assert_reg_eq(u64 id, const u8 *got, const u8 *want, const char *what)
{
	u64 size = KVM_REG_SIZE(id);
	u64 g = 0, w = 0;

	if (!memcmp(got, want, size))
		return;

	memcpy(&g, got, size < 8 ? size : 8);
	memcpy(&w, want, size < 8 ? size : 8);
	TEST_FAIL("Register 0x%llx is 0x%lx after the reset, %s 0x%lx",
		  (unsigned long long)id, (unsigned long)g, what, (unsigned long)w);
}

static struct kvm_vm *create_vcpu(bool el2, struct kvm_vcpu **vcpu)
{
	struct kvm_vcpu_init init;
	struct kvm_vm *vm;

	vm = vm_create(1);
	kvm_get_default_vcpu_target(vm, &init);
	if (el2)
		init.features[0] |= BIT(KVM_ARM_VCPU_HAS_EL2);
	else
		init.features[0] &= ~BIT(KVM_ARM_VCPU_HAS_EL2);
	*vcpu = aarch64_vcpu_add(vm, 0, &init, guest_code);
	kvm_arch_vm_finalize_vcpus(vm);
	return vm;
}

static void test_reset(bool el2)
{
	struct kvm_vcpu_events events = {};
	struct regs new, before, after;
	struct kvm_vcpu *vcpu, *ref;
	struct kvm_vm *vm, *ref_vm;
	u64 i, id;

	/*
	 * A new vcpu: initialised again to undo what the library set up, and
	 * through its first KVM_RUN without running any code.
	 */
	ref_vm = create_vcpu(el2, &ref);
	vcpu_ioctl(ref, KVM_ARM_VCPU_INIT, &ref->init);
	vcpu_run_complete_io(ref);
	get_regs(ref, &new);

	vm = create_vcpu(el2, &vcpu);
	virt_map(vm, MMIO_ADDR, MMIO_ADDR, 1);

	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_MMIO);
	TEST_ASSERT(!vcpu->run->mmio.is_write, "Expected an MMIO read");
	memset(vcpu->run->mmio.data, 0x5a, sizeof(vcpu->run->mmio.data));

	events.exception.serror_pending = true;
	vcpu_events_set(vcpu, &events);

	dirty_regs(vcpu);
	get_regs(vcpu, &before);

	vcpu_ioctl(vcpu, KVM_ARM_VCPU_INIT, &vcpu->init);

	vcpu_events_get(vcpu, &events);
	TEST_ASSERT(!events.exception.serror_pending, "An SError is still pending");

	/* Completes the MMIO read if it is still pending */
	vcpu_run_complete_io(vcpu);
	get_regs(vcpu, &after);

	TEST_ASSERT_EQ(new.list->n, after.list->n);
	TEST_ASSERT_EQ(before.list->n, after.list->n);
	for (i = 0; i < after.list->n; i++) {
		id = after.list->reg[i];
		TEST_ASSERT_EQ(id, new.list->reg[i]);
		TEST_ASSERT_EQ(id, before.list->reg[i]);

		if (is_counter(id))
			continue;
		if (is_preserved(id))
			assert_reg_eq(id, after.val[i], before.val[i], "userspace wrote");
		else
			assert_reg_eq(id, after.val[i], new.val[i], "a new vcpu has");
	}

	free_regs(&new);
	free_regs(&before);
	free_regs(&after);
	kvm_vm_free(vm);
	kvm_vm_free(ref_vm);
}

int main(void)
{
	bool el2 = test_supports_el2();

	ksft_print_header();
	ksft_set_plan(el2 ? 2 : 1);

	test_reset(false);
	ksft_test_result_pass("reset without EL2\n");

	if (el2) {
		test_reset(true);
		ksft_test_result_pass("reset with EL2\n");
	}

	ksft_finished();
}
