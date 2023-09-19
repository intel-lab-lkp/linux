// SPDX-License-Identifier: GPL-2.0-only
/*
 * pauth_test - Test for KVM guest pointer authentication.
 * Following PAuth instructions are tested:
 * paciza, pacizb, pacdza, pacdzb, autiza, autizb, autdza, autdzb,
 * pacga, xpaclri, xpaci, xpacd
 * It also shows the implemented algorithm for both address authentication
 * and generic code authentication.
 *
 * Copyright (c) 2023 Google LLC.
 *
 */

#define _GNU_SOURCE

#include <sched.h>

#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"

#define KEY1_LO		0x0123456789abcdefUL
#define KEY1_HI		0x123456789abcdef0UL
#define KEY2_LO		0x23456789abcdef01UL
#define KEY2_HI		0x3456789abcdef012UL
#define KEY3_LO		0x456789abcdef0123UL
#define KEY3_HI		0x56789abcdef01234UL
#define KEY4_LO		0x6789abcdef012345UL
#define KEY4_HI		0x789abcdef0123456UL
#define KEY5_LO		0x89abcdef01234567UL
#define KEY5_HI		0x9abcdef012345678UL

#define PAUTH_ENABLE	(SCTLR_ELx_ENIA | SCTLR_ELx_ENIB | SCTLR_ELx_ENDA | SCTLR_ELx_ENDB)

enum uc_args {
	WAIT_MIGRATION,
	ADDR_PAUTH_ALGO,
	GENERIC_PAUTH_ALGO,
	FAIL_GUEST_PAUTH,
};

enum pauth_algo {
	QARMA5,
	QARMA3,
	PACIMP,
	NOALGO,
};

static const char *const algo_string[] = {
	"QARMA5", "QARMA3", "PACIMP", "Undefined",
};

#define DEFINE_SIGN_FUNC(INSTR)						\
	static size_t INSTR##_sign(size_t ptr)				\
	{								\
		__asm__ __volatile__(					\
			#INSTR" %[p]"					\
			: [p] "+r" (ptr)				\
		);							\
		return ptr;						\
	}

DEFINE_SIGN_FUNC(paciza)
DEFINE_SIGN_FUNC(pacizb)
DEFINE_SIGN_FUNC(pacdza)
DEFINE_SIGN_FUNC(pacdzb)

static size_t pacga_sign(size_t ptr)
{
	size_t dest = 0;

	__asm__ __volatile__(
		"pacga %[d], %[p], %[m]"
		: [d] "=r" (dest)
		: [p] "r" (ptr), [m] "r" (0)
	);

	return dest;
}

#define DEFINE_AUTH_STRIP_FUNC(AUTH, STRIP)				\
	static size_t AUTH##_auth_##STRIP##_strip(size_t ptr)		\
	{								\
		__asm__ __volatile__(					\
			#AUTH" %[p]\n"					\
			#STRIP" %[p]\n"					\
			: [p] "+r" (ptr)				\
		);							\
		return ptr;						\
	}

DEFINE_AUTH_STRIP_FUNC(autiza, xpaci)
DEFINE_AUTH_STRIP_FUNC(autizb, xpaci)
DEFINE_AUTH_STRIP_FUNC(autdza, xpacd)
DEFINE_AUTH_STRIP_FUNC(autdzb, xpacd)

#define GUEST_ALGO(type, algo)	GUEST_SYNC_ARGS(type, algo, 0, 0, 0)

