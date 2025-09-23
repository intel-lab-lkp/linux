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

#define NR_SAVIC_VCPUS	4
#define NUM_ITERATIONS 1000

#define IDLE_HLT_INTR_VECTOR     0x30
#define IOAPIC_VECTOR_START      0x81
#define IOAPIC_NUM_EDGE_VECTORS 2
#define IOAPIC_NUM_LEVEL_VECTORS 2
#define RTC_GSI	8
#define RTC_GSI_IRQ 0x85
#define MSI_VECTOR 0x40
#define FIXED_IPI_VEC     0x31
#define FIXED_LOGICAL_IPI_VEC     0x32
#define BROADCAST_ALL_IPI_VEC     0x33
#define BROADCAST_NOSELF_IPI_VEC     0x34

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
	SAVIC_TEST_STATE(SAVIC_MSI),
	SAVIC_TEST_STATE(SAVIC_IPI),
	SAVIC_TEST_STATE(SAVIC_NMI),
	SAVIC_TEST_STATE(SAVIC_NMI2),
	SAVIC_TEST_STATE(SAVIC_NMI3),
	SAVIC_TEST_STATE(SAVIC_ICR_FIXED_PHYS_NMI),
	SAVIC_TEST_STATE(SAVIC_ICR_FIXED_LOGICAL_NMI),
	SAVIC_TEST_STATE(SAVIC_ICR_BROADCAST_NMI),
	SAVIC_TEST_STATE(SAVIC_ICR_BROADCAST_NOSELF_NMI),
};

