// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Loongson Technologies, Inc.
 */

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/spinlock.h>
#include <linux/msi.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/cpuhotplug.h>
#include <linux/radix-tree.h>

#include <asm/loongarch.h>
#include <asm/setup.h>

static phys_addr_t msi_base_v2;

typedef struct irq_data *irq_map_t[NR_VECTORS];
DECLARE_PER_CPU(irq_map_t, irq_map);
DEFINE_PER_CPU(irq_map_t, irq_map) = {
	[0 ... NR_VECTORS - 1] = NULL,
};

struct pending_list {
	struct list_head head;
	raw_spinlock_t	lock;
};

DEFINE_PER_CPU(struct pending_list, pending_list);

struct loongarch_avec_chip {
	struct fwnode_handle	*fwnode;
	struct irq_domain	*domain;
	struct irq_matrix	*vector_matrix;
	raw_spinlock_t		lock;
} loongarch_avec;

struct loongarch_avec_data {
	struct list_head entry;
	unsigned int cpu;
	unsigned int vec;
	unsigned int prev_cpu;
	unsigned int prev_vec;
};

static int assign_irq_vector(struct irq_data *irqd, const struct cpumask *dest,
		unsigned int *cpu, int *vector)
{
	int ret;

	ret = irq_matrix_alloc(loongarch_avec.vector_matrix, dest, false, cpu);
	if (ret < 0)
		return ret;
	*vector = ret;

	return 0;
}

static inline void loongarch_avec_ack_irq(struct irq_data *d)
{
}

static inline void loongarch_avec_unmask_irq(struct irq_data *d)
{
}

static inline void loongarch_avec_mask_irq(struct irq_data *d)
{
}

static void loongarch_avec_sync(struct loongarch_avec_data *adata)
{
	struct loongarch_avec_data *data;
	struct pending_list *plist;

	if (cpu_online(adata->prev_cpu)) {
		plist = per_cpu_ptr(&pending_list, adata->prev_cpu);

		data = kmalloc(sizeof(struct loongarch_avec_data), GFP_KERNEL);
		if (!data) {
			pr_warn("NO space for clean data\n");
			return;
		}
		memcpy(data, adata, sizeof(struct loongarch_avec_data));
		INIT_LIST_HEAD(&data->entry);

		list_add_tail(&data->entry, &plist->head);
		loongson_send_ipi_single(adata->prev_cpu, SMP_CLEAR_VECT);
	}
	adata->prev_cpu = adata->cpu;
	adata->prev_vec = adata->vec;
}

static int loongarch_avec_set_affinity(struct irq_data *data,
		const struct cpumask *dest, bool force)
{
	struct cpumask intersect_mask;
	struct loongarch_avec_data *adata;
	unsigned int cpu, vector;
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&loongarch_avec.lock, flags);
	adata = irq_data_get_irq_chip_data(data);

	if (adata->vec && cpu_online(adata->cpu)
			&& cpumask_test_cpu(adata->cpu, dest)) {
		raw_spin_unlock_irqrestore(&loongarch_avec.lock, flags);
		return 0;
	}

	if (!cpumask_intersects(dest, cpu_online_mask)) {
		raw_spin_unlock_irqrestore(&loongarch_avec.lock, flags);
		return -EINVAL;
	}

	cpumask_and(&intersect_mask, dest, cpu_online_mask);

	ret = assign_irq_vector(data, &intersect_mask, &cpu, &vector);
	if (ret) {
		raw_spin_unlock_irqrestore(&loongarch_avec.lock, flags);
		return ret;
	}

	adata->cpu = cpu;
	adata->vec = vector;
	per_cpu_ptr(irq_map, adata->cpu)[adata->vec] = data;
	loongarch_avec_sync(adata);

	raw_spin_unlock_irqrestore(&loongarch_avec.lock, flags);
	irq_data_update_effective_affinity(data, cpumask_of(cpu));

	return IRQ_SET_MASK_OK;
}

static void loongarch_avec_compose_msg(struct irq_data *d,
		struct msi_msg *msg)
{
	struct loongarch_avec_data *avec_data;

	avec_data = irq_data_get_irq_chip_data(d);

	msg->address_hi = 0x0;
	msg->address_lo = msi_base_v2 | ((avec_data->vec & 0xff) << 4) |
		((cpu_logical_map(avec_data->cpu & 0xffff)) << 12);
	msg->data = 0x0;

}

static struct irq_chip loongarch_avec_controller = {
	.name			= "CORE_AVEC",
	.irq_ack		= loongarch_avec_ack_irq,
	.irq_mask		= loongarch_avec_mask_irq,
	.irq_unmask		= loongarch_avec_unmask_irq,
	.irq_set_affinity	= loongarch_avec_set_affinity,
	.irq_compose_msi_msg	= loongarch_avec_compose_msg,
};

