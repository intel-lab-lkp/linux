// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Host Controller Helper Functions
 *
 * Copyright (C) 2025 Hans Zhang
 *
 * Author: Hans Zhang <18255117159@163.com>
 */

#include <linux/pci.h>

#include "../pci.h"

/*
 * These interfaces resemble the pci_find_*capability() interfaces, but these
 * are for configuring host controllers, which are bridges *to* PCI devices but
 * are not PCI devices themselves.
 */
static u8 __pci_host_bridge_find_next_cap(void *priv,
					  pci_host_bridge_read_cfg read_cfg,
					  u8 cap_ptr, u8 cap)
{
	u8 cap_id, next_cap_ptr;
	u16 reg;

	if (!cap_ptr)
		return 0;

	reg = read_cfg(priv, cap_ptr, 2);
	cap_id = (reg & 0x00ff);

	if (cap_id > PCI_CAP_ID_MAX)
		return 0;

	if (cap_id == cap)
		return cap_ptr;

	next_cap_ptr = (reg & 0xff00) >> 8;
	return __pci_host_bridge_find_next_cap(priv, read_cfg, next_cap_ptr,
					       cap);
}

u8 pci_host_bridge_find_capability(void *priv,
				   pci_host_bridge_read_cfg read_cfg, u8 cap)
{
	u8 next_cap_ptr;
	u16 reg;

	reg = read_cfg(priv, PCI_CAPABILITY_LIST, 2);
	next_cap_ptr = (reg & 0x00ff);

	return __pci_host_bridge_find_next_cap(priv, read_cfg, next_cap_ptr,
					       cap);
}
EXPORT_SYMBOL_GPL(pci_host_bridge_find_capability);

static u16 pci_host_bridge_find_next_ext_capability(
	void *priv, pci_host_bridge_read_cfg read_cfg, u16 start, u8 cap)
{
	u32 header;
	int ttl;
	int pos = PCI_CFG_SPACE_SIZE;

	/* minimum 8 bytes per capability */
	ttl = (PCI_CFG_SPACE_EXP_SIZE - PCI_CFG_SPACE_SIZE) / 8;

	if (start)
		pos = start;

	header = read_cfg(priv, pos, 4);
	/*
	 * If we have no capabilities, this is indicated by cap ID,
	 * cap version and next pointer all being 0.
	 */
	if (header == 0)
		return 0;

	while (ttl-- > 0) {
		if (PCI_EXT_CAP_ID(header) == cap && pos != start)
			return pos;

		pos = PCI_EXT_CAP_NEXT(header);
		if (pos < PCI_CFG_SPACE_SIZE)
			break;

		header = read_cfg(priv, pos, 4);
	}

	return 0;
}

u16 pci_host_bridge_find_ext_capability(void *priv,
					pci_host_bridge_read_cfg read_cfg,
					u8 cap)
{
	return pci_host_bridge_find_next_ext_capability(priv, read_cfg, 0, cap);
}
EXPORT_SYMBOL_GPL(pci_host_bridge_find_ext_capability);
