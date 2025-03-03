// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Loongson Technologies, Inc.
 */

#include <linux/cpuhotplug.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/spinlock.h>
#include <linux/msi.h>

#include <asm/irq.h>
#include <asm/loongarch.h>
#include <asm/setup.h>
#include <larchintrin.h>

#include "irq-loongson.h"
#include "irq-msi-lib.h"

#define IRD_ENTRIES			65536

/* redirect entry size 128bits */
#if (defined(CONFIG_PAGE_SIZE_4KB))
#define IRD_PAGE_ORDER			8
#elif (defined(CONFIG_PAGE_SIZE_16KB))
#define IRD_PAGE_ORDER			6
#endif

/* irt cache invalid queue */
#define	INVALID_QUEUE_SIZE		4096

#if (defined(CONFIG_PAGE_SIZE_4KB))
#define INVALID_QUEUE_PAGE_ORDER	4
#elif (defined(CONFIG_PAGE_SIZE_16KB))
#define INVALID_QUEUE_PAGE_ORDER	2
#endif

#define GPID_ADDR_MASK			0x3ffffffffffULL
#define GPID_ADDR_SHIFT			6

#define CQB_SIZE_SHIFT			0
#define CQB_SIZE_MASK			0xf
#define CQB_ADDR_SHIFT			12
#define CQB_ADDR_MASK			(0xfffffffffULL)

#define CFG_DISABLE_IDLE		2
#define INVALID_INDEX			0

#define MAX_IR_ENGINES			16

struct irq_domain *redirect_domain;
struct redirect_entry {
	struct  {
		__u64	valid	: 1,
			res1	: 5,
			gpid	: 42,
			res2	: 8,
			vector	: 8;
	} lo;
	__u64 hi;
};

struct redirect_gpid {
	u64	pir[4];      /* Pending interrupt requested */
	u8	en	: 1, /* doorbell */
		res0	: 7;
	u8	irqnum;
	u16	res1;
	u32	dst;
	u32	rsvd[6];
} __aligned(64);

struct irde_inv_cmd {
	union {
		__u64 cmd_info;
		struct {
			__u64	res1		: 4,
				type		: 1,
				need_notice	: 1,
				pad		: 2,
				index		: 16,
				pad2		: 40;
		} index;
	};
	__u64 notice_addr;
};

struct redirect_item {
	int index;
	struct redirect_entry *entry;
	struct redirect_gpid *gpid;
	struct redirect_table *table;
};

struct redirect_table {
	int node;
	struct redirect_entry *table;
	unsigned long	*bitmap;
	unsigned int	nr_ird;
	struct page	*page;
	raw_spinlock_t	lock;
};

struct redirect_queue {
	int node;
	u64 base;
	u32 max_size;
	int head;
	int tail;
	struct page *page;
	raw_spinlock_t	lock;
};

struct la_irde_desc {
	struct redirect_table ird_table;
	struct redirect_queue inv_queue;
};

struct smp_invalid_arg {
	struct redirect_queue *queue;
	struct irde_inv_cmd *cmd;
};

static struct la_irde_desc irde_descs[MAX_IR_ENGINES];
static phys_addr_t msi_base_addr;

static inline void redirect_enable(int node)
{
	u64 value;

	if (redirect_domain) {
		value = iocsr_read64(LOONGARCH_IOCSR_MISC_FUNC);
		value |= IOCSR_MISC_FUNC_REDIRECT_EN;
		iocsr_write64(value, LOONGARCH_IOCSR_MISC_FUNC);
	}
}

static inline bool invalid_queue_is_full(int node_id, u32 *tail)
{
	u32 head;

	head = iocsr_read32(LOONGARCH_IOCSR_REDIRECT_CQH);
	*tail = iocsr_read32(LOONGARCH_IOCSR_REDIRECT_CQT);

	return !!(head == ((*tail + 1) % INVALID_QUEUE_SIZE));
}

static void invalid_enqueue(struct redirect_queue *rqueue, struct irde_inv_cmd *cmd)
{
	struct irde_inv_cmd *inv_addr;
	u32 tail;

	guard(raw_spinlock_irqsave)(&rqueue->lock);

	while (invalid_queue_is_full(rqueue->node, &tail))
		cpu_relax();

	inv_addr = (struct irde_inv_cmd *)(rqueue->base + tail * sizeof(struct irde_inv_cmd));
	memcpy(inv_addr, cmd, sizeof(struct irde_inv_cmd));
	tail = (tail + 1) % INVALID_QUEUE_SIZE;

	wmb();

	iocsr_write32(tail, LOONGARCH_IOCSR_REDIRECT_CQT);
}

