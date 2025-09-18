// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Multikernel Technologies, Inc. All rights reserved
 */
#ifndef _LINUX_MULTIKERNEL_H
#define _LINUX_MULTIKERNEL_H

#include <linux/types.h>
#include <linux/irq_work.h>

/**
 * Multikernel IPI interface
 *
 * This header provides declarations for the multikernel IPI interface,
 * allowing modules to register callbacks for IPI events and pass data
 * between CPUs.
 */

/* Maximum data size that can be transferred via IPI */
#define MK_MAX_DATA_SIZE 256

/* Data structure for passing parameters via IPI */
struct mk_ipi_data {
	int sender_cpu;          /* Which CPU sent this IPI */
	unsigned int type;      /* User-defined type identifier */
	size_t data_size;        /* Size of the data */
	char buffer[MK_MAX_DATA_SIZE]; /* Actual data buffer */
};

/* Function pointer type for IPI callbacks */
typedef void (*mk_ipi_callback_t)(struct mk_ipi_data *data, void *ctx);

struct mk_ipi_handler {
	mk_ipi_callback_t callback;
	void *context;
	struct mk_ipi_handler *next;
	struct mk_ipi_data *saved_data;
	struct irq_work work;
};

/**
 * multikernel_register_handler - Register a callback for multikernel IPI
 * @callback: Function to call when IPI is received
 * @ctx: Context pointer passed to the callback
 *
 * Returns pointer to handler on success, NULL on failure
 */
struct mk_ipi_handler *multikernel_register_handler(mk_ipi_callback_t callback, void *ctx);

/**
 * multikernel_unregister_handler - Unregister a multikernel IPI callback
 * @handler: Handler pointer returned from multikernel_register_handler
 */
void multikernel_unregister_handler(struct mk_ipi_handler *handler);

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
int multikernel_send_ipi_data(int cpu, void *data, size_t data_size, unsigned long type);

void generic_multikernel_interrupt(void);

int __init multikernel_init(void);

/* Flexible shared memory APIs (PFN-based) */
int mk_send_pfn(int target_cpu, unsigned long pfn);
int mk_receive_pfn(struct mk_ipi_data *data, unsigned long *out_pfn);
void *mk_receive_map_page(struct mk_ipi_data *data);

#define mk_receive_unmap_page(p) memunmap(p)

#endif /* _LINUX_MULTIKERNEL_H */