void complete_irq_moving(int *restart)
{
	struct pending_list *plist = this_cpu_ptr(&pending_list);
	struct loongarch_avec_data *adata, *tmp;
	int cpu, vector;
	u32 bias;
	u64 irr;

	raw_spin_lock(&loongarch_avec.lock);

	list_for_each_entry_safe(adata, tmp, &plist->head, entry) {

		cpu = adata->prev_cpu;
		vector = adata->prev_vec;
		bias = vector/64;
		switch (bias) {
		case 0x0:
			irr = csr_read64(LOONGARCH_CSR_IRR0);
			break;
		case 0x1:
			irr = csr_read64(LOONGARCH_CSR_IRR1);
			break;
		case 0x2:
			irr = csr_read64(LOONGARCH_CSR_IRR2);
			break;
		case 0x3:
			irr = csr_read64(LOONGARCH_CSR_IRR3);
			break;
		default:
			return;
		}

		if (irr & (1UL << (vector % 64))) {
			loongson_send_ipi_single(cpu, SMP_CLEAR_VECT);
			continue;
		}
		list_del(&adata->entry);
		irq_matrix_free(loongarch_avec.vector_matrix, cpu, vector, false);
		this_cpu_ptr(irq_map)[vector] = 0;
		kfree(adata);
	}
	raw_spin_unlock(&loongarch_avec.lock);
}

static void loongarch_avec_dispatch(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct irq_data *d;
	unsigned long vector;

	chained_irq_enter(chip, desc);
	vector = csr_read64(LOONGARCH_CSR_ILR);
	if (vector & 0x80000000)
		return;

	vector &= 0xff;

	d = raw_cpu_ptr(irq_map)[vector];
	if (d)
		generic_handle_irq(d->irq);
	else
		pr_warn("IRQ ERROR:Unexpected irq  occur on cpu %d[vector %d]\n",
				smp_processor_id(), vector);

	chained_irq_exit(chip, desc);
}

static int loongarch_avec_alloc(struct irq_domain *domain, unsigned int virq,
		unsigned int nr_irqs, void *arg)
{
	struct loongarch_avec_data *adata;
	struct irq_data *irqd;
	unsigned int cpu, vector;
	unsigned long flags;
	int i, err;

	raw_spin_lock_irqsave(&loongarch_avec.lock, flags);
	for (i = 0; i < nr_irqs; i++) {
		irqd = irq_domain_get_irq_data(domain, virq + i);
		adata = kzalloc(sizeof(*adata), GFP_KERNEL);
		if (!adata) {
			raw_spin_unlock_irqrestore(&loongarch_avec.lock, flags);
			return -ENOMEM;
		}
		err = assign_irq_vector(irqd, cpu_online_mask, &cpu, &vector);
		if (err) {
			raw_spin_unlock_irqrestore(&loongarch_avec.lock, flags);
			return err;
		}
		adata->prev_cpu = adata->cpu = cpu;
		adata->prev_vec = adata->vec = vector;

		per_cpu_ptr(irq_map, adata->cpu)[adata->vec] = irqd;
		irq_domain_set_info(domain, virq + i, virq, &loongarch_avec_controller,
				adata, handle_edge_irq, NULL, NULL);
		irqd_set_single_target(irqd);
		irqd_set_affinity_on_activate(irqd);
	}
	raw_spin_unlock_irqrestore(&loongarch_avec.lock, flags);

	return err;
}

static void loongarch_avec_free(struct irq_domain *domain, unsigned int virq,
		unsigned int nr_irqs)
{
	struct loongarch_avec_data *adata;
	struct irq_data *d;
	unsigned long flags;
	unsigned int i;

	raw_spin_lock_irqsave(&loongarch_avec.lock, flags);
	for (i = 0; i < nr_irqs; i++) {
		d = irq_domain_get_irq_data(domain, virq + i);
		adata = irq_data_get_irq_chip_data(d);
		if (d) {
			irq_matrix_free(loongarch_avec.vector_matrix,
					adata->cpu,
					adata->vec, false);
			irq_domain_reset_irq_data(d);
		}
	}

	raw_spin_unlock_irqrestore(&loongarch_avec.lock, flags);
}

static const struct irq_domain_ops loongarch_avec_domain_ops = {
	.alloc		= loongarch_avec_alloc,
	.free		= loongarch_avec_free,
};

