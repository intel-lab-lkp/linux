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
#define NUM_ITERATIONS 2000

#define IDLE_HLT_INTR_VECTOR     0x30
#define IOAPIC_VECTOR_START      0x81
#define IOAPIC_NUM_EDGE_VECTORS 2
#define IOAPIC_NUM_LEVEL_VECTORS 2
#define RTC_GSI	8
#define RTC_GSI_IRQ 0x85

static bool irq_received;
static struct kvm_vcpu *vcpus[NR_SAVIC_VCPUS];
static pthread_t threads[NR_SAVIC_VCPUS];

#define SAVIC_TEST_STATE(STATE) \
	STATE ## _START, \
	STATE ## _END

enum savic_test_state {
	SAVIC_TEST_STATE(SAVIC_APIC_MSR_ACCESSES),
	SAVIC_TEST_STATE(SAVIC_IDLE_HALT),
	SAVIC_TEST_STATE(SAVIC_IOAPIC),
	SAVIC_TEST_STATE(SAVIC_IOAPIC2),
};

/* Data struct shared between host main thread and vCPUs */
struct test_data_page {
	uint64_t ioapic_eirq1_count;
	uint64_t ioapic_eirq2_count;
	uint64_t ioapic_lirq1_count;
	uint64_t ioapic_lirq2_count;
	uint64_t ioapic_rtc_gsi_irq_count;
};

