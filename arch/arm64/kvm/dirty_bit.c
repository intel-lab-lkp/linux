// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Ltd.
 * Author: Leonardo Bras <leo.bras@arm.com>
 */

#include <asm/kvm_dirty_bit.h>
#include <linux/kconfig.h>
#include <linux/acpi.h>

DEFINE_PER_CPU(struct hacdbs, hacdbs_pcp) = {
	.status = HACDBS_OFF,
	.size = 0,
};

/* HDBSS entry field definitions */
#define HDBSS_ENTRY_VALID BIT(0)
#define HDBSS_ENTRY_TTWL_SHIFT (1)
#define HDBSS_ENTRY_TTWL_MASK (GENMASK(3, 1))
#define HDBSS_ENTRY_TTWL(x) \
	(((x) << HDBSS_ENTRY_TTWL_SHIFT) & HDBSS_ENTRY_TTWL_MASK)
#define HDBSS_ENTRY_TTWL_RESV HDBSS_ENTRY_TTWL(-4)
#define HDBSS_ENTRY_IPA GENMASK_ULL(55, 12)

inline u64 hdbss_get_ttwl(u64 chunk_size)
{
	u64 hw_lvl = ARM64_HW_PGTABLE_LEVELS(ilog2(chunk_size));

	return HDBSS_ENTRY_TTWL(3 - hw_lvl);
}

static __ro_after_init int hacdbsirq = -1;

static irqreturn_t hacdbsirq_handler(int irq, void *pcpu)
{
	u64 cons = read_sysreg_s(SYS_HACDBSCONS_EL2);
	unsigned long err = FIELD_GET(HACDBSCONS_EL2_ERR_REASON, cons);

	switch (err) {
	case HACDBSCONS_EL2_ERR_REASON_NOF:
		this_cpu_write(hacdbs_pcp.status, HACDBS_IDLE);
		break;
	case HACDBSCONS_EL2_ERR_REASON_IPAHACF:
		/* When size not a power of two >= 4k, exit with reserved TTLW */
		int index = FIELD_GET(HACDBSCONS_EL2_INDEX, cons);

		if (index >= this_cpu_read(hacdbs_pcp.size)) {
			this_cpu_write(hacdbs_pcp.status, HACDBS_IDLE);
			break;
		}
		fallthrough;
	case HACDBSCONS_EL2_ERR_REASON_STRUCTF:
	case HACDBSCONS_EL2_ERR_REASON_IPAF:
		this_cpu_write(hacdbs_pcp.status, HACDBS_ERROR);
		break;
	}

	return IRQ_HANDLED;
}

void kvm_hacdbs_cpu_up(void)
{
	if (hacdbsirq < 0)
		return;

	enable_percpu_irq(hacdbsirq, IRQ_TYPE_LEVEL_HIGH);
	this_cpu_write(hacdbs_pcp.status, HACDBS_IDLE);
}

void kvm_hacdbs_cpu_down(void)
{
	if (hacdbsirq < 0)
		return;

	disable_percpu_irq(hacdbsirq);
	this_cpu_write(hacdbs_pcp.status, HACDBS_OFF);
}

#ifdef CONFIG_ACPI
static int __init hacdbs_acpi_get_irq(void)
{
	struct acpi_madt_generic_interrupt *gicc;
	u32 gsi;
	int irq;

	gicc = acpi_cpu_get_madt_gicc(smp_processor_id());
	if (gicc->header.length < ACPI_MADT_GICC_HACDBSIRQ)
		return -ENXIO;

	gsi =  gicc->hacdbsirq_gsi;

	irq = acpi_register_gsi(NULL, gsi, ACPI_LEVEL_SENSITIVE, ACPI_ACTIVE_HIGH);
	if (irq < 0) {
		pr_warn("ACPI: Unable to register HACDBS interrupt: %d\n", gsi);
		return -ENXIO;
	}

	return irq;
}
#else
#define hacdbs_acpi_get_irq() (-ENXIO)
#endif

void __init kvm_hacdbs_init(void)
{
	int irq;

	/* FEAT_HACDBS is only supported if Linux runs in EL2 (VHE) */
	if (!system_supports_hacdbs() || !is_kernel_in_hyp_mode())
		return;

	irq = hacdbs_acpi_get_irq();
	if (irq < 0)
		return;

	if (request_percpu_irq(irq, hacdbsirq_handler, "HACDBSIRQ", &hacdbs_pcp) < 0)
		return;

	hacdbsirq = irq;
}
