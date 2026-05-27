/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Marvell 88E6xxx Switch PTP support
 *
 * Copyright (c) 2008 Marvell Semiconductor
 *
 * Copyright (c) 2024 PADL Software Pty Ltd
 */

#ifndef _MV88E6XXX_AVB_H
#define _MV88E6XXX_AVB_H

#include "chip.h"

/* The Marvell 6352 and 6390 families support the Credit Based Shaper defined
 * in 802.1Qav. (The 6390 family also supports 802.1Qbv but that is presently
 * unimplemented.)
 *
 * On ingress, frame priority tags (PCP for L2) are mapped to an internal frame
 * priority, or FPri. This mapping is per-port on all switches that support
 * AVB. The 6352 family has a per-switch mapping between FPri and QPri (the TX
 * queue), whereas this mapping on the 6390 family is per-port. Both families
 * support per-port CBS policies.
 *
 * In addition to traffic shaping, the Marvell switches also support a form of
 * admission control, where true AVB frames are distinguished from other frames
 * that share the same frame priority. This is done by flagging ATU entries
 * with the ATU_DATA_STATE_{UC,MC}_STATIC_AVB_NRL flags. When the port is
 * configured in Enhanced, rather than Standard, AVB mode, AVB frames will only
 * be forwarded when the DA ATU entry has this bit set. Admission control would
 * typically be managed by a user-space 802.1Q Stream Reservation Protocol
 * (SRP) service. This, in combination with the global IsoPtrs register,
 * ensures that AVB streams always have priority over other traffic. These
 * features are necessary for Avnu certification.
 *
 * A final point is whether Linux TCs should be mapped to AVB classes or
 * directly to queues. Enhanced AVB support above requires dedicated, global
 * queues for Class A and B traffic, implying a mapping between TCs and AVB
 * classes. Unfortunately this means that Marvell switches that support a
 * larger number of TX queues (such as the 6390 family) must still funnel their
 * MQPRIO policy through these three TCs. Further, this limits the 6390 family
 * to per-switch MQPRIO policies whereas otherwise port-port policies could be
 * supported.
 *
 * With that in mind, the current implementation has the following properties:
 *
 *	- there are only three traffic classes, hi (2), lo (1) and legacy (0),
 *	  which correspond to AVB Class A, B, and non-AVB traffic
 *
 *	- only a single Ethernet frame priority can be mapped to either of the
 *	  AVB traffic classes
 *
 *	- legacy Ethernet frame priorities are distributed amongst the
 *	  remaining queues per the MQPRIO policy
 *
 *	- queue and frame priority policy is per-switch, not per-port, so
 *	  HW offload can only be enabled across multiple ports if the policy
 *	  on each port is the same
 *
 *	- on the 6352 family of switches, TC2 can only be in queue 2/3 and
 *	  TC1 only in queue 1/2; this does not apply to the 6390 family
 *
 *	- because the Netlink API has no way to distinguish between FDB/MDB
 *	  entries managed by SRP from those that are not, the
 *	  "marvell,mv88e6xxx-avb-mode" device tree property controls whether
 *	  a FDB or MDB entry is required in order for AVB frames to egress.
 *	  To avoid breaking static IP MDB entries, only multicast addresses
 *	  with OUI prefix of 91:e0:ff (IEEE 1722 Annex D) will have the AVB
 *	  flag set on their ATU entry.
 */

/* Global AVB registers */

/* Offset 0x00: AVB Global Config */

#define MV88E6XXX_AVB_CFG_AVB			0x00
#define MV88E6XXX_AVB_CFG_LEGACY		0x04

/* Common AVB Global Config */

#define MV88E6XXX_AVB_CFG_AVB_HI_FPRI_MASK	GENMASK(14, 12)
#define MV88E6XXX_AVB_CFG_AVB_HI_FPRI_GET(p)	FIELD_GET(MV88E6XXX_AVB_CFG_AVB_HI_FPRI_MASK, p)
#define MV88E6XXX_AVB_CFG_AVB_HI_FPRI_SET(p)	FIELD_PREP(MV88E6XXX_AVB_CFG_AVB_HI_FPRI_MASK, p)

#define MV88E6XXX_AVB_CFG_AVB_LO_FPRI_MASK	GENMASK(6, 4)
#define MV88E6XXX_AVB_CFG_AVB_LO_FPRI_GET(p)	FIELD_GET(MV88E6XXX_AVB_CFG_AVB_LO_FPRI_MASK, p)
#define MV88E6XXX_AVB_CFG_AVB_LO_FPRI_SET(p)	FIELD_PREP(MV88E6XXX_AVB_CFG_AVB_LO_FPRI_MASK, p)

