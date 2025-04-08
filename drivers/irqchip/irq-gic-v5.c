// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024-2025 ARM Limited, All Rights Reserved.
 */

#define pr_fmt(fmt)	"GICv5: " fmt

#include <linux/cpuhotplug.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/maple_tree.h>
#include <linux/slab.h>
#include <linux/wordpart.h>

#include <asm/cpufeature.h>
#include <asm/exception.h>

#include "irq-gic-v5.h"

static u8 pri_bits = 5;
#define GICV5_IRQ_PRIORITY_MASK 0x1f
#define GICV5_IRQ_PRIORITY_MI \
		(GICV5_IRQ_PRIORITY_MASK & GENMASK(4, 5 - pri_bits))

static bool gicv5_cpuif_has_gcie(void)
{
	return this_cpu_has_cap(ARM64_HAS_GCIE);
}

struct gicv5_chip_data gicv5_global_data __read_mostly;

static struct maple_tree lpi_mt;
static u32 num_lpis;

void __init gicv5_init_lpi(u32 lpis)
{
	mt_init_flags(&lpi_mt, MT_FLAGS_ALLOC_RANGE);
	num_lpis = lpis;
}

void __init gicv5_free_lpi(void)
{
	mtree_destroy(&lpi_mt);
	num_lpis = 0;
}

#define MT_ENTRY	((void *)&lpi_mt) /* Unused - just a valid pointer */

static int alloc_lpi_range(u32 lpis, u32 *base)
{
	int ret;
	void *entry;
	unsigned long lpi_base, startp, lastp;

	MA_STATE(mas, &lpi_mt, 0, 0);

	if (!num_lpis)
		return -ENODEV;

	mtree_lock(&lpi_mt);
	ret = mas_empty_area(&mas, 0, num_lpis - 1, lpis);
	if (ret) {
		pr_err("Failed to perform a dynamic alloc in the LPI MT!\n");
		return ret;
	}

	lpi_base = mas.index;

	/*
	 * We don't really care about the entry itself, only about
	 * allocation of a maple tree ranges describing in use LPIs.
	 * That's why, upon allocation, we try to merge slots adjacent
	 * with the empty one we are allocating to minimize the number
	 * of slots we take from maple tree nodes for nothing, all
	 * we need to keep track of is in use ranges.
	 */
	startp = mas.index;
	lastp = mas.last;

	entry = mas_next(&mas, num_lpis - 1);
	if (entry && mas.index == lastp + 1)
		lastp = mas.last;

	entry = mas_prev(&mas, 0);
	if (entry)
		startp = mas.index;
	mas_set_range(&mas, startp, lastp);
	mas_store_gfp(&mas, MT_ENTRY, GFP_KERNEL);
	mtree_unlock(&lpi_mt);

	// startp is the index at which we allocated, i.e. the base LPI.
	*base = lpi_base;

	return 0;
}

// Drop entries between min and max (inclusive)
static int release_lpi_range(u32 min, u32 max)
{
	return mtree_store_range(&lpi_mt, min, max, NULL, GFP_KERNEL);
}

static int gicv5_alloc_lpi_range(u32 nr_lpis, u32 *base)
{
	return alloc_lpi_range(nr_lpis, base);
}

static int gicv5_free_lpi_range(u32 base, u32 nr_lpis)
{
	if (nr_lpis < 1)
		return -EINVAL;

	return release_lpi_range(base, base + nr_lpis - 1);
}

static void gicv5_ppi_priority_init(void)
{
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRIORITY_MI),
				 SYS_ICC_PPI_PRIORITYR0_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRIORITY_MI),
				 SYS_ICC_PPI_PRIORITYR1_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRIORITY_MI),
				 SYS_ICC_PPI_PRIORITYR2_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRIORITY_MI),
				 SYS_ICC_PPI_PRIORITYR3_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRIORITY_MI),
				 SYS_ICC_PPI_PRIORITYR4_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRIORITY_MI),
				 SYS_ICC_PPI_PRIORITYR5_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRIORITY_MI),
				 SYS_ICC_PPI_PRIORITYR6_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRIORITY_MI),
				 SYS_ICC_PPI_PRIORITYR7_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRIORITY_MI),
				 SYS_ICC_PPI_PRIORITYR8_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRIORITY_MI),
				 SYS_ICC_PPI_PRIORITYR9_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRIORITY_MI),
				 SYS_ICC_PPI_PRIORITYR10_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRIORITY_MI),
				 SYS_ICC_PPI_PRIORITYR11_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRIORITY_MI),
				 SYS_ICC_PPI_PRIORITYR12_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRIORITY_MI),
				 SYS_ICC_PPI_PRIORITYR13_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRIORITY_MI),
				 SYS_ICC_PPI_PRIORITYR14_EL1);
	write_sysreg_s(REPEAT_BYTE(GICV5_IRQ_PRIORITY_MI),
				 SYS_ICC_PPI_PRIORITYR15_EL1);

	/*
	 * Context syncronization required to make sure system
	 * register writes effects are synchronized
	 */
	isb();
}

