// SPDX-License-Identifier: GPL-2.0-only

/*
 *  Copyright (C) 2024 Advanced Micro Devices, Inc.
 *
 */
#include <pthread.h>

#include "processor.h"
#include "apic.h"
#include "kvm_util.h"
#include "sev.h"
#include "test_util.h"
#include "savic.h"

#define NR_SAVIC_VCPUS	1

static struct kvm_vcpu *vcpus[NR_SAVIC_VCPUS];
static pthread_t threads[NR_SAVIC_VCPUS];

#define SAVIC_TEST_STATE(STATE) \
	STATE ## _START, \
	STATE ## _END

enum savic_test_state {
	SAVIC_TEST_STATE(SAVIC_APIC_MSR_ACCESSES),
};

#define SAVIC_GUEST_SYNC(sync, func) ({\
	GUEST_SYNC(sync ## _START); \
	func(id); \
	GUEST_SYNC(sync ## _END); \
})

static int savic_wrmsr(uint32_t reg, uint64_t val)
{
	switch (reg) {
	case APIC_LVR:
	case APIC_LDR:
	case APIC_ISR:
	case APIC_TMR:
	case APIC_IRR:
	case APIC_TMCCT:
		x2apic_write_reg_fault(reg, val);
		return -1;
	default:
		x2apic_write_reg(reg, val);
		break;
	}

	return 0;
}

static uint64_t savic_rdmsr(uint32_t reg)
{
	uint64_t val;
	uint32_t msr = APIC_BASE_MSR + (reg >> 4);

	switch (reg) {
	case APIC_EOI:
		uint8_t fault = rdmsr_safe(msr, &val);

		__GUEST_ASSERT(fault == GP_VECTOR,
				"Wanted #GP on RDMSR(%x) = %x, got 0x%x\n",
				msr, GP_VECTOR, fault);
		return val;
	default:
		return x2apic_read_reg(reg);
	}
}

static void guest_verify_host_guest_reg(struct guest_apic_page *apage, uint32_t reg,
		uint64_t val, char *regname)
{
	uint64_t hval, gval, gval2;

	if (savic_wrmsr(reg, val) == -1) {
		savic_write_reg(apage, reg, val);
		/*
		 * Write using PV interface if wrmsr fails. Skip for
		 * regs which trigger GP
		 */
		if (reg != APIC_LVR && reg != APIC_TMR && reg != APIC_IRR)
			savic_hv_write_reg(reg, val);
	}

	gval = savic_read_reg(apage, reg);
	gval2 = savic_rdmsr(reg);
	hval = savic_hv_read_reg(reg);
	__GUEST_ASSERT(gval == val, "Unexpected Guest %s 0x%lx, expected val:0x%lx\n",
			regname, gval, val);
	__GUEST_ASSERT(gval == gval2, "Unexpected Guest %s backing page value : 0x%lx, msr read val:0x%lx\n",
			regname, gval, gval2);

	switch (reg) {
	case APIC_LVR:
	case APIC_LDR:
	case APIC_ISR:
	case APIC_TMICT:
	case APIC_TDCR:
	case APIC_LVTT:
	case APIC_LVTTHMR:
	case APIC_LVTPC:
	case APIC_LVT0:
	case APIC_LVT1:
	case APIC_LVTERR:
	case APIC_SPIV:
		__GUEST_ASSERT(hval == gval, "Guest 0x%lx host 0x%lx %s mismatch\n",
			gval, hval, regname);
		break;
	case APIC_TASKPRI:
	case APIC_ICR:
	case APIC_TMR:
	case APIC_IRR:
		__GUEST_ASSERT(hval != gval, "Guest 0x%lx host 0x%lx reg: %x %s must not match\n",
			gval, hval, reg, regname);
		break;
	default:
		break;
	}
}

static inline uint32_t x2apic_ldr(uint32_t id)
{
	return ((id >> 4) << 16) | (1 << (id & 0xf));
}