static void check_pauth_algorithms(void)
{
	uint64_t isar1 = read_sysreg_s(SYS_ID_AA64ISAR1_EL1);
	uint64_t isar2 = read_sysreg_s(SYS_ID_AA64ISAR2_EL1);
	enum pauth_algo algo;

	/* Check generic authentication algorithm */
	if (isar1 & ARM64_FEATURE_MASK(ID_AA64ISAR1_GPI))
		algo = PACIMP;
	else if (isar1 & ARM64_FEATURE_MASK(ID_AA64ISAR1_GPA))
		algo = QARMA5;
	else if (isar2 & ARM64_FEATURE_MASK(ID_AA64ISAR2_GPA3))
		algo = QARMA3;
	else
		algo = NOALGO;

	GUEST_ALGO(GENERIC_PAUTH_ALGO, algo);

	/* Check address authentication algorithm */
	if (isar1 & ARM64_FEATURE_MASK(ID_AA64ISAR1_API))
		algo = PACIMP;
	else if (isar1 & ARM64_FEATURE_MASK(ID_AA64ISAR1_APA))
		algo = QARMA5;
	else if (isar2 & ARM64_FEATURE_MASK(ID_AA64ISAR2_APA3))
		algo = QARMA3;
	else
		algo = NOALGO;

	GUEST_ALGO(ADDR_PAUTH_ALGO, algo);
}

/* Setup PAuth keys and check their retainability */
static void check_keys_retainable(void)
{
	/* Address */
	write_sysreg_s(KEY1_LO, SYS_APIAKEYLO_EL1);
	write_sysreg_s(KEY1_HI, SYS_APIAKEYHI_EL1);
	write_sysreg_s(KEY2_LO, SYS_APIBKEYLO_EL1);
	write_sysreg_s(KEY2_HI, SYS_APIBKEYHI_EL1);
	/* Data */
	write_sysreg_s(KEY3_LO, SYS_APDAKEYLO_EL1);
	write_sysreg_s(KEY3_HI, SYS_APDAKEYHI_EL1);
	write_sysreg_s(KEY4_LO, SYS_APDBKEYLO_EL1);
	write_sysreg_s(KEY4_HI, SYS_APDBKEYHI_EL1);
	/* Generic */
	write_sysreg_s(KEY5_LO, SYS_APGAKEYLO_EL1);
	write_sysreg_s(KEY5_HI, SYS_APGAKEYHI_EL1);
	isb();

	GUEST_SYNC(WAIT_MIGRATION);

	/* Address */
	GUEST_ASSERT(read_sysreg_s(SYS_APIAKEYLO_EL1) == KEY1_LO);
	GUEST_ASSERT(read_sysreg_s(SYS_APIAKEYHI_EL1) == KEY1_HI);
	GUEST_ASSERT(read_sysreg_s(SYS_APIBKEYLO_EL1) == KEY2_LO);
	GUEST_ASSERT(read_sysreg_s(SYS_APIBKEYHI_EL1) == KEY2_HI);
	/* Data */
	GUEST_ASSERT(read_sysreg_s(SYS_APDAKEYLO_EL1) == KEY3_LO);
	GUEST_ASSERT(read_sysreg_s(SYS_APDAKEYHI_EL1) == KEY3_HI);
	GUEST_ASSERT(read_sysreg_s(SYS_APDBKEYLO_EL1) == KEY4_LO);
	GUEST_ASSERT(read_sysreg_s(SYS_APDBKEYHI_EL1) == KEY4_HI);
	/* Generic */
	GUEST_ASSERT(read_sysreg_s(SYS_APGAKEYLO_EL1) == KEY5_LO);
	GUEST_ASSERT(read_sysreg_s(SYS_APGAKEYHI_EL1) == KEY5_HI);
}

#define ADDR_START	0x8000
#define ADDR_END	0x8008

static void test_same_keys(void)
{
	/* Set the same keys */
	write_sysreg_s(KEY1_LO, SYS_APIAKEYLO_EL1);
	write_sysreg_s(KEY1_HI, SYS_APIAKEYHI_EL1);
	write_sysreg_s(KEY1_LO, SYS_APIBKEYLO_EL1);
	write_sysreg_s(KEY1_HI, SYS_APIBKEYHI_EL1);
	write_sysreg_s(KEY1_LO, SYS_APDAKEYLO_EL1);
	write_sysreg_s(KEY1_HI, SYS_APDAKEYHI_EL1);
	write_sysreg_s(KEY1_LO, SYS_APDBKEYLO_EL1);
	write_sysreg_s(KEY1_HI, SYS_APDBKEYHI_EL1);
	isb();

	/* Same algorithm, same address, same keys should have same PAC */
	for (size_t i = ADDR_START; i < ADDR_END; i++) {
		/* Assert if the PAuth instruction did nothing */
		GUEST_ASSERT(paciza_sign(i) != i);

		GUEST_ASSERT(paciza_sign(i) == pacizb_sign(i));
		GUEST_ASSERT(paciza_sign(i) == pacdza_sign(i));
		GUEST_ASSERT(paciza_sign(i) == pacdzb_sign(i));
	}
}

