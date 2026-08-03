/* SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause */
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 *
 * SRIOV support
 */

#ifndef _SRIOV_H
#define _SRIOV_H

#include <linux/pci.h>
#include "hfi2.h"

/* These describe how SRIOV allocates these resources to contexts */
#define HFI_MIN_PF0_RCVARY(c)	(512 * (c))
#define HFI_MIN_PF0_PIO(c)	(256 * (c))

int hfi2_sriov_set_si(struct hfi2_devdata *dd);
int hfi2_sriov_set_cfg(struct hfi2_devdata *dd);
void hfi2_sriov_free_cfg(struct hfi2_devdata *dd);
int hfi2_sriov_assign_rsrcs(struct hfi2_devdata *dd, struct hfi2_devrsrcs *vfr);
void hfi2_sriov_free_rsrcs(struct hfi2_devdata *dd, struct hfi2_devrsrcs *vfr);

int hfi2_sriov_init(struct pci_dev *pdev);
void hfi2_sriov_remove(struct pci_dev *pdev);
int hfi2_sriov_configure(struct pci_dev *pdev, int nvf);
int hfi2_sriov_auto_conf(struct hfi2_devdata *dd);

int hfi2_sriov_disable(struct pci_dev *pdev);

int hfi2_sriov_is_enabled(void);
int hfi2_sriov_get_config(struct hfi2_devdata *dd, struct hfi2_devrsrcs *out, int si);

#endif /* _SRIOV_H */
