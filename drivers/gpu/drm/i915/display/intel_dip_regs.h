/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef __INTEL_DIP_REGS_H__
#define __INTEL_DIP_REGS_H__

#include "intel_display_reg_defs.h"

/* EMP (Extended Metadata Packet) AS (Adaptive Sync) SDP Transmission Line */
#define _EMP_AS_SDP_TL_A			0x60204
#define EMP_AS_SDP_TL(display, trans)		_MMIO_TRANS2((display), (trans), _EMP_AS_SDP_TL_A)
#define   EMP_AS_SDP_DB_TL_MASK			REG_GENMASK(12, 0)
#define   EMP_AS_SDP_DB_TL(db_transmit_line)	REG_FIELD_PREP(EMP_AS_SDP_DB_TL_MASK, \
							       (db_transmit_line))

/* HDMI 2.1 Extended Metadata Packet (EMP) control/header; payload is HDMI_EMP_DATA */
#define _HDMI_EMP_CTL_A				0x600d0
#define HDMI_EMP_CTL(display, trans)		_MMIO_TRANS2((display), (trans), _HDMI_EMP_CTL_A)
#define   HDMI_EMP_CTL_TYPE_MASK		REG_GENMASK(3, 0)
#define   HDMI_EMP_CTL_TYPE_VTEM		REG_FIELD_PREP(HDMI_EMP_CTL_TYPE_MASK, 1)

#define _HDMI_EMP_HEADER_A			0x600d4
#define HDMI_EMP_HEADER(display, trans)		_MMIO_TRANS2((display), (trans), _HDMI_EMP_HEADER_A)
#define   HDMI_EMP_HEADER_END			REG_BIT(26)
#define   HDMI_EMP_HEADER_DS_TYPE_MASK		REG_GENMASK(25, 24)
#define   HDMI_EMP_HEADER_DS_TYPE_PSEUDO_STATIC	REG_FIELD_PREP(HDMI_EMP_HEADER_DS_TYPE_MASK, 0)
#define   HDMI_EMP_HEADER_NUM_PACKETS_MASK	REG_GENMASK(23, 16)
#define   HDMI_EMP_HEADER_NUM_PACKETS(n)	REG_FIELD_PREP(HDMI_EMP_HEADER_NUM_PACKETS_MASK, \
								       (n))
#define   HDMI_EMP_HEADER_HB0_MASK		REG_GENMASK(7, 0)
#define   HDMI_EMP_HEADER_HB0_EMP		REG_FIELD_PREP(HDMI_EMP_HEADER_HB0_MASK, 0x7f)

/* COMMON SDP TRANSMISSION LINE */
#define _CMN_SDP_TL_A			0x6020c
#define CMN_SDP_TL(display, trans)	_MMIO_TRANS2(display, (trans), _CMN_SDP_TL_A)
#define  TRANSMISSION_LINE_ENABLE	REG_BIT(31)
#define  BASE_TRANSMISSION_LINE_MASK	REG_GENMASK(12, 0)
#define  BASE_TRANSMISSION_LINE(x)	REG_FIELD_PREP(BASE_TRANSMISSION_LINE_MASK, x)

#define _CMN_SDP_TL_STGR_CTL_A			0x60214
#define CMN_SDP_TL_STGR_CTL(display, trans)	_MMIO_TRANS2(display, (trans), \
							     _CMN_SDP_TL_STGR_CTL_A)
#define  VSC_EXT_STAGGER_MASK			REG_GENMASK(11, 8)
#define  VSC_EXT_STAGGER(x)			REG_FIELD_PREP(VSC_EXT_STAGGER_MASK, x)
#define  VSC_EXT_STAGGER_DEFAULT		0x2
#define  PPS_STAGGER_MASK			REG_GENMASK(7, 4)
#define  PPS_STAGGER(x)				REG_FIELD_PREP(PPS_STAGGER_MASK, x)
#define  PPS_STAGGER_DEFAULT			0x1
#define  GMP_STAGGER_MASK			REG_GENMASK(3, 0)
#define  GMP_STAGGER(x)				REG_FIELD_PREP(GMP_STAGGER_MASK, x)
#define  GMP_STAGGER_DEFAULT			0x0

#endif /* __INTEL_DIP_REGS_H__ */