static struct test_data_page *test_data[NR_SAVIC_VCPUS];

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
	__GUEST_ASSERT(gval == gval2,
			"Unexpected %s Guest backing page value : 0x%lx, msr read val:0x%lx\n",
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
	savic_write_reg(apage, APIC_IRR + APIC_REG_OFF(vec), 0);

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

static void guest_idle_hlt_intr_handler(struct ex_regs *regs)
{
	struct guest_apic_page *apage = get_guest_apic_page();
	uint32_t isr, reg;

	WRITE_ONCE(irq_received, true);
	reg = APIC_ISR + APIC_REG_OFF(IDLE_HLT_INTR_VECTOR);
	isr = savic_read_reg(apage, reg);
	__GUEST_ASSERT(isr & BIT(APIC_VEC_POS(IDLE_HLT_INTR_VECTOR)),
				"Idle halt vector not set in APIC_ISR");
	x2apic_write_reg(APIC_EOI, 0);
	isr = savic_read_reg(apage, reg);
	__GUEST_ASSERT(!(isr & BIT(APIC_VEC_POS(IDLE_HLT_INTR_VECTOR))),
				"Idle halt vector set in APIC_ISR after EOI");
}

static void guest_savic_idle_halt(int id)
{
	uint32_t icr_val;
	uint32_t irr;
	int i;

	x2apic_write_reg(APIC_TASKPRI, 0);
	icr_val = (APIC_DEST_SELF | APIC_INT_ASSERT | IDLE_HLT_INTR_VECTOR);

	for (i = 0; i < NUM_ITERATIONS; i++) {
		asm volatile("cli");
		x2apic_write_reg(APIC_ICR, icr_val);
		irr = x2apic_read_reg(APIC_IRR + APIC_REG_OFF(IDLE_HLT_INTR_VECTOR));
		__GUEST_ASSERT(irr & BIT(APIC_VEC_POS(IDLE_HLT_INTR_VECTOR)),
				"Idle halt vector not set in APIC_IRR");
		asm volatile("sti; hlt;" : : : "memory");
		GUEST_ASSERT(READ_ONCE(irq_received));
		WRITE_ONCE(irq_received, false);
	}
}

static void _ioapic_level_irq_handler(int vec)
{
	uint32_t isr, tmr;
	int offset, pos;

	offset = APIC_REG_OFF(vec);
	pos = APIC_VEC_POS(vec);
	isr = savic_hv_read_reg(APIC_ISR + offset);
	tmr = savic_hv_read_reg(APIC_TMR + offset);

	__GUEST_ASSERT(tmr & BIT_ULL(pos),
		"IOAPIC level vector %d trigger mode in not set in host TMR: %x",
		vec, tmr);
	__GUEST_ASSERT(isr & BIT_ULL(pos),
			"IOAPIC level vector %d in not set in host ISR: %x",
			vec, isr);

	x2apic_write_reg(APIC_EOI, 0x00);
	savic_hv_write_reg(APIC_EOI, 0);

	isr = savic_hv_read_reg(APIC_ISR + offset);
	__GUEST_ASSERT(!(isr & BIT_ULL(pos)),
		"IOAPIC level vector %d set in host ISR after EOI",
		vec);
}

static void ioapic_level_irq1_intr_handler(struct ex_regs *regs)
{
	struct test_data_page *data = test_data[x2apic_read_reg(APIC_ID)];
	int vec;

	vec = IOAPIC_VECTOR_START + IOAPIC_NUM_EDGE_VECTORS;
	WRITE_ONCE(data->ioapic_lirq1_count, data->ioapic_lirq1_count + 1);
	_ioapic_level_irq_handler(vec);
}

static void ioapic_level_irq2_intr_handler(struct ex_regs *regs)
{
	struct test_data_page *data = test_data[x2apic_read_reg(APIC_ID)];
	int vec;

	vec = IOAPIC_VECTOR_START + IOAPIC_NUM_EDGE_VECTORS + 1;
	WRITE_ONCE(data->ioapic_lirq2_count, data->ioapic_lirq2_count + 1);
	_ioapic_level_irq_handler(vec);
}

static void ioapic_edge_irq1_intr_handler(struct ex_regs *regs)
{
	struct test_data_page *data = test_data[x2apic_read_reg(APIC_ID)];

	WRITE_ONCE(data->ioapic_eirq1_count, data->ioapic_eirq1_count + 1);
	x2apic_write_reg(APIC_EOI, 0x00);
}

static void ioapic_edge_irq2_intr_handler(struct ex_regs *regs)
{
	struct test_data_page *data = test_data[x2apic_read_reg(APIC_ID)];

	WRITE_ONCE(data->ioapic_eirq2_count, data->ioapic_eirq2_count + 1);
	x2apic_write_reg(APIC_EOI, 0x00);
}

static void ioapic_rtc_gsi_intr_handler(struct ex_regs *regs)
{
	struct test_data_page *data = test_data[x2apic_read_reg(APIC_ID)];

	WRITE_ONCE(data->ioapic_rtc_gsi_irq_count, data->ioapic_rtc_gsi_irq_count + 1);
	x2apic_write_reg(APIC_EOI, 0x00);
}

static void __savic_ioapic(int count)
{
	struct test_data_page *data = test_data[x2apic_read_reg(APIC_ID)];
	int vec = IOAPIC_VECTOR_START;

	__GUEST_ASSERT(READ_ONCE(data->ioapic_eirq1_count) == count,
			"Invalid ioapic edge irq %d count: %ld, expected: %d",
			vec, READ_ONCE(data->ioapic_eirq1_count), count);
	__GUEST_ASSERT(READ_ONCE(data->ioapic_eirq2_count) == count,
			"Invalid ioapic edge irq %d count: %ld, expected: %d",
			vec + 1, READ_ONCE(data->ioapic_eirq2_count), count);
	__GUEST_ASSERT(READ_ONCE(data->ioapic_lirq1_count) == count,
			"Invalid ioapic level irq %d count: %ld, expected: %d",
			vec + 2, READ_ONCE(data->ioapic_lirq1_count), count);
	__GUEST_ASSERT(READ_ONCE(data->ioapic_lirq2_count) == count,
			"Invalid ioapic level irq %d count: %ld, expected: %d",
			vec + 3, READ_ONCE(data->ioapic_lirq2_count), count);
	__GUEST_ASSERT(READ_ONCE(data->ioapic_rtc_gsi_irq_count) == count,
			"Invalid ioapic RTC irq %d count: %ld, expected: %d",
			RTC_GSI_IRQ, READ_ONCE(data->ioapic_rtc_gsi_irq_count),
			count);
}

static void savic_ioapic(int id)
{
	__savic_ioapic(1);
}

static void savic_ioapic2(int id)
{
	__savic_ioapic(2);
}

static void ioapic_set_redir(unsigned int line, unsigned int vec,
			     enum trigger_mode trig_mode)
{
	struct ioapic_redirect_entry e = {
		.vector = vec,
		.delivery_mode = 0,
		.dest_mode = 0,
		.trig_mode = trig_mode,
		.mask = 0,
		.dest_id = 0,
		.delivery_status = 0,
		.remote_irr = 0,
	};

	ioapic_write_redir(line, e);
}

static void guest_setup_ioapic(int id)
{
	int vec = IOAPIC_VECTOR_START;
	struct ioapic_redirect_entry e;
	int i, line = 0;

	for (i = 0; i < IOAPIC_NUM_EDGE_VECTORS; i++) {
		ioapic_set_redir(line, vec, TRIGGER_EDGE);
		e = ioapic_read_redir(line);
		__GUEST_ASSERT(
			e.vector == vec && e.trig_mode == TRIGGER_EDGE &&
			e.dest_id == 0,
			"Invalid IOAPIC redir entry for line : %d, trig_mode: %d vector: %d",
			line, e.trig_mode, e.vector);
		vec++;
		line++;
	}

	for (i = 0; i < IOAPIC_NUM_LEVEL_VECTORS; i++) {
		ioapic_set_redir(line, vec, TRIGGER_LEVEL);
		e = ioapic_read_redir(line);
		__GUEST_ASSERT(
			e.vector == vec && e.trig_mode == TRIGGER_LEVEL &&
			e.dest_id == 0,
			"Invalid IOAPIC redir entry for line : %d, trig_mode: %d vector: %d",
			line, e.trig_mode, e.vector);
		line++;
		vec++;
	}

	vec = RTC_GSI_IRQ;
	line = RTC_GSI;
	ioapic_set_redir(line, vec, TRIGGER_EDGE);
	e = ioapic_read_redir(line);
	__GUEST_ASSERT(
		e.vector == vec && e.trig_mode == TRIGGER_EDGE &&
		e.dest_id == 0,
		"Invalid IOAPIC redir entry for line : %d, trig_mode: %d vector: %d",
		line, e.trig_mode, e.vector);

	x2apic_write_reg(APIC_TASKPRI, 0);

	for (i = 0; i < (IOAPIC_NUM_EDGE_VECTORS + IOAPIC_NUM_LEVEL_VECTORS); i++) {
		vec = IOAPIC_VECTOR_START + i;
		savic_allow_vector(vec);
	}

	vec = RTC_GSI_IRQ;
	savic_allow_vector(vec);
}

static void guest_code(int id)
{
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SNP_SECURE_AVIC);

	x2apic_enable();

	savic_enable();

	SAVIC_GUEST_SYNC(SAVIC_APIC_MSR_ACCESSES, guest_savic_apic_msr_accesses);

	SAVIC_GUEST_SYNC(SAVIC_IDLE_HALT, guest_savic_idle_halt);

	guest_setup_ioapic(id);
	SAVIC_GUEST_SYNC(SAVIC_IOAPIC, savic_ioapic);
	SAVIC_GUEST_SYNC(SAVIC_IOAPIC2, savic_ioapic2);

	GUEST_DONE();
}

