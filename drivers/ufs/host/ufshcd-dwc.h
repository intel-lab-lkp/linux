/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * UFS Host driver for Synopsys Designware Core
 *
 * Copyright (C) 2015-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * Authors: Joao Pinto <jpinto@synopsys.com>
 */

#ifndef _UFSHCD_DWC_H
#define _UFSHCD_DWC_H

#include <ufs/ufshcd.h>

/* RMMI Attributes */
#define CBREFCLKCTRL2		0x8132
#define CBCRCTRL		0x811F
#define CBC10DIRECTCONF2	0x810E
#define CBRATESEL		0x8114
#define CBCREGADDRLSB		0x8116
#define CBCREGADDRMSB		0x8117
#define CBCREGWRLSB		0x8118
#define CBCREGWRMSB		0x8119
#define CBCREGRDLSB		0x811A
#define CBCREGRDMSB		0x811B
#define CBCREGRDWRSEL		0x811C

#define CBREFREFCLK_GATE_OVR_EN		BIT(7)

/* M-PHY registers */
#define RX_OVRD_IN_1(n)		(0x3006 + ((n) * 0x100))
#define RX_PCS_OUT(n)		(0x300F + ((n) * 0x100))
#define FAST_FLAGS(n)		(0x401C + ((n) * 0x100))
#define RX_AFE_ATT_IDAC(n)	(0x4000 + ((n) * 0x100))
#define RX_AFE_CTLE_IDAC(n)	(0x4001 + ((n) * 0x100))
#define FW_CALIB_CCFG(n)	(0x404D + ((n) * 0x100))

struct ufshcd_dme_attr_val {
	u32 attr_sel;
	u32 mib_val;
	u8 peer;
};

int ufshcd_dwc_link_startup_notify(struct ufs_hba *hba,
					enum ufs_notify_change_status status);
int ufshcd_dwc_dme_set_attrs(struct ufs_hba *hba,
				const struct ufshcd_dme_attr_val *v, int n);
int ufshcd_dwc_phy_reg_write(struct ufs_hba *hba, u32 addr, u32 val);
int ufshcd_dwc_phy_reg_read(struct ufs_hba *hba, u32 addr, u32 *val);
#endif /* End of Header */