static void smp_call_invalid_enqueue(void *arg)
{
	struct smp_invalid_arg *s_arg = (struct smp_invalid_arg *)arg;

	invalid_enqueue(s_arg->queue, s_arg->cmd);
}

static void irde_invlid_entry_node(struct redirect_item *item)
{
	struct redirect_queue *rqueue;
	struct smp_invalid_arg arg;
	struct irde_inv_cmd cmd;
	volatile u64 raddr = 0;
	int node = item->table->node, cpu;

	rqueue = &(irde_descs[node].inv_queue);
	cmd.cmd_info = 0;
	cmd.index.type = INVALID_INDEX;
	cmd.index.need_notice = 1;
	cmd.index.index = item->index;
	cmd.notice_addr = (u64)(__pa(&raddr));

	if (cpu_to_node(smp_processor_id()) == node)
		invalid_enqueue(rqueue, &cmd);
	else {
		for_each_cpu(cpu, cpumask_of_node(node)) {
			if (cpu_online(cpu))
				break;
		}
		arg.queue = rqueue;
		arg.cmd = &cmd;
		smp_call_function_single(cpu, smp_call_invalid_enqueue, &arg, 0);
	}

	while (!raddr)
		cpu_relax();

}

static inline struct avecintc_data *irq_data_get_avec_data(struct irq_data *data)
{
	return data->parent_data->chip_data;
}

static int redirect_table_alloc(struct redirect_item *item, struct redirect_table *ird_table)
{
	int index;

	guard(raw_spinlock_irqsave)(&ird_table->lock);

	index = find_first_zero_bit(ird_table->bitmap, IRD_ENTRIES);
	if (index > IRD_ENTRIES) {
		pr_err("No redirect entry to use\n");
		return -ENOMEM;
	}

	__set_bit(index, ird_table->bitmap);

	item->index = index;
	item->entry = &ird_table->table[index];
	item->table = ird_table;

	return 0;
}

static int redirect_table_free(struct redirect_item *item)
{
	struct redirect_table *ird_table;
	struct redirect_entry *entry;
	unsigned long flags;

	ird_table = item->table;

	entry = item->entry;
	memset(entry, 0, sizeof(struct redirect_entry));

	raw_spin_lock_irqsave(&ird_table->lock, flags);
	bitmap_release_region(ird_table->bitmap, item->index, 0);
	raw_spin_unlock_irqrestore(&ird_table->lock, flags);

	kfree(item->gpid);

	irde_invlid_entry_node(item);

	return 0;
}

static inline void redirect_domain_prepare_entry(struct redirect_item *item, struct avecintc_data *adata)
{
	struct redirect_entry *entry = item->entry;

	item->gpid->en = 1;
	item->gpid->irqnum = adata->vec;
	item->gpid->dst = adata->cpu;

	entry->lo.valid = 1;
	entry->lo.gpid = ((long)item->gpid >> GPID_ADDR_SHIFT) & (GPID_ADDR_MASK);
	entry->lo.vector = 0xff;
	wmb();
}

static int loongarch_redirect_set_affinity(struct irq_data *data, const struct cpumask *dest, bool force)
{
	struct redirect_item *item = data->chip_data;
	struct avecintc_data *adata;
	int ret;

	ret = irq_chip_set_affinity_parent(data, dest, force);
	if (ret == IRQ_SET_MASK_OK_DONE)
		return IRQ_SET_MASK_OK;
	else if (ret) {
		pr_err("IRDE:set_affinity error %d\n", ret);
		return ret;
	}

	adata = irq_data_get_avec_data(data);

	redirect_domain_prepare_entry(item, adata);

	irde_invlid_entry_node(item);

	avecintc_sync(adata);
	return IRQ_SET_MASK_OK;
}

static void loongarch_redirect_compose_msg(struct irq_data *d, struct msi_msg *msg)
{
	struct redirect_item *item;

	item = irq_data_get_irq_chip_data(d);
	msg->address_lo = (msi_base_addr | 1 << 2 | ((item->index & 0xffff) << 4));
	msg->address_hi = 0x0;
	msg->data = 0x0;
}

static inline void loongarch_redirect_ack_irq(struct irq_data *d)
{
}

static inline void loongarch_redirect_unmask_irq(struct irq_data *d)
{
}