static void host_send_ioapic_irq(struct kvm_vm *vm, int id)
{
	kvm_irq_line(vm, 0, 1);
	kvm_irq_line(vm, 1, 1);
	kvm_irq_line(vm, 0, 0);
	kvm_irq_line(vm, 1, 0);
	kvm_irq_line(vm, 2, 1);
	kvm_irq_line(vm, 2, 0);
	kvm_irq_line(vm, 3, 1);
	kvm_irq_line(vm, 3, 0);
	kvm_irq_line_status(vm, RTC_GSI, 1);
	kvm_irq_line_status(vm, RTC_GSI, 0);
}

static void host_test_savic(struct kvm_vm *vm, int id, enum savic_test_state test_state)
{
	switch (test_state) {
	case SAVIC_IOAPIC_START:
		host_send_ioapic_irq(vm, id);
		break;
	case SAVIC_IOAPIC2_START:
		host_send_ioapic_irq(vm, id);
		break;
	default:
		break;
	}
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
			host_test_savic(vcpu->vm, vcpu->id, uc.args[1]);
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

static void install_exception_handlers(struct kvm_vm *vm)
{
	vm_install_exception_handler(vm, IDLE_HLT_INTR_VECTOR, guest_idle_hlt_intr_handler);
	vm_install_exception_handler(vm, 29, savic_vc_handler);
	vm_install_exception_handler(vm, IOAPIC_VECTOR_START, ioapic_edge_irq1_intr_handler);
	vm_install_exception_handler(vm, IOAPIC_VECTOR_START + 1, ioapic_edge_irq2_intr_handler);
	vm_install_exception_handler(vm, IOAPIC_VECTOR_START + 2, ioapic_level_irq1_intr_handler);
	vm_install_exception_handler(vm, IOAPIC_VECTOR_START + 3, ioapic_level_irq2_intr_handler);
	vm_install_exception_handler(vm, RTC_GSI_IRQ, ioapic_rtc_gsi_intr_handler);
}

int main(int argc, char *argv[])
{
	struct kvm_sev_init args = {
		.vmsa_features = BIT_ULL(SVM_FEAT_SECURE_AVIC)
	};
	struct test_data_page *shared_data[NR_SAVIC_VCPUS];
	vm_vaddr_t test_data_page_vaddr;
	struct kvm_vm *vm;
	int i, r;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SEV_SNP));
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SECURE_AVIC));
	TEST_REQUIRE(this_cpu_has(X86_FEATURE_IDLE_HLT));

	vm = _vm_sev_create_with_one_vcpu(KVM_X86_SNP_VM, guest_code, &vcpus[0], &args);

	virt_pg_map(vm, APIC_DEFAULT_GPA, APIC_DEFAULT_GPA);
	virt_pg_map(vm, IOAPIC_DEFAULT_GPA, IOAPIC_DEFAULT_GPA);

	install_exception_handlers(vm);

	vcpu_args_set(vcpus[0], 1, vcpus[0]->id);

	for (i = 0; i < NR_SAVIC_VCPUS; i++) {
		test_data_page_vaddr = vm_vaddr_alloc_page_shared(vm);
		test_data[i] = (struct test_data_page *)test_data_page_vaddr;
		shared_data[i] = addr_gva2hva(vm, test_data_page_vaddr);
		vm_mem_set_shared(vm, addr_hva2gpa(vm, shared_data[i]), getpagesize());
	}

	sync_global_to_guest(vm, test_data);

	vm_sev_launch(vm, snp_default_policy(), NULL);

	r = pthread_create(&threads[0], NULL, vcpu_thread, vcpus[0]);
	TEST_ASSERT(r == 0, "pthread_create failed errno=%d", errno);

	pthread_join(threads[0], NULL);

	for (i = 0; i < NR_SAVIC_VCPUS; i++) {
		struct test_data_page *shared_state = shared_data[i];

		fprintf(stderr, "VCPU %d ioapic edge irq1 count: %ld edge irq2 count: %ld\n", i, shared_state->ioapic_eirq1_count, shared_state->ioapic_eirq2_count);
		fprintf(stderr, "VCPU %d ioapic level irq1 count: %ld level irq2 count: %ld\n", i, shared_state->ioapic_lirq1_count, shared_state->ioapic_lirq2_count);
		fprintf(stderr, "VCPU %d ioapic RTC GSI irq1 count: %ld\n", i, shared_state->ioapic_rtc_gsi_irq_count);
	}

	kvm_vm_free(vm);

	return 0;
}