#define MV88E6XXX_AVB_CFG_HI_LIMIT		0x08 /* max frame size for Class A */
#define MV88E6XXX_AVB_CFG_HI_LIMIT_MASK		GENMASK(10, 0)
#define MV88E6XXX_AVB_CFG_HI_LIMIT_GET(p)	FIELD_GET(MV88E6XXX_AVB_CFG_HI_LIMIT_MASK, p)
#define MV88E6XXX_AVB_CFG_HI_LIMIT_SET(p)	FIELD_PREP(MV88E6XXX_AVB_CFG_HI_LIMIT_MASK, p)

#define MV88E6XXX_AVB_CFG_OUI_HI		0x0C
#define MV88E6XXX_AVB_CFG_OUI_LO		0x0D

#define MV88E6XXX_PORT_QAV_CFG_RATE(queue)	((((queue) & 0x7) << 1))
#define MV88E6XXX_PORT_QAV_CFG_HI_LIMIT(queue)	((((queue) & 0x7) << 1) + 1)

/* 6352 Family AVB Global Config (4 TX queues) */

#define MV88E6352_AVB_CFG_AVB_HI_FPRI_GET(p)	MV88E6XXX_AVB_CFG_AVB_HI_FPRI_GET(p)
#define MV88E6352_AVB_CFG_AVB_HI_FPRI_SET(p)	MV88E6XXX_AVB_CFG_AVB_HI_FPRI_SET(p)

#define MV88E6352_AVB_CFG_AVB_HI_QPRI_MASK	GENMASK(9, 8)
#define MV88E6352_AVB_CFG_AVB_HI_QPRI_GET(p)	FIELD_GET(MV88E6352_AVB_CFG_AVB_HI_QPRI_MASK, p)
#define MV88E6352_AVB_CFG_AVB_HI_QPRI_SET(p)	FIELD_PREP(MV88E6352_AVB_CFG_AVB_HI_QPRI_MASK, p)

#define MV88E6352_AVB_CFG_AVB_LO_FPRI_GET(p)	MV88E6XXX_AVB_CFG_AVB_LO_FPRI_GET(p)
#define MV88E6352_AVB_CFG_AVB_LO_FPRI_SET(p)	MV88E6XXX_AVB_CFG_AVB_LO_FPRI_SET(p)

#define MV88E6352_AVB_CFG_AVB_LO_QPRI_MASK	GENMASK(1, 0)
#define MV88E6352_AVB_CFG_AVB_LO_QPRI_GET(p)	FIELD_GET(MV88E6352_AVB_CFG_AVB_LO_QPRI_MASK, p)
#define MV88E6352_AVB_CFG_AVB_LO_QPRI_SET(p)	FIELD_PREP(MV88E6352_AVB_CFG_AVB_LO_QPRI_MASK, p)

#define MV88E6352_PORT_QAV_CFG			0x08
#define MV88E6352_PORT_QAV_CFG_ENABLE		0x8000

/* 6390 Family AVB Global Config (8 TX queues) */

#define MV88E6390_AVB_CFG_AVB_HI_FPRI_GET(p)	MV88E6XXX_AVB_CFG_AVB_HI_FPRI_GET(p)
#define MV88E6390_AVB_CFG_AVB_HI_FPRI_SET(p)	MV88E6XXX_AVB_CFG_AVB_HI_FPRI_SET(p)

#define MV88E6390_AVB_CFG_AVB_HI_QPRI_MASK	GENMASK(10, 8)
#define MV88E6390_AVB_CFG_AVB_HI_QPRI_GET(p)	FIELD_GET(MV88E6390_AVB_CFG_AVB_HI_QPRI_MASK, p)
#define MV88E6390_AVB_CFG_AVB_HI_QPRI_SET(p)	FIELD_PREP(MV88E6390_AVB_CFG_AVB_HI_QPRI_MASK, p)

#define MV88E6390_AVB_CFG_AVB_LO_FPRI_GET(p)	MV88E6XXX_AVB_CFG_AVB_LO_FPRI_GET(p)
#define MV88E6390_AVB_CFG_AVB_LO_FPRI_SET(p)	MV88E6XXX_AVB_CFG_AVB_LO_FPRI_SET(p)

#define MV88E6390_AVB_CFG_AVB_LO_QPRI_MASK	GENMASK(2, 0)
#define MV88E6390_AVB_CFG_AVB_LO_QPRI_GET(p)	FIELD_GET(MV88E6390_AVB_CFG_AVB_LO_QPRI_MASK, p)
#define MV88E6390_AVB_CFG_AVB_LO_QPRI_SET(p)	FIELD_PREP(MV88E6390_AVB_CFG_AVB_LO_QPRI_MASK, p)

#define MV88E6352_AVB_QUEUE_MIN(tc)		(tc)
#define MV88E6352_AVB_QUEUE_MAX(tc)		((tc) + 1)

/* Global Qav registers */
#define MV88E6XXX_QAV_CFG			0x00

