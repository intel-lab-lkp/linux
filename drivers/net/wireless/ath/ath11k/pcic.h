/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (c) 2019-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2022, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _ATH11K_PCI_CMN_H
#define _ATH11K_PCI_CMN_H

#include "core.h"

#define ATH11K_PCI_IRQ_CE0_OFFSET	3
#define ATH11K_PCI_IRQ_DP_OFFSET	14

#define ATH11K_PCI_CE_WAKE_IRQ	2

/* The BAR memory access model is a type of paging.
 * So let's spilt the BAR addresses into two parts: window and offset.
 * And split the register addresses into two parts: page and offset.
 * The window is used to map page.
 * Window 0 is always map to page 0.
 * Windows 1, 2 and 3 can be configuarable by ATH11K_PCI_WINDOW_CONFIG.
 */

#define ATH11K_PCI_REGADDR_PAGE_MASK		GENMASK(24, 19)
#define  ATH11K_PCI_REGADDR_PAGE(x)		FIELD_GET(ATH11K_PCI_REGADDR_PAGE_MASK, (x))
#define ATH11K_PCI_REGADDR_OFFSET_MASK		GENMASK(18, 0)
#define  ATH11K_PCI_REGADDR_OFFSET(x)		FIELD_GET(ATH11K_PCI_REGADDR_OFFSET_MASK, (x))

#define  ATH11K_PCI_BARADDR_WINDOW_MASK	GENMASK(20, 19)
#define  ATH11K_PCI_BARADDR_OFFSET_MASK	GENMASK(18, 0)
#define ATH11K_PCI_BARADDR(ab, window, offset) \
	((ab)->mem + \
	 FIELD_PREP(ATH11K_PCI_BARADDR_WINDOW_MASK, window) + \
	 FIELD_PREP(ATH11K_PCI_BARADDR_OFFSET_MASK, offset))

#define ATH11K_PCI_WINDOW_CONFIG		0x310c
#define  ATH11K_PCI_WINDOW_CONFIG_ENABLE	BIT(30)
#define  ATH11K_PCI_WINDOW_MAP_MASK(i)		(GENMASK(5, 0) << (6 * ((i) - 1)))
#define   ATH11K_PCI_WINDOW_MAP(i, page)	FIELD_PREP(ATH11K_PCI_WINDOW_MAP_MASK(i), (page))

/* BAR0 + 4k is always accessible, and no
 * need to force wakeup.
 * 4K - 32 = 0xFE0
 */
#define ATH11K_PCI_ACCESS_ALWAYS_OFF 0xFE0

int ath11k_pcic_get_user_msi_assignment(struct ath11k_base *ab, char *user_name,
					int *num_vectors, u32 *user_base_data,
					u32 *base_vector);
void ath11k_pcic_write32(struct ath11k_base *ab, u32 offset, u32 value);
u32 ath11k_pcic_read32(struct ath11k_base *ab, u32 offset);
void ath11k_pcic_get_msi_address(struct ath11k_base *ab, u32 *msi_addr_lo,
				 u32 *msi_addr_hi);
void ath11k_pcic_get_ce_msi_idx(struct ath11k_base *ab, u32 ce_id, u32 *msi_idx);
void ath11k_pcic_free_irq(struct ath11k_base *ab);
int ath11k_pcic_config_irq(struct ath11k_base *ab);
void ath11k_pcic_ext_irq_enable(struct ath11k_base *ab);
void ath11k_pcic_ext_irq_disable(struct ath11k_base *ab);
void ath11k_pcic_stop(struct ath11k_base *ab);
int ath11k_pcic_start(struct ath11k_base *ab);
int ath11k_pcic_map_service_to_pipe(struct ath11k_base *ab, u16 service_id,
				    u8 *ul_pipe, u8 *dl_pipe);
void ath11k_pcic_ce_irqs_enable(struct ath11k_base *ab);
void ath11k_pcic_ce_irq_disable_sync(struct ath11k_base *ab);
int ath11k_pcic_init_msi_config(struct ath11k_base *ab);
int ath11k_pcic_register_pci_ops(struct ath11k_base *ab,
				 const struct ath11k_pci_ops *pci_ops);
int ath11k_pcic_read(struct ath11k_base *ab, void *buf, u32 start, u32 end);
void ath11k_pci_enable_ce_irqs_except_wake_irq(struct ath11k_base *ab);
void ath11k_pci_disable_ce_irqs_except_wake_irq(struct ath11k_base *ab);

#endif
