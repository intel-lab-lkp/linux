// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024-2025 ARM Limited, All Rights Reserved.
 */

#define pr_fmt(fmt)	"GICv5: " fmt

#include <linux/irqdomain.h>
#include <linux/wordpart.h>

#include <linux/irqchip.h>
#include <linux/irqchip/arm-gic-v5.h>

#include <asm/cpufeature.h>
#include <asm/exception.h>

static u8 pri_bits = 5;
#define GICV5_IRQ_PRI_MASK 0x1f
#define GICV5_IRQ_PRI_MI \
		(GICV5_IRQ_PRI_MASK & GENMASK(4, 5 - pri_bits))

#define PPI_NR	128

static bool gicv5_cpuif_has_gcie(void)
{
	return this_cpu_has_cap(ARM64_HAS_GICV5_CPUIF);
}

struct gicv5_chip_data {
	struct fwnode_handle	*fwnode;
	struct irq_domain	*ppi_domain;
};

static struct gicv5_chip_data gicv5_global_data __read_mostly;

static void gicv5_ppi_priority_init(void)
{
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRI_MI), SYS_ICC_PPI_PRIORITYR0_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRI_MI), SYS_ICC_PPI_PRIORITYR1_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRI_MI), SYS_ICC_PPI_PRIORITYR2_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRI_MI), SYS_ICC_PPI_PRIORITYR3_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRI_MI), SYS_ICC_PPI_PRIORITYR4_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRI_MI), SYS_ICC_PPI_PRIORITYR5_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRI_MI), SYS_ICC_PPI_PRIORITYR6_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRI_MI), SYS_ICC_PPI_PRIORITYR7_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRI_MI), SYS_ICC_PPI_PRIORITYR8_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRI_MI), SYS_ICC_PPI_PRIORITYR9_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRI_MI), SYS_ICC_PPI_PRIORITYR10_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRI_MI), SYS_ICC_PPI_PRIORITYR11_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRI_MI), SYS_ICC_PPI_PRIORITYR12_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRI_MI), SYS_ICC_PPI_PRIORITYR13_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRI_MI), SYS_ICC_PPI_PRIORITYR14_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRI_MI), SYS_ICC_PPI_PRIORITYR15_EL1);

	/*
	 * Context syncronization required to make sure system register writes
	 * effects are synchronised.
	 */
	isb();
}

static void gicv5_ppi_irq_mask(struct irq_data *d)
{
	u64 hwirq_id_bit = BIT_ULL(d->hwirq % 64);

	if (d->hwirq < 64)
		sysreg_clear_set_s(SYS_ICC_PPI_ENABLER0_EL1, hwirq_id_bit, 0);
	else
		sysreg_clear_set_s(SYS_ICC_PPI_ENABLER1_EL1, hwirq_id_bit, 0);

	/*
	 * We must ensure that the disable takes effect immediately to
	 * guarantee that the lazy-disabled IRQ mechanism works.
	 * A context synchronization event is required to guarantee it.
	 * Reference: I_ZLTKB/R_YRGMH GICv5 specification - section 2.9.1.
	 */
	isb();
}

static void gicv5_ppi_irq_unmask(struct irq_data *d)
{
	u64 hwirq_id_bit = BIT_ULL(d->hwirq % 64);

	if (d->hwirq < 64)
		sysreg_clear_set_s(SYS_ICC_PPI_ENABLER0_EL1, 0, hwirq_id_bit);
	else
		sysreg_clear_set_s(SYS_ICC_PPI_ENABLER1_EL1, 0, hwirq_id_bit);
	/*
	 * We must ensure that the enable takes effect in finite time - a
	 * context synchronization event is required to guarantee it, we
	 * can not take for granted that would happen (eg a core going straight
	 * into idle after enabling a PPI).
	 * Reference: I_ZLTKB/R_YRGMH GICv5 specification - section 2.9.1.
	 */
	isb();
}