static void gicv5_hwirq_init(irq_hw_number_t hwirq, u8 priority, u8 hwirq_type)
{
	u64 cdpri, cdaff;
	u16 iaffid;
	int ret;

	if (hwirq_type == GICV5_HWIRQ_TYPE_LPI ||
	    hwirq_type == GICV5_HWIRQ_TYPE_SPI) {
		cdpri = FIELD_PREP(GICV5_GIC_CDPRI_PRIORITY_MASK, priority)	|
			FIELD_PREP(GICV5_GIC_CDPRI_TYPE_MASK, hwirq_type)	|
			FIELD_PREP(GICV5_GIC_CDPRI_ID_MASK, hwirq);
		gic_insn(cdpri, GICV5_OP_GIC_CDPRI);

		ret = gicv5_irs_cpu_to_iaffid(smp_processor_id(), &iaffid);

		if (WARN_ON(ret))
			return;

		cdaff = FIELD_PREP(GICV5_GIC_CDAFF_IAFFID_MASK, iaffid)		|
			FIELD_PREP(GICV5_GIC_CDAFF_TYPE_MASK, hwirq_type)	|
			FIELD_PREP(GICV5_GIC_CDAFF_ID_MASK, hwirq);
		gic_insn(cdaff, GICV5_OP_GIC_CDAFF);
	}
}

static void gicv5_ppi_irq_mask(struct irq_data *d)
{
	u64 hwirq_id_bit = BIT_ULL(d->hwirq % 64);

	if (d->hwirq < 64)
		sysreg_clear_set_s(SYS_ICC_PPI_ENABLER0_EL1, hwirq_id_bit, 0);
	else
		sysreg_clear_set_s(SYS_ICC_PPI_ENABLER1_EL1, hwirq_id_bit, 0);

	/*
	 * Ensure that the disable takes effect
	 */
	isb();
}

static void gicv5_iri_irq_mask(struct irq_data *d, u8 hwirq_type)
{
	u64 cddis = d->hwirq | FIELD_PREP(GICV5_GIC_CDDIS_TYPE_MASK, hwirq_type);

	gic_insn(cddis, GICV5_OP_GIC_CDDIS);
	/*
	 * We must make sure that GIC CDDIS write effects are propagated
	 */
	gsb_sys();
}

static void gicv5_spi_irq_mask(struct irq_data *d)
{
	gicv5_iri_irq_mask(d, GICV5_HWIRQ_TYPE_SPI);
}

static void gicv5_lpi_irq_mask(struct irq_data *d)
{
	gicv5_iri_irq_mask(d, GICV5_HWIRQ_TYPE_LPI);
}

static void gicv5_ppi_irq_unmask(struct irq_data *d)
{
	u64 hwirq_id_bit = BIT_ULL(d->hwirq % 64);

	if (d->hwirq < 64)
		sysreg_clear_set_s(SYS_ICC_PPI_ENABLER0_EL1, 0, hwirq_id_bit);
	else
		sysreg_clear_set_s(SYS_ICC_PPI_ENABLER1_EL1, 0, hwirq_id_bit);
}

static void gicv5_iri_irq_unmask(struct irq_data *d, u8 hwirq_type)
{
	u64 cden = d->hwirq | FIELD_PREP(GICV5_GIC_CDEN_TYPE_MASK, hwirq_type);

	gic_insn(cden, GICV5_OP_GIC_CDEN);
}

static void gicv5_spi_irq_unmask(struct irq_data *d)
{
	gicv5_iri_irq_unmask(d, GICV5_HWIRQ_TYPE_SPI);
}

static void gicv5_lpi_irq_unmask(struct irq_data *d)
{
	gicv5_iri_irq_unmask(d, GICV5_HWIRQ_TYPE_LPI);
}

static void gicv5_hwirq_eoi(u32 hwirq_id, u8 hwirq_type)
{
	u64 cddi = hwirq_id | FIELD_PREP(GICV5_GIC_CDDI_TYPE_MASK, hwirq_type);

	gic_insn(cddi, GICV5_OP_GIC_CDDI);

	gic_insn(0, GICV5_OP_GIC_CDEOI);
}

static void gicv5_ppi_irq_eoi(struct irq_data *d)
{
	gicv5_hwirq_eoi(d->hwirq, GICV5_HWIRQ_TYPE_PPI);
}

static void gicv5_spi_irq_eoi(struct irq_data *d)
{
	gicv5_hwirq_eoi(d->hwirq, GICV5_HWIRQ_TYPE_SPI);
}

static void gicv5_lpi_irq_eoi(struct irq_data *d)
{
	gicv5_hwirq_eoi(d->hwirq, GICV5_HWIRQ_TYPE_LPI);
}