static void test_different_keys(void)
{
	write_sysreg_s(KEY1_LO, SYS_APIAKEYLO_EL1);
	write_sysreg_s(KEY1_HI, SYS_APIAKEYHI_EL1);
	write_sysreg_s(KEY2_LO, SYS_APIBKEYLO_EL1);
	write_sysreg_s(KEY2_HI, SYS_APIBKEYHI_EL1);
	write_sysreg_s(KEY3_LO, SYS_APDAKEYLO_EL1);
	write_sysreg_s(KEY3_HI, SYS_APDAKEYHI_EL1);
	write_sysreg_s(KEY4_LO, SYS_APDBKEYLO_EL1);
	write_sysreg_s(KEY4_HI, SYS_APDBKEYHI_EL1);
	isb();

	/* Same algorithm, same address, different keys should have different PAc */
	for (size_t i = ADDR_START; i < ADDR_END; i++) {
		/* Assert if the PAuth instruction did nothing */
		GUEST_ASSERT(paciza_sign(i) != i);

		GUEST_ASSERT(paciza_sign(i) != pacizb_sign(i));
		GUEST_ASSERT(paciza_sign(i) != pacdza_sign(i));
		GUEST_ASSERT(paciza_sign(i) != pacdzb_sign(i));
	}
}

static void test_generic_sign(void)
{
	size_t ga_signs[ADDR_END - ADDR_START];
	size_t i;

	write_sysreg_s(KEY1_LO, SYS_APGAKEYLO_EL1);
	write_sysreg_s(KEY1_HI, SYS_APGAKEYHI_EL1);
	isb();

	for (i = ADDR_START; i < ADDR_END; i++) {
		ga_signs[i - ADDR_START] = pacga_sign(i);
		/* Assert if the PAuth instruction did nothing */
		GUEST_ASSERT(ga_signs[i - ADDR_START] != i);
	}

	write_sysreg_s(KEY5_LO, SYS_APGAKEYLO_EL1);
	write_sysreg_s(KEY5_HI, SYS_APGAKEYHI_EL1);
	isb();

	/* Different key should have different sign */
	for (i = ADDR_START; i < ADDR_END; i++) {
		/* Assert if the PAuth instruction did nothing */
		GUEST_ASSERT(pacga_sign(i) != i);
		GUEST_ASSERT(pacga_sign(i) != ga_signs[i - ADDR_START]);
	}
}

static void test_ia_auth_strip(void)
{
	size_t ptr = ADDR_START;

	write_sysreg_s(KEY2_LO, SYS_APIAKEYLO_EL1);
	write_sysreg_s(KEY2_HI, SYS_APIAKEYHI_EL1);
	isb();

	ptr = paciza_sign(ptr);

	write_sysreg_s(KEY1_LO, SYS_APIAKEYLO_EL1);
	write_sysreg_s(KEY1_HI, SYS_APIAKEYHI_EL1);
	isb();

	/*
	 * Since key has changed, the authentication would fail and be trapped.
	 * In the trap handler, the pauth would be disabled to avoid future trap
	 * for auth failure.
	 */
	ptr = autiza_auth_xpaci_strip(ptr);

	/* Assert if the strip instruction didn't work */
	GUEST_ASSERT(ptr == ADDR_START);
}

