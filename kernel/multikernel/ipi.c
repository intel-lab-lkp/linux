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

/* Per-instance IPI data - no more global variables */
struct mk_instance_ipi_data {
	void *instance_pool;           /* Instance pool handle */
	struct mk_shared_data *shared_mem;  /* IPI shared memory for this instance */
	size_t shared_mem_size;        /* Size of shared memory */
};

/* Shared memory structures - per-instance design */
struct mk_shared_data {
	struct mk_ipi_data cpu_data[NR_CPUS];  /* Data area for each CPU */
};

#define MK_MAX_INSTANCES 256
static struct mk_instance_ipi_data *mk_instance_ipi_map[MK_MAX_INSTANCES];
static DEFINE_SPINLOCK(mk_ipi_map_lock);

static struct mk_shared_data *mk_this_kernel_ipi_data;
static phys_addr_t mk_ipi_shared_phys_addr;

/* Callback management */
static struct mk_ipi_handler *mk_handlers;
static raw_spinlock_t mk_handlers_lock = __RAW_SPIN_LOCK_UNLOCKED(mk_handlers_lock);

static void *multikernel_alloc_ipi_buffer(void *pool_handle, size_t buffer_size);
static void multikernel_free_ipi_buffer(void *pool_handle, void *virt_addr, size_t buffer_size);

static void handler_work(struct irq_work *work)
{
    struct mk_ipi_handler *handler = container_of(work, struct mk_ipi_handler, work);
    if (handler->callback)
        handler->callback(handler->saved_data, handler->context);
}

/**
 * mk_instance_ipi_create() - Create IPI data for a multikernel instance
 * @instance: The multikernel instance
 *
 * Allocates and initializes IPI communication buffers for the given instance.
 * Returns 0 on success, negative error code on failure.
 */
static int mk_instance_ipi_create(struct mk_instance *instance)
{
	struct mk_instance_ipi_data *ipi_data;
	unsigned long flags;
	int ret = 0;

	if (!instance || instance->id < 0 || instance->id >= MK_MAX_INSTANCES)
		return -EINVAL;

	ipi_data = kzalloc(sizeof(*ipi_data), GFP_KERNEL);
	if (!ipi_data)
		return -ENOMEM;

	/* Use the instance's own memory pool */
	ipi_data->instance_pool = instance->instance_pool;
	if (!ipi_data->instance_pool) {
		pr_err("Instance %d has no memory pool for IPI allocation\n", instance->id);
		kfree(ipi_data);
		return -ENODEV;
	}

	/* Allocate IPI buffer from the instance pool */
	ipi_data->shared_mem_size = sizeof(struct mk_shared_data);
	ipi_data->shared_mem = multikernel_alloc_ipi_buffer(ipi_data->instance_pool,
							    ipi_data->shared_mem_size);
	if (!ipi_data->shared_mem) {
		pr_err("Failed to allocate IPI shared memory for instance %d\n", instance->id);
		kfree(ipi_data);
		return -ENOMEM;
	}

	/* Initialize the shared memory structure */
	memset(ipi_data->shared_mem, 0, ipi_data->shared_mem_size);

	/* Register in the global map */
	spin_lock_irqsave(&mk_ipi_map_lock, flags);
	if (mk_instance_ipi_map[instance->id]) {
		pr_err("IPI data already exists for instance %d\n", instance->id);
		ret = -EEXIST;
	} else {
		mk_instance_ipi_map[instance->id] = ipi_data;
	}
	spin_unlock_irqrestore(&mk_ipi_map_lock, flags);

	if (ret) {
		multikernel_free_ipi_buffer(ipi_data->instance_pool,
					    ipi_data->shared_mem,
					    ipi_data->shared_mem_size);
		kfree(ipi_data);
		return ret;
	}

	pr_info("Created IPI data for instance %d (%s): virt=%px, size=%zu bytes\n",
		instance->id, instance->name, ipi_data->shared_mem, ipi_data->shared_mem_size);

	return 0;
}

/**
 * mk_instance_ipi_destroy() - Destroy IPI data for a multikernel instance
 * @instance_id: The instance ID
 *
 * Cleans up and frees IPI communication buffers for the given instance.
 */
