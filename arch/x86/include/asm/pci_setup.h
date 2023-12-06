/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_PCI_SETUP_H
#define _ASM_X86_PCI_SETUP_H

#include <asm/bootparam.h>

struct pci_setup_rom {
	struct setup_data data;
	uint16_t vendor;
	uint16_t devid;
	uint64_t pcilen;
	unsigned long segment;
	unsigned long bus;
	unsigned long device;
	unsigned long function;
	uint8_t romdata[];
};

#endif /* _ASM_X86_PCI_SETUP_H */