static int gicv5_ppi_set_type(struct irq_data *d, unsigned int type)
{
	/*
	 * The PPI trigger mode is not configurable at runtime,
	 * therefore this function simply confirms that the `type`
	 * parameter matches what is present.
	 */
	u64 hmr;

	if (d->hwirq < 64)
		hmr = read_sysreg_s(SYS_ICC_PPI_HMR0_EL1);
	else
		hmr = read_sysreg_s(SYS_ICC_PPI_HMR1_EL1);

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

static int gicv5_iri_irq_set_affinity(struct irq_data *d,
				      const struct cpumask *mask_val,
				      bool force, u8 hwirq_type)
{
	u16 iaffid;
	u64 cdaff;
	int ret, cpuid;

	if (force)
		cpuid = cpumask_first(mask_val);
	else
		cpuid = cpumask_any_and(mask_val, cpu_online_mask);

	ret = gicv5_irs_cpu_to_iaffid(cpuid, &iaffid);
	if (ret)
		return ret;

	cdaff = FIELD_PREP(GICV5_GIC_CDAFF_IAFFID_MASK, iaffid)		|
		FIELD_PREP(GICV5_GIC_CDAFF_TYPE_MASK, hwirq_type)	|
		FIELD_PREP(GICV5_GIC_CDAFF_ID_MASK, d->hwirq);
	gic_insn(cdaff, GICV5_OP_GIC_CDAFF);

	irq_data_update_effective_affinity(d, cpumask_of(cpuid));

	return IRQ_SET_MASK_OK_DONE;
}

static int gicv5_spi_irq_set_affinity(struct irq_data *d,
				      const struct cpumask *mask_val,
				      bool force)
{
	return gicv5_iri_irq_set_affinity(d, mask_val, force,
					  GICV5_HWIRQ_TYPE_SPI);
}

static int gicv5_lpi_irq_set_affinity(struct irq_data *d,
				      const struct cpumask *mask_val,
				      bool force)
{
	return gicv5_iri_irq_set_affinity(d, mask_val, force,
					  GICV5_HWIRQ_TYPE_LPI);
}

static int gicv5_ppi_irq_get_irqchip_state(struct irq_data *d,
					   enum irqchip_irq_state which,
					   bool *val)
{
	u64 pendr, activer, enabler, hwirq_id_bit = BIT_ULL(d->hwirq % 64);

	switch (which) {
	case IRQCHIP_STATE_PENDING:
		if (d->hwirq < 64)
			pendr = read_sysreg_s(SYS_ICC_PPI_SPENDR0_EL1);
		else
			pendr = read_sysreg_s(SYS_ICC_PPI_SPENDR1_EL1);

		*val = !!(pendr & hwirq_id_bit);

		return 0;
	case IRQCHIP_STATE_ACTIVE:
		if (d->hwirq < 64)
			activer = read_sysreg_s(SYS_ICC_PPI_SACTIVER0_EL1);
		else
			activer = read_sysreg_s(SYS_ICC_PPI_SACTIVER1_EL1);

		*val = !!(activer & hwirq_id_bit);

		return 0;
	case IRQCHIP_STATE_MASKED:
		if (d->hwirq < 64)
			enabler = read_sysreg_s(SYS_ICC_PPI_ENABLER0_EL1);
		else
			enabler = read_sysreg_s(SYS_ICC_PPI_ENABLER1_EL1);

		*val = !(enabler & hwirq_id_bit);

		return 0;
	default:
		pr_debug("Unexpected PPI irqchip state\n");
		return -EINVAL;
	}

	return -EINVAL;
}

static int gicv5_iri_irq_get_irqchip_state(struct irq_data *d,
					   enum irqchip_irq_state which,
					   bool *val, u8 hwirq_type)
{
	u64 icsr, cdrcfg = d->hwirq | FIELD_PREP(GICV5_GIC_CDRCFG_TYPE_MASK,
						 hwirq_type);

	preempt_disable();
	gic_insn(cdrcfg, GICV5_OP_GIC_CDRCFG);
	isb();
	icsr = read_sysreg_s(SYS_ICC_ICSR_EL1);
	preempt_enable();

	if (FIELD_GET(ICC_ICSR_EL1_F, icsr)) {
		pr_err("ICSR_EL1 is invalid\n");
		return -EINVAL;
	}

	switch (which) {
	case IRQCHIP_STATE_PENDING:
		*val = !!(FIELD_GET(ICC_ICSR_EL1_Pending, icsr));
		return 0;

	case IRQCHIP_STATE_ACTIVE:
		*val = !!(FIELD_GET(ICC_ICSR_EL1_Active, icsr));
		return 0;

	case IRQCHIP_STATE_MASKED:
		*val = !(FIELD_GET(ICC_ICSR_EL1_Enabled, icsr));
		return 0;

	default:
		pr_debug("Unexpected irqchip_irq_state\n");
		return -EINVAL;
	}

	return -EINVAL;
}

static int gicv5_spi_irq_get_irqchip_state(struct irq_data *d,
					   enum irqchip_irq_state which,
					   bool *val)
{
	return gicv5_iri_irq_get_irqchip_state(d, which, val,
					       GICV5_HWIRQ_TYPE_SPI);
}

static int gicv5_lpi_irq_get_irqchip_state(struct irq_data *d,
					   enum irqchip_irq_state which,
					   bool *val)
{
	return gicv5_iri_irq_get_irqchip_state(d, which, val,
					       GICV5_HWIRQ_TYPE_LPI);
}

static int gicv5_ppi_irq_set_irqchip_state(struct irq_data *d,
					   enum irqchip_irq_state which,
					   bool val)
{
	u64 hwirq_id_bit = BIT_ULL(d->hwirq % 64);

	switch (which) {
	case IRQCHIP_STATE_PENDING:
		if (val) {
			if (d->hwirq < 64)
				write_sysreg_s(hwirq_id_bit,
					       SYS_ICC_PPI_SPENDR0_EL1);
			else
				write_sysreg_s(hwirq_id_bit,
					       SYS_ICC_PPI_SPENDR1_EL1);

		} else {
			if (d->hwirq < 64)
				write_sysreg_s(hwirq_id_bit,
					       SYS_ICC_PPI_CPENDR0_EL1);
			else
				write_sysreg_s(hwirq_id_bit,
					       SYS_ICC_PPI_CPENDR1_EL1);
		}

		return 0;
	case IRQCHIP_STATE_ACTIVE:
		if (val) {
			if (d->hwirq < 64)
				write_sysreg_s(hwirq_id_bit,
					       SYS_ICC_PPI_SACTIVER0_EL1);
			else
				write_sysreg_s(hwirq_id_bit,
					       SYS_ICC_PPI_SACTIVER1_EL1);
		} else {
			if (d->hwirq < 64)
				write_sysreg_s(hwirq_id_bit,
					       SYS_ICC_PPI_CACTIVER0_EL1);
			else
				write_sysreg_s(hwirq_id_bit,
					       SYS_ICC_PPI_CACTIVER1_EL1);
		}

		return 0;
	case IRQCHIP_STATE_MASKED:
		if (val)
			gicv5_ppi_irq_mask(d);
		else
			gicv5_ppi_irq_unmask(d);
		return 0;
	default:
		pr_debug("Unexpected PPI irqchip state\n");
		return -EINVAL;
	}

	return -EINVAL;
}

static void gicv5_iri_irq_write_pending_state(struct irq_data *d, bool val,
					      u8 hwirq_type)
{
	u64 cdpend;

	cdpend = FIELD_PREP(GICV5_GIC_CDPEND_TYPE_MASK, hwirq_type)	|
		 FIELD_PREP(GICV5_GIC_CDPEND_ID_MASK, d->hwirq)		|
		 FIELD_PREP(GICV5_GIC_CDPEND_PENDING_MASK, val);

	gic_insn(cdpend, GICV5_OP_GIC_CDPEND);
}

static void gicv5_spi_irq_write_pending_state(struct irq_data *d, bool val)
{
	gicv5_iri_irq_write_pending_state(d, val, GICV5_HWIRQ_TYPE_SPI);
}

static void gicv5_lpi_irq_write_pending_state(struct irq_data *d, bool val)
{
	gicv5_iri_irq_write_pending_state(d, val, GICV5_HWIRQ_TYPE_LPI);
}

static int gicv5_spi_irq_set_irqchip_state(struct irq_data *d,
					   enum irqchip_irq_state which,
					   bool val)
{
	switch (which) {
	case IRQCHIP_STATE_PENDING:
		gicv5_spi_irq_write_pending_state(d, val);
		break;
	case IRQCHIP_STATE_MASKED:
		if (val)
			gicv5_spi_irq_mask(d);
		else
			gicv5_spi_irq_unmask(d);
		break;
	default:
		pr_debug("Unexpected irqchip_irq_state\n");
		return -EINVAL;
	}

	return 0;
}

static int gicv5_lpi_irq_set_irqchip_state(struct irq_data *d,
					   enum irqchip_irq_state which,
					   bool val)
{
	switch (which) {
	case IRQCHIP_STATE_PENDING:
		gicv5_lpi_irq_write_pending_state(d, val);
		break;
	case IRQCHIP_STATE_MASKED:
		if (val)
			gicv5_lpi_irq_mask(d);
		else
			gicv5_lpi_irq_unmask(d);
		break;

	default:
		pr_debug("Unexpected irqchip_irq_state\n");
		return -EINVAL;
	}

	return 0;
}

static int gicv5_spi_irq_retrigger(struct irq_data *data)
{
	return !gicv5_spi_irq_set_irqchip_state(data, IRQCHIP_STATE_PENDING,
						true);
}

static int gicv5_lpi_irq_retrigger(struct irq_data *data)
{
	return !gicv5_lpi_irq_set_irqchip_state(data, IRQCHIP_STATE_PENDING,
						true);
}

static void gicv5_ipi_send_single(struct irq_data *d, unsigned int cpu)
{
	/* Mark the LPI pending */
	irq_chip_retrigger_hierarchy(d);
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

static const struct irq_chip gicv5_spi_irq_chip = {
	.name			= "GICv5-SPI",
	.irq_mask		= gicv5_spi_irq_mask,
	.irq_unmask		= gicv5_spi_irq_unmask,
	.irq_eoi		= gicv5_spi_irq_eoi,
	.irq_set_type		= gicv5_spi_irq_set_type,
	.irq_set_affinity	= gicv5_spi_irq_set_affinity,
	.irq_retrigger		= gicv5_spi_irq_retrigger,
	.irq_get_irqchip_state	= gicv5_spi_irq_get_irqchip_state,
	.irq_set_irqchip_state	= gicv5_spi_irq_set_irqchip_state,
	.flags			= IRQCHIP_SET_TYPE_MASKED |
				  IRQCHIP_SKIP_SET_WAKE	  |
				  IRQCHIP_MASK_ON_SUSPEND
};

static const struct irq_chip gicv5_lpi_irq_chip = {
	.name			= "GICv5-LPI",
	.irq_mask		= gicv5_lpi_irq_mask,
	.irq_unmask		= gicv5_lpi_irq_unmask,
	.irq_eoi		= gicv5_lpi_irq_eoi,
	.irq_set_affinity	= gicv5_lpi_irq_set_affinity,
	.irq_retrigger		= gicv5_lpi_irq_retrigger,
	.irq_get_irqchip_state	= gicv5_lpi_irq_get_irqchip_state,
	.irq_set_irqchip_state	= gicv5_lpi_irq_set_irqchip_state,
	.flags			= IRQCHIP_SET_TYPE_MASKED |
				  IRQCHIP_SKIP_SET_WAKE	  |
				  IRQCHIP_MASK_ON_SUSPEND
};

static const struct irq_chip gicv5_ipi_irq_chip = {
	.name			= "GICv5-IPI",
	.irq_mask		= irq_chip_mask_parent,
	.irq_unmask		= irq_chip_unmask_parent,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_set_affinity	= irq_chip_set_affinity_parent,
	.irq_get_irqchip_state	= irq_chip_get_parent_state,
	.irq_set_irqchip_state	= irq_chip_set_parent_state,
	// We only handle this one in the IPI domain - the rest go to parent
	.ipi_send_single	= gicv5_ipi_send_single,
	.flags			= IRQCHIP_SET_TYPE_MASKED |
				  IRQCHIP_SKIP_SET_WAKE	  |
				  IRQCHIP_MASK_ON_SUSPEND
};

static int gicv5_irq_ppi_domain_translate(struct irq_domain *d,
					  struct irq_fwspec *fwspec,
					  irq_hw_number_t *hwirq,
					  unsigned int *type)
{
	if (is_of_node(fwspec->fwnode)) {
		if (fwspec->param_count < 3)
			return -EINVAL;

		if (fwspec->param[0] != GICV5_HWIRQ_TYPE_PPI)
			return -EINVAL;

		*hwirq = fwspec->param[1];
		*type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;

		return 0;
	}

	return -EINVAL;
}

static int gicv5_irq_ppi_domain_alloc(struct irq_domain *domain,
				      unsigned int virq, unsigned int nr_irqs,
				      void *arg)
{
	unsigned int type = IRQ_TYPE_NONE;
	struct irq_fwspec *fwspec = arg;
	irq_hw_number_t hwirq;
	int ret;

	if (WARN_ON(nr_irqs != 1))
		return -EINVAL;

	ret = gicv5_irq_ppi_domain_translate(domain, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	irq_set_percpu_devid(virq);
	irq_domain_set_info(domain, virq, hwirq, &gicv5_ppi_irq_chip, NULL,
			    handle_percpu_devid_irq, NULL, NULL);

	return 0;
}

static void gicv5_irq_domain_free(struct irq_domain *domain,
				  unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *d;

	if (WARN_ON(nr_irqs != 1))
		return;

	d = irq_domain_get_irq_data(domain, virq);

	irq_set_handler(virq, NULL);
	irq_domain_reset_irq_data(d);
}

static int gicv5_irq_ppi_domain_select(struct irq_domain *d,
				       struct irq_fwspec *fwspec,
				       enum irq_domain_bus_token bus_token)
{
	/* Not for us */
	if (fwspec->fwnode != d->fwnode)
		return 0;

	if (fwspec->param[0] != GICV5_HWIRQ_TYPE_PPI) {
		// only handle PPIs
		return 0;
	}

	return (d == gicv5_global_data.ppi_domain);
}

static const struct irq_domain_ops gicv5_irq_ppi_domain_ops = {
	.translate	= gicv5_irq_ppi_domain_translate,
	.alloc		= gicv5_irq_ppi_domain_alloc,
	.free		= gicv5_irq_domain_free,
	.select		= gicv5_irq_ppi_domain_select
};

static int gicv5_irq_spi_domain_translate(struct irq_domain *d,
					  struct irq_fwspec *fwspec,
					  irq_hw_number_t *hwirq,
					  unsigned int *type)
{
	if (is_of_node(fwspec->fwnode)) {
		if (fwspec->param_count < 3)
			return -EINVAL;

		if (fwspec->param[0] != GICV5_HWIRQ_TYPE_SPI)
			return -EINVAL;

		*hwirq = fwspec->param[1];
		*type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;

		return 0;
	}

	return -EINVAL;
}

static int gicv5_irq_spi_domain_alloc(struct irq_domain *domain,
				      unsigned int virq, unsigned int nr_irqs,
				      void *arg)
{
	struct gicv5_irs_chip_data *chip_data;
	struct irq_data *irqd;
	unsigned int type = IRQ_TYPE_NONE;
	struct irq_fwspec *fwspec = arg;
	irq_hw_number_t hwirq;
	int ret;

	if (WARN_ON(nr_irqs != 1))
		return -EINVAL;

	ret = gicv5_irq_spi_domain_translate(domain, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	irqd = irq_desc_get_irq_data(irq_to_desc(virq));
	chip_data = gicv5_irs_lookup_by_spi_id(hwirq);

	irq_domain_set_info(domain, virq, hwirq, &gicv5_spi_irq_chip, chip_data,
		handle_fasteoi_irq, NULL, NULL);
	irq_set_probe(virq);
	irqd_set_single_target(irqd);

	gicv5_hwirq_init(hwirq, GICV5_IRQ_PRIORITY_MI, GICV5_HWIRQ_TYPE_SPI);

	return 0;
}

static int gicv5_irq_spi_domain_select(struct irq_domain *d,
				       struct irq_fwspec *fwspec,
				       enum irq_domain_bus_token bus_token)
{
	if (fwspec->fwnode != d->fwnode)
		return 0;

	if (fwspec->param[0] != GICV5_HWIRQ_TYPE_SPI) {
		// only handle SPIs
		return 0;
	}

	return (d == gicv5_global_data.spi_domain);
}

static const struct irq_domain_ops gicv5_irq_spi_domain_ops = {
	.translate	= gicv5_irq_spi_domain_translate,
	.alloc		= gicv5_irq_spi_domain_alloc,
	.free		= gicv5_irq_domain_free,
	.select		= gicv5_irq_spi_domain_select
};

static int gicv5_irq_lpi_domain_alloc(struct irq_domain *domain,
				      unsigned int virq, unsigned int nr_irqs,
				      void *arg)
{
	irq_hw_number_t hwirq;
	struct irq_data *irqd;
	u32 *base_lpi = arg;
	int i, ret;

	hwirq = *base_lpi;

	for (i = 0; i < nr_irqs; i++) {
		irqd = irq_desc_get_irq_data(irq_to_desc(virq + i));

		irq_domain_set_info(domain, virq + i, hwirq + i,
				    &gicv5_lpi_irq_chip, NULL,
				    handle_fasteoi_irq, NULL, NULL);
		irqd_set_single_target(irqd);

		ret = gicv5_irs_iste_alloc(hwirq + i);
		if (ret < 0)
			return ret;

		gicv5_hwirq_init((hwirq + i), GICV5_IRQ_PRIORITY_MI,
					      GICV5_HWIRQ_TYPE_LPI);
	}

	return 0;
}

static void gicv5_irq_lpi_domain_free(struct irq_domain *domain,
				      unsigned int virq, unsigned int nr_irqs)
{
	int i;

	for (i = 0; i < nr_irqs; i++) {
		struct irq_data *d = irq_domain_get_irq_data(domain, virq + i);

		irq_set_handler(virq + i, NULL);
		irq_domain_reset_irq_data(d);
	}
}

static const struct irq_domain_ops gicv5_irq_lpi_domain_ops = {
	.alloc	= gicv5_irq_lpi_domain_alloc,
	.free	= gicv5_irq_lpi_domain_free,
};

void __init gicv5_init_lpi_domain(void)
{
	gicv5_global_data.lpi_domain = irq_domain_create_tree(NULL,
				&gicv5_irq_lpi_domain_ops, NULL);
}

void __init gicv5_free_lpi_domain(void)
{
	irq_domain_remove(gicv5_global_data.lpi_domain);
}

static int gicv5_irq_ipi_domain_alloc(struct irq_domain *domain,
				      unsigned int virq, unsigned int nr_irqs,
				      void *arg)
{
	int ret, i;
	u32 lpi;
	struct irq_data *irqd = irq_desc_get_irq_data(irq_to_desc(virq));

	// Get LPIs for the IPIs
	ret = gicv5_alloc_lpi_range(nr_irqs, &lpi);
	if (ret)
		return ret;

	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, &lpi);
	if (ret) {
		gicv5_free_lpi_range(lpi, nr_irqs);
		return ret;
	}

	for (i = 0; i < nr_irqs; i++) {
		irqd = irq_desc_get_irq_data(irq_to_desc(virq + i));

		irq_domain_set_hwirq_and_chip(domain, virq + i, i,
				&gicv5_ipi_irq_chip, NULL);

		irqd_set_single_target(irqd);

		irq_set_handler(virq + i, handle_percpu_irq);
	}

	return 0;
}

static void gicv5_irq_ipi_domain_free(struct irq_domain *domain,
				      unsigned int virq, unsigned int nr_irqs)
{
	for (unsigned int i = 0; i < nr_irqs; i++) {
		struct irq_data *d = irq_domain_get_irq_data(domain, virq + i);

		if (WARN_ON(!d))
			return;

		gicv5_free_lpi_range(d->parent_data->hwirq, 1);

		irq_set_handler(virq + i, NULL);
		irq_domain_reset_irq_data(d);
	}
	irq_domain_free_irqs_parent(domain, virq, nr_irqs);
}

static const struct irq_domain_ops gicv5_irq_ipi_domain_ops = {
	.alloc	= gicv5_irq_ipi_domain_alloc,
	.free	= gicv5_irq_ipi_domain_free,
};

static inline void handle_irq_per_domain(u32 hwirq)
{
	u32 hwirq_id;
	struct irq_domain *domain = NULL;
	u8 hwirq_type = FIELD_GET(GICV5_HWIRQ_TYPE, hwirq);

	hwirq_id = FIELD_GET(GICV5_HWIRQ_ID, hwirq);

	if (hwirq_type == GICV5_HWIRQ_TYPE_PPI)
		domain = gicv5_global_data.ppi_domain;
	else if (hwirq_type == GICV5_HWIRQ_TYPE_SPI)
		domain = gicv5_global_data.spi_domain;
	else if (hwirq_type == GICV5_HWIRQ_TYPE_LPI)
		domain = gicv5_global_data.lpi_domain;

	if (generic_handle_domain_irq(domain, hwirq_id)) {
		pr_err("Could not handle, hwirq = 0x%x", hwirq_id);
		gicv5_hwirq_eoi(hwirq_id, hwirq_type);
	}
}

static asmlinkage void __exception_irq_entry
gicv5_handle_irq(struct pt_regs *regs)
{
	u64 ia;
	bool valid;
	u32 hwirq;

	ia = gicr_insn(GICV5_OP_GICR_CDIA);
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

/*
 * Disable IRQs for the executing CPU
 */
static void gicv5_cpu_disable_interrupts(void)
{
	u64 cr0;

	// Disable interrupts for the Interrupt Domain
	cr0 = FIELD_PREP(ICC_CR0_EL1_EN, 0);
	write_sysreg_s(cr0, SYS_ICC_CR0_EL1);
}

/*
 * Enable IRQs for the executing CPU
 */
static void gicv5_cpu_enable_interrupts(void)
{
	u64 cr0, pcr;

	write_sysreg_s(0, SYS_ICC_PPI_ENABLER0_EL1);
	write_sysreg_s(0, SYS_ICC_PPI_ENABLER1_EL1);

	gicv5_ppi_priority_init();

	// Explicitly set the physical interrupt priority of the CPU
	pcr = FIELD_PREP(ICC_PCR_EL1_PRIORITY, GICV5_IRQ_PRIORITY_MI);
	write_sysreg_s(pcr, SYS_ICC_PCR_EL1);

	// Enable interrupts for the Interrupt Domain
	cr0 = FIELD_PREP(ICC_CR0_EL1_EN, 1);
	write_sysreg_s(cr0, SYS_ICC_CR0_EL1);
}

static int base_ipi_virq;

static int gicv5_check_ipi_affinity(unsigned int cpu)
{
	int ret;
	u64 cdrcfg, icsr;
	u16 programmed_iaffid, requested_iaffid;
	struct irq_data *d;
	unsigned int i, virq_base = base_ipi_virq + cpu * GICV5_IPIS_PER_CPU;

	for (i = 0; i < GICV5_IPIS_PER_CPU; i++) {
		d = irq_get_irq_data(virq_base + i);

		if (WARN_ON(!d))
			return -ENODEV;

		/*
		 * We need to know the actual LPI that is being used, rather
		 * than the IPI domain hwirq.
		 *
		 * Hence, use the hwirq from the parent (LPI domain) here.
		 */
		cdrcfg = d->parent_data->hwirq |
			 FIELD_PREP(GICV5_HWIRQ_TYPE, GICV5_HWIRQ_TYPE_LPI);
		preempt_disable();
		gic_insn(cdrcfg, GICV5_OP_GIC_CDRCFG);
		isb();
		icsr = read_sysreg_s(SYS_ICC_ICSR_EL1);
		preempt_enable();

		if (FIELD_GET(ICC_ICSR_EL1_F, icsr)) {
			pr_err("SYS_ID_ICC_ICSR_EL1 is not valid");
			return -ENXIO;
		}

		if (!FIELD_GET(ICC_ICSR_EL1_Enabled, icsr)) {
			pr_err("interrupt is disabled");
			return -ENXIO;
		}

		if (FIELD_GET(ICC_ICSR_EL1_IRM, icsr)) {
			pr_debug("Interrupt not using targeted routing");
			return -ENXIO;
		}

		programmed_iaffid = (u16)FIELD_GET(ICC_ICSR_EL1_IAFFID, icsr);

		ret = gicv5_irs_cpu_to_iaffid(cpu, &requested_iaffid);
		if (ret)
			return ret;

		if (programmed_iaffid != requested_iaffid) {
			pr_err("Mismatch between programmed_iaffid (%u) and requested_iaffid (%u)",
			       programmed_iaffid, requested_iaffid);

			return -EIO;
		}
	}

	return 0;
}

static int gicv5_starting_cpu(unsigned int cpu)
{
	if (WARN(!gicv5_cpuif_has_gcie(),
	    "GICv5 system components present but CPU does not have FEAT_GCIE"))
		return -ENODEV;

	gicv5_cpu_enable_interrupts();

	return gicv5_irs_register_cpu(cpu);
}

static void __init gicv5_smp_init(void)
{
	unsigned int num_ipis = GICV5_IPIS_PER_CPU * nr_cpu_ids;

	cpuhp_setup_state_nocalls(CPUHP_AP_IRQ_GIC_STARTING,
				  "irqchip/arm/gicv5:starting",
				  gicv5_starting_cpu, NULL);

	base_ipi_virq = irq_domain_alloc_irqs(gicv5_global_data.ipi_domain,
					  num_ipis, NUMA_NO_NODE, NULL);
	if (WARN(base_ipi_virq <= 0, "IPI IRQ allocation was not successful"))
		return;

	set_smp_ipi_range_percpu(base_ipi_virq, GICV5_IPIS_PER_CPU, nr_cpu_ids);

	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "irqchip/arm/gicv5:online",
				  gicv5_check_ipi_affinity, NULL);
}

static void __init gicv5_free_domains(void)
{
	if (gicv5_global_data.ppi_domain)
		irq_domain_remove(gicv5_global_data.ppi_domain);
	if (gicv5_global_data.spi_domain)
		irq_domain_remove(gicv5_global_data.spi_domain);
	if (gicv5_global_data.ipi_domain)
		irq_domain_remove(gicv5_global_data.ipi_domain);
}

static int __init gicv5_init_domains(struct fwnode_handle *handle)
{
	gicv5_global_data.fwnode = handle;
	gicv5_global_data.ppi_domain = irq_domain_create_linear(
		handle, 128, &gicv5_irq_ppi_domain_ops, NULL);

	if (WARN_ON(!gicv5_global_data.ppi_domain))
		return -ENOMEM;
	irq_domain_update_bus_token(gicv5_global_data.ppi_domain,
				    DOMAIN_BUS_WIRED);

	if (gicv5_global_data.global_spi_count) {
		gicv5_global_data.spi_domain = irq_domain_create_linear(
			handle, gicv5_global_data.global_spi_count,
			&gicv5_irq_spi_domain_ops, NULL);

		if (WARN_ON(!gicv5_global_data.spi_domain)) {
			gicv5_free_domains();
			return -ENOMEM;
		}
		irq_domain_update_bus_token(gicv5_global_data.spi_domain,
					    DOMAIN_BUS_WIRED);
	}

	if (!WARN(!gicv5_global_data.lpi_domain,
		  "LPI domain uninitialized, can't set up IPIs")) {
		gicv5_global_data.ipi_domain = irq_domain_create_hierarchy(
			gicv5_global_data.lpi_domain, 0,
			GICV5_IPIS_PER_CPU * nr_cpu_ids,
			NULL, &gicv5_irq_ipi_domain_ops, NULL);

		if (WARN_ON(!gicv5_global_data.ipi_domain)) {
			gicv5_free_domains();
			return -ENOMEM;
		}
	}

	return 0;
}

static void gicv5_set_cpuif_pribits(void)
{
	u64 icc_idr0 = read_sysreg_s(SYS_ICC_IDR0_EL1);

	switch (FIELD_GET(ICC_IDR0_EL1_PRI_BITS, icc_idr0)) {
	case ICC_IDR0_EL1_PRI_BITS_4BITS:
		gicv5_global_data.cpuif_pri_bits = 4;
		break;
	case ICC_IDR0_EL1_PRI_BITS_5BITS:
		gicv5_global_data.cpuif_pri_bits = 5;
		break;
	default:
		pr_err("Unexpected ICC_IDR0_EL1_PRI_BITS value, default to 4");
		gicv5_global_data.cpuif_pri_bits = 4;
		break;
	}
}

static void gicv5_set_cpuif_idbits(void)
{
	u32 icc_idr0 = read_sysreg_s(SYS_ICC_IDR0_EL1);

	switch (FIELD_GET(ICC_IDR0_EL1_ID_BITS, icc_idr0)) {
	case ICC_IDR0_EL1_ID_BITS_16BITS:
		gicv5_global_data.cpuif_id_bits = 16;
		break;
	case ICC_IDR0_EL1_ID_BITS_24BITS:
		gicv5_global_data.cpuif_id_bits = 24;
		break;
	default:
		pr_err("Unexpected ICC_IDR0_EL1_ID_BITS value, default to 16");
		gicv5_global_data.cpuif_id_bits = 16;
		break;
	}
}


static int __init gicv5_of_init(struct device_node *node,
				struct device_node *parent)
{
	int ret;

	ret = gicv5_irs_of_probe(node);
	if (ret)
		return ret;

	ret = gicv5_init_domains(&node->fwnode);
	if (ret) {
		gicv5_irs_remove();
		return ret;
	}

	gicv5_set_cpuif_pribits();
	gicv5_set_cpuif_idbits();

	pri_bits = min_not_zero(gicv5_global_data.cpuif_pri_bits,
		       gicv5_global_data.irs_pri_bits);

	ret = gicv5_starting_cpu(smp_processor_id());
	if (ret) {
		gicv5_irs_remove();
		gicv5_free_domains();
		return ret;
	}

	ret = set_handle_irq(gicv5_handle_irq);
	if (ret) {
		gicv5_irs_remove();
		gicv5_free_domains();
		gicv5_cpu_disable_interrupts();
	}

	ret = gicv5_irs_enable();
	if (ret) {
		gicv5_irs_remove();
		gicv5_free_domains();
		gicv5_cpu_disable_interrupts();
		return ret;
	}

	gicv5_smp_init();

	return 0;
}
IRQCHIP_DECLARE(gic_v5, "arm,gic-v5", gicv5_of_init);