static void mk_instance_ipi_destroy(int instance_id)
{
	struct mk_instance_ipi_data *ipi_data;
	unsigned long flags;

	if (instance_id < 0 || instance_id >= MK_MAX_INSTANCES)
		return;

	spin_lock_irqsave(&mk_ipi_map_lock, flags);
	ipi_data = mk_instance_ipi_map[instance_id];
	mk_instance_ipi_map[instance_id] = NULL;
	spin_unlock_irqrestore(&mk_ipi_map_lock, flags);

	if (!ipi_data)
		return;

	pr_debug("Destroying IPI data for instance %d\n", instance_id);

	/* Free the shared memory buffer */
	if (ipi_data->shared_mem) {
		multikernel_free_ipi_buffer(ipi_data->instance_pool,
					    ipi_data->shared_mem,
					    ipi_data->shared_mem_size);
	}

	kfree(ipi_data);
}

/**
 * mk_instance_ipi_get() - Get IPI data for a multikernel instance
 * @instance_id: The instance ID
 *
 * Returns the IPI data for the given instance, or NULL if not found.
 */
static struct mk_instance_ipi_data *mk_instance_ipi_get(int instance_id)
{
	struct mk_instance_ipi_data *ipi_data;
	unsigned long flags;

	if (instance_id < 0 || instance_id >= MK_MAX_INSTANCES)
		return NULL;

	spin_lock_irqsave(&mk_ipi_map_lock, flags);
	ipi_data = mk_instance_ipi_map[instance_id];
	spin_unlock_irqrestore(&mk_ipi_map_lock, flags);

	return ipi_data;
}

/**
 * multikernel_register_handler - Register a callback for multikernel IPI
 * @callback: Function to call when IPI is received
 * @ctx: Context pointer passed to the callback
 * @ipi_type: IPI type this handler should process
 *
 * Returns pointer to handler on success, NULL on failure
 */
struct mk_ipi_handler *multikernel_register_handler(mk_ipi_callback_t callback, void *ctx, unsigned int ipi_type)
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
	handler->ipi_type = ipi_type;

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
 * @instance_id: Target multikernel instance ID
 * @data: Pointer to data to send
 * @data_size: Size of data
 * @type: User-defined type identifier
 *
 * This function copies the data to per-CPU storage and sends an IPI
 * to the target CPU. The cpu parameter must be a physical CPU ID.
 *
 * Returns 0 on success, negative error code on failure
 */
int multikernel_send_ipi_data(int instance_id, void *data, size_t data_size, unsigned long type)
{
	struct mk_instance_ipi_data *ipi_data;
	struct mk_ipi_data *target;
	struct mk_instance *instance = mk_instance_find(instance_id);
	int cpu ;

	if (!instance)
		return -EINVAL;
	if (data_size > MK_MAX_DATA_SIZE)
		return -EINVAL;

	cpu = cpumask_first(instance->cpus);
	/* Get the IPI data for the target instance */
	ipi_data = mk_instance_ipi_get(instance_id);
	if (!ipi_data || !ipi_data->shared_mem) {
		pr_debug("Multikernel IPI shared memory not available for instance %d\n", instance_id);
		return -ENODEV;
	}

	/* Get target CPU's data area from shared memory */
	target = &ipi_data->shared_mem->cpu_data[cpu];

	/* Initialize/clear the IPI data structure to prevent stale data */
	memset(target, 0, sizeof(*target));

	/* Set header information */
	target->data_size = data_size;
	target->sender_cpu = arch_cpu_physical_id(smp_processor_id());
	target->type = type;

	/* Copy the actual data into the buffer */
	if (data && data_size > 0)
		memcpy(target->buffer, data, data_size);

	/* Send IPI to target CPU using physical CPU ID */
	__apic_send_IPI(cpu, MULTIKERNEL_VECTOR);

	return 0;
}

/**
 * multikernel_interrupt_handler - Handle the multikernel IPI
 *
 * This function is called when a multikernel IPI is received.
 * It invokes all registered callbacks with the per-CPU data.
 *
 * In spawned kernels, we use the shared IPI data passed via boot parameter.
 * In host kernels, we may need to check instance mappings.
 */
