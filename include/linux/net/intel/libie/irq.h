/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2025 Intel Corporation */

#ifndef __LIBIE_IRQ_H
#define __LIBIE_IRQ_H

#include <linux/pci.h>

/* In whole code in libie_irq index means the software 0-based irq index
 * for driver purpose, virq means the linux irq line number. Index can be used
 * for getting HW registers address and HW indexes (ex. for idpf there is
 * another irq index used in virtchnl communication which isn't driver index or
 * linux irq number). It is following the scheme from structure msi_map.
 */

/**
 * enum libie_irq_type - enum representing types of irq entries
 * @LIBIE_IRQ_STATIC: irq static allocated from kernel at driver probe
 * @LIBIE_IRQ_DYNAMIC: irq dynamic allocated during normal driver operation
 * @LIBIE_IRQ_NUM_TYPES: must be the last one, used to define the array size
 *
 * Enum is used to get software irq indexes from some kind of pool. The pool is
 * based on xa_array. Depending on the limit value passed to xa_alloc() software
 * irq indexes only from limited range can be returned.
 *
 * LIBIE_IRQ_STATIC .min = 0, max = 5 -> 5 irq static allocated to be sure that
 *	all default vport can operate
 * LIBIE_IRQ_DYNAMIC .min = 6, max = HW irq max -> rest to be dynamically used
 *	when needed
 */
enum libie_irq_type {
	LIBIE_IRQ_STATIC,
	LIBIE_IRQ_DYNAMIC,
	LIBIE_IRQ_NUM_TYPES,
};

/**
 * struct libie_irq_entry - structure to store irq entry information
 * @index: managed by software 0 based irq index, used to get correct hardware
 *	   information about irq (HW index and HW registers address)
 * @type: the type of irq, look at enum libie_irq_type for more information
 *
 * This structure is used to store the basic information about irq used during
 * alloc and free. Type needs to be known, because freeing dynamic type needs
 * extra call.
 */
struct libie_irq_entry {
	int index;
	enum libie_irq_type type;
};

/**
 * struct libie_irq - main structure to be used by libie_irq code
 * @pdev: pdev of driver that is using this lib
 * @limits: the irq pool scheme definition, take a look at irq_type note
 * @entries: xarray to store irq entries
 *
 * pdev and limits values need to be passed by the driver during lib
 * initialization.
 */
struct libie_irq {
	struct pci_dev *pdev;
	struct xa_limit limits[LIBIE_IRQ_NUM_TYPES];
	struct xarray entries;
};

/**
 * struct libie_vec_regs - hardware registers related to vector
 * @dyn_ctl: Dynamic control interrupt register offset
 * @itrn: Interrupt Throttling Rate register offset
 * @itrn_index_spacing: Register spacing between ITR registers of the same
 *			vector
 */
struct libie_vec_regs {
	u32 dyn_ctl;
	u32 itrn;
	u32 itrn_index_spacing;
};

/**
 * struct libie_hw_vector - single hardware vector info
 * @regs: address of irq registers
 * @idx: hardware vector index
 */
struct libie_hw_vector {
	struct libie_vec_regs regs;
	int idx;
};

/**
 * struct libie_irq_info - hardware data needed to setup irq
 * @vectors: allocated during initialization store hardware information
 *	     for all vectors that can be used on a whole device
 * @num: amount of vectors stored here
 */
struct libie_irq_info {
	struct libie_hw_vector *vectors;
	int num;
};

int libie_irq_init(struct libie_irq *irq, struct pci_dev *pdev,
		   int min, int max);
void libie_irq_deinit(struct libie_irq *irq);
struct msi_map libie_irq_alloc(struct libie_irq *irq, enum libie_irq_type type);
void libie_irq_free(struct libie_irq *irq, struct msi_map map);
int libie_irq_reserve(struct libie_irq *irq);
void libie_put_irq(struct libie_irq *irq, unsigned int index);

#endif /* __LIBIE_IRQ_H */