static inline void loongarch_redirect_mask_irq(struct irq_data *d)
{
}

static struct irq_chip loongarch_redirect_chip = {
	.name = "redirect",
	.irq_ack		= loongarch_redirect_ack_irq,
	.irq_mask		= loongarch_redirect_mask_irq,
	.irq_unmask		= loongarch_redirect_unmask_irq,
	.irq_set_affinity	= loongarch_redirect_set_affinity,
	.irq_compose_msi_msg	= loongarch_redirect_compose_msg,
};

static void loongarch_irq_redirect_free_resources(struct irq_domain *domain,
						unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *irq_data;
	struct redirect_item *item;

	for (int i = 0; i < nr_irqs; i++) {
		irq_data = irq_domain_get_irq_data(domain, virq  + i);
		if (irq_data && irq_data->chip_data) {
			item = irq_data->chip_data;
			redirect_table_free(item);
			kfree(item);
		}
	}
}

static int loongarch_irq_redirect_alloc(struct irq_domain *domain,
					unsigned int virq, unsigned int nr_irqs,
					 void *arg)
{
	struct redirect_table *ird_table;
	struct avecintc_data *avec_data;
	struct irq_data *irq_data;
	int ret, i, node;

	node = ((msi_alloc_info_t *)arg)->desc->dev->numa_node;
	ird_table = &irde_descs[node].ird_table;
	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, arg);
	if (ret < 0)
		return ret;

	for (i = 0; i < nr_irqs; i++) {
		struct redirect_item *item;

		item = kzalloc(sizeof(struct redirect_item), GFP_KERNEL);
		if (!item) {
			pr_err("Alloc redirect descriptor failed\n");
			goto out_free_resources;
		}

		irq_data = irq_domain_get_irq_data(domain, virq + i);

		avec_data = irq_data_get_avec_data(irq_data);
		ret = redirect_table_alloc(item, ird_table);
		if (ret) {
			pr_err("Alloc redirect table entry failed\n");
			goto out_free_resources;
		}

		item->gpid = kzalloc_node(sizeof(struct redirect_gpid), GFP_KERNEL, node);
		if (!item->gpid) {
			pr_err("Alloc redirect GPID failed\n");
			goto out_free_resources;
		}

		irq_data->chip_data = item;
		irq_data->chip = &loongarch_redirect_chip;
		redirect_domain_prepare_entry(item, avec_data);
	}
	return 0;

out_free_resources:
	loongarch_irq_redirect_free_resources(domain, virq, nr_irqs);
	irq_domain_free_irqs_common(domain, virq, nr_irqs);

	return -EINVAL;
}

static void loongarch_irq_redirect_free(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs)
{
	loongarch_irq_redirect_free_resources(domain, virq, nr_irqs);
	return irq_domain_free_irqs_common(domain, virq, nr_irqs);
}

static const struct irq_domain_ops loongarch_redirect_domain_ops = {
	.alloc = loongarch_irq_redirect_alloc,
	.free = loongarch_irq_redirect_free,
	.select	= msi_lib_irq_domain_select,
};

static int redirect_queue_init(int node)
{
	struct redirect_queue *rqueue = &(irde_descs[node].inv_queue);
	struct page *pages;

	pages = alloc_pages_node(0, GFP_KERNEL | __GFP_ZERO, INVALID_QUEUE_PAGE_ORDER);
	if (!pages) {
		pr_err("Node [%d] Invalid Queue alloc pages failed!\n", node);
		return -ENOMEM;
	}

	rqueue->page = pages;
	rqueue->base = (u64)page_address(pages);
	rqueue->max_size = INVALID_QUEUE_SIZE;
	rqueue->head = 0;
	rqueue->tail = 0;
	rqueue->node = node;
	raw_spin_lock_init(&rqueue->lock);

	iocsr_write32(0, LOONGARCH_IOCSR_REDIRECT_CQH);
	iocsr_write32(0, LOONGARCH_IOCSR_REDIRECT_CQT);
	iocsr_write64(((rqueue->base & (CQB_ADDR_MASK << CQB_ADDR_SHIFT)) |
				(CQB_SIZE_MASK << CQB_SIZE_SHIFT)), LOONGARCH_IOCSR_REDIRECT_CQB);
	return 0;
}