static void test_ib_auth_strip(void)
{
	size_t ptr = ADDR_START;

	write_sysreg_s(KEY3_LO, SYS_APIBKEYLO_EL1);
	write_sysreg_s(KEY3_HI, SYS_APIBKEYHI_EL1);
	isb();

	ptr = pacizb_sign(ptr);

	write_sysreg_s(KEY2_LO, SYS_APIBKEYLO_EL1);
	write_sysreg_s(KEY2_HI, SYS_APIBKEYHI_EL1);
	isb();

	/*
	 * Since key has changed, the authentication would fail and be trapped.
	 * In the trap handler, the pauth would be disabled to avoid future trap
	 * for auth failure.
	 */
	ptr = autizb_auth_xpaci_strip(ptr);

	/* Assert if the strip instruction didn't work */
	GUEST_ASSERT(ptr == ADDR_START);
}

static void test_da_auth_strip(void)
{
	size_t ptr = ADDR_START;

	write_sysreg_s(KEY4_LO, SYS_APDAKEYLO_EL1);
	write_sysreg_s(KEY4_HI, SYS_APDAKEYHI_EL1);
	isb();

	ptr = pacdza_sign(ptr);

	write_sysreg_s(KEY3_LO, SYS_APDAKEYLO_EL1);
	write_sysreg_s(KEY3_HI, SYS_APDAKEYHI_EL1);
	isb();

	/*
	 * Since key has changed, the authentication would fail and be trapped.
	 * In the trap handler, the pauth would be disabled to avoid future trap
	 * for auth failure.
	 */
	ptr = autdza_auth_xpacd_strip(ptr);

	/* Assert if the strip instruction didn't work */
	GUEST_ASSERT(ptr == ADDR_START);
}

static void test_db_auth_strip(void)
{
	size_t ptr = ADDR_START;

	write_sysreg_s(KEY5_LO, SYS_APDBKEYLO_EL1);
	write_sysreg_s(KEY5_HI, SYS_APDBKEYHI_EL1);
	isb();

	ptr = pacdzb_sign(ptr);

	write_sysreg_s(KEY4_LO, SYS_APDBKEYLO_EL1);
	write_sysreg_s(KEY4_HI, SYS_APDBKEYHI_EL1);
	isb();

	/*
	 * Since key has changed, the authentication would fail and be trapped.
	 * In the trap handler, the pauth would be disabled to avoid future trap
	 * for auth failure.
	 */
	ptr = autdzb_auth_xpacd_strip(ptr);

	/* Assert if the strip instruction didn't work */
	GUEST_ASSERT(ptr == ADDR_START);
}

static void test_auth_strip(void)
{
	test_ia_auth_strip();
	test_ib_auth_strip();
	test_da_auth_strip();
	test_db_auth_strip();

	/*
	 * If all authentication instructions have failed, the PAuth enable bits
	 * in SCTLR should have been cleared in the trap handler.
	 * Otherwise, the auth instructions didn't work as expected.
	 */
	GUEST_ASSERT(!(read_sysreg(sctlr_el1) & PAUTH_ENABLE));
}

static void guest_code(void)
{
	uint64_t sctlr = read_sysreg(sctlr_el1);

	check_pauth_algorithms();
	check_keys_retainable();

	/* Enable PAuth */
	sctlr |= PAUTH_ENABLE;
	write_sysreg(sctlr, sctlr_el1);
	isb();

	test_same_keys();
	test_different_keys();
	test_generic_sign();
	test_auth_strip();

	GUEST_DONE();
}

/* Guest will get an unknown exception (UNDEF) if guest PAuth is not enabled. */
static void guest_unknown_handler(struct ex_regs *regs)
{
	GUEST_SYNC(FAIL_GUEST_PAUTH);
	GUEST_DONE();
}

