/* SPDX-License-Identifier: GPL-2.0-only */

/*
 *   Hardware driver for NI Mite PCI interface chip
 *
 *   Copyright (C) 1999 David A. Schleef <ds@stm.lbl.gov>
 */

#ifndef _MITE_H_
#define _MITE_H_

#include <linux/pci.h>

#define PCI_VENDOR_ID_NATINST		0x1093

#define MITE_RING_SIZE 3000
struct mite_dma_chain {
	u32 count;
	u32 addr;
	u32 next;
};

struct mite_struct {
	struct mite_struct *next;
	int used;

	struct pci_dev *pcidev;
	unsigned long mite_phys_addr;
	void __iomem *mite_io_addr;
	unsigned long daq_phys_addr;
	void __iomem *daq_io_addr;

	int DMA_CheckNearEnd;

	struct mite_dma_chain ring[MITE_RING_SIZE];
};

extern struct mite_struct *mite_devices;

static inline unsigned int mite_irq(struct mite_struct *mite)
{
	return mite->pcidev->irq;
};

static inline unsigned int mite_device_id(struct mite_struct *mite)
{
	return mite->pcidev->device;
};

void mite_init(void);
void mite_cleanup(void);
int mite_setup(struct mite_struct *mite);
void mite_unsetup(struct mite_struct *mite);

enum mite_registers {
	MITE_IODWBSR = 0xc0,	// IO Device Window Base Size Register
	MITE_CSIGR = 0x460,	// chip signature
	MITE_IODWBSR_1 = 0xc4,	// IO Device Window Base Size Register 1 (used by 6602 boards)
	MITE_IODWCR_1 = 0xf4
};

enum MITE_IODWBSR_bits {
	WENAB = 0x80,		// window enable
	WENAB_6602 = 0x8c	// window enable for 6602 boards
};

#endif