static void gicv5_hwirq_eoi(u32 hwirq_id, u8 hwirq_type)
{
	u64 cddi = hwirq_id | FIELD_PREP(GICV5_GIC_CDDI_TYPE_MASK, hwirq_type);

	gic_insn(cddi, CDDI);

	gic_insn(0, CDEOI);
}

static void gicv5_ppi_irq_eoi(struct irq_data *d)
{
	gicv5_hwirq_eoi(d->hwirq, GICV5_HWIRQ_TYPE_PPI);
}

#define READ_PPI_REG(irq, reg)							\
	({									\
		u64 __ppi_val;							\
										\
		if (irq < 64)							\
			__ppi_val = read_sysreg_s(SYS_ICC_PPI_##reg##R0_EL1);	\
		else								\
			__ppi_val = read_sysreg_s(SYS_ICC_PPI_##reg##R1_EL1);	\
		__ppi_val;							\
	})

#define WRITE_PPI_REG(set, irq, bit, reg)					\
	do {									\
		if (set) {							\
			if (irq < 64)						\
				write_sysreg_s(bit, SYS_ICC_PPI_S##reg##R0_EL1);\
			else							\
				write_sysreg_s(bit, SYS_ICC_PPI_S##reg##R1_EL1);\
		} else {							\
			if (irq < 64)						\
				write_sysreg_s(bit, SYS_ICC_PPI_C##reg##R0_EL1);\
			else							\
				write_sysreg_s(bit, SYS_ICC_PPI_C##reg##R1_EL1);\
		}								\
	} while (0)

static int gicv5_ppi_set_type(struct irq_data *d, unsigned int type)
{
	/*
	 * The PPI trigger mode is not configurable at runtime,
	 * therefore this function simply confirms that the `type`
	 * parameter matches what is present.
	 */
	u64 hmr = READ_PPI_REG(d->hwirq, HM);

	switch (type) {
	case IRQ_TYPE_LEVEL_HIGH:
	case IRQ_TYPE_LEVEL_LOW:
		if (((hmr >> (d->hwirq % 64)) & 0x1) != GICV5_PPI_HM_LEVEL)
			return -EINVAL;
		break;
	case IRQ_TYPE_EDGE_RISING:
	case IRQ_TYPE_EDGE_FALLING:
		if (((hmr >> (d->hwirq % 64)) & 0x1) != GICV5_PPI_HM_EDGE)
			return -EINVAL;
		break;
	default:
		pr_debug("Unexpected PPI trigger mode");
		return -EINVAL;
	}

	return 0;
}

static int gicv5_ppi_irq_get_irqchip_state(struct irq_data *d,
					   enum irqchip_irq_state which,
					   bool *val)
{
	u64 pendr, activer, hwirq_id_bit = BIT_ULL(d->hwirq % 64);

	switch (which) {
	case IRQCHIP_STATE_PENDING:
		pendr = READ_PPI_REG(d->hwirq, SPEND);

		*val = !!(pendr & hwirq_id_bit);

		return 0;
	case IRQCHIP_STATE_ACTIVE:
		activer = READ_PPI_REG(d->hwirq, SACTIVE);

		*val = !!(activer & hwirq_id_bit);

		return 0;
	default:
		pr_debug("Unexpected PPI irqchip state\n");
	}

	return -EINVAL;
}

static int gicv5_ppi_irq_set_irqchip_state(struct irq_data *d,
					   enum irqchip_irq_state which,
					   bool val)
{
	u64 hwirq_id_bit = BIT_ULL(d->hwirq % 64);

	switch (which) {
	case IRQCHIP_STATE_PENDING:
		WRITE_PPI_REG(val, d->hwirq, hwirq_id_bit, PEND);
		return 0;
	case IRQCHIP_STATE_ACTIVE:
		WRITE_PPI_REG(val, d->hwirq, hwirq_id_bit, ACTIVE);
		return 0;
	default:
		pr_debug("Unexpected PPI irqchip state\n");
	}

	return -EINVAL;
}

static const struct irq_chip gicv5_ppi_irq_chip = {
	.name			= "GICv5-PPI",
	.irq_mask		= gicv5_ppi_irq_mask,
	.irq_unmask		= gicv5_ppi_irq_unmask,
	.irq_eoi		= gicv5_ppi_irq_eoi,
	.irq_set_type		= gicv5_ppi_set_type,
	.irq_get_irqchip_state	= gicv5_ppi_irq_get_irqchip_state,
	.irq_set_irqchip_state	= gicv5_ppi_irq_set_irqchip_state,
	.flags			= IRQCHIP_SET_TYPE_MASKED |
				  IRQCHIP_SKIP_SET_WAKE	  |
				  IRQCHIP_MASK_ON_SUSPEND
};

static int gicv5_irq_ppi_domain_translate(struct irq_domain *d,
				      struct irq_fwspec *fwspec,
				      irq_hw_number_t *hwirq,
				      unsigned int *type)
{
	if (!is_of_node(fwspec->fwnode))
		return -EINVAL;

	if (fwspec->param_count < 3)
		return -EINVAL;

	if (fwspec->param[0] != GICV5_HWIRQ_TYPE_PPI)
		return -EINVAL;

	*hwirq = fwspec->param[1];
	*type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;

	return 0;
}

static int gicv5_irq_ppi_domain_alloc(struct irq_domain *domain, unsigned int virq,
				      unsigned int nr_irqs, void *arg)
{
	unsigned int type = IRQ_TYPE_NONE;
	struct irq_fwspec *fwspec = arg;
	irq_hw_number_t hwirq;
	int ret;

	if (WARN_ON_ONCE(nr_irqs != 1))
		return -EINVAL;

	ret = gicv5_irq_ppi_domain_translate(domain, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	irq_set_percpu_devid(virq);
	irq_domain_set_info(domain, virq, hwirq, &gicv5_ppi_irq_chip, NULL,
			    handle_percpu_devid_irq, NULL, NULL);

	return 0;
}

static void gicv5_irq_domain_free(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs)
{
	struct irq_data *d;

	if (WARN_ON_ONCE(nr_irqs != 1))
		return;

	d = irq_domain_get_irq_data(domain, virq);

	irq_set_handler(virq, NULL);
	irq_domain_reset_irq_data(d);
}

static int gicv5_irq_ppi_domain_select(struct irq_domain *d, struct irq_fwspec *fwspec,
				       enum irq_domain_bus_token bus_token)
{
	if (fwspec->fwnode != d->fwnode)
		return 0;

	if (fwspec->param[0] != GICV5_HWIRQ_TYPE_PPI)
		return 0;

	return (d == gicv5_global_data.ppi_domain);
}

static const struct irq_domain_ops gicv5_irq_ppi_domain_ops = {
	.translate	= gicv5_irq_ppi_domain_translate,
	.alloc		= gicv5_irq_ppi_domain_alloc,
	.free		= gicv5_irq_domain_free,
	.select		= gicv5_irq_ppi_domain_select
};

static void handle_irq_per_domain(u32 hwirq)
{
	u8 hwirq_type = FIELD_GET(GICV5_HWIRQ_TYPE, hwirq);
	u32 hwirq_id = FIELD_GET(GICV5_HWIRQ_ID, hwirq);
	struct irq_domain *domain;

	switch (hwirq_type) {
	case GICV5_HWIRQ_TYPE_PPI:
		domain = gicv5_global_data.ppi_domain;
		break;
	default:
		pr_err_once("Unknown IRQ type, bail out\n");
		return;
	}

	if (generic_handle_domain_irq(domain, hwirq_id)) {
		pr_err_once("Could not handle, hwirq = 0x%x", hwirq_id);
		gicv5_hwirq_eoi(hwirq_id, hwirq_type);
	}
}

static void __exception_irq_entry gicv5_handle_irq(struct pt_regs *regs)
{
	bool valid;
	u32 hwirq;
	u64 ia;

	ia = gicr_insn(CDIA);
	valid = GICV5_GIC_CDIA_VALID(ia);

	if (!valid)
		return;

	/*
	 * Ensure that the CDIA instruction effects (ie IRQ activation) are
	 * completed before handling the interrupt.
	 */
	gsb_ack();

	/*
	 * Ensure instruction ordering between an acknowledgment and subsequent
	 * instructions in the IRQ handler using an ISB.
	 */
	isb();

	hwirq = FIELD_GET(GICV5_HWIRQ_INTID, ia);

	handle_irq_per_domain(hwirq);
}

static void gicv5_cpu_disable_interrupts(void)
{
	u64 cr0;

	cr0 = FIELD_PREP(ICC_CR0_EL1_EN, 0);
	write_sysreg_s(cr0, SYS_ICC_CR0_EL1);
}

static void gicv5_cpu_enable_interrupts(void)
{
	u64 cr0, pcr;

	write_sysreg_s(0, SYS_ICC_PPI_ENABLER0_EL1);
	write_sysreg_s(0, SYS_ICC_PPI_ENABLER1_EL1);

	gicv5_ppi_priority_init();

	pcr = FIELD_PREP(ICC_PCR_EL1_PRIORITY, GICV5_IRQ_PRI_MI);
	write_sysreg_s(pcr, SYS_ICC_PCR_EL1);

	cr0 = FIELD_PREP(ICC_CR0_EL1_EN, 1);
	write_sysreg_s(cr0, SYS_ICC_CR0_EL1);
}

static int gicv5_starting_cpu(unsigned int cpu)
{
	if (WARN(!gicv5_cpuif_has_gcie(),
	    "GICv5 system components present but CPU does not have FEAT_GCIE"))
		return -ENODEV;

	gicv5_cpu_enable_interrupts();

	return 0;
}

static void __init gicv5_free_domains(void)
{
	if (gicv5_global_data.ppi_domain)
		irq_domain_remove(gicv5_global_data.ppi_domain);

	gicv5_global_data.ppi_domain = NULL;
}

static int __init gicv5_init_domains(struct fwnode_handle *handle)
{
	struct irq_domain *d;

	d = irq_domain_create_linear(handle, PPI_NR, &gicv5_irq_ppi_domain_ops,
				     NULL);
	if (!d)
		return -ENOMEM;

	irq_domain_update_bus_token(d, DOMAIN_BUS_WIRED);
	gicv5_global_data.ppi_domain = d;

	gicv5_global_data.fwnode = handle;

	return 0;
}

static void gicv5_set_cpuif_pribits(void)
{
	u64 icc_idr0 = read_sysreg_s(SYS_ICC_IDR0_EL1);

	switch (FIELD_GET(ICC_IDR0_EL1_PRI_BITS, icc_idr0)) {
	case ICC_IDR0_EL1_PRI_BITS_4BITS:
		pri_bits = 4;
		break;
	case ICC_IDR0_EL1_PRI_BITS_5BITS:
		pri_bits = 5;
		break;
	default:
		pr_err("Unexpected ICC_IDR0_EL1_PRI_BITS value, default to 4");
		pri_bits = 4;
		break;
	}
}

static int __init gicv5_of_init(struct device_node *node, struct device_node *parent)
{
	int ret;

	ret = gicv5_init_domains(&node->fwnode);
	if (ret)
		return ret;

	gicv5_set_cpuif_pribits();

	ret = gicv5_starting_cpu(smp_processor_id());
	if (ret)
		goto out_dom;

	ret = set_handle_irq(gicv5_handle_irq);
	if (ret)
		goto out_int;

	return 0;
out_int:
	gicv5_cpu_disable_interrupts();
out_dom:
	gicv5_free_domains();

	return ret;
}
IRQCHIP_DECLARE(gic_v5, "arm,gic-v5", gicv5_of_init);