#define MV88E6XXX_QAV_CFG_GLOBAL_ISO_PTR_MASK	GENMASK(9, 0)
#define MV88E6XXX_QAV_CFG_GLOBAL_ISO_PTR_GET(x)	FIELD_GET(MV88E6XXX_QAV_CFG_GLOBAL_ISO_PTR_MASK, x)
#define MV88E6XXX_QAV_CFG_GLOBAL_ISO_PTR_SET(x)	FIELD_PREP(MV88E6XXX_QAV_CFG_GLOBAL_ISO_PTR_MASK, x)

/* allow mgmt frames in isochronous pointer pool */
#define MV88E6XXX_QAV_CFG_ADMIT_MGMT		0x8000

/* Per-port AVB registers */

/* Offset 0x00: AVB Port Config */
#define MV88E6XXX_PORT_AVB_CFG				0x00
#define MV88E6XXX_PORT_AVB_CFG_AVB_MODE			GENMASK(15, 14)
/* all frames legacy (non-AVB) unless overridden */
#define MV88E6XXX_PORT_AVB_CFG_AVB_MODE_LEGACY		0x0000
/* AVB frames indicated by priority */
#define MV88E6XXX_PORT_AVB_CFG_AVB_MODE_STANDARD	0x4000
/* STANDARD && ATU has STATIC_AVB_NRL bit set */
#define MV88E6XXX_PORT_AVB_CFG_AVB_MODE_ENHANCED	0x8000
/* ENHANCED && source port in destination port vector */
#define MV88E6XXX_PORT_AVB_CFG_AVB_MODE_SECURE		0xc000

#define MV88E6XXX_PORT_AVB_CFG_AVB_OVERRIDE		0x2000
#define MV88E6XXX_PORT_AVB_CFG_AVB_FILTER_BAD_AVB	0x1000
#define MV88E6XXX_PORT_AVB_CFG_AVB_TUNNEL		0x0800
#define MV88E6XXX_PORT_AVB_CFG_AVB_DISCARD_BAD		0x0400

/* action is mv88e6xxx_policy_action */
#define MV88E6XXX_PORT_AVB_CFG_AVB_HI_POLICY_MASK	GENMASK(3, 2)
#define MV88E6XXX_PORT_AVB_CFG_AVB_HI_POLICY_GET(p)	\
	FIELD_GET(MV88E6XXX_PORT_AVB_CFG_AVB_HI_POLICY_MASK, p)
#define MV88E6XXX_PORT_AVB_CFG_AVB_HI_POLICY_SET(p)	\
	FIELD_PREP(MV88E6XXX_PORT_AVB_CFG_AVB_HI_POLICY_MASK, p)

#define MV88E6XXX_PORT_AVB_CFG_AVB_LO_POLICY_MASK	GENMASK(1, 0)
#define MV88E6XXX_PORT_AVB_CFG_AVB_LO_POLICY_GET(p)	\
	FIELD_GET(MV88E6XXX_PORT_AVB_CFG_AVB_LO_POLICY_MASK, p)
#define MV88E6XXX_PORT_AVB_CFG_AVB_LO_POLICY_SET(p)	\
	FIELD_PREP(MV88E6XXX_PORT_AVB_CFG_AVB_LO_POLICY_MASK, p)

/* Per-family 802.1Qav operation tables */
extern const struct mv88e6xxx_tc_ops mv88e6341_tc_ops;
extern const struct mv88e6xxx_tc_ops mv88e6352_tc_ops;
extern const struct mv88e6xxx_tc_ops mv88e6390_tc_ops;

/* Enable or disable a port for AVB. Caller must acquire register lock.
 *
 * @param chip		Marvell switch chip instance
 * @param port		Switch port
 * @param enable	If true, will enable AVB queues on this port.
 *
 * @return		0 on success, or a negative error value otherwise
 */
int mv88e6xxx_avb_set_port_avb_mode(struct mv88e6xxx_chip *chip,
				    int port, bool enable);

/* Set AVB queue priority policy. Caller must acquire register lock.
 *
 * @param chip		Marvell switch chip instance
 * @param policy	policy settings to apply
 *
 * @return		0 on success, or a negative error value otherwise
 */
int mv88e6xxx_avb_tc_enable(struct mv88e6xxx_chip *chip,
			    const struct mv88e6xxx_avb_tc_policy *policy);

/* Clear AVB queue priority policy. Caller must acquire register lock.
 *
 * @param chip		Marvell switch chip instance
 *
 * @return		0 on success, or a negative error value otherwise
 */
int mv88e6xxx_avb_tc_disable(struct mv88e6xxx_chip *chip);

struct tc_cbs_qopt_offload;

/* Set AVB credit based shaper policy. Caller must acquire register lock.
 *
 * @param chip		Marvell switch chip instance
 * @param port		Switch port
 * @param cbs_qopt	CBS policy to apply
 *
 * @return		0 on success, or a negative error value otherwise
 */
int mv88e6xxx_qav_set_port_cbs_qopt(struct mv88e6xxx_chip *chip,
				    int port,
				    const struct tc_cbs_qopt_offload *cbs_qopt);

#endif /* _MV88E6XXX_AVB_H */
