// SPDX-License-Identifier: GPL-2.0-only
/*
 * mmio_sign_ext - Test sign-extending MMIO load emulation (LDRSB/LDRSH/LDRSW)
 *
 * Copyright (c) 2026 Google LLC
 */

#include "processor.h"
#include "test_util.h"

#define MMIO_ADDR	0x8000000ULL

struct mmio_test {
	const char *name;
	uint64_t data;
	uint8_t len;
	uint64_t expected;
};

/* Paired 1:1, in order, with the loads in guest_code() below. */
static const struct mmio_test tests[] = {
	/* LDRSB Xt: byte sign-extended to 64 bits */
	{ "LDRSB Xt 0xFF",	0xFF,		1, 0xFFFFFFFFFFFFFFFFULL },
	{ "LDRSB Xt 0x7F",	0x7F,		1, 0x7FULL },

	/* LDRSB Wt: byte sign-extended to 32 bits, upper 32 bits zeroed */
	{ "LDRSB Wt 0xFF",	0xFF,		1, 0xFFFFFFFFULL },
	{ "LDRSB Wt 0x7F",	0x7F,		1, 0x7FULL },

	/* LDRSH Xt: halfword sign-extended to 64 bits */
	{ "LDRSH Xt 0x8001",	0x8001,		2, 0xFFFFFFFFFFFF8001ULL },
	{ "LDRSH Xt 0x7FFF",	0x7FFF,		2, 0x7FFFULL },

	/* LDRSH Wt: halfword sign-extended to 32 bits, upper 32 bits zeroed */
	{ "LDRSH Wt 0x8001",	0x8001,		2, 0xFFFF8001ULL },
	{ "LDRSH Wt 0x7FFF",	0x7FFF,		2, 0x7FFFULL },

	/* LDRSW Xt: word sign-extended to 64 bits (no Wt form) */
	{ "LDRSW Xt 0x80000001", 0x80000001,	4, 0xFFFFFFFF80000001ULL },
	{ "LDRSW Xt 0x7FFFFFFF", 0x7FFFFFFF,	4, 0x7FFFFFFFULL },
};

static void guest_code(void)
{
	uint64_t val;

	asm volatile("ldrsb %0, [%1]" : "=r"(val) : "r"(MMIO_ADDR) : "memory");
	GUEST_SYNC(val);

	asm volatile("ldrsb %0, [%1]" : "=r"(val) : "r"(MMIO_ADDR) : "memory");
	GUEST_SYNC(val);

	asm volatile("ldrsb %w0, [%1]" : "=r"(val) : "r"(MMIO_ADDR) : "memory");
	GUEST_SYNC(val);

	asm volatile("ldrsb %w0, [%1]" : "=r"(val) : "r"(MMIO_ADDR) : "memory");
	GUEST_SYNC(val);

	asm volatile("ldrsh %0, [%1]" : "=r"(val) : "r"(MMIO_ADDR) : "memory");
	GUEST_SYNC(val);

	asm volatile("ldrsh %0, [%1]" : "=r"(val) : "r"(MMIO_ADDR) : "memory");
	GUEST_SYNC(val);

	asm volatile("ldrsh %w0, [%1]" : "=r"(val) : "r"(MMIO_ADDR) : "memory");
	GUEST_SYNC(val);

	asm volatile("ldrsh %w0, [%1]" : "=r"(val) : "r"(MMIO_ADDR) : "memory");
	GUEST_SYNC(val);

	asm volatile("ldrsw %0, [%1]" : "=r"(val) : "r"(MMIO_ADDR) : "memory");
	GUEST_SYNC(val);

	asm volatile("ldrsw %0, [%1]" : "=r"(val) : "r"(MMIO_ADDR) : "memory");
	GUEST_SYNC(val);

	GUEST_DONE();
}

static void handle_mmio(struct kvm_run *run, const struct mmio_test *t)
{
	TEST_ASSERT_EQ(run->mmio.phys_addr, MMIO_ADDR);
	TEST_ASSERT(!run->mmio.is_write, "Expected MMIO read for %s", t->name);
	TEST_ASSERT_EQ(run->mmio.len, t->len);

	memset(run->mmio.data, 0, sizeof(run->mmio.data));
	/* Works because arm64 KVM hosts are always little-endian. */
	memcpy(run->mmio.data, &t->data, t->len);
}

int main(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	unsigned int i;

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);
	virt_map(vm, MMIO_ADDR, MMIO_ADDR, 1);

	ksft_print_header();
	ksft_set_plan(ARRAY_SIZE(tests));

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		const struct mmio_test *t = &tests[i];

		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_MMIO);

		handle_mmio(vcpu->run, t);
		vcpu_run(vcpu);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_SYNC:
			TEST_ASSERT_EQ(uc.args[1], t->expected);
			break;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			break;
		default:
			TEST_FAIL("Unexpected ucall for %s", t->name);
		}

		ksft_test_result_pass("%s\n", t->name);
	}

	vcpu_run(vcpu);
	TEST_ASSERT(get_ucall(vcpu, &uc) == UCALL_DONE, "Expected UCALL_DONE");

	kvm_vm_free(vm);

	ksft_finished();
}