static int __init irq_matrix_init(void)
{
	int i;

	loongarch_avec.vector_matrix = irq_alloc_matrix(NR_VECTORS, 0, NR_VECTORS - 1);
	if (!loongarch_avec.vector_matrix)
		return -ENOMEM;
	for (i = 0; i < NR_LEGACY_VECTORS; i++)
		irq_matrix_assign_system(loongarch_avec.vector_matrix, i, false);

	irq_matrix_online(loongarch_avec.vector_matrix);

	return 0;
}

static int __init loongarch_avec_init(struct irq_domain *parent)
{
	int ret = 0, parent_irq;
	unsigned long tmp;

	tmp = iocsr_read64(LOONGARCH_IOCSR_MISC_FUNC);
	tmp |= IOCSR_MISC_FUNC_AVEC_EN;
	iocsr_write64(tmp, LOONGARCH_IOCSR_MISC_FUNC);

	raw_spin_lock_init(&loongarch_avec.lock);

	loongarch_avec.fwnode = irq_domain_alloc_named_fwnode("CORE_AVEC");
	if (!loongarch_avec.fwnode) {
		pr_err("Unable to allocate domain handle\n");
		ret = -ENOMEM;
		goto out;
	}

	loongarch_avec.domain = irq_domain_create_tree(loongarch_avec.fwnode,
			&loongarch_avec_domain_ops, NULL);
	if (!loongarch_avec.domain) {
		pr_err("core-vec: cannot create IRQ domain\n");
		ret = -ENOMEM;
		goto out_free_handle;
	}

	parent_irq = irq_create_mapping(parent, INT_AVEC);
	if (!parent_irq) {
		pr_err("Failed to mapping hwirq\n");
		ret = -EINVAL;
		goto out_remove_domain;
	}
	irq_set_chained_handler_and_data(parent_irq, loongarch_avec_dispatch, NULL);

	ret = irq_matrix_init();
	if (ret) {
		pr_err("Failed to init irq matrix\n");
		goto out_free_matrix;
	}

	return ret;

out_free_matrix:
	kfree(loongarch_avec.vector_matrix);
out_remove_domain:
	irq_domain_remove(loongarch_avec.domain);
out_free_handle:
	irq_domain_free_fwnode(loongarch_avec.fwnode);
out:
	return ret;
}

static int loongarch_avec_offline_cpu(unsigned int cpu)
{
	unsigned long flags;
	struct pending_list *plist = per_cpu_ptr(&pending_list, cpu);

	raw_spin_lock_irqsave(&loongarch_avec.lock, flags);
	if (list_empty(&plist->head)) {
		irq_matrix_offline(loongarch_avec.vector_matrix);
	} else {
		pr_warn("cpu %d advanced extioi is busy\n");
		raw_spin_unlock_irqrestore(&loongarch_avec.lock, flags);
		return -EBUSY;
	}
	raw_spin_unlock_irqrestore(&loongarch_avec.lock, flags);
	return 0;
}

static int loongarch_avec_online_cpu(unsigned int cpu)
{
	struct pending_list *plist = per_cpu_ptr(&pending_list, cpu);
	unsigned long flags;

	raw_spin_lock_irqsave(&loongarch_avec.lock, flags);

	irq_matrix_online(loongarch_avec.vector_matrix);

	INIT_LIST_HEAD(&plist->head);

	raw_spin_unlock_irqrestore(&loongarch_avec.lock, flags);
	return 0;
}
#if defined(CONFIG_ACPI)
static int __init pch_msi_parse_madt(union acpi_subtable_headers *header,
		const unsigned long end)
{
	struct acpi_madt_msi_pic *pchmsi_entry = (struct acpi_madt_msi_pic *)header;

	msi_base_v2 = pchmsi_entry->msg_address;
	return pch_msi_acpi_init_v2(loongarch_avec.domain, pchmsi_entry);
}

static inline int __init acpi_cascade_irqdomain_init(void)
{
	return acpi_table_parse_madt(ACPI_MADT_TYPE_MSI_PIC, pch_msi_parse_madt, 1);
}

int __init loongarch_avec_acpi_init(struct irq_domain *parent)
{
	int ret = 0;

	ret = loongarch_avec_init(parent);
	if (ret) {
		pr_err("Failed to init irq domain\n");
		return ret;
	}

	ret = acpi_cascade_irqdomain_init();
	if (ret) {
		pr_err("Failed to cascade IRQ domain\n");
		return ret;
	}

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
			"loongarch_avec:online",
			loongarch_avec_online_cpu, loongarch_avec_offline_cpu);
	if (ret < 0) {
		pr_err("loongarch_avec: failed to register hotplug callbacks.\n");
		return ret;
	}

	return ret;
}
#endif
