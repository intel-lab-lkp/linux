// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Multikernel Technologies, Inc. All rights reserved
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/multikernel.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <asm/apic.h>
#include <linux/memblock.h>

/* Memory parameters for shared region */
#define MK_IPI_DATA_SIZE  (sizeof(struct mk_ipi_data) * NR_CPUS)
#define MK_MEM_BASE_SIZE  (sizeof(struct mk_shared_data))
#define MK_MEM_SIZE       (MK_MEM_BASE_SIZE + PAGE_SIZE)

/* Boot parameter for physical address */
static unsigned long mk_phys_addr_param;

/* Parse multikernel physical address from kernel command line */
static int __init multikernel_phys_addr_setup(char *str)
{
	return kstrtoul(str, 0, &mk_phys_addr_param);
}
early_param("mk_shared_memory", multikernel_phys_addr_setup);

/* Allocated/assigned physical address for shared memory */
static phys_addr_t mk_phys_addr_base;

/* Resource structure for tracking the memory in /proc/iomem */
static struct resource mk_mem_res __ro_after_init = {
	.name = "Multikernel Shared Memory",
	.flags = IORESOURCE_MEM | IORESOURCE_BUSY,
};

/* Shared memory structures */
struct mk_shared_data {
	struct mk_ipi_data cpu_data[NR_CPUS];  /* Data area for each CPU */
};

/* Pointer to the shared memory area (remapped virtual address) */
static struct mk_shared_data *mk_shared_mem;

/* Callback management */
static struct mk_ipi_handler *mk_handlers;
static raw_spinlock_t mk_handlers_lock = __RAW_SPIN_LOCK_UNLOCKED(mk_handlers_lock);

static void handler_work(struct irq_work *work)
{
    struct mk_ipi_handler *handler = container_of(work, struct mk_ipi_handler, work);
    if (handler->callback)
        handler->callback(handler->saved_data, handler->context);
}

/**
 * multikernel_register_handler - Register a callback for multikernel IPI
 * @callback: Function to call when IPI is received
 * @ctx: Context pointer passed to the callback
 *
 * Returns pointer to handler on success, NULL on failure
 */
struct mk_ipi_handler *multikernel_register_handler(mk_ipi_callback_t callback, void *ctx)
{
	struct mk_ipi_handler *handler;
	unsigned long flags;

	if (!callback)
		return NULL;

	handler = kzalloc(sizeof(*handler), GFP_KERNEL);
	if (!handler)
		return NULL;

	handler->callback = callback;
	handler->context = ctx;

	init_irq_work(&handler->work, handler_work);

	raw_spin_lock_irqsave(&mk_handlers_lock, flags);
	handler->next = mk_handlers;
	mk_handlers = handler;
	raw_spin_unlock_irqrestore(&mk_handlers_lock, flags);

	return handler;
}
EXPORT_SYMBOL(multikernel_register_handler);

/**
 * multikernel_unregister_handler - Unregister a multikernel IPI callback
 * @handler: Handler pointer returned from multikernel_register_handler
 */
void multikernel_unregister_handler(struct mk_ipi_handler *handler)
{
	struct mk_ipi_handler **pp, *p;
	unsigned long flags;

	if (!handler)
		return;

	raw_spin_lock_irqsave(&mk_handlers_lock, flags);
	pp = &mk_handlers;
	while ((p = *pp) != NULL) {
		if (p == handler) {
			*pp = p->next;
			break;
		}
		pp = &p->next;
	}
	raw_spin_unlock_irqrestore(&mk_handlers_lock, flags);

    /* Wait for pending work to complete */
    irq_work_sync(&handler->work);
    kfree(p);
}
EXPORT_SYMBOL(multikernel_unregister_handler);

/**
 * multikernel_send_ipi_data - Send data to another CPU via IPI
 * @cpu: Target CPU
 * @data: Pointer to data to send
 * @data_size: Size of data
 * @type: User-defined type identifier
 *
 * This function copies the data to per-CPU storage and sends an IPI
 * to the target CPU.
 *
 * Returns 0 on success, negative error code on failure
 */
int multikernel_send_ipi_data(int cpu, void *data, size_t data_size, unsigned long type)
{
	struct mk_ipi_data *target;

	if (cpu < 0 || cpu >= nr_cpu_ids)
		return -EINVAL;

	if (data_size > MK_MAX_DATA_SIZE)
		return -EINVAL;  /* Data too large for buffer */

	/* Ensure shared memory is initialized */
	if (!mk_shared_mem)
		return -ENOMEM;

	/* Get target CPU's data area from shared memory */
	target = &mk_shared_mem->cpu_data[cpu];

	/* Set header information */
	target->data_size = data_size;
	target->sender_cpu = arch_cpu_physical_id(smp_processor_id());
	target->type = type;

	/* Copy the actual data into the buffer */
	if (data && data_size > 0)
		memcpy(target->buffer, data, data_size);

	/* Send IPI to target CPU */
	__apic_send_IPI(cpu, MULTIKERNEL_VECTOR);

	return 0;
}
EXPORT_SYMBOL(multikernel_send_ipi_data);

/**
 * multikernel_interrupt_handler - Handle the multikernel IPI
 *
 * This function is called when a multikernel IPI is received.
 * It invokes all registered callbacks with the per-CPU data.
 */