static void guest_savic_apic_msr_accesses(int id)
{
	struct guest_apic_page *apage = get_guest_apic_page();
	uint64_t val, hval;
	uint32_t reg;
	int vec;
	int i;
	uint32_t lvt_regs[] = {
		APIC_LVTT, APIC_LVTTHMR, APIC_LVTPC,
		APIC_LVT0, APIC_LVT1, APIC_LVTERR
	};

	reg = APIC_LVR;
	val = savic_hv_read_reg(reg);
	/* APIC_LVR state is in sync between host and guest. */
	guest_verify_host_guest_reg(apage, reg, val, "APIC_LVR");

	reg = APIC_TASKPRI;
	val = 0x30;
	/* Write new TASKPRI to host using PV interface. */
	savic_hv_write_reg(reg, val);
	val = 0x40;
	/* TASKPRI is accelerated and state is not up-to-date in host. */
	guest_verify_host_guest_reg(apage, reg, val, "APIC_TASKPRI");

	reg = APIC_PROCPRI;
	val = x2apic_read_reg(reg);
	/* APIC_PROCPRI is updated with the APIC_TASKPRI update above. */
	GUEST_ASSERT((val & 0xf0) == (x2apic_read_reg(APIC_TASKPRI) & 0xf0));
	GUEST_ASSERT((val & 0xf0) == 0x40);
	vec = 0x20;
	x2apic_write_reg(APIC_ICR, APIC_DEST_SELF | APIC_INT_ASSERT | vec);
	/* Interrupt remains pending in APIC_IRR. */
	val = savic_read_reg(apage, APIC_IRR + APIC_REG_OFF(vec));
	GUEST_ASSERT((val & BIT_ULL(APIC_VEC_POS(vec))) == BIT_ULL(APIC_VEC_POS(vec)));
	savic_wrmsr(APIC_TASKPRI, 0x0);

	/* Triggers GP fault */
	savic_rdmsr(APIC_EOI);

	reg = APIC_LDR;
	val = x2apic_ldr(savic_rdmsr(APIC_ID));
	hval = savic_hv_read_reg(APIC_LDR);
	__GUEST_ASSERT(val == hval, "APIC_LDR mismatch between host %lx and guest %lx",
			hval, val);

	/* APIC_SPIV state is not visible to host. */
	reg = APIC_SPIV;
	val = savic_rdmsr(APIC_SPIV) & ~APIC_SPIV_APIC_ENABLED;
	savic_hv_write_reg(reg, val);
	val = savic_rdmsr(APIC_SPIV) | APIC_SPIV_APIC_ENABLED;
	guest_verify_host_guest_reg(apage, reg, val, "APIC_SPIV");

	reg = APIC_ISR;
	(void) savic_rdmsr(reg);
	/* Triggers GP fault */
	savic_wrmsr(reg, 0x10);

	/* APIC_TMR is not synced to host. */
	reg = APIC_TMR;
	val = 0x10000;
	guest_verify_host_guest_reg(apage, reg, val, "APIC_TMR");
	vec = 0x20;
	savic_write_reg(apage, reg + APIC_REG_OFF(vec),  BIT_ULL(APIC_VEC_POS(vec)));
	GUEST_ASSERT(x2apic_read_reg(reg + APIC_REG_OFF(vec)) & BIT_ULL(APIC_VEC_POS(vec)));

	reg = APIC_IRR;
	val = 0x10000;
	guest_verify_host_guest_reg(apage, reg, val, "APIC_IRR");
	savic_write_reg(apage, reg, 0x0);

	reg = APIC_TMICT;
	val = 0x555;
	guest_verify_host_guest_reg(apage, reg, val, "APIC_TMICT");

	reg = APIC_TMCCT;
	savic_rdmsr(reg);
	savic_wrmsr(reg, 0xf);

	reg = APIC_TDCR;
	val = 0x1;
	savic_hv_write_reg(reg, val);
	val = 0x3;
	guest_verify_host_guest_reg(apage, reg, val, "APIC_TDCR");

	for (i = 0; i < ARRAY_SIZE(lvt_regs); i++) {
		reg = lvt_regs[i];
		val = 0x41;
		savic_hv_write_reg(reg, val);
		val = 0x42;
		guest_verify_host_guest_reg(apage, reg, val, "APIC_LVTx");
	}
}

static void guest_code(int id)
{
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SNP_SECURE_AVIC);

	x2apic_enable();

	savic_enable();

	SAVIC_GUEST_SYNC(SAVIC_APIC_MSR_ACCESSES, guest_savic_apic_msr_accesses);

	GUEST_DONE();
}

static void *vcpu_thread(void *arg)
{
	struct kvm_vcpu *vcpu = (struct kvm_vcpu *)arg;
	struct ucall uc;

	fprintf(stderr, "vCPU thread running vCPU %u\n", vcpu->id);

	while (true) {
		vcpu_run(vcpu);
		switch (get_ucall(vcpu, &uc)) {
		case UCALL_SYNC:
			break;
		case UCALL_DONE:
			return NULL;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			break;
		case UCALL_NONE:
			continue;
		default:
			TEST_FAIL("Unknown ucall 0x%lx.", uc.cmd);
		}

	}

	return NULL;
}

int main(int argc, char *argv[])
{
	struct kvm_sev_init args = {
		.vmsa_features = BIT_ULL(SVM_FEAT_SECURE_AVIC)
	};
	struct kvm_vm *vm;
	int r;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SEV_SNP));
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SECURE_AVIC));

	vm = _vm_sev_create_with_one_vcpu(KVM_X86_SNP_VM, guest_code, &vcpus[0], &args);

	virt_pg_map(vm, APIC_DEFAULT_GPA, APIC_DEFAULT_GPA);

	vcpu_args_set(vcpus[0], 1, vcpus[0]->id);

	vm_install_exception_handler(vm, 29, savic_vc_handler);
	vm_sev_launch(vm, snp_default_policy(), NULL);

	r = pthread_create(&threads[0], NULL, vcpu_thread, vcpus[0]);
	TEST_ASSERT(r == 0, "pthread_create failed errno=%d", errno);

	pthread_join(threads[0], NULL);

	kvm_vm_free(vm);

	return 0;
}