static int redirect_table_init(int node)
{
	struct redirect_table *ird_table = &(irde_descs[node].ird_table);
	struct page *pages;
	unsigned long *bitmap;

	pages = alloc_pages_node(node, GFP_KERNEL | __GFP_ZERO, IRD_PAGE_ORDER);
	if (!pages) {
		pr_err("Node [%d] redirect table alloc pages failed!\n", node);
		return -ENOMEM;
	}
	ird_table->page = pages;
	ird_table->table = page_address(pages);

	bitmap = bitmap_zalloc(IRD_ENTRIES, GFP_KERNEL);
	if (!bitmap) {
		pr_err("Node [%d] redirect table bitmap alloc pages failed!\n", node);
		return -ENOMEM;
	}

	ird_table->bitmap = bitmap;
	ird_table->nr_ird = IRD_ENTRIES;
	ird_table->node = node;

	raw_spin_lock_init(&ird_table->lock);

	if (redirect_queue_init(node))
		return -EINVAL;

	iocsr_write64(CFG_DISABLE_IDLE, LOONGARCH_IOCSR_REDIRECT_CFG);
	iocsr_write64(__pa(ird_table->table), LOONGARCH_IOCSR_REDIRECT_TBR);

	return 0;
}

static void redirect_table_fini(int node)
{
	struct redirect_table *ird_table = &(irde_descs[node].ird_table);
	struct redirect_queue *rqueue = &(irde_descs[node].inv_queue);

	if (ird_table->page) {
		__free_pages(ird_table->page, IRD_PAGE_ORDER);
		ird_table->table = NULL;
		ird_table->page = NULL;
	}

	if (ird_table->page) {
		bitmap_free(ird_table->bitmap);
		ird_table->bitmap = NULL;
	}

	if (rqueue->page) {
		__free_pages(rqueue->page, INVALID_QUEUE_PAGE_ORDER);
		rqueue->page = NULL;
		rqueue->base = 0;
	}

	iocsr_write64(0, LOONGARCH_IOCSR_REDIRECT_CQB);
	iocsr_write64(0, LOONGARCH_IOCSR_REDIRECT_TBR);
}

static int redirect_cpu_online(unsigned int cpu)
{
	int ret, node = cpu_to_node(cpu);

	if (cpu != cpumask_first(cpumask_of_node(node)))
		return 0;

	ret = redirect_table_init(node);
	if (ret) {
		redirect_table_fini(node);
		return -EINVAL;
	}

	redirect_enable(node);
	return 0;
}

#if defined(CONFIG_ACPI)
static int __init pch_msi_parse_madt(union acpi_subtable_headers *header,
		const unsigned long end)
{
	struct acpi_madt_msi_pic *pchmsi_entry = (struct acpi_madt_msi_pic *)header;

	msi_base_addr = pchmsi_entry->msg_address - AVEC_MSG_OFFSET;

	return pch_msi_acpi_init_avec(redirect_domain);
}

static int __init acpi_cascade_irqdomain_init(void)
{
	return acpi_table_parse_madt(ACPI_MADT_TYPE_MSI_PIC, pch_msi_parse_madt, 1);
}

int __init loongarch_irq_redirect_init(struct irq_domain *parent)
{
	struct fwnode_handle *fwnode;
	struct irq_domain *domain;
	int ret;

	fwnode = irq_domain_alloc_named_fwnode("redirect");
	if (!fwnode) {
		pr_err("Unable to alloc redirect domain handle\n");
		goto fail;
	}

	domain = irq_domain_create_hierarchy(parent, 0, IRD_ENTRIES, fwnode,
			&loongarch_redirect_domain_ops, irde_descs);
	if (!domain) {
		pr_err("Unable to alloc redirect domain\n");
		goto out_free_fwnode;
	}

	redirect_domain = domain;

	ret = redirect_table_init(0);
	if (ret)
		goto out_free_table;

	ret = acpi_cascade_irqdomain_init();
	if (ret < 0) {
		pr_err("Failed to cascade IRQ domain, ret=%d\n", ret);
		goto out_free_table;
	}

	cpuhp_setup_state_nocalls(CPUHP_AP_IRQ_REDIRECT_STARTING,
				  "irqchip/loongarch/redirect:starting",
				  redirect_cpu_online, NULL);

	pr_info("loongarch irq redirect modules init succeeded\n");
	redirect_enable(0);
	return 0;

out_free_table:
	redirect_table_fini(0);
	irq_domain_remove(redirect_domain);
	redirect_domain = NULL;
out_free_fwnode:
	irq_domain_free_fwnode(fwnode);
fail:
	return -EINVAL;
}
#endif