/* Data struct shared between host main thread and vCPUs */
struct test_data_page {
	uint64_t ioapic_eirq1_count;
	uint64_t ioapic_eirq2_count;
	uint64_t ioapic_lirq1_count;
	uint64_t ioapic_lirq2_count;
	uint64_t ioapic_rtc_gsi_irq_count;
	uint64_t msi_irq_count;
	uint64_t fixed_phys_ipi_wake_count;
	uint64_t fixed_phys_ipi_hlt_count;
	uint64_t fixed_logical_ipi_hlt_count;
	uint64_t fixed_logical_ipi_wake_count;
	uint64_t broadcast_ipi_hlt_count;
	uint64_t broadcast_ipi_wake_count;
	uint64_t broadcast_noself_ipi_hlt_count;
	uint64_t broadcast_noself_ipi_wake_count;
	uint64_t fixed_phys_ipi_count;
	uint64_t fixed_logical_ipi_count;
	uint64_t broadcast_ipi_count;
	uint64_t broadcast_noself_ipi_count;
	uint64_t *nmi_count_p;
	uint64_t nmi_count;
	uint64_t fixed_phys_nmi_hlt_count;
	uint64_t fixed_phys_nmi_wake_count;
	uint64_t fixed_phys_nmi_count;
	uint64_t fixed_logical_nmi_hlt_count;
	uint64_t fixed_logical_nmi_wake_count;
	uint64_t fixed_logical_nmi_count;
	uint64_t broadcast_nmi_hlt_count;
	uint64_t broadcast_nmi_wake_count;
	uint64_t broadcast_nmi_count;
	uint64_t broadcast_noself_nmi_hlt_count;
	uint64_t broadcast_noself_nmi_wake_count;
	uint64_t broadcast_noself_nmi_count;
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

static inline struct test_data_page *get_test_data(void)
{
	return test_data[x2apic_read_reg(APIC_ID)];
}

static void ioapic_level_irq1_intr_handler(struct ex_regs *regs)
{
	struct test_data_page *data = get_test_data();
	int vec;

	vec = IOAPIC_VECTOR_START + IOAPIC_NUM_EDGE_VECTORS;
	WRITE_ONCE(data->ioapic_lirq1_count, data->ioapic_lirq1_count + 1);
	_ioapic_level_irq_handler(vec);
}

static void ioapic_level_irq2_intr_handler(struct ex_regs *regs)
{
	struct test_data_page *data = get_test_data();
	int vec;

	vec = IOAPIC_VECTOR_START + IOAPIC_NUM_EDGE_VECTORS + 1;
	WRITE_ONCE(data->ioapic_lirq2_count, data->ioapic_lirq2_count + 1);
	_ioapic_level_irq_handler(vec);
}

static void ioapic_edge_irq1_intr_handler(struct ex_regs *regs)
{
	struct test_data_page *data = get_test_data();

	WRITE_ONCE(data->ioapic_eirq1_count, data->ioapic_eirq1_count + 1);
	x2apic_write_reg(APIC_EOI, 0x00);
}

static void ioapic_edge_irq2_intr_handler(struct ex_regs *regs)
{
	struct test_data_page *data = get_test_data();

	WRITE_ONCE(data->ioapic_eirq2_count, data->ioapic_eirq2_count + 1);
	x2apic_write_reg(APIC_EOI, 0x00);
}

static void ioapic_rtc_gsi_intr_handler(struct ex_regs *regs)
{
	struct test_data_page *data = get_test_data();

	WRITE_ONCE(data->ioapic_rtc_gsi_irq_count, data->ioapic_rtc_gsi_irq_count + 1);
	x2apic_write_reg(APIC_EOI, 0x00);
}

static void __savic_ioapic(int count)
{
	struct test_data_page *data = get_test_data();
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

static void set_fixed_counters(
		struct test_data_page *data,
		uint64_t **fixed_ipi_p,
		uint64_t **fixed_ipi_hlt_cnt_p,
		uint64_t **fixed_ipi_wake_cnt_p,
		bool logical, bool nmi)
{
	if (logical) {
		*fixed_ipi_p =
			nmi ? &data->fixed_logical_nmi_count :
			      &data->fixed_logical_ipi_count;
		*fixed_ipi_hlt_cnt_p =
			nmi ? &data->fixed_logical_nmi_hlt_count :
			      &data->fixed_logical_ipi_hlt_count;
		*fixed_ipi_wake_cnt_p =
			nmi ? &data->fixed_logical_nmi_wake_count :
			      &data->fixed_logical_ipi_wake_count;
	} else {
		*fixed_ipi_p =
			nmi ? &data->fixed_phys_nmi_count :
			      &data->fixed_phys_ipi_count;
		*fixed_ipi_hlt_cnt_p =
			nmi ? &data->fixed_phys_nmi_hlt_count :
			      &data->fixed_phys_ipi_hlt_count;
		*fixed_ipi_wake_cnt_p =
			nmi ? &data->fixed_phys_nmi_wake_count :
			      &data->fixed_phys_ipi_wake_count;
	}
}

static void savic_fixed_ipi(bool logical, bool nmi)
{
	uint64_t last_wake_cnt, last_hlt_cnt;
	uint64_t last_fixed_phys_ipi_cnt;
	uint64_t tsc_start;
	uint64_t *fixed_ipi_p;
	uint64_t *fixed_ipi_hlt_cnt_p;
	uint64_t *fixed_ipi_wake_cnt_p;
	int vec;
	int i, j;

	for (i = 1; i < NR_SAVIC_VCPUS; i++) {
		struct test_data_page *data = test_data[i];
		uint64_t dst_apic_id = i;

		set_fixed_counters(data, &fixed_ipi_p,
				&fixed_ipi_hlt_cnt_p, &fixed_ipi_wake_cnt_p,
				logical, nmi);
		if (logical) {
			vec = FIXED_LOGICAL_IPI_VEC | APIC_DEST_LOGICAL;
			dst_apic_id = 1 << i;
		} else {
			vec = FIXED_IPI_VEC;
			dst_apic_id = i;
		}

		if (nmi)
			vec |= APIC_DM_NMI;

		last_wake_cnt = READ_ONCE(*fixed_ipi_wake_cnt_p);
		while (!READ_ONCE(*fixed_ipi_hlt_cnt_p))
			;

		last_hlt_cnt = READ_ONCE(*fixed_ipi_hlt_cnt_p);
		last_fixed_phys_ipi_cnt = READ_ONCE(*fixed_ipi_p);

		for (j = 0; j < NUM_ITERATIONS; j++) {
			tsc_start = rdtsc();
			x2apic_write_reg(APIC_ICR, dst_apic_id << 32 |
					APIC_INT_ASSERT | vec);
			while (rdtsc() - tsc_start < 1000000000) {
				if (READ_ONCE(*fixed_ipi_wake_cnt_p) != last_wake_cnt &&
				    READ_ONCE(*fixed_ipi_hlt_cnt_p) != last_hlt_cnt &&
				    READ_ONCE(*fixed_ipi_p) != last_fixed_phys_ipi_cnt)
					break;
			}

			__GUEST_ASSERT(READ_ONCE(*fixed_ipi_wake_cnt_p) != last_wake_cnt &&
				       READ_ONCE(*fixed_ipi_hlt_cnt_p) != last_hlt_cnt &&
				       READ_ONCE(*fixed_ipi_p) != last_fixed_phys_ipi_cnt,
				       "%s fixed-%s wake: %ld last_wake: %ld hlt: %ld last_hlt: %ld ipi: %ld last_ipi: %ld",
				       nmi ? "nmi" : "ipi",
				       logical ? "logical" : "phys",
				       READ_ONCE(*fixed_ipi_wake_cnt_p), last_wake_cnt,
				       READ_ONCE(*fixed_ipi_hlt_cnt_p), last_hlt_cnt,
				       READ_ONCE(*fixed_ipi_p), last_fixed_phys_ipi_cnt);

			last_wake_cnt = READ_ONCE(*fixed_ipi_wake_cnt_p);
			last_hlt_cnt = READ_ONCE(*fixed_ipi_hlt_cnt_p);
			last_fixed_phys_ipi_cnt = READ_ONCE(*fixed_ipi_p);
		}
	}
}

static uint64_t *get_broadcast_ipi_counter(struct test_data_page *data,
		int dsh, bool nmi)
{
	if (dsh == APIC_DEST_ALLINC)
		return nmi ?
			&data->broadcast_nmi_count :
			&data->broadcast_ipi_count;
	else
		return nmi ?
			&data->broadcast_noself_nmi_count :
			&data->broadcast_noself_ipi_count;
}


static uint64_t *get_broadcast_hlt_counter(struct test_data_page *data,
		int dsh, bool nmi)
{
	if (dsh == APIC_DEST_ALLINC)
		return nmi ?
			&data->broadcast_nmi_hlt_count :
			&data->broadcast_ipi_hlt_count;
	else
		return nmi ?
			&data->broadcast_noself_nmi_hlt_count :
			&data->broadcast_noself_ipi_hlt_count;
}

static uint64_t *get_broadcast_wake_counter(struct test_data_page *data,
		int dsh, bool nmi)
{
	if (dsh == APIC_DEST_ALLINC)
		return nmi ?
			&data->broadcast_nmi_wake_count :
			&data->broadcast_ipi_wake_count;
	else
		return nmi ?
			&data->broadcast_noself_nmi_wake_count :
			&data->broadcast_noself_ipi_wake_count;
}

static void savic_send_broadcast(int dsh, bool nmi)
{
	uint64_t last_wake_cnt[NR_SAVIC_VCPUS], last_hlt_cnt[NR_SAVIC_VCPUS];
	uint64_t last_ipi_cnt[NR_SAVIC_VCPUS];
	uint64_t tsc_start;
	uint64_t *broadcast_ipi_cnt_p;
	uint64_t *broadcast_ipi_hlt_cnt_p;
	uint64_t *broadcast_ipi_wake_cnt_p;
	struct test_data_page *data;
	int i, j;
	int vec;

	if (dsh == APIC_DEST_ALLINC)
		vec = BROADCAST_ALL_IPI_VEC;
	else
		vec = BROADCAST_NOSELF_IPI_VEC;

	for (i = 1; i < NR_SAVIC_VCPUS; i++) {
		data = test_data[i];

		broadcast_ipi_hlt_cnt_p = get_broadcast_hlt_counter(
				data, dsh, nmi);

		while (!READ_ONCE(*broadcast_ipi_hlt_cnt_p))
			;
	}

	for (j = 0; j < NUM_ITERATIONS; j++) {
		for (i = 1; i < NR_SAVIC_VCPUS; i++) {
			data = test_data[i];

			broadcast_ipi_cnt_p = get_broadcast_ipi_counter(
				data, dsh, nmi);
			broadcast_ipi_hlt_cnt_p = get_broadcast_hlt_counter(
				data, dsh, nmi);
			broadcast_ipi_wake_cnt_p = get_broadcast_wake_counter(
				data, dsh, nmi);
			last_ipi_cnt[i] = *broadcast_ipi_cnt_p;
			last_hlt_cnt[i] = *broadcast_ipi_hlt_cnt_p;
			last_wake_cnt[i] = *broadcast_ipi_wake_cnt_p;
		}

		if (nmi)
			vec |= APIC_DM_NMI;

		x2apic_write_reg(APIC_ICR, APIC_INT_ASSERT | dsh | vec);

		tsc_start = rdtsc();

		for (i = 1; i < NR_SAVIC_VCPUS; i++) {
			data = test_data[i];

			broadcast_ipi_cnt_p = get_broadcast_ipi_counter(
				data, dsh, nmi);
			broadcast_ipi_hlt_cnt_p = get_broadcast_hlt_counter(
				data, dsh, nmi);
			broadcast_ipi_wake_cnt_p = get_broadcast_wake_counter(
				data, dsh, nmi);

			while (rdtsc() - tsc_start < 1000000000) {
				if (READ_ONCE(*broadcast_ipi_wake_cnt_p) != last_wake_cnt[i] &&
				    READ_ONCE(*broadcast_ipi_hlt_cnt_p) != last_hlt_cnt[i] &&
				    READ_ONCE(*broadcast_ipi_cnt_p) != last_ipi_cnt[i])
					break;
			}

			__GUEST_ASSERT(READ_ONCE(*broadcast_ipi_wake_cnt_p) != last_wake_cnt[i] &&
				       READ_ONCE(*broadcast_ipi_hlt_cnt_p) != last_hlt_cnt[i] &&
				       READ_ONCE(*broadcast_ipi_cnt_p) != last_ipi_cnt[i],
				       "%s broadcast-%s wake: %ld last_wake: %ld hlt: %ld last_hlt: %ld ipi: %ld last_ipi: %ld",
				       nmi ? "nmi" : "ipi",
				       dsh == APIC_DEST_ALLINC ? "all" : "excl-self",
				       READ_ONCE(*broadcast_ipi_wake_cnt_p), last_wake_cnt[i],
				       READ_ONCE(*broadcast_ipi_hlt_cnt_p), last_hlt_cnt[i],
				       READ_ONCE(*broadcast_ipi_cnt_p), last_ipi_cnt[i]);

			last_wake_cnt[i] = READ_ONCE(*broadcast_ipi_wake_cnt_p);
			last_hlt_cnt[i] = READ_ONCE(*broadcast_ipi_hlt_cnt_p);
			last_ipi_cnt[i] = READ_ONCE(*broadcast_ipi_cnt_p);
		}
	}
}

void savic_ipi(int id)
{
	savic_fixed_ipi(false, false);
	savic_fixed_ipi(true, false);

	asm volatile("sti;":::"memory");
	x2apic_write_reg(APIC_TASKPRI, 0);
	savic_send_broadcast(APIC_DEST_ALLINC, false);
	savic_send_broadcast(APIC_DEST_ALLBUT, false);
}

void guest_fixed_phys_ipi_handler(struct ex_regs *regs)
{
	struct test_data_page *data = get_test_data();

	WRITE_ONCE(data->fixed_phys_ipi_count, data->fixed_phys_ipi_count + 1);
	x2apic_write_reg(APIC_EOI, 0x00);
}

void guest_fixed_logical_ipi_handler(struct ex_regs *regs)
{
	struct test_data_page *data = get_test_data();

	WRITE_ONCE(data->fixed_logical_ipi_count, data->fixed_logical_ipi_count + 1);
	x2apic_write_reg(APIC_EOI, 0x00);
}

void guest_broadcast_ipi_handler(struct ex_regs *regs)
{
	struct test_data_page *data = get_test_data();

	WRITE_ONCE(data->broadcast_ipi_count, data->broadcast_ipi_count + 1);
	x2apic_write_reg(APIC_EOI, 0x00);
}

void guest_broadcast_noself_ipi_handler(struct ex_regs *regs)
{
	struct test_data_page *data = get_test_data();

	WRITE_ONCE(data->broadcast_noself_ipi_count, data->broadcast_noself_ipi_count + 1);
	x2apic_write_reg(APIC_EOI, 0x00);
}

static void savic_nmi(int id)
{
	struct test_data_page *data = get_test_data();

	__GUEST_ASSERT(!data->nmi_count, "Invalid NMI count: %ld\n", data->nmi_count);
	set_savic_control_msr(get_guest_apic_page(), true, true);
}

static void savic_nmi2(int id)
{
	struct test_data_page *data = get_test_data();

	__GUEST_ASSERT(data->nmi_count == 2, "Invalid NMI count: %ld\n", data->nmi_count);
}

static void savic_nmi3(int id)
{
	struct test_data_page *data = get_test_data();

	__GUEST_ASSERT(data->nmi_count == 4, "Invalid NMI count: %ld\n", data->nmi_count);
}

static void savic_icr_fixed_phys(int id)
{
	savic_fixed_ipi(false, true);
}

static void savic_icr_fixed_logical(int id)
{
	savic_fixed_ipi(true, true);
}

static void savic_icr_bcast(int id)
{
	savic_send_broadcast(APIC_DEST_ALLINC, true);
}

static void savic_icr_bcast_noself(int id)
{
	savic_send_broadcast(APIC_DEST_ALLBUT, true);
}

static void guest_nmi_handler(struct ex_regs *regs)
{
	struct test_data_page *data = get_test_data();

	WRITE_ONCE(*data->nmi_count_p, *data->nmi_count_p + 1);
	/* Skip NMI completed notification for ICR based NMI. */
	if (data->nmi_count_p == &data->nmi_count)
		sev_es_nmi_complete();
}

static void savic_msi_not_allowed(int id)
{
	struct test_data_page *data = get_test_data();

	savic_allow_vector(MSI_VECTOR);

	__GUEST_ASSERT(READ_ONCE(data->msi_irq_count) == 0,
			"Invalid MSI IRQ count: %ld, should be 0",
			READ_ONCE(data->msi_irq_count));
}

static void savic_msi_allowed(int id)
{
	struct test_data_page *data = get_test_data();

	__GUEST_ASSERT(READ_ONCE(data->msi_irq_count) == 1,
			"Invalid MSI IRQ count: %ld",
			READ_ONCE(data->msi_irq_count));
}

static void msi_intr_handler(struct ex_regs *regs)
{
	struct test_data_page *data = get_test_data();

	 WRITE_ONCE(data->msi_irq_count, data->msi_irq_count + 1);
	 x2apic_write_reg(APIC_EOI, 0x00);
}

static void ipi_guest_code(int id)
{
	struct test_data_page *data;
	uint64_t *ipi_count_p, *hlt_count_p, *wake_count_p;
	int i;

	x2apic_enable();
	id = x2apic_read_reg(APIC_ID);
	data = test_data[id];
	savic_enable();
	x2apic_write_reg(APIC_TASKPRI, 0);

	uint64_t *ipi_count_types[][3] = {
		{
			&data->fixed_phys_ipi_hlt_count,
			&data->fixed_phys_ipi_count,
			&data->fixed_phys_ipi_wake_count
		},
		{
			&data->fixed_logical_ipi_hlt_count,
			&data->fixed_logical_ipi_count,
			&data->fixed_logical_ipi_wake_count
		},
		{
			&data->broadcast_ipi_hlt_count,
			&data->broadcast_ipi_count,
			&data->broadcast_ipi_wake_count
		},
		{
			&data->broadcast_noself_ipi_hlt_count,
			&data->broadcast_noself_ipi_count,
			&data->broadcast_noself_ipi_wake_count
		},
		{
			&data->fixed_phys_nmi_hlt_count,
			&data->fixed_phys_nmi_count,
			&data->fixed_phys_nmi_wake_count
		},
		{
			&data->fixed_logical_nmi_hlt_count,
			&data->fixed_logical_nmi_count,
			&data->fixed_logical_nmi_wake_count
		},
		{
			&data->broadcast_nmi_hlt_count,
			&data->broadcast_nmi_count,
			&data->broadcast_nmi_wake_count
		},
		{
			&data->broadcast_noself_nmi_hlt_count,
			&data->broadcast_noself_nmi_count,
			&data->broadcast_noself_nmi_wake_count
		},
	};

	for (i = 0; i < ARRAY_SIZE(ipi_count_types); i++) {
		hlt_count_p = ipi_count_types[i][0];
		ipi_count_p = ipi_count_types[i][1];
		wake_count_p = ipi_count_types[i][2];

		while (READ_ONCE(*ipi_count_p) != NUM_ITERATIONS) {
			if (i < 4)
				asm volatile("cli");
			WRITE_ONCE(*hlt_count_p, *hlt_count_p + 1);
			if (i < 4)
				asm volatile("sti; hlt" : : : "memory");
			WRITE_ONCE(*wake_count_p, *wake_count_p + 1);
		}

		WRITE_ONCE(*hlt_count_p, *hlt_count_p + 1);
	}

	GUEST_DONE();
}

static void guest_code(int id)
{
	struct test_data_page *data;
	int i;

	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SNP_SECURE_AVIC);

	x2apic_enable();

	savic_enable();

	SAVIC_GUEST_SYNC(SAVIC_APIC_MSR_ACCESSES, guest_savic_apic_msr_accesses);

	SAVIC_GUEST_SYNC(SAVIC_IDLE_HALT, guest_savic_idle_halt);

	guest_setup_ioapic(id);
	SAVIC_GUEST_SYNC(SAVIC_IOAPIC, savic_ioapic);
	SAVIC_GUEST_SYNC(SAVIC_IOAPIC2, savic_ioapic2);

	SAVIC_GUEST_SYNC(SAVIC_MSI, savic_msi_not_allowed);
	SAVIC_GUEST_SYNC(SAVIC_MSI, savic_msi_allowed);

	SAVIC_GUEST_SYNC(SAVIC_IPI, savic_ipi);

	/* Disable host NMI injection in control MSR. */
	set_savic_control_msr(get_guest_apic_page(), true, false);

	data = test_data[id];
	data->nmi_count_p = &data->nmi_count;
	SAVIC_GUEST_SYNC(SAVIC_NMI, savic_nmi);
	SAVIC_GUEST_SYNC(SAVIC_NMI2, savic_nmi2);
	SAVIC_GUEST_SYNC(SAVIC_NMI3, savic_nmi3);

	for (i = 0; i < NR_SAVIC_VCPUS; i++) {
		data = test_data[i];
		data->nmi_count_p = &data->fixed_phys_nmi_count;
	}
	SAVIC_GUEST_SYNC(SAVIC_ICR_FIXED_PHYS_NMI, savic_icr_fixed_phys);

	for (i = 0; i < NR_SAVIC_VCPUS; i++) {
		data = test_data[i];
		data->nmi_count_p = &data->fixed_logical_nmi_count;
	}
	SAVIC_GUEST_SYNC(SAVIC_ICR_FIXED_LOGICAL_NMI, savic_icr_fixed_logical);

	for (i = 0; i < NR_SAVIC_VCPUS; i++) {
		data = test_data[i];
		data->nmi_count_p = &data->broadcast_nmi_count;
	}
	SAVIC_GUEST_SYNC(SAVIC_ICR_BROADCAST_NMI, savic_icr_bcast);

	for (i = 0; i < NR_SAVIC_VCPUS; i++) {
		data = test_data[i];
		data->nmi_count_p = &data->broadcast_noself_nmi_count;
	}
	SAVIC_GUEST_SYNC(SAVIC_ICR_BROADCAST_NOSELF_NMI, savic_icr_bcast_noself);

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

static void host_send_msi(struct kvm_vm *vm)
{
	struct kvm_msi msi = {
		.address_lo = 0,
		.address_hi = 0,
		.data = MSI_VECTOR,
	};

	__vm_ioctl(vm, KVM_SIGNAL_MSI, &msi);
}

static void host_send_nmi(int id)
{
	vcpu_nmi(vcpus[id]);
	vcpu_nmi(vcpus[id]);
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
	case SAVIC_MSI_START:
		host_send_msi(vm);
		break;
	case SAVIC_NMI_START:
	case SAVIC_NMI2_START:
	case SAVIC_NMI3_START:
		host_send_nmi(id);
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
	vm_install_exception_handler(vm, FIXED_IPI_VEC, guest_fixed_phys_ipi_handler);
	vm_install_exception_handler(vm, FIXED_LOGICAL_IPI_VEC, guest_fixed_logical_ipi_handler);
	vm_install_exception_handler(vm, BROADCAST_ALL_IPI_VEC, guest_broadcast_ipi_handler);
	vm_install_exception_handler(vm, BROADCAST_NOSELF_IPI_VEC,
			guest_broadcast_noself_ipi_handler);
	vm_install_exception_handler(vm, NMI_VECTOR, guest_nmi_handler);
	vm_install_exception_handler(vm, MSI_VECTOR, msi_intr_handler);
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
	struct vm_shape shape = {
		.mode = VM_MODE_DEFAULT,
		.type = KVM_X86_SNP_VM,
	};

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SEV_SNP));
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SECURE_AVIC));
	TEST_REQUIRE(this_cpu_has(X86_FEATURE_IDLE_HLT));

	vm = __vm_create_with_args(shape, NR_SAVIC_VCPUS, 0, &args);

	vcpus[0] = vm_vcpu_add(vm, 0, guest_code);
	for (i = 1; i < NR_SAVIC_VCPUS; ++i)
		vcpus[i] = vm_vcpu_add(vm, i, ipi_guest_code);

	virt_pg_map(vm, APIC_DEFAULT_GPA, APIC_DEFAULT_GPA);
	virt_pg_map(vm, IOAPIC_DEFAULT_GPA, IOAPIC_DEFAULT_GPA);

	install_exception_handlers(vm);

	for (i = 0; i < NR_SAVIC_VCPUS; i++)
		vcpu_args_set(vcpus[i], 1, vcpus[i]->id);

	for (i = 0; i < NR_SAVIC_VCPUS; i++) {
		test_data_page_vaddr = vm_vaddr_alloc_page_shared(vm);
		test_data[i] = (struct test_data_page *)test_data_page_vaddr;
		shared_data[i] = addr_gva2hva(vm, test_data_page_vaddr);
		vm_mem_set_shared(vm, addr_hva2gpa(vm, shared_data[i]), getpagesize());
	}

	sync_global_to_guest(vm, test_data);

	vm_sev_launch(vm, snp_default_policy(), NULL);

	for (i = 0; i < NR_SAVIC_VCPUS; i++) {
		r = pthread_create(&threads[i], NULL, vcpu_thread, vcpus[i]);
		TEST_ASSERT(r == 0, "pthread_create failed errno=%d", errno);
	}

	for (i = 0; i < NR_SAVIC_VCPUS; i++)
		pthread_join(threads[i], NULL);

	for (i = 0; i < NR_SAVIC_VCPUS; i++) {
		struct test_data_page *shared_state = shared_data[i];

		fprintf(stderr, "VCPU %d ioapic edge irq1 count: %ld edge irq2 count: %ld\n", i, shared_state->ioapic_eirq1_count, shared_state->ioapic_eirq2_count);
		fprintf(stderr, "VCPU %d ioapic level irq1 count: %ld level irq2 count: %ld\n", i, shared_state->ioapic_lirq1_count, shared_state->ioapic_lirq2_count);
		fprintf(stderr, "VCPU %d ioapic RTC GSI irq1 count: %ld\n", i, shared_state->ioapic_rtc_gsi_irq_count);
		fprintf(stderr, "vCPU %d fixed IPI counts wake: %ld hlt: %ld num-IPI: %ld\n",
			i, shared_state->fixed_phys_ipi_wake_count,
			shared_state->fixed_phys_ipi_hlt_count,
			shared_state->fixed_phys_ipi_count);
		fprintf(stderr, "vCPU %d fixed-logical IPI counts wake: %ld hlt: %ld num-IPI: %ld\n",
			i, shared_state->fixed_logical_ipi_wake_count,
			shared_state->fixed_logical_ipi_hlt_count,
			shared_state->fixed_logical_ipi_count);
		fprintf(stderr, "vCPU %d broadcast IPI counts wake: %ld hlt: %ld num-IPI: %ld\n",
			i, shared_state->broadcast_ipi_wake_count,
			shared_state->broadcast_ipi_hlt_count,
			shared_state->broadcast_ipi_count);
		fprintf(stderr, "vCPU %d broadcast excluding self IPI counts wake: %ld hlt: %ld num-IPI: %ld\n",
			i, shared_state->broadcast_noself_ipi_wake_count,
			shared_state->broadcast_noself_ipi_hlt_count,
			shared_state->broadcast_noself_ipi_count);
		fprintf(stderr, "vCPU %d nmi count: %ld\n",
			i, shared_state->nmi_count);
		fprintf(stderr, "vCPU %d nmi fixed IPI counts wake: %ld hlt: %ld num-IPI: %ld\n",
			i, shared_state->fixed_phys_nmi_wake_count,
			shared_state->fixed_phys_nmi_hlt_count,
			shared_state->fixed_phys_nmi_count);
		fprintf(stderr, "vCPU %d nmi fixed-logical IPI counts wake: %ld hlt: %ld num-IPI: %ld\n",
			i, shared_state->fixed_logical_nmi_wake_count,
			shared_state->fixed_logical_nmi_hlt_count,
			shared_state->fixed_logical_nmi_count);
		fprintf(stderr, "vCPU %d nmi broadcast IPI counts wake: %ld hlt: %ld num-IPI: %ld\n",
			i, shared_state->broadcast_nmi_wake_count,
			shared_state->broadcast_nmi_hlt_count,
			shared_state->broadcast_nmi_count);
		fprintf(stderr, "vCPU %d nmi broadcast excluding self IPI counts wake: %ld hlt: %ld num-IPI: %ld\n",
			i, shared_state->broadcast_noself_nmi_wake_count,
			shared_state->broadcast_noself_nmi_hlt_count,
			shared_state->broadcast_noself_nmi_count);
	}

	kvm_vm_free(vm);

	return 0;
}