static void multikernel_interrupt_handler(void)
{
	struct mk_ipi_data *data;
	struct mk_ipi_handler *handler;
	int current_cpu = smp_processor_id();
	int current_physical_id = arch_cpu_physical_id(current_cpu);

	if (!mk_this_kernel_ipi_data)
		return;

	data = &mk_this_kernel_ipi_data->cpu_data[current_physical_id];

	if (data->data_size == 0 || data->data_size > MK_MAX_DATA_SIZE) {
		pr_debug("Multikernel IPI received on CPU %d but no valid data\n", current_cpu);
		return;
	}

	pr_info("Multikernel IPI received on CPU %d (physical id %d) from CPU %d type=%u\n",
		current_cpu, current_physical_id, data->sender_cpu, data->type);

	raw_spin_lock(&mk_handlers_lock);
	for (handler = mk_handlers; handler; handler = handler->next) {
		if (handler->ipi_type == data->type) {
			handler->saved_data = data;
			irq_work_queue(&handler->work);
		}
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
 * multikernel_alloc_ipi_buffer() - Allocate IPI communication buffer
 * @pool_handle: Instance pool handle
 * @buffer_size: Size of IPI buffer needed
 *
 * Allocates and maps a buffer suitable for IPI communication.
 * Returns virtual address of mapped buffer, or NULL on failure.
 */
static void *multikernel_alloc_ipi_buffer(void *pool_handle, size_t buffer_size)
{
	phys_addr_t phys_addr;
	void *virt_addr;

	phys_addr = multikernel_instance_alloc(pool_handle, buffer_size, PAGE_SIZE);
	if (!phys_addr) {
		pr_err("Failed to allocate %zu bytes for IPI buffer\n", buffer_size);
		return NULL;
	}

	/* Map to virtual address space */
	virt_addr = memremap(phys_addr, buffer_size, MEMREMAP_WB);
	if (!virt_addr) {
		pr_err("Failed to map IPI buffer at 0x%llx\n", (unsigned long long)phys_addr);
		multikernel_instance_free(pool_handle, phys_addr, buffer_size);
		return NULL;
	}

	pr_debug("Allocated IPI buffer: phys=0x%llx, virt=%px, size=%zu\n",
		 (unsigned long long)phys_addr, virt_addr, buffer_size);

	return virt_addr;
}

/**
 * multikernel_free_ipi_buffer() - Free IPI communication buffer
 * @pool_handle: Instance pool handle
 * @virt_addr: Virtual address returned by multikernel_alloc_ipi_buffer()
 * @buffer_size: Size of the buffer
 *
 * Unmaps and frees an IPI buffer back to the instance pool.
 */
static void multikernel_free_ipi_buffer(void *pool_handle, void *virt_addr, size_t buffer_size)
{
	phys_addr_t phys_addr;

	if (!virt_addr)
		return;

	/* Convert virtual address back to physical */
	phys_addr = virt_to_phys(virt_addr);

	/* Unmap virtual address */
	memunmap(virt_addr);

	/* Free back to instance pool */
	multikernel_instance_free(pool_handle, phys_addr, buffer_size);

	pr_debug("Freed IPI buffer: phys=0x%llx, virt=%px, size=%zu\n",
		 (unsigned long long)phys_addr, virt_addr, buffer_size);
}

static int __init mk_ipi_shared_setup(char *str)
{
	if (!str)
		return -EINVAL;

	mk_ipi_shared_phys_addr = memparse(str, NULL);
	if (!mk_ipi_shared_phys_addr) {
		pr_err("Invalid multikernel IPI shared memory address: %s\n", str);
		return -EINVAL;
	}

	pr_info("Multikernel IPI shared memory address: 0x%llx\n",
		(unsigned long long)mk_ipi_shared_phys_addr);
	return 0;
}
early_param("mk_ipi_shared", mk_ipi_shared_setup);

/**
 * multikernel_ipi_init - Initialize multikernel IPI subsystem
 *
 * Sets up IPI handling infrastructure.
 * - In spawned kernels: IPI buffer is mapped from boot parameter address
 * Returns 0 on success, negative error code on failure
 */
static int __init multikernel_ipi_init(void)
{
	/* Check if we're in a spawned kernel with IPI shared memory address */
	if (mk_ipi_shared_phys_addr) {
		/* Spawned kernel: Map the shared IPI memory */
		mk_this_kernel_ipi_data = memremap(mk_ipi_shared_phys_addr,
						   sizeof(struct mk_shared_data),
						   MEMREMAP_WB);
		if (!mk_this_kernel_ipi_data) {
			pr_err("Failed to map multikernel IPI shared memory at 0x%llx\n",
			       (unsigned long long)mk_ipi_shared_phys_addr);
			return -ENOMEM;
		}

		pr_info("Multikernel IPI subsystem initialized (spawned kernel): virt=%px, phys=0x%llx\n",
			mk_this_kernel_ipi_data, (unsigned long long)mk_ipi_shared_phys_addr);
	}

	return 0;
}
subsys_initcall(multikernel_ipi_init);

/* ---- Flexible shared memory APIs (PFN-based) ---- */
#define MK_PFN_IPI_TYPE 0x80000001U

/* Send a PFN to another kernel via mk_ipi_data */
int mk_send_pfn(int instance_id, unsigned long pfn)
{
	return multikernel_send_ipi_data(instance_id, &pfn, sizeof(pfn), MK_PFN_IPI_TYPE);
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