static void multikernel_interrupt_handler(void)
{
	struct mk_ipi_data *data;
	struct mk_ipi_handler *handler;
	int current_cpu = smp_processor_id();
	int current_physical_id = arch_cpu_physical_id(current_cpu);

	/* Ensure shared memory is initialized */
	if (!mk_shared_mem) {
		pr_err("Multikernel IPI received but shared memory not initialized\n");
		return;
	}

	/* Get this CPU's data area from shared memory */
	data = &mk_shared_mem->cpu_data[current_physical_id];

	pr_info("Multikernel IPI received on CPU %d (physical id %d) from CPU %d type=%u\n",
		 current_cpu, current_physical_id, data->sender_cpu, data->type);

    raw_spin_lock(&mk_handlers_lock);
    for (handler = mk_handlers; handler; handler = handler->next) {
        handler->saved_data = data;
        irq_work_queue(&handler->work);
    }
    raw_spin_unlock(&mk_handlers_lock);
}

/**
 * Generic multikernel interrupt handler - called by the IPI vector
 *
 * This is the function that gets called by the IPI vector handler.
 */
void generic_multikernel_interrupt(void)
{
	multikernel_interrupt_handler();
}

/**
 * setup_shared_memory - Initialize shared memory for inter-kernel communication
 *
 * Maps a fixed physical memory region for sharing IPI data between kernels
 * Returns 0 on success, negative error code on failure
 */
static int __init setup_shared_memory(void)
{
	/* Check if a fixed physical address was provided via parameter */
	if (mk_phys_addr_param) {
		/* Use the provided physical address */
		mk_phys_addr_base = (phys_addr_t)mk_phys_addr_param;
		pr_info("Using specified physical address 0x%llx for multikernel shared memory\n",
		       (unsigned long long)mk_phys_addr_base);
	} else {
		/* Dynamically allocate contiguous physical memory using memblock */
		mk_phys_addr_base = memblock_phys_alloc(MK_MEM_SIZE, PAGE_SIZE);
		if (!mk_phys_addr_base) {
			pr_err("Failed to allocate physical memory for multikernel IPI data\n");
			return -ENOMEM;
		}
	}

	/* Map the physical memory region to virtual address space */
	mk_shared_mem = memremap(mk_phys_addr_base, MK_MEM_SIZE, MEMREMAP_WB);
	if (!mk_shared_mem) {
		pr_err("Failed to map shared memory at 0x%llx for multikernel IPI data\n",
		       (unsigned long long)mk_phys_addr_base);

		/* Only free the memory if we allocated it dynamically */
		if (!mk_phys_addr_param)
			memblock_phys_free(mk_phys_addr_base, MK_MEM_SIZE);
		return -ENOMEM;
	}

	/* Initialize the memory to zero */
	memset(mk_shared_mem, 0, sizeof(struct mk_shared_data));

	pr_info("Allocated and mapped multikernel shared memory: phys=0x%llx, virt=%px, size=%lu bytes\n",
		(unsigned long long)mk_phys_addr_base, mk_shared_mem, MK_MEM_SIZE);

	return 0;
}

int __init multikernel_init(void)
{
	int ret;

	ret = setup_shared_memory();
	if (ret < 0)
		return ret;

	pr_info("Multikernel IPI support initialized\n");
	return 0;
}

static int __init init_shared_memory(void)
{
	/* Set up resource structure for /proc/iomem visibility */
	mk_mem_res.start = mk_phys_addr_base;
	mk_mem_res.end = mk_phys_addr_base + MK_MEM_SIZE - 1;

	/* Register the resource in the global resource tree */
	if (insert_resource(&iomem_resource, &mk_mem_res)) {
		pr_warn("Could not register multikernel shared memory region in resource tracking\n");
		/* Continue anyway as this is not fatal */
		return -1;
	}

	pr_info("Registered multikernel shared memory in resource tree: 0x%llx-0x%llx\n",
		(unsigned long long)mk_mem_res.start, (unsigned long long)mk_mem_res.end);
	return 0;
}
core_initcall(init_shared_memory);

/* ---- Flexible shared memory APIs (PFN-based) ---- */
#define MK_PFN_IPI_TYPE 0x80000001U

/* Send a PFN to another kernel via mk_ipi_data */
int mk_send_pfn(int target_cpu, unsigned long pfn)
{
	return multikernel_send_ipi_data(target_cpu, &pfn, sizeof(pfn), MK_PFN_IPI_TYPE);
}

/* Receive a PFN from mk_ipi_data. Caller must check type. */
int mk_receive_pfn(struct mk_ipi_data *data, unsigned long *out_pfn)
{
	if (!data || !out_pfn)
		return -EINVAL;
	if (data->type != MK_PFN_IPI_TYPE || data->data_size != sizeof(unsigned long))
		return -EINVAL;
	*out_pfn = *(unsigned long *)data->buffer;
	return 0;
}

void *mk_receive_map_page(struct mk_ipi_data *data)
{
	unsigned long pfn;
	int ret;

	ret = mk_receive_pfn(data, &pfn);
	if (ret < 0)
		return NULL;
	return memremap(pfn << PAGE_SHIFT, PAGE_SIZE, MEMREMAP_WB);
}