/* Guest will get a FPAC exception if KVM support guest PAuth */
static void guest_fpac_handler(struct ex_regs *regs)
{
	uint64_t sctlr = read_sysreg(sctlr_el1);

	if (sctlr & SCTLR_ELx_ENIA) {
		sctlr &= ~SCTLR_ELx_ENIA;
		write_sysreg(sctlr, sctlr_el1);
		isb();
	} else if (sctlr & SCTLR_ELx_ENIB) {
		sctlr &= ~SCTLR_ELx_ENIB;
		write_sysreg(sctlr, sctlr_el1);
		isb();
	} else if (sctlr & SCTLR_ELx_ENDA) {
		sctlr &= ~SCTLR_ELx_ENDA;
		write_sysreg(sctlr, sctlr_el1);
		isb();
	} else if (sctlr & SCTLR_ELx_ENDB) {
		sctlr &= ~SCTLR_ELx_ENDB;
		write_sysreg(sctlr, sctlr_el1);
		isb();
	}
}

int main(void)
{
	struct kvm_vcpu_init init;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	cpu_set_t cpu_mask;
	bool guest_done = false;

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_ARM_PTRAUTH_ADDRESS));
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_ARM_PTRAUTH_GENERIC));

	vm = vm_create(1);

	vm_ioctl(vm, KVM_ARM_PREFERRED_TARGET, &init);
	init.features[0] |= ((1 << KVM_ARM_VCPU_PTRAUTH_ADDRESS) |
			     (1 << KVM_ARM_VCPU_PTRAUTH_GENERIC));

	vcpu = aarch64_vcpu_add(vm, 0, &init, guest_code);

	vm_init_descriptor_tables(vm);
	vcpu_init_descriptor_tables(vcpu);

	vm_install_sync_handler(vm, VECTOR_SYNC_CURRENT,
				ESR_EC_UNKNOWN, guest_unknown_handler);
	vm_install_sync_handler(vm, VECTOR_SYNC_CURRENT,
				ESR_EC_FPAC, guest_fpac_handler);

	ksft_print_header();
	ksft_set_plan(3);

	while (!guest_done) {
		vcpu_run(vcpu);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			break;
		case UCALL_SYNC:
			switch (uc.args[1]) {
			case WAIT_MIGRATION:
				sched_getaffinity(0, sizeof(cpu_mask), &cpu_mask);
				CPU_CLR(sched_getcpu(), &cpu_mask);
				sched_setaffinity(0, sizeof(cpu_mask), &cpu_mask);
				break;
			case FAIL_GUEST_PAUTH:
				/*
				 * KVM has already claimed that both itself and
				 * HW support PAuth, but the guest still got the
				 * UNDEF with PAuth instruction.
				 * Usually this shouldn't happen unless KVM
				 * screwed up the emulation somehow or the PAuth
				 * was not enabled for the guest.
				 */
				TEST_FAIL("Guest PAuth was not enabled!\n");
				break;
			case GENERIC_PAUTH_ALGO: {
				enum pauth_algo algo = uc.args[2];

				if (algo == NOALGO) {
					ksft_print_msg("Make sure the VCPU feature is enabled:\n");
					ksft_print_msg("KVM_ARM_VCPU_PTRAUTH_GENERIC\n");
					TEST_FAIL("No generic PAuth algorithm in guest!\n");
				}

				ksft_test_result_pass("Generic PAuth Algorithm: %s\n",
						      algo_string[algo]);
				break;
			}
			case ADDR_PAUTH_ALGO: {
				enum pauth_algo algo = uc.args[2];

				if (algo == NOALGO) {
					ksft_print_msg("Make sure the VCPU feature is enabled:\n");
					ksft_print_msg("KVM_ARM_VCPU_PTRAUTH_ADDRESS\n");
					TEST_FAIL("No address PAuth algorithm in guest!\n");
				}

				ksft_test_result_pass("Address PAuth Algorithm: %s\n",
						      algo_string[algo]);
				break;
			}
			default:
				ksft_print_msg("Unexpected guest sync arg: 0x%016llx\n",
					       uc.args[1]);
				break;
			}
			break;
		case UCALL_DONE:
			ksft_test_result_pass("Guest PAuth\n");
			guest_done = true;
			break;
		default:
			TEST_FAIL("Unexpected ucall: %lu", uc.cmd);
		}
	}

	ksft_finished();
	kvm_vm_free(vm);
}
