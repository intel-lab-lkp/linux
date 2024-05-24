/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __INTEL_VRR_REG_H__
#define __INTEL_VRR_REG_H__

#define   VRR_CTL_CMRR_ENABLE			REG_BIT(27)

#define	_TRANS_CMRR_M_LO_A                  0x604F0
#define	_TRANS_CMRR_M_HI_A		            0x604F4
#define	_TRANS_CMRR_N_LO_A		            0x604F8
#define	_TRANS_CMRR_N_HI_A		            0x604FC
#define	TRANS_CMRR_M_LO(dev_priv, trans)    _MMIO_TRANS2(dev_priv, trans, _TRANS_CMRR_M_LO_A)
#define	TRANS_CMRR_M_HI(dev_priv, trans)    _MMIO_TRANS2(dev_priv, trans, _TRANS_CMRR_M_HI_A)
#define	TRANS_CMRR_N_LO(dev_priv, trans)    _MMIO_TRANS2(dev_priv, trans, _TRANS_CMRR_N_LO_A)
#define	TRANS_CMRR_N_HI(dev_priv, trans)    _MMIO_TRANS2(dev_priv, trans, _TRANS_CMRR_N_HI_A)

#endif /* __INTEL_VRR_REGS__ */

